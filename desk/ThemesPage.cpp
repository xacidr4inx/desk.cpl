/* ---------------------------------------------------------
* Themes page
* 
* Responsible for the "Themes" tab
* 
------------------------------------------------------------*/

#include "pch.h"
#include "desk.h"
#include "helper.h"
#include "theme.h"
#include "ThemesPage.h"
#include "uxtheme.h"
#include "cscheme.h"
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Details;

namespace
{
CThemeDlgProc* g_themePageForApply = nullptr;
}

namespace
{
constexpr LPARAM THEME_COMBO_BROWSE_ONLINE = -2;
constexpr LPARAM THEME_COMBO_BROWSE_FILE = -3;
constexpr LPARAM THEME_COMBO_SAVED_FILE = -4;
constexpr LPARAM THEME_COMBO_CURRENT = -5;
constexpr LPARAM THEME_COMBO_STARTUP_BASELINE = -6;
constexpr LPARAM THEME_COMBO_SESSION_MODIFIED = -7;
constexpr LPCWSTR THEME_FILE_HISTORY_KEY = L"Software\\deskn\\SavedThemeHistory";
constexpr DWORD THEME_FILE_HISTORY_LIMIT = 256;
constexpr LPCWSTR CURRENT_THEME_KEY =
	L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\CurrentTheme";
constexpr LPCWSTR MODIFIED_THEME_VALUE = L"DisplayName of Modified";
constexpr LPCWSTR MODIFIED_THEME_SUFFIX = L" (Modified)";

std::wstring MakeThemeFileFilter()
{
	std::wstring filter = LoadDeskString(IDS_THEME_FILE_LABEL);
	filter.push_back(L'\0');
	filter += L"*.theme";
	filter.push_back(L'\0');
	filter += LoadDeskString(IDS_ALL_FILES_LABEL);
	filter.push_back(L'\0');
	filter += L"*.*";
	filter.push_back(L'\0');
	filter.push_back(L'\0');
	return filter;
}

bool GetCurrentThemeFilePath(WCHAR (&path)[MAX_PATH]);

void LogThemeApplyDebug(LPCWSTR format, ...)
{
	WCHAR tempPath[MAX_PATH] = {};
	if (!GetTempPathW(ARRAYSIZE(tempPath), tempPath)) return;
	if (FAILED(StringCchCatW(tempPath, ARRAYSIZE(tempPath), L"deskn-theme-apply.log"))) return;

	WCHAR line[1024] = {};
	va_list args;
	va_start(args, format);
	HRESULT formatResult = StringCchVPrintfW(line, ARRAYSIZE(line), format, args);
	va_end(args);
	if (FAILED(formatResult)) return;
	StringCchCatW(line, ARRAYSIZE(line), L"\r\n");

	HANDLE file = CreateFileW(tempPath, FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	WriteFile(file, line, static_cast<DWORD>(lstrlenW(line) * sizeof(WCHAR)),
		&written, nullptr);
	CloseHandle(file);
}

struct THEME_NONCLIENTMETRICSA
{
	DWORD cbSize;
	int iBorderWidth;
	int iScrollWidth;
	int iScrollHeight;
	int iCaptionWidth;
	int iCaptionHeight;
	LOGFONTA lfCaptionFont;
	int iSmCaptionWidth;
	int iSmCaptionHeight;
	LOGFONTA lfSmCaptionFont;
	int iMenuWidth;
	int iMenuHeight;
	LOGFONTA lfMenuFont;
	LOGFONTA lfStatusFont;
	LOGFONTA lfMessageFont;
};
static_assert(sizeof(THEME_NONCLIENTMETRICSA) == 340);

std::wstring ReplaceThemeToken(const std::wstring& input, LPCWSTR token, LPCWSTR replacement)
{
	std::wstring output;
	const WCHAR* cursor = input.c_str();
	while (const WCHAR* match = StrStrIW(cursor, token))
	{
		output.append(cursor, match);
		output.append(replacement);
		cursor = match + lstrlenW(token);
	}
	output.append(cursor);
	return output;
}

bool ReadThemeFilePath(LPCWSTR themeFile, LPCWSTR section, LPCWSTR key,
	WCHAR (&resolvedPath)[MAX_PATH], bool requireExistingFile = true)
{
	WCHAR rawPath[MAX_PATH] = {};
	if (!GetPrivateProfileStringW(section, key, L"", rawPath,
		ARRAYSIZE(rawPath), themeFile) || !rawPath[0])
		return false;

	WCHAR windowsDir[MAX_PATH] = {};
	if (!GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir)))
		return false;
	WCHAR resourceDir[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(resourceDir, ARRAYSIZE(resourceDir),
		L"%s\\Resources", windowsDir)))
		return false;

	std::wstring expandedInput = ReplaceThemeToken(rawPath, L"%ResourceDir%", resourceDir);
	expandedInput = ReplaceThemeToken(expandedInput, L"%WinDir%", windowsDir);
	DWORD expandedLength = ExpandEnvironmentStringsW(expandedInput.c_str(),
		resolvedPath, ARRAYSIZE(resolvedPath));
	if (!expandedLength || expandedLength > ARRAYSIZE(resolvedPath))
		return false;

	if (PathIsRelativeW(resolvedPath))
	{
		WCHAR themeDirectory[MAX_PATH] = {};
		if (FAILED(StringCchCopyW(themeDirectory, ARRAYSIZE(themeDirectory), themeFile)))
			return false;
		PathRemoveFileSpecW(themeDirectory);
		WCHAR absolutePath[MAX_PATH] = {};
		if (!PathCombineW(absolutePath, themeDirectory, resolvedPath) ||
			FAILED(StringCchCopyW(resolvedPath, ARRAYSIZE(resolvedPath), absolutePath)))
			return false;
	}
	return !requireExistingFile || PathFileExistsW(resolvedPath) != FALSE;
}

bool SavedThemeWallpaperMatchesCurrent(LPCWSTR themeFile)
{
	WCHAR savedRawPath[MAX_PATH] = {};
	GetPrivateProfileStringW(L"Control Panel\\Desktop", L"Wallpaper", L"",
		savedRawPath, ARRAYSIZE(savedRawPath), themeFile);

	WCHAR currentRawPath[MAX_PATH] = {};
	if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(currentRawPath),
		currentRawPath, 0))
		return false;
	if (!savedRawPath[0])
		return !currentRawPath[0];

	WCHAR savedPath[MAX_PATH] = {};
	if (!ReadThemeFilePath(themeFile, L"Control Panel\\Desktop", L"Wallpaper",
		savedPath, false))
		return false;

	WCHAR expandedCurrentPath[MAX_PATH] = {};
	DWORD expandedLength = ExpandEnvironmentStringsW(currentRawPath,
		expandedCurrentPath, ARRAYSIZE(expandedCurrentPath));
	if (!expandedLength || expandedLength > ARRAYSIZE(expandedCurrentPath))
		return false;

	WCHAR absoluteSavedPath[MAX_PATH] = {};
	WCHAR absoluteCurrentPath[MAX_PATH] = {};
	DWORD savedLength = GetFullPathNameW(savedPath, ARRAYSIZE(absoluteSavedPath),
		absoluteSavedPath, nullptr);
	DWORD currentLength = GetFullPathNameW(expandedCurrentPath,
		ARRAYSIZE(absoluteCurrentPath), absoluteCurrentPath, nullptr);
	if (!savedLength || savedLength >= ARRAYSIZE(absoluteSavedPath) ||
		!currentLength || currentLength >= ARRAYSIZE(absoluteCurrentPath))
		return false;

	return StrCmpIW(absoluteSavedPath, absoluteCurrentPath) == 0;
}

bool ReadPersistedModifiedThemeName(WCHAR (&name)[MAX_PATH])
{
	name[0] = L'\0';
	DWORD size = sizeof(name);
	return RegGetValueW(HKEY_CURRENT_USER, CURRENT_THEME_KEY,
		MODIFIED_THEME_VALUE, RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS &&
		name[0] != L'\0';
}

LSTATUS PersistModifiedThemeName(bool modified, LPCWSTR parentName)
{
	if (!modified)
	{
		HKEY key = nullptr;
		LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, CURRENT_THEME_KEY, 0,
			KEY_SET_VALUE, &key);
		if (status == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
		if (status != ERROR_SUCCESS) return status;
		status = RegDeleteValueW(key, MODIFIED_THEME_VALUE);
		RegCloseKey(key);
		return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
	}

	WCHAR value[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(value, ARRAYSIZE(value), L"%s%s",
		parentName && parentName[0] ? parentName : LoadDeskString(IDS_MY_CURRENT_THEME).c_str(),
		MODIFIED_THEME_SUFFIX)))
		return ERROR_INSUFFICIENT_BUFFER;

	HKEY key = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, CURRENT_THEME_KEY, 0,
		nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (status != ERROR_SUCCESS) return status;
	status = RegSetValueExW(key, MODIFIED_THEME_VALUE, 0, REG_SZ,
		reinterpret_cast<const BYTE*>(value),
		(static_cast<DWORD>(lstrlenW(value)) + 1) * sizeof(WCHAR));
	RegCloseKey(key);
	return status;
}

void RemoveModifiedSuffix(WCHAR (&name)[MAX_PATH])
{
	const size_t nameLength = wcslen(name);
	const size_t suffixLength = wcslen(MODIFIED_THEME_SUFFIX);
	if (nameLength >= suffixLength &&
		CompareStringOrdinal(name + nameLength - suffixLength,
			static_cast<int>(suffixLength), MODIFIED_THEME_SUFFIX,
			static_cast<int>(suffixLength), TRUE) == CSTR_EQUAL)
		name[nameLength - suffixLength] = L'\0';
}

class ScopedPreviewScheme
{
	SCHEMEDATA* previousScheme;
public:
	explicit ScopedPreviewScheme(SCHEMEDATA* previewScheme)
		: previousScheme(selectedTheme ? selectedTheme->selectedScheme : nullptr)
	{
		if (selectedTheme && previewScheme)
			selectedTheme->selectedScheme = previewScheme;
	}
	~ScopedPreviewScheme()
	{
		if (selectedTheme)
			selectedTheme->selectedScheme = previousScheme;
	}
};

class ScopedThemeSelection
{
public:
	ScopedThemeSelection()
	{
		InterlockedExchange(&g_themeSelectionInProgress, TRUE);
	}
	~ScopedThemeSelection()
	{
		InterlockedExchange(&g_themeSelectionInProgress, FALSE);
	}
};

LSTATUS ApplyClassicScheme(HWND hwnd, int dpi, SCHEMEDATA* scheme,
	LPCWSTR sourceThemePath = nullptr)
{
	static constexpr LPCWSTR colorNames[MAX_COLORS] =
	{
		L"Scrollbar", L"Background", L"ActiveTitle", L"InactiveTitle",
		L"Menu", L"Window", L"WindowFrame", L"MenuText", L"WindowText",
		L"TitleText", L"ActiveBorder", L"InactiveBorder", L"AppWorkSpace",
		L"Hilight", L"HilightText", L"ButtonFace", L"ButtonShadow",
		L"GrayText", L"ButtonText", L"InactiveTitleText", L"ButtonHilight",
		L"ButtonDkShadow", L"ButtonLight", L"InfoText", L"InfoWindow",
		L"ButtonAlternateFace", L"HotTrackingColor", L"GradientActiveTitle",
		L"GradientInactiveTitle"
	};
	if (!scheme) return ERROR_NOT_ENOUGH_MEMORY;
	if (!scheme->name[0]) return ERROR_INVALID_PARAMETER;

	// The legacy scheme structure stops at GradientInactiveTitle (index 28),
	// but the Classic palette worker reads Color0 through Color30 as one set.
	// A newly created scheme with only 29 values makes that read fail and lets
	// the worker restore its old palette after Theme Manager has applied ours.
	constexpr int colorCount = COLOR_MENUBAR + 1;
	COLORREF colors[colorCount] = {};
	memcpy(colors, scheme->rgb, sizeof(scheme->rgb));
	static constexpr LPCWSTR extendedNames[] = { L"MenuHilight", L"MenuBar" };
	for (int i = MAX_COLORS; i < colorCount; ++i)
	{
		colors[i] = GetSysColor(i) & 0x00FFFFFF;
		if (!sourceThemePath || !PathFileExistsW(sourceThemePath)) continue;
		WCHAR value[64] = {};
		if (!GetPrivateProfileStringW(L"Control Panel\\Colors",
			extendedNames[i - MAX_COLORS], L"", value, ARRAYSIZE(value),
			sourceThemePath)) continue;
		unsigned red = 0, green = 0, blue = 0;
		if (swscanf_s(value, L"%u %u %u", &red, &green, &blue) == 3 &&
			red <= 255 && green <= 255 && blue <= 255)
			colors[i] = RGB(red, green, blue);
	}

	HKEY colorsKey = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Colors",
		0, nullptr, 0, KEY_SET_VALUE, nullptr, &colorsKey, nullptr);
	if (status != ERROR_SUCCESS) return status;
	for (int i = 0; i < colorCount; ++i)
	{
		WCHAR value[32] = {};
		COLORREF color = colors[i] & 0x00FFFFFF;
		if (FAILED(StringCchPrintfW(value, ARRAYSIZE(value), L"%u %u %u",
			GetRValue(color), GetGValue(color), GetBValue(color))))
		{
			status = ERROR_INVALID_DATA;
			break;
		}
		status = RegSetValueExW(colorsKey,
			i < MAX_COLORS ? colorNames[i] : extendedNames[i - MAX_COLORS], 0, REG_SZ,
			reinterpret_cast<const BYTE*>(value),
			static_cast<DWORD>((lstrlenW(value) + 1) * sizeof(WCHAR)));
		if (status != ERROR_SUCCESS) break;
	}
	RegCloseKey(colorsKey);
	if (status != ERROR_SUCCESS) return status;

	HKEY schemesKey = nullptr;
	status = RegCreateKeyExW(HKEY_CURRENT_USER,
		L"Control Panel\\Appearance\\ClassicSchemes", 0, nullptr, 0,
		KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &schemesKey, nullptr);
	if (status != ERROR_SUCCESS) return status;
	HKEY selectedKey = nullptr;
	status = RegCreateKeyExW(schemesKey, scheme->name, 0, nullptr, 0,
		KEY_SET_VALUE, nullptr, &selectedKey, nullptr);
	if (status == ERROR_SUCCESS)
	{
		for (int i = 0; i < colorCount; ++i)
		{
			WCHAR valueName[16] = {};
			StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Color%d", i);
			DWORD color = colors[i] & 0x00FFFFFF;
			status = RegSetValueExW(selectedKey, valueName, 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&color), sizeof(color));
			if (status != ERROR_SUCCESS) break;
		}
		RegCloseKey(selectedKey);
	}
	if (status == ERROR_SUCCESS)
	{
		status = RegSetValueExW(schemesKey, L"SelectedScheme", 0, REG_SZ,
			reinterpret_cast<const BYTE*>(scheme->name),
			static_cast<DWORD>((lstrlenW(scheme->name) + 1) * sizeof(WCHAR)));
	}
	RegCloseKey(schemesKey);
	if (status != ERROR_SUCCESS) return status;

	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	memcpy(&ncm, &scheme->ncm, sizeof(scheme->ncm));
	ncm.cbSize = sizeof(ncm);
	ncm.iPaddedBorderWidth = MulDiv(scheme->iPaddedBorderWidth, dpi, 96);
	ScaleNonClientMetrics(ncm, dpi);
	LOGFONT iconFont = scheme->lfIconTitle;
	ScaleLogFont(iconFont, dpi);
	if (!SystemParametersInfoW(SPI_SETICONTITLELOGFONT, sizeof(iconFont), &iconFont,
		SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
		return GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
	if (!SystemParametersInfoW(SPI_SETNONCLIENTMETRICS, sizeof(ncm), &ncm,
		SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
		return GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
	int elements[colorCount];
	for (int i = 0; i < colorCount; ++i) elements[i] = i;
	if (!SetSysColors(colorCount, elements, colors))
		return GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
	UNREFERENCED_PARAMETER(hwnd);
	return ERROR_SUCCESS;
}

LSTATUS ApplyWindowsStandardClassicScheme(HWND hwnd, int dpi)
{
	return ApplyClassicScheme(hwnd, dpi, LoadWindowsStandardClassicScheme());
}

bool IsWindowsStandardScheme(const SCHEMEDATA* scheme)
{
	return !scheme || !scheme->name[0] || StrCmpI(scheme->name, L"Windows Standard") == 0;
}

bool IsCustomClassicScheme(const SCHEMEDATA* scheme)
{
	return scheme && scheme->name[0] && !IsWindowsStandardScheme(scheme);
}

bool LoadClassicSchemeFromThemeFileInternal(LPCWSTR path, SCHEMEDATA& scheme)
{
	static constexpr LPCWSTR colorNames[MAX_COLORS] =
	{
		L"Scrollbar", L"Background", L"ActiveTitle", L"InactiveTitle",
		L"Menu", L"Window", L"WindowFrame", L"MenuText", L"WindowText",
		L"TitleText", L"ActiveBorder", L"InactiveBorder", L"AppWorkSpace",
		L"Hilight", L"HilightText", L"ButtonFace", L"ButtonShadow",
		L"GrayText", L"ButtonText", L"InactiveTitleText", L"ButtonHilight",
		L"ButtonDkShadow", L"ButtonLight", L"InfoText", L"InfoWindow",
		L"ButtonAlternateFace", L"HotTrackingColor", L"GradientActiveTitle",
		L"GradientInactiveTitle"
	};
	if (!path || !PathFileExistsW(path)) return false;

	SCHEMEDATA* standardScheme = LoadWindowsStandardClassicScheme();
	if (!standardScheme) return false;
	scheme = *standardScheme;
	scheme.variant = 0x18;
	scheme.dpiScaled = FALSE;

	WCHAR displayName[ARRAYSIZE(scheme.name)] = {};
	GetPrivateProfileStringW(L"Theme", L"DisplayName", L"", displayName,
		ARRAYSIZE(displayName), path);
	if (!displayName[0])
	{
		StringCchCopyW(displayName, ARRAYSIZE(displayName), PathFindFileNameW(path));
		PathRemoveExtensionW(displayName);
	}
	StringCchCopyW(scheme.name, ARRAYSIZE(scheme.name), displayName);
	WCHAR parentSchemeName[ARRAYSIZE(scheme.name)] = {};
	if (GetPrivateProfileStringW(L"Control Panel\\Appearance", L"SchemeName",
		L"", parentSchemeName, ARRAYSIZE(parentSchemeName), path))
		StringCchCopyW(scheme.name, ARRAYSIZE(scheme.name), parentSchemeName);

	// Preserve the saved theme's own Classic font and metrics. In particular,
	// Rose-derived themes can intentionally differ from Windows Standard.
	WCHAR serializedMetrics[4096] = {};
	if (GetPrivateProfileStringW(L"Metrics", L"NonclientMetrics", L"",
		serializedMetrics, ARRAYSIZE(serializedMetrics), path))
	{
		BYTE metricBytes[sizeof(THEME_NONCLIENTMETRICSA)] = {};
		const WCHAR* cursor = serializedMetrics;
		bool valid = true;
		for (size_t i = 0; i < ARRAYSIZE(metricBytes); ++i)
		{
			while (*cursor == L' ' || *cursor == L'\t') ++cursor;
			if (!*cursor)
			{
				valid = false;
				break;
			}
			WCHAR* end = nullptr;
			unsigned long value = wcstoul(cursor, &end, 10);
			if (end == cursor || value > 255)
			{
				valid = false;
				break;
			}
			metricBytes[i] = static_cast<BYTE>(value);
			cursor = end;
		}
		while (*cursor == L' ' || *cursor == L'\t') ++cursor;
		valid = valid && !*cursor;
		if (valid)
		{
			THEME_NONCLIENTMETRICSA metrics = {};
			memcpy(&metrics, metricBytes, sizeof(metrics));
			if (metrics.cbSize == sizeof(metrics))
			{
				const auto convertFont = [](const LOGFONTA& source,
					LOGFONTW& destination) -> bool
				{
					destination.lfHeight = source.lfHeight;
					destination.lfWidth = source.lfWidth;
					destination.lfEscapement = source.lfEscapement;
					destination.lfOrientation = source.lfOrientation;
					destination.lfWeight = source.lfWeight;
					destination.lfItalic = source.lfItalic;
					destination.lfUnderline = source.lfUnderline;
					destination.lfStrikeOut = source.lfStrikeOut;
					destination.lfCharSet = source.lfCharSet;
					destination.lfOutPrecision = source.lfOutPrecision;
					destination.lfClipPrecision = source.lfClipPrecision;
					destination.lfQuality = source.lfQuality;
					destination.lfPitchAndFamily = source.lfPitchAndFamily;
					return MultiByteToWideChar(CP_ACP, 0, source.lfFaceName, -1,
						destination.lfFaceName, LF_FACESIZE) != 0;
				};

				NONCLIENTMETRICSW_2k& ncm = scheme.ncm;
				ncm.iBorderWidth = metrics.iBorderWidth;
				ncm.iScrollWidth = metrics.iScrollWidth;
				ncm.iScrollHeight = metrics.iScrollHeight;
				ncm.iCaptionWidth = metrics.iCaptionWidth;
				ncm.iCaptionHeight = metrics.iCaptionHeight;
				ncm.iSmCaptionWidth = metrics.iSmCaptionWidth;
				ncm.iSmCaptionHeight = metrics.iSmCaptionHeight;
				ncm.iMenuWidth = metrics.iMenuWidth;
				ncm.iMenuHeight = metrics.iMenuHeight;
				if (convertFont(metrics.lfCaptionFont, ncm.lfCaptionFont) &&
					convertFont(metrics.lfSmCaptionFont, ncm.lfSmCaptionFont) &&
					convertFont(metrics.lfMenuFont, ncm.lfMenuFont) &&
					convertFont(metrics.lfStatusFont, ncm.lfStatusFont) &&
					convertFont(metrics.lfMessageFont, ncm.lfMessageFont))
					scheme.dpiScaled = FALSE;
			}
		}
	}

	bool hasColors = false;
	for (int i = 0; i < MAX_COLORS; ++i)
	{
		WCHAR value[64] = {};
		if (!GetPrivateProfileStringW(L"Control Panel\\Colors", colorNames[i],
			L"", value, ARRAYSIZE(value), path))
			continue;
		unsigned red = 0, green = 0, blue = 0;
		if (swscanf_s(value, L"%u %u %u", &red, &green, &blue) != 3 ||
			red > 255 || green > 255 || blue > 255)
			continue;
		scheme.rgb[i] = RGB(red, green, blue);
		hasColors = true;
	}
	return hasColors;
}

LSTATUS WriteClassicSchemeToThemeFileInternal(LPCWSTR path, const SCHEMEDATA* scheme)
{
	static constexpr LPCWSTR colorNames[MAX_COLORS] =
	{
		L"Scrollbar", L"Background", L"ActiveTitle", L"InactiveTitle",
		L"Menu", L"Window", L"WindowFrame", L"MenuText", L"WindowText",
		L"TitleText", L"ActiveBorder", L"InactiveBorder", L"AppWorkSpace",
		L"Hilight", L"HilightText", L"ButtonFace", L"ButtonShadow",
		L"GrayText", L"ButtonText", L"InactiveTitleText", L"ButtonHilight",
		L"ButtonDkShadow", L"ButtonLight", L"InfoText", L"InfoWindow",
		L"ButtonAlternateFace", L"HotTrackingColor", L"GradientActiveTitle",
		L"GradientInactiveTitle"
	};
	if (!path || !scheme) return ERROR_INVALID_PARAMETER;
	if (!WritePrivateProfileStringW(L"Control Panel\\Appearance", L"SchemeName",
		scheme->name, path))
		return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
	for (int i = 0; i < MAX_COLORS; ++i)
	{
		WCHAR value[32] = {};
		COLORREF color = scheme->rgb[i] & 0x00FFFFFF;
		if (FAILED(StringCchPrintfW(value, ARRAYSIZE(value), L"%u %u %u",
			GetRValue(color), GetGValue(color), GetBValue(color))))
			return ERROR_INVALID_DATA;
		if (!WritePrivateProfileStringW(L"Control Panel\\Colors", colorNames[i], value, path))
			return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
	}

	const auto convertFont = [](const LOGFONTW& source, LOGFONTA& destination) -> bool
	{
		destination.lfHeight = source.lfHeight;
		destination.lfWidth = source.lfWidth;
		destination.lfEscapement = source.lfEscapement;
		destination.lfOrientation = source.lfOrientation;
		destination.lfWeight = source.lfWeight;
		destination.lfItalic = source.lfItalic;
		destination.lfUnderline = source.lfUnderline;
		destination.lfStrikeOut = source.lfStrikeOut;
		destination.lfCharSet = source.lfCharSet;
		destination.lfOutPrecision = source.lfOutPrecision;
		destination.lfClipPrecision = source.lfClipPrecision;
		destination.lfQuality = source.lfQuality;
		destination.lfPitchAndFamily = source.lfPitchAndFamily;
		return WideCharToMultiByte(CP_ACP, 0, source.lfFaceName, -1,
			destination.lfFaceName, LF_FACESIZE, nullptr, nullptr) != 0;
	};

	THEME_NONCLIENTMETRICSA metrics = {};
	metrics.cbSize = sizeof(metrics);
	metrics.iBorderWidth = scheme->ncm.iBorderWidth;
	metrics.iScrollWidth = scheme->ncm.iScrollWidth;
	metrics.iScrollHeight = scheme->ncm.iScrollHeight;
	metrics.iCaptionWidth = scheme->ncm.iCaptionWidth;
	metrics.iCaptionHeight = scheme->ncm.iCaptionHeight;
	metrics.iSmCaptionWidth = scheme->ncm.iSmCaptionWidth;
	metrics.iSmCaptionHeight = scheme->ncm.iSmCaptionHeight;
	metrics.iMenuWidth = scheme->ncm.iMenuWidth;
	metrics.iMenuHeight = scheme->ncm.iMenuHeight;
	if (!convertFont(scheme->ncm.lfCaptionFont, metrics.lfCaptionFont) ||
		!convertFont(scheme->ncm.lfSmCaptionFont, metrics.lfSmCaptionFont) ||
		!convertFont(scheme->ncm.lfMenuFont, metrics.lfMenuFont) ||
		!convertFont(scheme->ncm.lfStatusFont, metrics.lfStatusFont) ||
		!convertFont(scheme->ncm.lfMessageFont, metrics.lfMessageFont))
		return GetLastError() ? GetLastError() : ERROR_NO_UNICODE_TRANSLATION;

	const BYTE* metricBytes = reinterpret_cast<const BYTE*>(&metrics);
	std::wstring serializedMetrics;
	serializedMetrics.reserve(sizeof(metrics) * 4);
	for (size_t i = 0; i < sizeof(metrics); ++i)
	{
		if (i) serializedMetrics.push_back(L' ');
		serializedMetrics += std::to_wstring(metricBytes[i]);
	}
	if (!WritePrivateProfileStringW(L"Metrics", L"NonclientMetrics",
		serializedMetrics.c_str(), path))
		return GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
	return ERROR_SUCCESS;
}

bool GetThemeIdRegistryPath(ITheme10* theme, WCHAR (&keyPath)[128])
{
	GUID id = {};
	WCHAR idText[64] = {};
	if (!theme || FAILED(theme->get_ThemeId(&id)) ||
		!StringFromGUID2(id, idText, ARRAYSIZE(idText))) return false;
	return SUCCEEDED(StringCchPrintfW(keyPath, ARRAYSIZE(keyPath),
		L"Software\\deskn\\SavedThemes\\%s", idText));
}

bool GetRegisteredThemeFilePath(ITheme10* theme, WCHAR (&path)[MAX_PATH])
{
	path[0] = L'\0';
	WCHAR keyPath[128] = {};
	if (!GetThemeIdRegistryPath(theme, keyPath)) return false;
	DWORD size = sizeof(path);
	return RegGetValueW(HKEY_CURRENT_USER, keyPath, L"Path", RRF_RT_REG_SZ,
		nullptr, path, &size) == ERROR_SUCCESS && path[0] &&
		StrCmpIW(PathFindExtensionW(path), L".theme") == 0;
}

void ShowMissingThemeFileError(HWND owner, LPCWSTR path, bool suppressDuplicate = false)
{
	if (!path || !path[0]) return;
	static std::wstring lastReportedPath;
	if (suppressDuplicate && StrCmpIW(lastReportedPath.c_str(), path) == 0) return;
	lastReportedPath = path;
	WCHAR message[MAX_PATH + 96] = {};
	if (FAILED(StringCchPrintfW(message, ARRAYSIZE(message),
		LoadDeskString(IDS_THEME_FILE_INVALID).c_str(), path)))
		StringCchCopyW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_THEME_FILE_UNAVAILABLE).c_str());
	::MessageBoxW(owner, message, LoadDeskString(IDS_ERROR_LOADING_THEME).c_str(),
		MB_OK | MB_ICONERROR);
}

bool IsReservedCurrentThemeFile(LPCWSTR path)
{
	// Windows' generated working theme and deskn's baseline snapshot are not
	// user-saved theme files.
	LPCWSTR fileName = PathFindFileNameW(path);
	return StrCmpIW(fileName, L"Custom.theme") == 0 ||
		StrCmpIW(fileName, L"deskn-current.theme") == 0;
}

bool GetCurrentThemeFilePath(WCHAR (&path)[MAX_PATH])
{
	path[0] = L'\0';
	WCHAR registryPath[MAX_PATH] = {};
	DWORD size = sizeof(registryPath);
	if (RegGetValueW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes",
		L"CurrentTheme", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
		nullptr, registryPath, &size) == ERROR_SUCCESS &&
		ExpandEnvironmentStringsW(registryPath, path, ARRAYSIZE(path)) &&
		PathFileExistsW(path))
		return true;

	// Some Windows builds do not maintain the CurrentTheme registry value even
	// though Theme Manager keeps its generated current theme here.
	WCHAR localAppData[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
		SHGFP_TYPE_CURRENT, localAppData)))
		return false;
	return SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path),
		L"%s\\Microsoft\\Windows\\Themes\\Custom.theme", localAppData)) &&
		PathFileExistsW(path);
}

bool UpdateCurrentThemeSnapshot(std::wstring& snapshotPath)
{
	WCHAR localAppData[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
		SHGFP_TYPE_CURRENT, localAppData)))
		return false;

	WCHAR themesDirectory[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(themesDirectory, ARRAYSIZE(themesDirectory),
		L"%s\\Microsoft\\Windows\\Themes", localAppData)))
		return false;
	if (!CreateDirectoryW(themesDirectory, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
		return false;

	WCHAR destination[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(destination, ARRAYSIZE(destination),
		L"%s\\deskn-current.theme", themesDirectory)))
		return false;

	WCHAR source[MAX_PATH] = {};
	if (!GetCurrentThemeFilePath(source))
		return false;
	if (StrCmpIW(source, destination) != 0 && !CopyFileW(source, destination, FALSE))
		return false;

	snapshotPath = destination;
	return true;
}

bool IsAvailableThemeFile(LPCWSTR path)
{
	return path && path[0] && !IsReservedCurrentThemeFile(path) &&
		StrCmpIW(PathFindExtensionW(path), L".theme") == 0 && PathFileExistsW(path);
}

bool IsSystemThemeFilePath(LPCWSTR path)
{
	if (!path || !path[0]) return false;
	WCHAR windowsDir[MAX_PATH] = {};
	UINT windowsDirLength = GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir));
	if (!windowsDirLength || windowsDirLength >= ARRAYSIZE(windowsDir)) return false;
	WCHAR themeDirectory[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(themeDirectory, ARRAYSIZE(themeDirectory),
		L"%s\\Resources\\Themes\\", windowsDir))) return false;
	WCHAR fullPath[MAX_PATH] = {};
	DWORD fullPathLength = GetFullPathNameW(path, ARRAYSIZE(fullPath), fullPath, nullptr);
	if (!fullPathLength || fullPathLength >= ARRAYSIZE(fullPath)) return false;
	return _wcsnicmp(fullPath, themeDirectory, lstrlenW(themeDirectory)) == 0;
}

void RememberThemeFileInHistory(LPCWSTR path)
{
	if (!IsAvailableThemeFile(path)) return;
	HKEY historyKey = nullptr;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, THEME_FILE_HISTORY_KEY, 0, nullptr, 0,
		KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &historyKey, nullptr) != ERROR_SUCCESS)
		return;

	DWORD firstFree = THEME_FILE_HISTORY_LIMIT;
	for (DWORD i = 0; i < THEME_FILE_HISTORY_LIMIT; ++i)
	{
		WCHAR valueName[16] = {};
		StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Path%03u", i);
		WCHAR existingPath[MAX_PATH] = {};
		DWORD size = sizeof(existingPath);
		LSTATUS status = RegGetValueW(historyKey, nullptr, valueName, RRF_RT_REG_SZ,
			nullptr, existingPath, &size);
		if (status != ERROR_SUCCESS)
		{
			if (firstFree == THEME_FILE_HISTORY_LIMIT) firstFree = i;
			continue;
		}
		if (!PathFileExistsW(existingPath))
		{
			RegDeleteValueW(historyKey, valueName);
			if (firstFree == THEME_FILE_HISTORY_LIMIT) firstFree = i;
			continue;
		}
		if (StrCmpIW(existingPath, path) == 0)
		{
			RegCloseKey(historyKey);
			return;
		}
	}

	if (firstFree < THEME_FILE_HISTORY_LIMIT)
	{
		WCHAR valueName[16] = {};
		StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Path%03u", firstFree);
		RegSetValueExW(historyKey, valueName, 0, REG_SZ,
			reinterpret_cast<const BYTE*>(path),
			(static_cast<DWORD>(lstrlenW(path)) + 1) * sizeof(WCHAR));
	}
	RegCloseKey(historyKey);
}

bool GetThemeFileFromHistory(DWORD index, WCHAR (&path)[MAX_PATH])
{
	path[0] = L'\0';
	WCHAR valueName[16] = {};
	StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Path%03u", index);
	HKEY historyKey = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, THEME_FILE_HISTORY_KEY, 0,
		KEY_QUERY_VALUE | KEY_SET_VALUE, &historyKey) != ERROR_SUCCESS)
		return false;
	DWORD size = sizeof(path);
	LSTATUS status = RegGetValueW(historyKey, nullptr, valueName, RRF_RT_REG_SZ,
		nullptr, path, &size);
	RegCloseKey(historyKey);
	if (status != ERROR_SUCCESS) return false;
	if (!IsAvailableThemeFile(path))
	{
		if (RegOpenKeyExW(HKEY_CURRENT_USER, THEME_FILE_HISTORY_KEY, 0,
			KEY_SET_VALUE, &historyKey) == ERROR_SUCCESS)
		{
			RegDeleteValueW(historyKey, valueName);
			RegCloseKey(historyKey);
		}
		path[0] = L'\0';
		return false;
	}
	return true;
}

class ScopedThemeSaveCleanupMarker
{
	static constexpr LPCWSTR kEnvironmentVariable = L"DESKN_THEME_SAVE_CLEANUP_PATH";
	WCHAR previousValue[MAX_PATH] = {};
	bool hadPreviousValue = false;
	bool active = false;
public:
	explicit ScopedThemeSaveCleanupMarker(LPCWSTR savedThemePath)
	{
		WCHAR localAppData[MAX_PATH] = {};
		if (!savedThemePath || !savedThemePath[0] ||
			FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
				SHGFP_TYPE_CURRENT, localAppData)))
			return;

		WCHAR cleanupPath[MAX_PATH] = {};
		if (FAILED(StringCchPrintfW(cleanupPath, ARRAYSIZE(cleanupPath),
			L"%s\\Microsoft\\Windows\\Themes\\%s",
			localAppData, PathFindFileNameW(savedThemePath))))
			return;

		DWORD previousLength = GetEnvironmentVariableW(kEnvironmentVariable,
			previousValue, ARRAYSIZE(previousValue));
		if (previousLength >= ARRAYSIZE(previousValue))
			return;
		hadPreviousValue = previousLength != 0;
		active = SetEnvironmentVariableW(kEnvironmentVariable, cleanupPath) != FALSE;
	}

	~ScopedThemeSaveCleanupMarker()
	{
		if (active)
			SetEnvironmentVariableW(kEnvironmentVariable,
				hadPreviousValue ? previousValue : nullptr);
	}
};

void RememberSavedThemePath(ITheme10* theme, LPCWSTR path)
{
	RememberThemeFileInHistory(path);
	WCHAR keyPath[128] = {};
	if (!GetThemeIdRegistryPath(theme, keyPath))
	{
		LogThemeApplyDebug(L"Remember path failed to get ThemeId path=%ls", path ? path : L"(null)");
		return;
	}
	HKEY key = nullptr;
	LSTATUS createStatus = RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0,
		KEY_SET_VALUE, nullptr, &key, nullptr);
	if (createStatus == ERROR_SUCCESS)
	{
		LSTATUS setStatus = RegSetValueExW(key, L"Path", 0, REG_SZ,
			reinterpret_cast<const BYTE*>(path),
			(static_cast<DWORD>(lstrlenW(path)) + 1) * sizeof(WCHAR));
		RegCloseKey(key);
		LogThemeApplyDebug(L"Remember path key=%ls setStatus=%ld path=%ls",
			keyPath, setStatus, path ? path : L"(null)");
	}
	else LogThemeApplyDebug(L"Remember path key=%ls createStatus=%ld path=%ls",
		keyPath, createStatus, path ? path : L"(null)");
}

DWORD RemoveGeneratedThemeCopyWithoutConfirmation(LPCWSTR savedThemePath)
{
	if (!savedThemePath || !savedThemePath[0]) return ERROR_INVALID_PARAMETER;
	// Never remove a same-named per-user theme merely because a theme from the
	// Windows resources directory was selected.
	if (IsSystemThemeFilePath(savedThemePath)) return ERROR_SUCCESS;

	WCHAR localAppData[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
		SHGFP_TYPE_CURRENT, localAppData)))
		return ERROR_PATH_NOT_FOUND;

	WCHAR generatedThemePath[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(generatedThemePath, ARRAYSIZE(generatedThemePath),
		L"%s\\Microsoft\\Windows\\Themes\\%s",
		localAppData, PathFindFileNameW(savedThemePath))))
		return ERROR_FILENAME_EXCED_RANGE;

	// AddAndSelectTheme replaces this per-user staging copy through the shell's
	// transfer-confirmation API. Remove the exact generated copy first, using
	// the same no-confirmation recycle-bin path as our explicit Delete button.
	if (StrCmpIW(savedThemePath, generatedThemePath) == 0 ||
		!PathFileExistsW(generatedThemePath))
		return ERROR_SUCCESS;

	WCHAR deletePath[MAX_PATH + 1] = {};
	if (FAILED(StringCchCopyW(deletePath, ARRAYSIZE(deletePath), generatedThemePath)))
		return ERROR_FILENAME_EXCED_RANGE;

	SHFILEOPSTRUCTW deleteOperation = {};
	deleteOperation.wFunc = FO_DELETE;
	deleteOperation.pFrom = deletePath;
	deleteOperation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION |
		FOF_SILENT | FOF_NOERRORUI;
	int result = SHFileOperationW(&deleteOperation);
	if (result != 0) return static_cast<DWORD>(result);
	if (deleteOperation.fAnyOperationsAborted) return ERROR_CANCELLED;
	return ERROR_SUCCESS;
}

bool IsUnsavedThemeEntry(LPCWSTR name)
{
	if (!name) return false;
	constexpr LPCWSTR unsavedName = L"Unsaved Theme";
	constexpr LPCWSTR modifiedSuffix = L" (Modified)";
	constexpr size_t unsavedNameLength = ARRAYSIZE(L"Unsaved Theme") - 1;
	if (CompareStringOrdinal(name, static_cast<int>(unsavedNameLength),
		unsavedName, static_cast<int>(unsavedNameLength), TRUE) == CSTR_EQUAL &&
		(name[unsavedNameLength] == L'\0' ||
		StrCmpIW(name + unsavedNameLength, modifiedSuffix) == 0))
		return true;
	return StrCmpIW(name, L"Modified Theme") == 0;
}

bool IsCurrentThemeLabel(LPCWSTR name)
{
	if (!name) return false;
	std::wstring currentLabel = LoadDeskString(IDS_MY_CURRENT_THEME);
	std::wstring modifiedLabel = currentLabel + MODIFIED_THEME_SUFFIX;
	return StrCmpIW(name, currentLabel.c_str()) == 0 ||
		StrCmpIW(name, modifiedLabel.c_str()) == 0;
}

LPCWSTR GetCurrentThemeComboLabel()
{
	static const std::wstring label = LoadDeskString(IDS_MY_CURRENT_THEME);
	return label.c_str();
}

bool IsDefaultBlueTheme(LPCWSTR name)
{
	return name && (StrCmpIW(name, L"Default [blue]") == 0 ||
		StrCmpIW(name, L"Default (blue)") == 0);
}

void GetModifiedThemeComboLabel(LPCWSTR parentName, WCHAR (&label)[MAX_PATH])
{
	const bool hasParentName = parentName && parentName[0];
	std::wstring displayParent = hasParentName ? parentName : GetCurrentThemeComboLabel();
	if (IsDefaultBlueTheme(displayParent.c_str()))
		displayParent = LoadDeskString(IDS_WINDOWS_XP);
	StringCchPrintfW(label, ARRAYSIZE(label),
		LoadDeskString(IDS_MODIFIED_THEME_FORMAT).c_str(), displayParent.c_str());
}

bool IsNonDeletableBuiltinTheme(LPCWSTR name)
{
	return IsDefaultBlueTheme(name) || IsWindowsClassicThemeName(name) ||
		(name && (StrCmpIW(name, L"Zune") == 0 ||
			StrCmpIW(name, L"Royale") == 0 ||
			StrCmpIW(name, L"Royale Noir") == 0));
}

bool IsHiddenBuiltinTheme(LPCWSTR name)
{
	static constexpr LPCWSTR hiddenNames[] =
	{
		L"Olive Green", L"Silver"
	};
	for (LPCWSTR hiddenName : hiddenNames)
		if (name && StrCmpIW(name, hiddenName) == 0)
			return true;
	return false;
}

bool IsStockBuiltinTheme(LPCWSTR name)
{
	return IsNonDeletableBuiltinTheme(name) || IsHiddenBuiltinTheme(name) ||
		(name && StrCmpIW(name, L"Embedded") == 0);
}

bool IsBundledSystemThemeFile(LPCWSTR path, LPCWSTR displayName)
{
	if (!IsSystemThemeFilePath(path) || !IsStockBuiltinTheme(displayName)) return false;
	static constexpr LPCWSTR bundledFiles[] = {
		L"aero.theme", L"Embedded.theme", L"Olive Green.theme",
		L"Royale Noir.theme", L"Royale.theme", L"Silver.theme",
		L"Windows Classic.theme", L"Zune.theme"
	};
	LPCWSTR fileName = PathFindFileNameW(path);
	for (LPCWSTR bundledFile : bundledFiles)
		if (StrCmpIW(fileName, bundledFile) == 0) return true;
	return false;
}
}

LSTATUS WriteClassicSchemeToThemeFile(LPCWSTR path, const SCHEMEDATA* scheme)
{
	return WriteClassicSchemeToThemeFileInternal(path, scheme);
}

bool LoadClassicSchemeFromThemeFile(LPCWSTR path, SCHEMEDATA& scheme)
{
	return LoadClassicSchemeFromThemeFileInternal(path, scheme);
}

void CThemeDlgProc::PopulateThemeCombo(int selectedIndex)
{
	const bool redrawCombo = ::IsWindow(hCombobox);
	if (redrawCombo)
		SendMessageW(hCombobox, WM_SETREDRAW, FALSE, 0);
	ComboBox_ResetContent(hCombobox);
	savedThemePaths.clear();
	currentThemeIndex = selectedIndex;
	int count = 0;
	if (FAILED(pThemeManager->GetThemeCount(&count))) count = 0;
	int defaultThemeIndex = -1;
	const HRESULT defaultThemeResult =
		pThemeManager->GetDefaultTheme(&defaultThemeIndex);
	const bool defaultThemeIsSet = SUCCEEDED(defaultThemeResult) &&
		defaultThemeIndex >= 0 && defaultThemeIndex < count;
	LogThemeApplyDebug(L"Populate default theme result=0x%08X index=%d set=%d",
		defaultThemeResult, defaultThemeIndex, defaultThemeIsSet);
	if (startupThemeIdValid)
	{
		for (int i = 0; i < count; ++i)
		{
			ComPtr<ITheme10> candidate;
			GUID candidateId = {};
			if (SUCCEEDED(pThemeManager->GetTheme(i, &candidate)) && candidate &&
				SUCCEEDED(candidate->get_ThemeId(&candidateId)) &&
				IsEqualGUID(candidateId, startupThemeId))
			{
				startupThemeIndex = i;
				break;
			}
		}
	}
	const bool derivativeModified = currentThemeDerivativeModified;
	const bool classic = selectedTheme && selectedTheme->szMsstylePath == L"(classic)";
	int selectedComboIndex = CB_ERR;
	int defaultBlueComboIndex = CB_ERR;
	// This combo is intentionally unsorted. Insert the launch-time baseline
	// before registered and file-backed themes so it remains the first row.
	const bool startupThemeIsDefault = defaultThemeIsSet &&
		defaultThemeIndex == startupThemeIndex;
	const bool pendingNonStartupThemeFile = !pendingThemeFilePath.empty() &&
		(startupThemeSnapshotPath.empty() ||
			StrCmpIW(pendingThemeFilePath.c_str(), startupThemeSnapshotPath.c_str()) != 0);
	int startupComboIndex = CB_ERR;
	if (!startupThemeIsDefault)
		startupComboIndex = ComboBox_AddString(hCombobox, GetCurrentThemeComboLabel());
	if (startupComboIndex != CB_ERR && startupComboIndex != CB_ERRSPACE)
	{
		ComboBox_SetItemData(hCombobox, startupComboIndex, THEME_COMBO_STARTUP_BASELINE);
		if (startupSnapshotIsCurrent && !pendingNonStartupThemeFile &&
			!derivativeModified && selectedIndex == startupThemeIndex)
			selectedComboIndex = startupComboIndex;
	}
	// Keep the modified working state adjacent to its baseline, ahead of the
	// manager and file-backed rows. If the baseline is hidden, this is row one.
	int modifiedComboIndex = CB_ERR;
	if (derivativeModified)
	{
		WCHAR modifiedLabel[MAX_PATH] = {};
		GetModifiedThemeComboLabel(currentThemeParentLabel.c_str(), modifiedLabel);
		modifiedComboIndex = ComboBox_AddString(hCombobox, modifiedLabel);
		if (modifiedComboIndex != CB_ERR && modifiedComboIndex != CB_ERRSPACE)
		{
			ComboBox_SetItemData(hCombobox, modifiedComboIndex,
				THEME_COMBO_SESSION_MODIFIED);
			selectedComboIndex = modifiedComboIndex;
		}
	}
	WCHAR selectedSavedThemePath[MAX_PATH] = {};
	for (int i = 0; i < count; ++i)
	{
		ComPtr<ITheme10> theme;
		if (FAILED(pThemeManager->GetTheme(i, &theme)) || !theme) continue;
		LPWSTR name = nullptr;
		if (FAILED(theme->get_DisplayName(&name)) || !name || !name[0]) continue;
		// The applet's launch-time snapshot is an implementation detail, but
		// Theme Manager may enumerate its file as a regular theme and expose the
		// filename ("deskn-current") as the display name. The synthetic baseline
		// and parent-based Modified rows below are the user-facing entries.
		WCHAR registeredThemePath[MAX_PATH] = {};
		if (StrCmpIW(name, L"deskn-current") == 0 ||
			StrCmpIW(name, L"deskn-current.theme") == 0 ||
			(GetRegisteredThemeFilePath(theme.Get(), registeredThemePath) &&
				IsReservedCurrentThemeFile(registeredThemePath)))
			continue;
		if (IsHiddenBuiltinTheme(name)) continue;
		bool unsavedTheme = IsUnsavedThemeEntry(name);
		WCHAR savedDisplayName[MAX_PATH] = {};
		if (unsavedTheme)
		{
			// Theme Manager can keep the selected custom theme named "Unsaved theme"
			// after Save As. If its recorded saved file still exists, use that saved
			// theme's display name on every refresh instead of showing a false
			// stock Modified entry.
			WCHAR keyPath[128] = {};
			WCHAR savedPath[MAX_PATH] = {};
			DWORD savedPathSize = sizeof(savedPath);
			const bool hasThemeIdKey = GetThemeIdRegistryPath(theme.Get(), keyPath);
			const LSTATUS savedPathStatus = hasThemeIdKey
				? RegGetValueW(HKEY_CURRENT_USER, keyPath, L"Path", RRF_RT_REG_SZ,
					nullptr, savedPath, &savedPathSize)
				: ERROR_NOT_FOUND;
			LogThemeApplyDebug(L"Populate unsaved index=%d selected=%d idKey=%ls pathStatus=%ld path=%ls",
				i, i == selectedIndex, hasThemeIdKey ? keyPath : L"(no ID)",
				savedPathStatus, savedPath);
			if (savedPathStatus == ERROR_SUCCESS && IsAvailableThemeFile(savedPath))
			{
				const bool wallpaperMatches = SavedThemeWallpaperMatchesCurrent(savedPath);
				if (i == selectedIndex)
					StringCchCopyW(selectedSavedThemePath, ARRAYSIZE(selectedSavedThemePath), savedPath);
				if (i == selectedIndex && wallpaperMatches)
					GetPrivateProfileStringW(L"Theme", L"DisplayName", L"", savedDisplayName,
						ARRAYSIZE(savedDisplayName), savedPath);
				// A valid saved .theme is not a modified/unsaved theme just because
				// its author omitted [Theme] DisplayName. Use its filename as the
				// visible name so applying it unchanged does not show a false
				// "(Modified)" entry.
				if (!savedDisplayName[0] &&
					SUCCEEDED(StringCchCopyW(savedDisplayName, ARRAYSIZE(savedDisplayName),
						PathFindFileNameW(savedPath))))
					PathRemoveExtensionW(savedDisplayName);
				if (savedDisplayName[0] && wallpaperMatches)
				{
					name = savedDisplayName;
					unsavedTheme = false;
				}
			}
			else if (savedPathStatus == ERROR_SUCCESS && i == selectedIndex &&
				StrCmpIW(PathFindExtensionW(savedPath), L".theme") == 0 &&
				!PathFileExistsW(savedPath))
			{
				ShowMissingThemeFileError(m_hWnd, savedPath, true);
			}
			LogThemeApplyDebug(L"Resolved unsaved index=%d title=%ls stillUnsaved=%d path=%ls",
				i, name ? name : L"(null)", unsavedTheme, savedPath);
		}
		const bool defaultBlueTheme = IsDefaultBlueTheme(name);
		const bool windowsClassicTheme = IsWindowsClassicThemeName(name);
		const bool nonDeletableBuiltin = IsNonDeletableBuiltinTheme(name);
		const bool zuneTheme = StrCmpIW(name, L"Zune") == 0;
		const bool royaleTheme = StrCmpIW(name, L"Royale") == 0;
		const bool royaleNoirTheme = StrCmpIW(name, L"Royale Noir") == 0;
		int comboIndex = CB_ERR;
		if (unsavedTheme)
		{
			// Theme Manager can retain stale "Unsaved Theme" entries after a
			// refresh. They have no user-facing identity, so never expose them in
			// the combo; the active working state is represented by our synthetic
			// baseline/Modified rows, while saved entries were renamed above.
			continue;
		}
		if (IsCurrentThemeLabel(name))
		{
			// The launch-time baseline is inserted as its own row below.
			continue;
		}
		// Theme Manager can expose both "Default [blue]" and "Default (blue)"
		// for the same built-in XP theme. They share one user-facing combo item.
		if (defaultBlueTheme && defaultBlueComboIndex != CB_ERR)
		{
			// Keep the single visible row mapped to the active manager entry if
			// Theme Manager selected the second alias.
			if (i == selectedIndex)
			{
				ComboBox_SetItemData(hCombobox, defaultBlueComboIndex, i);
				selectedComboIndex = defaultBlueComboIndex;
			}
			continue;
		}
		if (defaultBlueTheme)
			comboIndex = ComboBox_AddString(hCombobox, LoadDeskString(IDS_WINDOWS_XP).c_str());
		else if (zuneTheme)
			comboIndex = ComboBox_AddString(hCombobox, LoadDeskString(IDS_ZUNE).c_str());
		else if (royaleTheme)
			comboIndex = ComboBox_AddString(hCombobox, LoadDeskString(IDS_ROYALE).c_str());
		else if (royaleNoirTheme)
			comboIndex = ComboBox_AddString(hCombobox, LoadDeskString(IDS_ROYALE_NOIR).c_str());
		else
			comboIndex = ComboBox_AddString(hCombobox, name);
		if (comboIndex != CB_ERR)
		{
			ComboBox_SetItemData(hCombobox, comboIndex, i);
			if (defaultBlueTheme)
				defaultBlueComboIndex = comboIndex;
			if (!unsavedTheme && !nonDeletableBuiltin)
			{
				WCHAR savedThemePath[MAX_PATH] = {};
				if (GetRegisteredThemeFilePath(theme.Get(), savedThemePath) &&
					!PathFileExistsW(savedThemePath))
				{
					if (i == selectedIndex)
						ShowMissingThemeFileError(m_hWnd, savedThemePath, true);
					ComboBox_DeleteString(hCombobox, comboIndex);
					continue;
				}
				if (!GetDeletableThemePath(comboIndex, savedThemePath))
				{
					ComboBox_DeleteString(hCombobox, comboIndex);
					continue;
				}
			}
			if (i == selectedIndex) selectedComboIndex = comboIndex;
		}
	}

	// The Theme Manager may omit user-saved .theme files from GetThemeCount.
	// Add discovered files and persistent history as file-backed rows.
	auto addSavedThemeFile = [&](LPCWSTR candidate)
	{
		if (!IsAvailableThemeFile(candidate)) return;
		WCHAR savedName[MAX_PATH] = {};
		GetPrivateProfileStringW(L"Theme", L"DisplayName", L"", savedName,
			ARRAYSIZE(savedName), candidate);
		if (!pendingThemeFilePath.empty() &&
			StrCmpIW(pendingThemeFilePath.c_str(), candidate) == 0)
		{
			StringCchCopyW(savedName, ARRAYSIZE(savedName), PathFindFileNameW(candidate));
			PathRemoveExtensionW(savedName);
		}
		if (!savedName[0])
		{
			StringCchCopyW(savedName, ARRAYSIZE(savedName), PathFindFileNameW(candidate));
			PathRemoveExtensionW(savedName);
		}
		if (IsBundledSystemThemeFile(candidate, savedName)) return;
		if (IsCurrentThemeLabel(savedName)) return;
		RememberThemeFileInHistory(candidate);

		for (int i = 0; i < ComboBox_GetCount(hCombobox); ++i)
		{
			WCHAR existingPath[MAX_PATH] = {};
			LPARAM itemData = ComboBox_GetItemData(hCombobox, i);
			if (itemData == THEME_COMBO_SAVED_FILE &&
				i < static_cast<int>(savedThemePaths.size()))
				StringCchCopyW(existingPath, ARRAYSIZE(existingPath), savedThemePaths[i].c_str());
			else if (itemData >= 0)
			{
				ComPtr<ITheme10> existingTheme;
				LPWSTR existingName = nullptr;
				if (SUCCEEDED(pThemeManager->GetTheme(static_cast<int>(itemData), &existingTheme)) &&
					existingTheme && SUCCEEDED(existingTheme->get_DisplayName(&existingName)) &&
					IsUnsavedThemeEntry(existingName))
					continue;
				if (!existingTheme || !GetRegisteredThemeFilePath(existingTheme.Get(), existingPath))
					GetDeletableThemePath(i, existingPath);
			}
			if (existingPath[0] && StrCmpIW(existingPath, candidate) == 0)
			{
				// A manager row may resolve to the same file while displaying a
				// transient current-state name. Only consider
				// that a duplicate when the actual visible row is the saved name.
				if (itemData == THEME_COMBO_SAVED_FILE)
				{
					if (selectedSavedThemePath[0] &&
						StrCmpIW(selectedSavedThemePath, candidate) == 0)
						selectedComboIndex = i;
					return;
				}
				WCHAR existingName[MAX_PATH] = {};
				if (ComboBox_GetLBText(hCombobox, i, existingName) != CB_ERR &&
					StrCmpIW(existingName, savedName) == 0)
				{
					if (selectedSavedThemePath[0] &&
						StrCmpIW(selectedSavedThemePath, candidate) == 0)
						selectedComboIndex = i;
					return;
				}
			}
		}

		WCHAR comboName[MAX_PATH] = {};
		StringCchCopyW(comboName, ARRAYSIZE(comboName), savedName);
		if (ComboBox_FindStringExact(hCombobox, -1, comboName) != CB_ERR)
		{
			WCHAR leafName[MAX_PATH] = {};
			StringCchCopyW(leafName, ARRAYSIZE(leafName), PathFindFileNameW(candidate));
			PathRemoveExtensionW(leafName);
			StringCchPrintfW(comboName, ARRAYSIZE(comboName), L"%s (%s)", savedName, leafName);
		}

		int comboIndex = ComboBox_AddString(hCombobox, comboName);
		if (comboIndex == CB_ERR) return;
		ComboBox_SetItemData(hCombobox, comboIndex, THEME_COMBO_SAVED_FILE);
		if (savedThemePaths.size() <= static_cast<size_t>(comboIndex))
			savedThemePaths.resize(static_cast<size_t>(comboIndex) + 1);
		savedThemePaths[comboIndex] = candidate;
		if (selectedSavedThemePath[0] &&
			StrCmpIW(selectedSavedThemePath, candidate) == 0)
			selectedComboIndex = comboIndex;
	};

	// Include installed system themes as well as per-user files, even when
	// Theme Manager has not registered them yet.
	WCHAR directories[4][MAX_PATH] = {};
	SHGetFolderPathW(m_hWnd, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, directories[0]);
	ExpandEnvironmentStringsW(L"%USERPROFILE%\\Documents",
		directories[1], ARRAYSIZE(directories[1]));
	ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Themes",
		directories[2], ARRAYSIZE(directories[2]));
	WCHAR windowsDir[MAX_PATH] = {};
	UINT windowsDirLength = GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir));
	if (windowsDirLength && windowsDirLength < ARRAYSIZE(windowsDir))
		StringCchPrintfW(directories[3], ARRAYSIZE(directories[3]),
			L"%s\\Resources\\Themes", windowsDir);
	for (const auto& directory : directories)
	{
		if (!directory[0]) continue;
		WCHAR pattern[MAX_PATH] = {};
		if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.theme", directory)))
			continue;
		WIN32_FIND_DATAW findData = {};
		HANDLE find = FindFirstFileW(pattern, &findData);
		if (find == INVALID_HANDLE_VALUE) continue;
		do
		{
			WCHAR candidate[MAX_PATH] = {};
			if (SUCCEEDED(StringCchPrintfW(candidate, ARRAYSIZE(candidate), L"%s\\%s",
				directory, findData.cFileName)))
				addSavedThemeFile(candidate);
		} while (FindNextFileW(find, &findData));
		FindClose(find);
	}

	for (DWORD i = 0; i < THEME_FILE_HISTORY_LIMIT; ++i)
	{
		WCHAR historicalPath[MAX_PATH] = {};
		if (GetThemeFileFromHistory(i, historicalPath))
			addSavedThemeFile(historicalPath);
	}

	if (derivativeModified && modifiedComboIndex != CB_ERR &&
		modifiedComboIndex != CB_ERRSPACE)
		selectedComboIndex = modifiedComboIndex;
		else if (startupSnapshotIsCurrent && !pendingNonStartupThemeFile &&
			!derivativeModified &&
			selectedIndex == startupThemeIndex &&
		startupComboIndex != CB_ERR)
		selectedComboIndex = startupComboIndex;
		else if (startupSnapshotIsCurrent && !pendingNonStartupThemeFile &&
			selectedComboIndex == CB_ERR &&
			startupComboIndex != CB_ERR)
		selectedComboIndex = startupComboIndex;

	int onlineIndex = ComboBox_AddString(hCombobox,
		LoadDeskString(IDS_MORE_THEMES_ONLINE).c_str());
	if (onlineIndex != CB_ERR)
		ComboBox_SetItemData(hCombobox, onlineIndex, THEME_COMBO_BROWSE_ONLINE);
	int browseIndex = ComboBox_AddString(hCombobox, LoadDeskString(IDS_BROWSE_THEMES).c_str());
	if (browseIndex != CB_ERR)
		ComboBox_SetItemData(hCombobox, browseIndex, THEME_COMBO_BROWSE_FILE);
	if (selectedComboIndex != CB_ERR)
		ComboBox_SetCurSel(hCombobox, selectedComboIndex);
	if (redrawCombo)
	{
		SendMessageW(hCombobox, WM_SETREDRAW, TRUE, 0);
		::RedrawWindow(hCombobox, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
	}
	WCHAR selectedName[MAX_PATH] = {};
	int visibleSelection = ComboBox_GetCurSel(hCombobox);
	if (visibleSelection != CB_ERR)
		ComboBox_GetLBText(hCombobox, visibleSelection, selectedName);
	LogThemeApplyDebug(L"Populate result selectedIndex=%d row=%ls item=%lld count=%d syntheticIndex=%d",
		visibleSelection, selectedName,
		static_cast<long long>(visibleSelection == CB_ERR ? static_cast<LPARAM>(CB_ERR)
			: ComboBox_GetItemData(hCombobox, visibleSelection)),
		ComboBox_GetCount(hCombobox), currentThemeIndex);
	lastThemeIndex = selectedIndex;
}

int CThemeDlgProc::GetThemeIndexFromCombo(int comboIndex) const
{
	if (comboIndex < 0 || comboIndex >= ComboBox_GetCount(hCombobox)) return -1;
	LPARAM itemData = ComboBox_GetItemData(hCombobox, comboIndex);
	if (itemData == THEME_COMBO_STARTUP_BASELINE) return startupThemeIndex;
	if (itemData == THEME_COMBO_SESSION_MODIFIED) return currentThemeIndex;
	if (itemData == THEME_COMBO_CURRENT) return currentThemeIndex;
	if (itemData == THEME_COMBO_SAVED_FILE) return static_cast<int>(itemData);
	return itemData < 0 || itemData == CB_ERR ? -1 : static_cast<int>(itemData);
}

int CThemeDlgProc::FindThemeComboIndex(int themeIndex) const
{
	const bool sessionModified = currentThemeDerivativeModified;
	for (int comboIndex = 0; comboIndex < ComboBox_GetCount(hCombobox); ++comboIndex)
	{
		LPARAM itemData = ComboBox_GetItemData(hCombobox, comboIndex);
		if (sessionModified && itemData == THEME_COMBO_SESSION_MODIFIED)
			return comboIndex;
		if (startupSnapshotIsCurrent && !sessionModified &&
			themeIndex == startupThemeIndex &&
			itemData == THEME_COMBO_STARTUP_BASELINE)
			return comboIndex;
	}
	for (int comboIndex = 0; comboIndex < ComboBox_GetCount(hCombobox); ++comboIndex)
	{
		if (GetThemeIndexFromCombo(comboIndex) == themeIndex)
			return comboIndex;
	}
	return CB_ERR;
}

SCHEMEDATA* CThemeDlgProc::GetPreviewScheme()
{
	if (!selectedTheme || selectedTheme->szMsstylePath != L"(classic)")
		return nullptr;

	WCHAR selectedThemeName[256] = {};
	int selectedIndex = ComboBox_GetCurSel(hCombobox);
	if (selectedIndex != CB_ERR)
		ComboBox_GetLBText(hCombobox, selectedIndex, selectedThemeName);
	if (IsWindowsClassicThemeName(selectedThemeName))
		return standardPreviewScheme;

	return IsCustomClassicScheme(selectedTheme->selectedScheme)
		? selectedTheme->selectedScheme : standardPreviewScheme;
}


BOOL CThemeDlgProc::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	g_themePageForApply = this;
	// store HWNDs
	hCombobox = GetDlgItem(1101);
	hPreview = GetDlgItem(1103);
	size = GetClientSIZE(hPreview);

	// select current theme in combobox
	int currThem = 0;
	pThemeManager->GetCurrentTheme(&currThem);
	startupThemeIndex = currThem;
	currentThemeParentLabel = LoadDeskString(IDS_MY_CURRENT_THEME);
	startupThemeParentLabel = currentThemeParentLabel;
	startupSnapshotIsCurrent = true;
	ComPtr<ITheme10> startupTheme;
	WCHAR startupThemeDisplayName[MAX_PATH] = {};
	WCHAR startupSavedThemePath[MAX_PATH] = {};
	const bool startupHasSavedThemeParent =
		SUCCEEDED(pThemeManager->GetTheme(currThem, &startupTheme)) && startupTheme &&
		GetRegisteredThemeFilePath(startupTheme.Get(), startupSavedThemePath) &&
		IsAvailableThemeFile(startupSavedThemePath);
	if (SUCCEEDED(pThemeManager->GetTheme(currThem, &startupTheme)) && startupTheme)
	{
		startupThemeIdValid = SUCCEEDED(startupTheme->get_ThemeId(&startupThemeId));
		LPWSTR activeDisplayName = nullptr;
		const bool hasActiveDisplayName =
			SUCCEEDED(startupTheme->get_DisplayName(&activeDisplayName)) &&
			activeDisplayName && activeDisplayName[0] &&
			SUCCEEDED(StringCchCopyW(startupThemeDisplayName,
				ARRAYSIZE(startupThemeDisplayName), activeDisplayName));
		if (startupHasSavedThemeParent && hasActiveDisplayName &&
			!IsUnsavedThemeEntry(activeDisplayName) &&
			!IsCurrentThemeLabel(activeDisplayName))
		{
			WCHAR parentName[MAX_PATH] = {};
			if (SUCCEEDED(StringCchCopyW(parentName, ARRAYSIZE(parentName), activeDisplayName)))
			{
				RemoveModifiedSuffix(parentName);
				if (parentName[0]) startupThemeParentLabel = parentName;
			}
		}
		else
		{
			// A transient Theme Manager row can still point at a user-saved parent.
			WCHAR savedDisplayName[MAX_PATH] = {};
			if (startupHasSavedThemeParent)
			{
				GetPrivateProfileStringW(L"Theme", L"DisplayName", L"",
					savedDisplayName, ARRAYSIZE(savedDisplayName), startupSavedThemePath);
				if (!savedDisplayName[0])
				{
					StringCchCopyW(savedDisplayName, ARRAYSIZE(savedDisplayName),
						PathFindFileNameW(startupSavedThemePath));
					PathRemoveExtensionW(savedDisplayName);
				}
				if (savedDisplayName[0])
					StringCchCopyW(startupThemeDisplayName,
						ARRAYSIZE(startupThemeDisplayName), savedDisplayName);
				if (savedDisplayName[0] && !IsUnsavedThemeEntry(savedDisplayName))
					startupThemeParentLabel = savedDisplayName;
			}
		}
	}
	currentThemeParentLabel = startupThemeParentLabel;
	WCHAR persistedModifiedName[MAX_PATH] = {};
	currentThemeDerivativeModified = startupHasSavedThemeParent &&
		ReadPersistedModifiedThemeName(persistedModifiedName);
	if (!startupHasSavedThemeParent)
	{
		// The persisted Modified label belongs to the previous applet session.
		// If Theme Manager has no saved parent file, the current launch snapshot
		// is the identity now; do not resurrect an unsaved parent such as Tiger.
		LSTATUS markerStatus = PersistModifiedThemeName(false, nullptr);
		if (markerStatus != ERROR_SUCCESS)
			LogThemeApplyDebug(L"Could not clear stale unsaved Modified marker: %ld", markerStatus);
	}
	if (currentThemeDerivativeModified)
	{
		RemoveModifiedSuffix(persistedModifiedName);
		if (StrCmpIW(persistedModifiedName, L"Unsaved Theme") == 0 ||
			StrCmpIW(persistedModifiedName, L"Modified Theme") == 0)
			currentThemeParentLabel = startupThemeParentLabel;
		else if (persistedModifiedName[0])
			currentThemeParentLabel = startupThemeParentLabel = persistedModifiedName;
	}
	else
	{
		const bool startupThemeIsStock = IsStockBuiltinTheme(startupThemeDisplayName);
		const bool wallpaperMatches = !startupHasSavedThemeParent ||
			SavedThemeWallpaperMatchesCurrent(startupSavedThemePath);
		LogThemeApplyDebug(L"Startup Modified check parent=%d stock=%d display=%ls wallpaperMatch=%d path=%ls",
			startupHasSavedThemeParent, startupThemeIsStock,
			startupThemeDisplayName, wallpaperMatches, startupSavedThemePath);
		// A stock theme remains the selected stock theme when the user has merely
		// customized the desktop wallpaper. Do not infer an applet-side Modified
		// state from that wallpaper difference alone.
		if (startupHasSavedThemeParent && !startupThemeIsStock && !wallpaperMatches)
			currentThemeDerivativeModified = true;
	}
	if (!UpdateCurrentThemeSnapshot(startupThemeSnapshotPath))
		LogThemeApplyDebug(L"Could not capture the launch-time My Current Theme snapshot");

	pThemeManager->GetTheme(currThem, &currentITheme);

	// update THEMEINFO before setting bitmap for now
	WCHAR ws[MAX_PATH] = { 0 };
	SystemParametersInfo(SPI_GETDESKWALLPAPER, MAX_PATH, ws, 0);
	UpdateThemeInfo(ws);

	// The Themes tab previews the selected theme, even when the desktop is
	// currently running with Classic theming enabled. The Windows Classic entry
	// always previews Windows Standard; other Classic themes use a selected
	// custom scheme when present.
	auto activeTheme = std::make_unique<CTheme>(currentITheme);
	LPWSTR activePath = nullptr;
	activeTheme->get_VisualStyle(&activePath);
	HANDLE loadedTheme = activePath ? LoadThemeFromFilePath(activePath) : nullptr;
	if (loadedTheme)
	{
		selectedTheme->szMsstylePath = activePath;
		CleanupThemeFile(&loadedTheme);
	}
	else
		selectedTheme->szMsstylePath = L"(classic)";
	if (selectedTheme->szMsstylePath == L"(classic)")
	{
		if (selectedTheme->selectedScheme && selectedTheme->selectedScheme->variant == 0x8)
			free(selectedTheme->selectedScheme);
		currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
		selectedTheme->selectedScheme = currentRegistryScheme;
	}
	standardPreviewScheme = LoadWindowsStandardClassicScheme();
	// Modified-row labels need the resolved Classic/visual-style state.
	PopulateThemeCombo(currThem);
	UpdateDeleteButton();

	pWndPreview = Make<CWindowPreview>(size, wnd, (int)ARRAYSIZE(wnd), PAGETYPE::PT_THEMES, nullptr, GetDpiForWindow(m_hWnd));

	HBITMAP bmp;
	{
		ScopedPreviewScheme previewScheme(GetPreviewScheme());
		pWndPreview->GetPreviewImage(&bmp);
	}
	SetBitmap(hPreview, bmp);

	return 0;
}

BOOL CThemeDlgProc::OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	if (g_themePageForApply == this)
		g_themePageForApply = nullptr;
	pWndPreview = nullptr;
	currentRegistryScheme = nullptr;
	return 0;
}

bool CThemeDlgProc::IsStartupThemeSnapshotSelected() const
{
	int comboIndex = ComboBox_GetCurSel(hCombobox);
	return comboIndex != CB_ERR &&
		ComboBox_GetItemData(hCombobox, comboIndex) == THEME_COMBO_STARTUP_BASELINE;
}

void CThemeDlgProc::OnPropertySheetApplyCompleted()
{
	if (!UpdateCurrentThemeSnapshot(startupThemeSnapshotPath))
	{
		LogThemeApplyDebug(L"Apply completed with My Current Theme selected, but snapshot refresh failed");
		return;
	}

	pThemeManager->Refresh();
	int activeIndex = -1;
	if (SUCCEEDED(pThemeManager->GetCurrentTheme(&activeIndex)) && activeIndex >= 0)
	{
		startupSnapshotIsCurrent = true;
		startupThemeIndex = activeIndex;
		ComPtr<ITheme10> activeTheme;
		if (SUCCEEDED(pThemeManager->GetTheme(activeIndex, &activeTheme)) && activeTheme)
			startupThemeIdValid = SUCCEEDED(activeTheme->get_ThemeId(&startupThemeId));
		lastThemeIndex = activeIndex;
		currentThemeIndex = activeIndex;
		currentThemeParentLabel = startupThemeParentLabel;
		currentThemeDerivativeModified = false;
		LSTATUS markerStatus = PersistModifiedThemeName(false, nullptr);
		if (markerStatus != ERROR_SUCCESS)
			LogThemeApplyDebug(L"Could not clear modified-theme marker: %ld", markerStatus);
		InterlockedExchange(&g_currentSessionModified, FALSE);
		PopulateThemeCombo(activeIndex);
		int baselineIndex = FindThemeComboIndex(activeIndex);
		if (baselineIndex != CB_ERR)
			ComboBox_SetCurSel(hCombobox, baselineIndex);
	}
	themeApplyPending = false;
	SetModified(FALSE);
}

void CThemeDlgProc::OnPropertySheetChanged()
{
	if (!hCombobox || currentThemeIndex < 0) return;
	currentThemeDerivativeModified = true;
	PopulateThemeCombo(currentThemeIndex);
}

void CThemeDlgProc::PersistModifiedStateAfterApply()
{
	LSTATUS status = PersistModifiedThemeName(currentThemeDerivativeModified,
		currentThemeParentLabel.c_str());
	LogThemeApplyDebug(L"Apply persisted derivative modified=%d parent=%ls status=%ld",
		currentThemeDerivativeModified, currentThemeParentLabel.c_str(), status);
}

bool IsMyCurrentThemeSelectedForApply()
{
	return g_themePageForApply &&
		g_themePageForApply->IsStartupThemeSnapshotSelected();
}

void CompleteMyCurrentThemeApply()
{
	if (g_themePageForApply)
		g_themePageForApply->OnPropertySheetApplyCompleted();
}

void NotifyThemePagePropertySheetChanged()
{
	if (g_themePageForApply)
		g_themePageForApply->OnPropertySheetChanged();
}

void PersistThemeModifiedStateAfterApply()
{
	if (g_themePageForApply)
		g_themePageForApply->PersistModifiedStateAfterApply();
}

BOOL CThemeDlgProc::OnThemeComboboxChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	ScopedThemeSelection selectionScope;
	int comboIndex = ComboBox_GetCurSel(hCombobox);
	LPARAM selectedItem = comboIndex == CB_ERR ? CB_ERR : ComboBox_GetItemData(hCombobox, comboIndex);
	const bool selectingStartupSnapshot =
		selectedItem == THEME_COMBO_STARTUP_BASELINE &&
		!startupThemeSnapshotPath.empty();
	if (selectedItem == THEME_COMBO_SAVED_FILE || selectingStartupSnapshot)
	{
		WCHAR themePath[MAX_PATH] = {};
		if (selectingStartupSnapshot)
		{
			if (FAILED(StringCchCopyW(themePath, ARRAYSIZE(themePath),
				startupThemeSnapshotPath.c_str())) || !PathFileExistsW(themePath))
			{
				::MessageBoxW(m_hWnd, LoadDeskString(IDS_THEME_SNAPSHOT_MISSING).c_str(),
					LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
				return 0;
			}
		}
		else if (!GetDeletableThemePath(comboIndex, themePath))
		{
			if (comboIndex >= 0 && comboIndex < static_cast<int>(savedThemePaths.size()) &&
				savedThemePaths[comboIndex][0] &&
				!PathFileExistsW(savedThemePaths[comboIndex].c_str()))
				ShowMissingThemeFileError(m_hWnd, savedThemePaths[comboIndex].c_str());
			return 0;
		}
		pendingThemeFilePath = themePath;

		WCHAR stylePath[MAX_PATH] = {};
		const bool hasMsstyle = ReadThemeFilePath(themePath, L"VisualStyles", L"Path", stylePath);
		HANDLE previewTheme = hasMsstyle ? LoadThemeFromFilePath(stylePath) : nullptr;
		if (previewTheme)
		{
			selectedTheme->szMsstylePath = stylePath;
			if (selectedTheme->selectedScheme && selectedTheme->selectedScheme != currentRegistryScheme &&
				selectedTheme->selectedScheme->variant == 0x8)
			{
				free(selectedTheme->selectedScheme);
				selectedTheme->selectedScheme = nullptr;
			}
			CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), previewTheme);
		}
		else
		{
			selectedTheme->szMsstylePath = L"(classic)";
			currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
			selectedTheme->selectedScheme = currentRegistryScheme;
		}

		WCHAR wallpaperPath[MAX_PATH] = {};
		if (ReadThemeFilePath(themePath, L"Control Panel\\Desktop", L"Wallpaper", wallpaperPath))
		{
			selectedTheme->wallpaperPath = wallpaperPath;
			selectedTheme->wallpaperType = WT_PICTURE;
		}
		else
		{
			selectedTheme->wallpaperPath.clear();
			selectedTheme->wallpaperType = WT_NOWALL;
		}
		selectedTheme->fSlideshowSelection = false;
		selectedTheme->newColor = 0xB0000000;
		selectedTheme->customWallpaperSelection = false;
		selectedTheme->posChanged = -1;
		selectedTheme->useDesktopColor = false;
		themeSelected = TRUE;
		SCHEMEDATA classicFileScheme = {};
		const bool hasClassicFileScheme = !previewTheme &&
			LoadClassicSchemeFromThemeFile(themePath, classicFileScheme);
		selectedTheme->classicSchemeSourcePath = themePath;

		HBITMAP previewBitmap = nullptr;
		{
			ScopedPreviewScheme previewScheme(hasClassicFileScheme
				? &classicFileScheme : GetPreviewScheme());
			pWndPreview->GetUpdatedPreviewImage(wnd, previewTheme, &previewBitmap, UPDATE_ALL);
		}
		SetBitmap(hPreview, previewBitmap);
		themeApplyPending = true;
		WCHAR parentLabel[MAX_PATH] = {};
		ComboBox_GetLBText(hCombobox, comboIndex, parentLabel);
		currentThemeParentLabel = selectingStartupSnapshot
			? startupThemeParentLabel
			: parentLabel[0] ? parentLabel : GetCurrentThemeComboLabel();
		currentThemeDerivativeModified = false;
		SetModified(TRUE);
		PopulateThemeCombo(lastThemeIndex);
		for (int candidate = 0; candidate < ComboBox_GetCount(hCombobox); ++candidate)
		{
			if ((selectingStartupSnapshot &&
				ComboBox_GetItemData(hCombobox, candidate) == THEME_COMBO_STARTUP_BASELINE) ||
				(!selectingStartupSnapshot &&
				ComboBox_GetItemData(hCombobox, candidate) == THEME_COMBO_SAVED_FILE &&
				candidate < static_cast<int>(savedThemePaths.size()) &&
				StrCmpIW(savedThemePaths[candidate].c_str(), themePath) == 0))
			{
				ComboBox_SetCurSel(hCombobox, candidate);
				break;
			}
		}
		UpdateDeleteButton();
		return 0;
	}
	if (selectedItem == THEME_COMBO_BROWSE_ONLINE || selectedItem == THEME_COMBO_BROWSE_FILE)
	{
		WCHAR themePath[MAX_PATH] = {};

		int restoreIndex = FindThemeComboIndex(lastThemeIndex);
		if (restoreIndex != CB_ERR)
			ComboBox_SetCurSel(hCombobox, restoreIndex);
		if (selectedItem == THEME_COMBO_BROWSE_ONLINE)
		{
			ShellExecuteW(m_hWnd, L"open",
				L"https://www.deviantart.com/search?q=windows%20themes",
				nullptr, nullptr, SW_SHOWNORMAL);
			return 0;
		}

		if (selectedItem == THEME_COMBO_BROWSE_FILE)
		{
			WCHAR documentsPath[MAX_PATH] = {};
			SHGetFolderPathW(m_hWnd, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documentsPath);
			std::wstring themeFilter = MakeThemeFileFilter();
			std::wstring browseTitle = LoadDeskString(IDS_BROWSE_FOR_THEME);
			OPENFILENAMEW dialog = {};
			dialog.lStructSize = sizeof(dialog);
			dialog.hwndOwner = m_hWnd;
			dialog.lpstrFilter = themeFilter.c_str();
			dialog.lpstrFile = themePath;
			dialog.nMaxFile = ARRAYSIZE(themePath);
			dialog.lpstrInitialDir = documentsPath[0] ? documentsPath : nullptr;
			dialog.lpstrTitle = browseTitle.c_str();
			dialog.lpstrDefExt = L"theme";
			dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
			if (!GetOpenFileNameW(&dialog)) return 0;
		}
		if (!PathFileExistsW(themePath))
		{
			ShowMissingThemeFileError(m_hWnd, themePath);
			return 0;
		}

		// Browsing an unregistered theme stages it for preview. Theme Manager is
		// updated only when the user explicitly presses Apply.
		pendingThemeFilePath = themePath;
		RememberThemeFileInHistory(themePath);
		PopulateThemeCombo(lastThemeIndex);
		int fileComboIndex = CB_ERR;
		for (int candidate = 0; candidate < ComboBox_GetCount(hCombobox); ++candidate)
		{
			if (ComboBox_GetItemData(hCombobox, candidate) == THEME_COMBO_SAVED_FILE &&
				candidate < static_cast<int>(savedThemePaths.size()) &&
				StrCmpIW(savedThemePaths[candidate].c_str(), themePath) == 0)
			{
				fileComboIndex = candidate;
				break;
			}
		}
		if (fileComboIndex == CB_ERR)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_THEME_ADD_TO_LIST_ERROR).c_str(), themePath);
			::MessageBoxW(m_hWnd, message,
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return 0;
		}

		ComboBox_SetCurSel(hCombobox, fileComboIndex);
		BOOL handled = FALSE;
		return OnThemeComboboxChange(CBN_SELCHANGE, id, hWnd, handled);
	}
	if (selectedItem >= 0)
	{
		ComPtr<ITheme10> selectedFileTheme;
		WCHAR registeredPath[MAX_PATH] = {};
		int managerIndex = GetThemeIndexFromCombo(comboIndex);
		if (managerIndex >= 0 &&
			SUCCEEDED(pThemeManager->GetTheme(managerIndex, &selectedFileTheme)) &&
			selectedFileTheme &&
			GetRegisteredThemeFilePath(selectedFileTheme.Get(), registeredPath) &&
			!PathFileExistsW(registeredPath))
		{
			ShowMissingThemeFileError(m_hWnd, registeredPath);
			int restoreIndex = FindThemeComboIndex(lastThemeIndex);
			if (restoreIndex != CB_ERR) ComboBox_SetCurSel(hCombobox, restoreIndex);
			return 0;
		}
	}
	const bool selectingModifiedEntry = selectedItem == THEME_COMBO_SESSION_MODIFIED;
	const bool selectingStartupBaseline = selectedItem == THEME_COMBO_STARTUP_BASELINE;
	if (!selectingModifiedEntry)
	{
		currentThemeDerivativeModified = false;
		if (selectingStartupBaseline)
			currentThemeParentLabel = startupThemeParentLabel;
		else
		{
			WCHAR parentLabel[MAX_PATH] = {};
			if (ComboBox_GetLBText(hCombobox, comboIndex, parentLabel) != CB_ERR && parentLabel[0])
				currentThemeParentLabel = parentLabel;
		}
	}
	selectedTheme->classicSchemeSourcePath.clear();
	pendingThemeFilePath.clear();
	int index = GetThemeIndexFromCombo(comboIndex);
	if (index < 0) return 0;
	lastThemeIndex = index;
	UpdateDeleteButton();

	currentITheme->Release();
	pThemeManager->GetTheme(index, &currentITheme);

	LPWSTR ws = NULL;
	auto themeClass = std::make_unique<CTheme>(currentITheme);
	themeClass->get_background(&ws);

	LPWSTR path = nullptr;
	themeClass->get_VisualStyle(&path);
	ComPtr<ITheme10> selectedThemeInterface;
	LPWSTR selectedThemeName = nullptr;
	const bool windowsClassicTheme =
		SUCCEEDED(pThemeManager->GetTheme(index, &selectedThemeInterface)) && selectedThemeInterface &&
		SUCCEEDED(selectedThemeInterface->get_DisplayName(&selectedThemeName)) &&
		IsWindowsClassicThemeName(selectedThemeName);

	// update the string if different
	if (windowsClassicTheme)
	{
		if (selectedTheme->szMsstylePath != L"(classic)")
		{
			selectedTheme->szMsstylePath = L"(classic)";
			selectedTheme->fMsstyleChanged = true;
		}
	}
	else if (path && PathFileExists(path)
		&& selectedTheme->szMsstylePath.compare(path) != 0)
	{
		selectedTheme->szMsstylePath = path;
		selectedTheme->fMsstyleChanged = true;
	}

	// update THEMEINFO
	UpdateThemeInfo(ws);

	// update the metrics
	void* theme = windowsClassicTheme ? nullptr : LoadThemeFromFilePath(path);
	if (theme && selectedTheme->selectedScheme && selectedTheme->selectedScheme != currentRegistryScheme)
	{
		if (selectedTheme->selectedScheme->variant == 0x8)
		{
			free(selectedTheme->selectedScheme);
			selectedTheme->selectedScheme = NULL;
		}
	}
	if (theme)
	{
		CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), theme);
	}
	else
	{
		if (selectedTheme->szMsstylePath != L"(classic)")
		{
			selectedTheme->szMsstylePath = L"(classic)";
			selectedTheme->fMsstyleChanged = true;
		}
		currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
		selectedTheme->selectedScheme = currentRegistryScheme;
	}
	themeSelected = TRUE;

	// set the preview bitmap to the static control
	HBITMAP ebmp;
	{
		ScopedPreviewScheme previewScheme(GetPreviewScheme());
		pWndPreview->GetUpdatedPreviewImage(wnd, theme, &ebmp, UPDATE_ALL);
	}
	SetBitmap(hPreview, ebmp);

	SetModified(TRUE);
	themeApplyPending = true;
	if (!selectingModifiedEntry)
	{
		if (selectingStartupBaseline)
			currentThemeParentLabel = startupThemeParentLabel;
		else
		{
			WCHAR parentLabel[MAX_PATH] = {};
			if (ComboBox_GetLBText(hCombobox, comboIndex, parentLabel) != CB_ERR &&
				parentLabel[0])
				currentThemeParentLabel = parentLabel;
		}
		PopulateThemeCombo(index);
		int selectedRow = FindThemeComboIndex(index);
		if (selectedRow != CB_ERR) ComboBox_SetCurSel(hCombobox, selectedRow);
	}
	return 0;
}

BOOL CThemeDlgProc::OnSaveAs(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	WCHAR sourcePath[MAX_PATH] = {};
	DWORD sourcePathSize = sizeof(sourcePath);
	LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes", L"CurrentTheme",
		RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, sourcePath, &sourcePathSize);
	WCHAR expandedSourcePath[MAX_PATH] = {};
	if (status != ERROR_SUCCESS ||
		!ExpandEnvironmentStringsW(sourcePath, expandedSourcePath, ARRAYSIZE(expandedSourcePath)) ||
		!PathFileExistsW(expandedSourcePath))
	{
		::MessageBoxW(m_hWnd, LoadDeskString(IDS_CURRENT_THEME_FILE_MISSING).c_str(),
			LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	WCHAR initialDirectory[MAX_PATH] = {};
	if (FAILED(SHGetFolderPathW(m_hWnd, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT,
		initialDirectory)))
		ExpandEnvironmentStringsW(L"%USERPROFILE%\\Documents",
			initialDirectory, ARRAYSIZE(initialDirectory));
	WCHAR destination[MAX_PATH] = {};
	if (FAILED(StringCchPrintfW(destination, ARRAYSIZE(destination), L"%s\\%s",
		initialDirectory, LoadDeskString(IDS_DEFAULT_THEME_FILENAME).c_str())))
	{
		::MessageBoxW(m_hWnd, LoadDeskString(IDS_DEFAULT_THEME_FILENAME_ERROR).c_str(),
			LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}
	std::wstring themeFilter = MakeThemeFileFilter();
	std::wstring saveTitle = LoadDeskString(IDS_SAVE_THEME_AS);
	OPENFILENAMEW dialog = {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = m_hWnd;
	dialog.lpstrFilter = themeFilter.c_str();
	dialog.lpstrFile = destination;
	dialog.nMaxFile = ARRAYSIZE(destination);
	dialog.lpstrInitialDir = initialDirectory;
	dialog.lpstrTitle = saveTitle.c_str();
	dialog.lpstrDefExt = L"theme";
	dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
	if (!GetSaveFileNameW(&dialog)) return 0;
	if (!PathFindExtensionW(destination)[0])
		StringCchCatW(destination, ARRAYSIZE(destination), L".theme");

	WCHAR displayName[MAX_PATH] = {};
	StringCchCopyW(displayName, ARRAYSIZE(displayName), PathFindFileNameW(destination));
	PathRemoveExtensionW(displayName);
	SCHEMEDATA classicSchemeCopy = {};
	const bool saveCustomClassicScheme = selectedTheme->szMsstylePath == L"(classic)" &&
		IsCustomClassicScheme(selectedTheme->selectedScheme);
	if (saveCustomClassicScheme)
		classicSchemeCopy = *selectedTheme->selectedScheme;

	if (StrCmpI(expandedSourcePath, destination) != 0 && !CopyFileW(expandedSourcePath, destination, FALSE))
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_THEME_COPY_ERROR).c_str(), GetLastError());
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	GUID themeId = {};
	WCHAR themeIdText[64] = {};
	if (SUCCEEDED(CoCreateGuid(&themeId)) && StringFromGUID2(themeId, themeIdText, ARRAYSIZE(themeIdText)))
		WritePrivateProfileStringW(L"Theme", L"ThemeId", themeIdText, destination);
	if (!WritePrivateProfileStringW(L"Theme", L"DisplayName", displayName, destination))
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_THEME_NAME_WRITE_ERROR).c_str(), GetLastError());
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}
	if (saveCustomClassicScheme)
	{
		status = WriteClassicSchemeToThemeFile(destination, &classicSchemeCopy);
		if (status != ERROR_SUCCESS)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_THEME_CLASSIC_SAVE_ERROR).c_str(), status);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return 0;
		}
	}
	RememberThemeFileInHistory(destination);

	DWORD stagedThemeCleanup = RemoveGeneratedThemeCopyWithoutConfirmation(destination);
	if (stagedThemeCleanup != ERROR_SUCCESS)
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_THEME_STAGED_COPY_ERROR).c_str(),
			stagedThemeCleanup);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	ScopedThemeSaveCleanupMarker cleanupMarker(destination);
	HRESULT applyResult = pThemeManager->AddAndSelectTheme(m_hWnd, destination,
		THEMETOOL_APPLY_FLAG_IGNORE_BACKGROUND, 0);
	if (FAILED(applyResult))
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_THEME_ADD_ERROR).c_str(), applyResult);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	pThemeManager->Refresh();
	int currentTheme = 0;
	if (SUCCEEDED(pThemeManager->GetCurrentTheme(&currentTheme)))
	{
		startupSnapshotIsCurrent = false;
		currentThemeParentLabel = displayName;
		currentThemeDerivativeModified = false;
		LSTATUS markerStatus = PersistModifiedThemeName(false, nullptr);
		if (markerStatus != ERROR_SUCCESS)
			LogThemeApplyDebug(L"Save As could not clear modified-theme marker: %ld", markerStatus);
		InterlockedExchange(&g_currentSessionModified, FALSE);
		if (currentITheme) currentITheme->Release();
		currentITheme = nullptr;
		pThemeManager->GetTheme(currentTheme, &currentITheme);
		ComPtr<ITheme10> savedTheme;
		if (SUCCEEDED(pThemeManager->GetTheme(currentTheme, &savedTheme)) && savedTheme)
			RememberSavedThemePath(savedTheme.Get(), destination);
		PopulateThemeCombo(currentTheme);
		int currentComboIndex = FindThemeComboIndex(currentTheme);
		if (currentComboIndex != CB_ERR)
			ComboBox_SetCurSel(hCombobox, currentComboIndex);
		UpdateDeleteButton();
		auto activeTheme = std::make_unique<CTheme>(currentITheme);
		LPWSTR activeStylePath = nullptr;
		if (SUCCEEDED(activeTheme->get_VisualStyle(&activeStylePath)) &&
			activeStylePath && PathFileExistsW(activeStylePath))
			selectedTheme->szMsstylePath = activeStylePath;
	}
	if (saveCustomClassicScheme && selectedTheme->szMsstylePath == L"(classic)")
	{
		LSTATUS schemeStatus = ApplyClassicScheme(m_hWnd,
			GetDpiForWindow(m_hWnd), &classicSchemeCopy, destination);
		if (schemeStatus != ERROR_SUCCESS)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_THEME_CLASSIC_APPLY_ERROR).c_str(), schemeStatus);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		}
		else
		{
			currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
			if (selectedTheme->selectedScheme &&
				selectedTheme->selectedScheme != currentRegistryScheme &&
				selectedTheme->selectedScheme->variant == 0x8)
				free(selectedTheme->selectedScheme);
			selectedTheme->selectedScheme = currentRegistryScheme;
		}
	}

	themeApplyPending = false;
	SetModified(FALSE);
	return 0;
}

bool CThemeDlgProc::GetDeletableThemePath(int themeIndex, WCHAR (&path)[MAX_PATH])
{
	path[0] = L'\0';
	int comboIndex = themeIndex;
	if (comboIndex < 0 || comboIndex >= ComboBox_GetCount(hCombobox)) return false;
	LPARAM itemData = ComboBox_GetItemData(hCombobox, comboIndex);
	if (itemData == THEME_COMBO_CURRENT ||
		itemData == THEME_COMBO_STARTUP_BASELINE ||
		itemData == THEME_COMBO_SESSION_MODIFIED)
		return false;
	if (ComboBox_GetItemData(hCombobox, comboIndex) == THEME_COMBO_SAVED_FILE)
	{
		if (comboIndex >= static_cast<int>(savedThemePaths.size()) ||
			!PathFileExistsW(savedThemePaths[comboIndex].c_str()))
			return false;
		return SUCCEEDED(StringCchCopyW(path, ARRAYSIZE(path), savedThemePaths[comboIndex].c_str()));
	}
	themeIndex = GetThemeIndexFromCombo(comboIndex);
	if (themeIndex < 0) return false;
	ComPtr<ITheme10> theme;
	if (FAILED(pThemeManager->GetTheme(themeIndex, &theme)) || !theme)
		return false;
	LPWSTR themeName = nullptr;
	if (SUCCEEDED(theme->get_DisplayName(&themeName)) &&
		IsNonDeletableBuiltinTheme(themeName))
		return false;
	if (IsUnsavedThemeEntry(themeName))
		return false;

	WCHAR keyPath[128] = {};
	if (GetThemeIdRegistryPath(theme.Get(), keyPath))
	{
		DWORD size = sizeof(path);
		if (RegGetValueW(HKEY_CURRENT_USER, keyPath, L"Path", RRF_RT_REG_SZ,
			nullptr, path, &size) == ERROR_SUCCESS && PathFileExistsW(path) &&
			StrCmpIW(PathFindExtensionW(path), L".theme") == 0)
			return !IsReservedCurrentThemeFile(path);
		path[0] = L'\0';
	}

	// Older custom themes may not have a saved-path entry. XP saved these under
	// My Documents; also recognize themes in the modern per-user Themes folder.
	WCHAR directories[2][MAX_PATH] = {};
	SHGetFolderPathW(m_hWnd, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, directories[0]);
	ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Themes",
		directories[1], ARRAYSIZE(directories[1]));
	LPWSTR displayName = nullptr;
	if (FAILED(theme->get_DisplayName(&displayName)) || !displayName) return false;
	WCHAR matchingPath[MAX_PATH] = {};
	bool ambiguousMatch = false;
	for (const auto& directory : directories)
	{
		if (!directory[0]) continue;
		WCHAR pattern[MAX_PATH] = {};
		if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.theme", directory)))
			continue;
		WIN32_FIND_DATAW findData = {};
		HANDLE find = FindFirstFileW(pattern, &findData);
		if (find == INVALID_HANDLE_VALUE) continue;
		do
		{
			WCHAR candidate[MAX_PATH] = {};
			if (FAILED(StringCchPrintfW(candidate, ARRAYSIZE(candidate), L"%s\\%s",
				directory, findData.cFileName))) continue;
			WCHAR candidateName[MAX_PATH] = {};
			GetPrivateProfileStringW(L"Theme", L"DisplayName", L"", candidateName,
				ARRAYSIZE(candidateName), candidate);
			if (!IsReservedCurrentThemeFile(candidate) &&
				StrCmpIW(candidateName, displayName) == 0)
			{
				// Display names are not unique. Guessing the first matching file
				// can make two dropdown rows point at different files in turn,
				// which makes Delete appear to need a second press. Only use this
				// legacy fallback when the name identifies exactly one file.
				if (!matchingPath[0])
					StringCchCopyW(matchingPath, ARRAYSIZE(matchingPath), candidate);
				else if (StrCmpIW(matchingPath, candidate) != 0)
					ambiguousMatch = true;
			}
		} while (FindNextFileW(find, &findData));
		FindClose(find);
	}
	// Theme Manager's display-name allocator is undocumented; keep the string
	// rather than risk freeing it with the wrong heap.
	if (!matchingPath[0] || ambiguousMatch) return false;
	if (FAILED(StringCchCopyW(path, ARRAYSIZE(path), matchingPath))) return false;

	return true;
}

void CThemeDlgProc::UpdateDeleteButton()
{
	WCHAR path[MAX_PATH] = {};
	bool canDelete = GetDeletableThemePath(ComboBox_GetCurSel(hCombobox), path) &&
		!IsSystemThemeFilePath(path);
	::EnableWindow(GetDlgItem(1108), canDelete);
}

BOOL CThemeDlgProc::OnDelete(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	int index = ComboBox_GetCurSel(hCombobox);
	WCHAR path[MAX_PATH] = {};
	if (!GetDeletableThemePath(index, path) || IsSystemThemeFilePath(path))
	{
		UpdateDeleteButton();
		return 0;
	}

	WCHAR deletePath[MAX_PATH + 1] = {};
	if (FAILED(StringCchCopyW(deletePath, ARRAYSIZE(deletePath), path)))
	{
		return 0;
	}
	SHFILEOPSTRUCTW deleteOperation = {};
	deleteOperation.hwnd = m_hWnd;
	deleteOperation.wFunc = FO_DELETE;
	deleteOperation.pFrom = deletePath;
	deleteOperation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
	int deleteResult = SHFileOperationW(&deleteOperation);
	if (deleteResult != 0 || deleteOperation.fAnyOperationsAborted)
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_THEME_DELETE_ERROR).c_str(),
			deleteResult ? deleteResult : ERROR_CANCELLED);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		UpdateDeleteButton();
		return 0;
	}

	pThemeManager->Refresh();
	int activeIndex = 0;
	pThemeManager->GetCurrentTheme(&activeIndex);
	PopulateThemeCombo(activeIndex);
	int activeComboIndex = FindThemeComboIndex(activeIndex);
	if (activeComboIndex != CB_ERR)
		ComboBox_SetCurSel(hCombobox, activeComboIndex);
	OnThemeComboboxChange(CBN_SELCHANGE, 1101, hCombobox, bHandled);
	themeApplyPending = false;
	SetModified(FALSE);
	UpdateDeleteButton();
	return 0;
}

BOOL CThemeDlgProc::OnApply()
{
	// Avoid calling Theme Manager for wallpaper-only (or other-page-only)
	// applies. SetCurrentTheme reapplies the selected theme's msstyles.
	if (!themeApplyPending)
		return 0;

	// default apply flag, when applied in windows (ignore nothing)
	ULONG apply_flags = 0;
	int comboIndex = ComboBox_GetCurSel(hCombobox);
	const bool applySavedThemeFile =
		comboIndex != CB_ERR &&
		!pendingThemeFilePath.empty() &&
		(ComboBox_GetItemData(hCombobox, comboIndex) == THEME_COMBO_SAVED_FILE ||
			(ComboBox_GetItemData(hCombobox, comboIndex) == THEME_COMBO_STARTUP_BASELINE &&
				StrCmpIW(pendingThemeFilePath.c_str(), startupThemeSnapshotPath.c_str()) == 0));
	const bool applyingStartupSnapshot = applySavedThemeFile &&
		ComboBox_GetItemData(hCombobox, comboIndex) == THEME_COMBO_STARTUP_BASELINE;
	int index = applySavedThemeFile ? -1 : GetThemeIndexFromCombo(comboIndex);
	if (!applySavedThemeFile && index < 0) return 0;

	if (selectedTheme->customWallpaperSelection)
		apply_flags |= THEMETOOL_APPLY_FLAG_IGNORE_BACKGROUND;

	if (selectedTheme->fCustomDesktopColorPending)
		apply_flags |= THEMETOOL_APPLY_FLAG_IGNORE_COLOR;

	// Manager-backed custom themes can be exposed as a transient "Unsaved
	// theme" entry after SetCurrentTheme. Preserve the selected saved file so
	// the refreshed current entry can still be identified as that unchanged
	// custom theme instead of being labeled Modified.
	WCHAR selectedSavedThemePath[MAX_PATH] = {};
	const bool applyingSavedManagerTheme = !applySavedThemeFile &&
		GetDeletableThemePath(comboIndex, selectedSavedThemePath);
	LogThemeApplyDebug(L"OnApply combo=%d item=%lld savedFile=%d pending=%ls managerFile=%d source=%ls",
		comboIndex, static_cast<long long>(comboIndex == CB_ERR
			? static_cast<LPARAM>(CB_ERR) : ComboBox_GetItemData(hCombobox, comboIndex)),
		applySavedThemeFile, pendingThemeFilePath.c_str(), applyingSavedManagerTheme,
		selectedSavedThemePath);
	if (applySavedThemeFile && !PathFileExistsW(pendingThemeFilePath.c_str()))
	{
		ShowMissingThemeFileError(m_hWnd, pendingThemeFilePath.c_str());
		return PSNRET_INVALID_NOCHANGEPAGE;
	}
	if (!applySavedThemeFile && index >= 0)
	{
		ComPtr<ITheme10> selectedFileTheme;
		WCHAR registeredPath[MAX_PATH] = {};
		if (SUCCEEDED(pThemeManager->GetTheme(index, &selectedFileTheme)) &&
			selectedFileTheme &&
			GetRegisteredThemeFilePath(selectedFileTheme.Get(), registeredPath) &&
			!PathFileExistsW(registeredPath))
		{
			ShowMissingThemeFileError(m_hWnd, registeredPath);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
	}

	if (applySavedThemeFile)
	{
		SCHEMEDATA classicFileScheme = {};
		const bool hasClassicFileScheme = LoadClassicSchemeFromThemeFile(
			pendingThemeFilePath.c_str(), classicFileScheme);
		selectedTheme->classicSchemeSourcePath = pendingThemeFilePath;
		DWORD cleanupResult = RemoveGeneratedThemeCopyWithoutConfirmation(
			pendingThemeFilePath.c_str());
		if (cleanupResult != ERROR_SUCCESS)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_THEME_PREPARE_ERROR).c_str(), cleanupResult);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return 0;
		}

		ScopedThemeSaveCleanupMarker cleanupMarker(pendingThemeFilePath.c_str());
		HRESULT fileApplyResult = pThemeManager->AddAndSelectTheme(m_hWnd,
			pendingThemeFilePath.c_str(), apply_flags, 0);
		LogThemeApplyDebug(L"Apply saved-file AddAndSelectTheme result=0x%08X source=%ls",
			static_cast<unsigned int>(fileApplyResult), pendingThemeFilePath.c_str());
		if (FAILED(fileApplyResult))
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_THEME_APPLY_ERROR).c_str(), fileApplyResult);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return 0;
		}
		startupSnapshotIsCurrent = applyingStartupSnapshot;

		pThemeManager->Refresh();
		int activeIndex = 0;
		if (SUCCEEDED(pThemeManager->GetCurrentTheme(&activeIndex)))
		{
			if (currentITheme) currentITheme->Release();
			currentITheme = nullptr;
			if (SUCCEEDED(pThemeManager->GetTheme(activeIndex, &currentITheme)) && currentITheme)
			{
				ComPtr<ITheme10> appliedTheme;
				if (!applyingStartupSnapshot &&
					SUCCEEDED(pThemeManager->GetTheme(activeIndex, &appliedTheme)) && appliedTheme)
					RememberSavedThemePath(appliedTheme.Get(), pendingThemeFilePath.c_str());

				auto activeTheme = std::make_unique<CTheme>(currentITheme);
				LPWSTR wallpaper = nullptr;
				activeTheme->get_background(&wallpaper);
				UpdateThemeInfo(wallpaper);

				LPWSTR activeStylePath = nullptr;
				activeTheme->get_VisualStyle(&activeStylePath);
				HANDLE previewTheme = activeStylePath && PathFileExistsW(activeStylePath)
					? LoadThemeFromFilePath(activeStylePath) : nullptr;
				if (previewTheme)
				{
					selectedTheme->szMsstylePath = activeStylePath;
					if (selectedTheme->selectedScheme &&
						selectedTheme->selectedScheme != currentRegistryScheme &&
						selectedTheme->selectedScheme->variant == 0x8)
					{
						free(selectedTheme->selectedScheme);
						selectedTheme->selectedScheme = nullptr;
					}
					CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), previewTheme);
					if (selectedTheme->selectedScheme &&
						!ApplySchemeMetrics(selectedTheme->selectedScheme,
							GetDpiForWindow(m_hWnd)))
						LogThemeApplyDebug(L"Applying saved msstyle non-client metrics failed");
				}
				else
				{
					selectedTheme->szMsstylePath = L"(classic)";
					if (hasClassicFileScheme)
					{
						LSTATUS schemeStatus = ApplyClassicScheme(m_hWnd,
							GetDpiForWindow(m_hWnd), &classicFileScheme,
							pendingThemeFilePath.c_str());
						if (schemeStatus != ERROR_SUCCESS)
						{
							WCHAR message[256] = {};
							StringCchPrintfW(message, ARRAYSIZE(message),
								LoadDeskString(IDS_THEME_APPLIED_CLASSIC_ERROR).c_str(),
								schemeStatus);
							::MessageBoxW(m_hWnd, message,
								LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
						}
					}
					currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
					selectedTheme->selectedScheme = currentRegistryScheme;
				}

				HBITMAP previewBitmap = nullptr;
				{
					ScopedPreviewScheme previewScheme(GetPreviewScheme());
					pWndPreview->GetUpdatedPreviewImage(wnd, previewTheme,
						&previewBitmap, UPDATE_ALL);
				}
				SetBitmap(hPreview, previewBitmap);
			}

			PopulateThemeCombo(activeIndex);
			int activeComboIndex = CB_ERR;
			if (applyingStartupSnapshot)
			{
				activeComboIndex = FindThemeComboIndex(activeIndex);
			}
			else
			{
				// Theme Manager can reuse the launch entry's index and label the
				// active object "Unsaved Theme" after applying a saved .theme. In
				// that case select the file row by path, never by the old index.
				for (int candidate = 0; candidate < ComboBox_GetCount(hCombobox); ++candidate)
				{
					WCHAR candidatePath[MAX_PATH] = {};
					LPARAM candidateData = ComboBox_GetItemData(hCombobox, candidate);
					if (candidateData == THEME_COMBO_SAVED_FILE &&
						candidate < static_cast<int>(savedThemePaths.size()))
						StringCchCopyW(candidatePath, ARRAYSIZE(candidatePath),
							savedThemePaths[candidate].c_str());
					else if (candidateData >= 0)
						GetDeletableThemePath(candidate, candidatePath);
					if (candidatePath[0] &&
						StrCmpIW(candidatePath, pendingThemeFilePath.c_str()) == 0)
					{
						activeComboIndex = candidate;
						break;
					}
				}
			}
			if (activeComboIndex != CB_ERR)
				ComboBox_SetCurSel(hCombobox, activeComboIndex);
			else
				LogThemeApplyDebug(L"Could not select applied theme file row source=%ls active=%d",
					pendingThemeFilePath.c_str(), activeIndex);
		}
		pendingThemeFilePath.clear();
		themeApplyPending = false;
		SetModified(FALSE);
		UpdateDeleteButton();
		return 0;
	}

	// The built-in Windows Classic theme always means Windows Standard. Other
	// Classic .theme files may intentionally carry a separately selected scheme.
	const bool applyClassicTheme = selectedTheme->szMsstylePath == L"(classic)";
	ComPtr<ITheme10> selectedThemeInterface;
	LPWSTR selectedThemeName = nullptr;
	const bool applyWindowsClassic = applyClassicTheme &&
		SUCCEEDED(pThemeManager->GetTheme(index, &selectedThemeInterface)) && selectedThemeInterface &&
		SUCCEEDED(selectedThemeInterface->get_DisplayName(&selectedThemeName)) &&
		IsWindowsClassicThemeName(selectedThemeName);
	SCHEMEDATA savedManagerClassicScheme = {};
	const bool hasSavedManagerClassicScheme = applyingSavedManagerTheme && applyClassicTheme &&
		LoadClassicSchemeFromThemeFile(selectedSavedThemePath, savedManagerClassicScheme);
	HRESULT applyResult = E_UNEXPECTED;
	if (applyingSavedManagerTheme)
	{
		// Applying a registered saved theme by manager index can leave the
		// generated Custom.theme as the current "Unsaved Theme" entry even when
		// the user made no edits. Re-apply its source file through the same path
		// used for file-backed rows so Theme Manager retains the saved identity.
		DWORD cleanupResult = RemoveGeneratedThemeCopyWithoutConfirmation(
			selectedSavedThemePath);
		if (cleanupResult != ERROR_SUCCESS)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_THEME_PREPARE_ERROR).c_str(), cleanupResult);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return 0;
		}
		ScopedThemeSaveCleanupMarker cleanupMarker(selectedSavedThemePath);
		applyResult = pThemeManager->AddAndSelectTheme(m_hWnd,
			selectedSavedThemePath, apply_flags, 0);
	}
	else
	{
		applyResult = pThemeManager->SetCurrentTheme(m_hWnd, index, TRUE, apply_flags, 0);
	}
	LogThemeApplyDebug(L"Apply api=%ls result=0x%08X source=%ls",
		applyingSavedManagerTheme ? L"AddAndSelectTheme" : L"SetCurrentTheme",
		static_cast<unsigned int>(applyResult), selectedSavedThemePath);
	if (SUCCEEDED(applyResult))
		startupSnapshotIsCurrent = false;
	if (SUCCEEDED(applyResult) && applyClassicTheme)
	{
		SCHEMEDATA* schemeToApply = applyWindowsClassic
			? standardPreviewScheme
			: hasSavedManagerClassicScheme ? &savedManagerClassicScheme
			: selectedTheme->selectedScheme;
		const bool applyCustomScheme = !applyWindowsClassic &&
			IsCustomClassicScheme(schemeToApply);
		LSTATUS schemeStatus = applyCustomScheme
			? ApplyClassicScheme(m_hWnd, GetDpiForWindow(m_hWnd),
				schemeToApply, hasSavedManagerClassicScheme
					? selectedSavedThemePath : nullptr)
			: ApplyWindowsStandardClassicScheme(m_hWnd, GetDpiForWindow(m_hWnd));
		if (schemeStatus != ERROR_SUCCESS)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				applyCustomScheme
					? LoadDeskString(IDS_CLASSIC_SCHEME_APPLY_ERROR).c_str()
					: LoadDeskString(IDS_STANDARD_SCHEME_APPLY_ERROR).c_str(),
				schemeStatus);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		}
		else
		{
			currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
			if (selectedTheme->selectedScheme &&
				selectedTheme->selectedScheme != currentRegistryScheme &&
				selectedTheme->selectedScheme->variant == 0x8)
				free(selectedTheme->selectedScheme);
			selectedTheme->selectedScheme = currentRegistryScheme;
		}
	}
	if (SUCCEEDED(applyResult) && applyingSavedManagerTheme)
	{
		pThemeManager->Refresh();
		int activeIndex = -1;
		HRESULT currentResult = pThemeManager->GetCurrentTheme(&activeIndex);
		LogThemeApplyDebug(L"Refresh currentResult=0x%08X activeIndex=%d",
			static_cast<unsigned int>(currentResult), activeIndex);
		if (SUCCEEDED(currentResult) && activeIndex >= 0)
		{
			ComPtr<ITheme10> appliedTheme;
			HRESULT getThemeResult = pThemeManager->GetTheme(activeIndex, &appliedTheme);
			if (SUCCEEDED(getThemeResult) && appliedTheme)
			{
				RememberSavedThemePath(appliedTheme.Get(), selectedSavedThemePath);
				GUID appliedId = {};
				WCHAR appliedIdText[64] = {};
				WCHAR keyPath[128] = {};
				GetThemeIdRegistryPath(appliedTheme.Get(), keyPath);
				if (SUCCEEDED(appliedTheme->get_ThemeId(&appliedId)))
					StringFromGUID2(appliedId, appliedIdText, ARRAYSIZE(appliedIdText));
				LogThemeApplyDebug(L"Mapped active theme result=0x%08X id=%ls key=%ls source=%ls",
					static_cast<unsigned int>(getThemeResult), appliedIdText, keyPath,
					selectedSavedThemePath);
			}

			PopulateThemeCombo(activeIndex);
			int activeComboIndex = FindThemeComboIndex(activeIndex);
			if (activeComboIndex != CB_ERR)
				ComboBox_SetCurSel(hCombobox, activeComboIndex);
			lastThemeIndex = activeIndex;
		}
	}
	if (SUCCEEDED(applyResult) && !applyClassicTheme)
	{
		int activeThemeIndex = -1;
		if (SUCCEEDED(pThemeManager->GetCurrentTheme(&activeThemeIndex)) &&
			activeThemeIndex >= 0)
		{
			ComPtr<ITheme10> activeThemeInterface;
			if (SUCCEEDED(pThemeManager->GetTheme(activeThemeIndex,
				&activeThemeInterface)) && activeThemeInterface)
			{
				CTheme activeTheme(activeThemeInterface.Get());
				LPWSTR activeStylePath = nullptr;
				if (SUCCEEDED(activeTheme.get_VisualStyle(&activeStylePath)) &&
					activeStylePath && PathFileExistsW(activeStylePath))
				{
					HANDLE activeStyle = LoadThemeFromFilePath(activeStylePath);
					if (activeStyle)
					{
						CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), activeStyle);
						if (selectedTheme->selectedScheme &&
							!ApplySchemeMetrics(selectedTheme->selectedScheme,
								GetDpiForWindow(m_hWnd)))
							LogThemeApplyDebug(L"Applying active msstyle non-client metrics failed");
					}
				}
			}
		}
	}
	themeApplyPending = false;
	SetModified(FALSE);
	return 0;
}

BOOL CThemeDlgProc::OnSetActive()
{
	selectionPicker = true;
	int selectedThemeIndex = lastThemeIndex;
	if (!themeApplyPending)
	{
		// Other pages can apply a visual style or wallpaper and cause Theme
		// Manager to replace its current/Modified entry. Refresh and select the
		// actual current theme instead of reusing the old combo index.
		pThemeManager->Refresh();
		if (FAILED(pThemeManager->GetCurrentTheme(&selectedThemeIndex)))
			selectedThemeIndex = lastThemeIndex;
		lastThemeIndex = selectedThemeIndex;
	}
	PopulateThemeCombo(selectedThemeIndex);
	if (!pendingThemeFilePath.empty())
	{
		for (int i = 0; i < static_cast<int>(savedThemePaths.size()); ++i)
			if (ComboBox_GetItemData(hCombobox, i) == THEME_COMBO_SAVED_FILE &&
				StrCmpIW(savedThemePaths[i].c_str(), pendingThemeFilePath.c_str()) == 0)
			{
				ComboBox_SetCurSel(hCombobox, i);
				break;
			}
	}
	UpdateDeleteButton();
	if (selectedTheme->customWallpaperSelection 
		|| selectedTheme->newColor != 0xB0000000 
		|| selectedTheme->posChanged != -1 
		|| selectedTheme->updateWallThemesPg)
	{
		// set the preview bitmap to the static control
		HBITMAP ebmp;
		{
			ScopedPreviewScheme previewScheme(GetPreviewScheme());
			pWndPreview->GetUpdatedPreviewImage(wnd, nullptr, &ebmp, UPDATE_WALLPAPER | UPDATE_SOLIDCLR);
		}
		SetBitmap(hPreview, ebmp);

		selectedTheme->updateWallThemesPg = false;
	}
	if (selectedTheme->fMsstyleChanged || selectedTheme->fThemePgMsstyleUpdate)
	{
		HBITMAP ebmp;
		{
			ScopedPreviewScheme previewScheme(GetPreviewScheme());
			pWndPreview->GetUpdatedPreviewImage(wnd, LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str()), &ebmp, UPDATE_WINDOW);
		}
		SetBitmap(hPreview, ebmp);

		selectedTheme->fThemePgMsstyleUpdate = selectedTheme->fMsstyleChanged = false;
	}

	_TerminateProcess(pi);
	return 0;
}

// set relevant info accordng to this page
// if u select a new theme, the settings set by background page
// becomes irrelevant, overwrite them
void CThemeDlgProc::UpdateThemeInfo(LPWSTR ws)
{
	// update THEMEINFO
	if (lstrlen(ws) == 0 || PathFileExists(ws) == FALSE)
	{
		// no wallpaper applied
		selectedTheme->wallpaperPath = L"";
		selectedTheme->wallpaperType = WT_NOWALL;
	}
	else
	{
		BOOL isEn = FALSE;
		auto themeClass = std::make_unique<CTheme>(currentITheme);
		themeClass->IsSlideshowEnabled(&isEn);
		if (isEn)
		{
			selectedTheme->wallpaperType = WT_SLIDESHOW;
			selectedTheme->fSlideshowSelection = true;

		}
		else
		{
			selectedTheme->wallpaperType = WT_PICTURE;
		}

		// wndprvw uses this to display the preview
		selectedTheme->wallpaperPath = ws;
	}

	// common properties
	selectedTheme->newColor = 0xB0000000;
	selectedTheme->customWallpaperSelection = false;
	selectedTheme->posChanged = -1;
	selectedTheme->useDesktopColor = false;
}
