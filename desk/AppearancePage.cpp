#include "pch.h"
#include "AppearanceDlgBox.h"
#include "AppearancePage.h"
#include "cscheme.h"
#include "desk.h"
#include "EffectsDlg.h"
#include "helper.h"
#include "uxtheme.h"

using namespace Gdiplus;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Details;

#define HAS_NORMAL 0x1
#define HAS_LARGE 0x2
#define HAS_EXTRA_LARGE 0x4
#define CUSTOM_SCHEME 0x8

static constexpr LPCWSTR APPEARANCE_XP_STYLE = L"(xp)";
static constexpr LPCWSTR APPEARANCE_CLASSIC_STYLE = L"(classic)";
static SCHEMEDATA browsedClassicScheme = {};

static bool LogFontsMatch(const LOGFONTW& first, const LOGFONTW& second)
{
	return first.lfHeight == second.lfHeight && first.lfWidth == second.lfWidth &&
		first.lfEscapement == second.lfEscapement &&
		first.lfOrientation == second.lfOrientation &&
		first.lfWeight == second.lfWeight && first.lfItalic == second.lfItalic &&
		first.lfUnderline == second.lfUnderline &&
		first.lfStrikeOut == second.lfStrikeOut && first.lfCharSet == second.lfCharSet &&
		first.lfOutPrecision == second.lfOutPrecision &&
		first.lfClipPrecision == second.lfClipPrecision &&
		first.lfQuality == second.lfQuality &&
		first.lfPitchAndFamily == second.lfPitchAndFamily &&
		StrCmpIW(first.lfFaceName, second.lfFaceName) == 0;
}

static bool ClassicMetricsMatch(const SCHEMEDATA* first, const SCHEMEDATA* second)
{
	if (!first || !second) return false;
	const auto& a = first->ncm;
	const auto& b = second->ncm;
	return a.iBorderWidth == b.iBorderWidth &&
		a.iScrollWidth == b.iScrollWidth && a.iScrollHeight == b.iScrollHeight &&
		a.iCaptionWidth == b.iCaptionWidth && a.iCaptionHeight == b.iCaptionHeight &&
		a.iSmCaptionWidth == b.iSmCaptionWidth &&
		a.iSmCaptionHeight == b.iSmCaptionHeight &&
		a.iMenuWidth == b.iMenuWidth && a.iMenuHeight == b.iMenuHeight &&
		LogFontsMatch(a.lfCaptionFont, b.lfCaptionFont) &&
		LogFontsMatch(a.lfSmCaptionFont, b.lfSmCaptionFont) &&
		LogFontsMatch(a.lfMenuFont, b.lfMenuFont) &&
		LogFontsMatch(a.lfStatusFont, b.lfStatusFont) &&
		LogFontsMatch(a.lfMessageFont, b.lfMessageFont) &&
		LogFontsMatch(first->lfIconTitle, second->lfIconTitle) &&
		first->iPaddedBorderWidth == second->iPaddedBorderWidth;
}

static bool HasClassicParentName(const SCHEMEDATA* scheme)
{
	return scheme && scheme->name[0] &&
		StrCmpI(scheme->name, L"Custom Classic") != 0 &&
		StrCmpI(scheme->name, L"Unsaved Theme") != 0 &&
		StrCmpI(scheme->name, L"Windows Default") != 0;
}

static SCHEMEDATA* LoadCurrentClassicSchemeFromThemeDerivative()
{
	static SCHEMEDATA derivativeScheme = {};
	WCHAR path[MAX_PATH] = {};
	if (!ExpandEnvironmentStringsW(
		L"%LOCALAPPDATA%\\Microsoft\\Windows\\Themes\\Custom.theme",
		path, ARRAYSIZE(path)) || !PathFileExistsW(path) ||
		!LoadClassicSchemeFromThemeFile(path, derivativeScheme))
		return nullptr;

	// Custom.theme carries the derivative palette/name, but its serialized
	// NonclientMetrics can lag the metrics currently applied to Windows. Use the
	// live metric set for the appearance preview and editing baseline.
	SCHEMEDATA* liveScheme = LoadCurrentClassicSchemeFromRegistry();
	if (liveScheme)
	{
		derivativeScheme.ncm = liveScheme->ncm;
		derivativeScheme.dpiScaled = liveScheme->dpiScaled;
		derivativeScheme.lfIconTitle = liveScheme->lfIconTitle;
		derivativeScheme.iPaddedBorderWidth = liveScheme->iPaddedBorderWidth;
		// The selected Classic scheme in the live registry is authoritative for
		// the parent identity. Theme Manager can leave an older SchemeName in
		// Custom.theme (for example, "Windows Default") after a Classic change.
		if (liveScheme->name[0] &&
			StrCmpI(liveScheme->name, L"Custom Classic") != 0 &&
			StrCmpI(liveScheme->name, L"Unsaved Theme") != 0)
			StringCchCopyW(derivativeScheme.name,
				ARRAYSIZE(derivativeScheme.name), liveScheme->name);
		static constexpr WCHAR modifiedSuffix[] = L" (Modified)";
		const size_t nameLength = wcslen(derivativeScheme.name);
		const size_t suffixLength = ARRAYSIZE(modifiedSuffix) - 1;
		if (nameLength >= suffixLength &&
			StrCmpIW(derivativeScheme.name + nameLength - suffixLength,
				modifiedSuffix) == 0)
			derivativeScheme.name[nameLength - suffixLength] = L'\0';
	}
	return &derivativeScheme;
}

// Classic scheme data is meaningful as the current/editing baseline only
// while a Classic theme is actually applied. Do not resurrect an old
// Custom.theme derivative or registry selection while an msstyle is active.
static SCHEMEDATA* LoadAppliedClassicScheme(bool classicStyleIsActive)
{
	if (!classicStyleIsActive)
		return nullptr;

	SCHEMEDATA* scheme = LoadCurrentClassicSchemeFromThemeDerivative();
	if (!scheme)
		scheme = LoadCurrentClassicSchemeFromRegistry();

	if (selectedTheme && !selectedTheme->classicSchemeSourcePath.empty() &&
		LoadClassicSchemeFromThemeFile(selectedTheme->classicSchemeSourcePath.c_str(),
			browsedClassicScheme))
		scheme = &browsedClassicScheme;

	return scheme;
}

static bool ResolveMsstylePath(LPCWSTR path, WCHAR (&resolvedPath)[MAX_PATH])
{
	if (!path || !path[0]) return false;
	WCHAR expandedPath[MAX_PATH] = {};
	DWORD expandedLength = ExpandEnvironmentStringsW(path, expandedPath,
		ARRAYSIZE(expandedPath));
	if (!expandedLength || expandedLength >= ARRAYSIZE(expandedPath)) return false;

	WCHAR resolvedInput[MAX_PATH] = {};
	LPCWSTR resourceToken = StrStrIW(expandedPath, L"%ResourceDir%");
	if (resourceToken)
	{
		WCHAR windowsDirectory[MAX_PATH] = {};
		WCHAR resourceDirectory[MAX_PATH] = {};
		if (!GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory)) ||
			FAILED(StringCchPrintfW(resourceDirectory, ARRAYSIZE(resourceDirectory),
				L"%s\\Resources", windowsDirectory)) ||
			FAILED(StringCchPrintfW(resolvedInput, ARRAYSIZE(resolvedInput), L"%s%s",
				resourceDirectory, resourceToken + ARRAYSIZE(L"%ResourceDir%") - 1)))
			return false;
	}
	else if (FAILED(StringCchCopyW(resolvedInput, ARRAYSIZE(resolvedInput), expandedPath)))
	{
		return false;
	}

	if (!PathCanonicalizeW(resolvedPath, resolvedInput)) return false;
	return PathFileExistsW(resolvedPath) != FALSE;
}

static bool RefreshActiveThemeStylePath()
{
	if (!pThemeManager || !selectedTheme) return false;
	int activeIndex = 0;
	if (FAILED(pThemeManager->GetCurrentTheme(&activeIndex))) return false;

	ComPtr<ITheme10> activeThemeInfo;
	if (FAILED(pThemeManager->GetTheme(activeIndex, &activeThemeInfo)) || !activeThemeInfo)
		return false;
	LPWSTR displayName = nullptr;
	const bool windowsClassicTheme =
		SUCCEEDED(activeThemeInfo->get_DisplayName(&displayName)) &&
		IsWindowsClassicThemeName(displayName);
	if (currentITheme) currentITheme->Release();
	currentITheme = activeThemeInfo.Detach();

	std::wstring activeStyle = APPEARANCE_CLASSIC_STYLE;
	if (!windowsClassicTheme)
	{
		auto acceptStylePath = [&activeStyle](LPCWSTR stylePath)
		{
			WCHAR resolvedStylePath[MAX_PATH] = {};
			if (!ResolveMsstylePath(stylePath, resolvedStylePath) ||
				StrCmpIW(PathFindExtensionW(resolvedStylePath), L".msstyles") != 0)
				return false;

			activeStyle = resolvedStylePath;
			return true;
		};

		// The Theme Manager's theme object can retain the theme-pack variant
		// (for example, Olive) even after Windows has loaded another msstyles
		// variant. Prefer uxtheme's live path so the page follows the style
		// actually applied, including Luna's default blue variant.
		WCHAR liveStylePath[MAX_PATH] = {};
		WCHAR liveColorName[MAX_PATH] = {};
		WCHAR liveSizeName[MAX_PATH] = {};
		const HRESULT liveStyleResult = GetCurrentThemeName(
			liveStylePath, ARRAYSIZE(liveStylePath),
			liveColorName, ARRAYSIZE(liveColorName),
			liveSizeName, ARRAYSIZE(liveSizeName));
		bool liveStyleAccepted = SUCCEEDED(liveStyleResult) &&
			acceptStylePath(liveStylePath);
		if (!liveStyleAccepted)
		{
			auto activeTheme = std::make_unique<CTheme>(currentITheme);
			LPWSTR activeStylePath = nullptr;
			if (SUCCEEDED(activeTheme->get_VisualStyle(&activeStylePath)))
				acceptStylePath(activeStylePath);
		}
	}

	selectedTheme->szMsstylePath = activeStyle;
	return true;
}

static bool MsstylePathsMatch(LPCWSTR firstPath, LPCWSTR secondPath)
{
	WCHAR firstResolved[MAX_PATH] = {};
	WCHAR secondResolved[MAX_PATH] = {};
	return ResolveMsstylePath(firstPath, firstResolved) &&
		ResolveMsstylePath(secondPath, secondResolved) &&
		StrCmpIW(firstResolved, secondResolved) == 0;
}

static HANDLE LoadCurrentMsstyleVariant(LPCWSTR stylePath)
{
	WCHAR liveStylePath[MAX_PATH] = {};
	WCHAR liveColorName[MAX_PATH] = {};
	WCHAR liveSizeName[MAX_PATH] = {};
	if (SUCCEEDED(GetCurrentThemeName(liveStylePath, ARRAYSIZE(liveStylePath),
		liveColorName, ARRAYSIZE(liveColorName),
		liveSizeName, ARRAYSIZE(liveSizeName))) &&
		MsstylePathsMatch(stylePath, liveStylePath))
	{
		WCHAR resolvedLiveStylePath[MAX_PATH] = {};
		if (ResolveMsstylePath(liveStylePath, resolvedLiveStylePath))
			return LoadThemeFromFilePath(resolvedLiveStylePath, liveColorName, liveSizeName);
	}
	return LoadThemeFromFilePath(stylePath);
}

static LPCWSTR GetBuiltInMsstyleDisplayName(LPCWSTR stylePath)
{
	LPCWSTR fileName = PathFindFileNameW(stylePath);
	static const std::wstring names[] = {
		LoadDeskString(IDS_DEFAULT_BLUE_STYLE), LoadDeskString(IDS_ZUNE_STYLE),
		LoadDeskString(IDS_ROYALE_STYLE), LoadDeskString(IDS_ROYALE_NOIR_STYLE),
		LoadDeskString(IDS_EMBEDDED_STYLE)
	};
	if (StrCmpIW(fileName, L"Luna.msstyles") == 0) return names[0].c_str();
	if (StrCmpIW(fileName, L"Zune.msstyles") == 0) return names[1].c_str();
	if (StrCmpIW(fileName, L"Royale.msstyles") == 0) return names[2].c_str();
	if (StrCmpIW(fileName, L"RoyaleNoir.msstyles") == 0) return names[3].c_str();
	if (StrCmpIW(fileName, L"Embedded.msstyles") == 0) return names[4].c_str();
	return nullptr;
}

static HRESULT ApplyMsstyleWithoutChangingWallpaper(HWND hwnd, LPCWSTR stylePath)
{
	if (!pThemeManager || !GetThemeDefaults || !stylePath || !PathFileExistsW(stylePath))
		return E_INVALIDARG;

	WCHAR colorName[MAX_PATH] = {};
	WCHAR sizeName[MAX_PATH] = {};
	HRESULT hr = GetThemeDefaults(stylePath, colorName, ARRAYSIZE(colorName),
		sizeName, ARRAYSIZE(sizeName));
	if (FAILED(hr)) return hr;

	int themeCount = 0;
	hr = pThemeManager->GetThemeCount(&themeCount);
	if (FAILED(hr)) return hr;
	for (int index = 0; index < themeCount; ++index)
	{
		ComPtr<ITheme10> theme;
		hr = pThemeManager->GetTheme(index, &theme);
		if (FAILED(hr) || !theme) continue;

		LPWSTR candidatePath = nullptr;
		CTheme themeClass(theme.Get());
		if (FAILED(themeClass.get_VisualStyle(&candidatePath)) || !candidatePath) continue;

		WCHAR expandedCandidate[MAX_PATH] = {};
		WCHAR expandedSelected[MAX_PATH] = {};
		ExpandEnvironmentStringsW(candidatePath, expandedCandidate, ARRAYSIZE(expandedCandidate));
		ExpandEnvironmentStringsW(stylePath, expandedSelected, ARRAYSIZE(expandedSelected));
		if (StrCmpI(expandedCandidate, expandedSelected) == 0)
			return pThemeManager->SetCurrentTheme(hwnd, index, TRUE,
				THEMETOOL_APPLY_FLAG_IGNORE_BACKGROUND, 0);
	}

	// No installed .theme entry points at this style; apply the .msstyles itself.
	return SetSystemVisualStyle
		? SetSystemVisualStyle(stylePath, colorName, sizeName,
			(ApplyThemeFlags)(AT_FORCE_GLOBAL | AT_SET_METRICS))
		: E_UNEXPECTED;
}

static HRESULT ApplyClassicThemeWithoutChangingWallpaper(HWND hwnd)
{
	if (!pThemeManager)
		return E_UNEXPECTED;

	WCHAR classicThemeName[128] = {};
	StringCchCopyW(classicThemeName, ARRAYSIZE(classicThemeName),
		LoadDeskString(IDS_WINDOWS_CLASSIC_NAME).c_str());
	WCHAR localizedClassicName[128] = {};
	if (LoadStringW(g_hThemeUI, 2016, localizedClassicName,
		ARRAYSIZE(localizedClassicName)) > 0)
		StringCchCopyW(classicThemeName, ARRAYSIZE(classicThemeName), localizedClassicName);

	int themeCount = 0;
	HRESULT hr = pThemeManager->GetThemeCount(&themeCount);
	if (FAILED(hr)) return hr;

	for (int index = 0; index < themeCount; ++index)
	{
		ComPtr<ITheme10> theme;
		hr = pThemeManager->GetTheme(index, &theme);
		if (FAILED(hr) || !theme) continue;

		LPWSTR displayName = nullptr;
		hr = theme->get_DisplayName(&displayName);
		const bool isClassic = SUCCEEDED(hr) && displayName &&
			StrCmpI(displayName, classicThemeName) == 0;
		// The Theme Manager interface does not document the allocator for this
		// returned string. Do not free it with a guessed allocator; ThemesPage
		// uses the same API and keeps the returned name alive for the page.
		if (isClassic)
		{
			return pThemeManager->SetCurrentTheme(hwnd, index, TRUE,
				THEMETOOL_APPLY_FLAG_IGNORE_BACKGROUND, 0);
		}
	}

	return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static void LogClassicSchemeApply(LPCWSTR stage, HWND colorCombo, const SCHEMEDATA* scheme)
{
	WCHAR comboText[128] = {};
	int comboIndex = ComboBox_GetCurSel(colorCombo);
	if (comboIndex != CB_ERR)
		ComboBox_GetLBText(colorCombo, comboIndex, comboText);

	WCHAR line[512] = {};
	StringCchPrintfW(line, ARRAYSIZE(line),
		L"%s: combo='%s' scheme='%s' expected Window=%06X actual Window=%06X; expected ActiveTitle=%06X actual ActiveTitle=%06X\r\n",
		stage, comboText, scheme ? scheme->name : L"<null>",
		scheme ? scheme->rgb[5] : 0, GetSysColor(COLOR_WINDOW),
		scheme ? scheme->rgb[2] : 0, GetSysColor(COLOR_ACTIVECAPTION));

	WCHAR tempPath[MAX_PATH] = {};
	WCHAR logPath[MAX_PATH] = {};
	if (!GetTempPathW(ARRAYSIZE(tempPath), tempPath) ||
		FAILED(StringCchPrintfW(logPath, ARRAYSIZE(logPath), L"%sdeskn-classic-apply.log", tempPath)))
		return;

	HANDLE logFile = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (logFile == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	WriteFile(logFile, line, static_cast<DWORD>(lstrlenW(line) * sizeof(WCHAR)), &written, nullptr);
	CloseHandle(logFile);
}

static LSTATUS PersistClassicSchemeColors(const SCHEMEDATA* scheme)
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
	if (!scheme) return ERROR_INVALID_PARAMETER;

	HKEY colorsKey = nullptr;
	LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Colors",
		0, KEY_SET_VALUE, &colorsKey);
	if (status != ERROR_SUCCESS) return status;

	for (int i = 0; i < MAX_COLORS; ++i)
	{
		WCHAR value[32] = {};
		if (FAILED(StringCchPrintfW(value, ARRAYSIZE(value), L"%u %u %u",
			GetRValue(scheme->rgb[i]), GetGValue(scheme->rgb[i]), GetBValue(scheme->rgb[i]))))
		{
			status = ERROR_INVALID_DATA;
			break;
		}

		status = RegSetValueExW(colorsKey, colorNames[i], 0, REG_SZ,
			reinterpret_cast<const BYTE*>(value),
			static_cast<DWORD>((lstrlenW(value) + 1) * sizeof(WCHAR)));
		if (status != ERROR_SUCCESS) break;
	}

	RegCloseKey(colorsKey);
	return status;
}

// The theme-switching layer restores the scheme named by ClassicSchemes' root
// SelectedScheme value. Keep that cache in sync with the Appearance scheme
// selected here, or its later refresh can put the prior palette back.
static LSTATUS PersistClassicSchemeSelection(const SCHEMEDATA* scheme)
{
	if (!scheme || !scheme->name[0]) return ERROR_INVALID_PARAMETER;

	HKEY schemesKey = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
		L"Control Panel\\Appearance\\ClassicSchemes", 0, nullptr, 0,
		KEY_READ | KEY_SET_VALUE | KEY_CREATE_SUB_KEY, nullptr, &schemesKey, nullptr);
	if (status != ERROR_SUCCESS) return status;

	WCHAR previousName[256] = {};
	DWORD previousNameSize = sizeof(previousName);
	RegGetValueW(schemesKey, nullptr, L"SelectedScheme", RRF_RT_REG_SZ,
		nullptr, previousName, &previousNameSize);

	HKEY previousSchemeKey = nullptr;
	if (!previousName[0] || RegOpenKeyExW(schemesKey, previousName, 0,
		KEY_READ, &previousSchemeKey) != ERROR_SUCCESS)
		RegOpenKeyExW(schemesKey, L"Windows Standard", 0, KEY_READ, &previousSchemeKey);

	HKEY selectedSchemeKey = nullptr;
	status = RegCreateKeyExW(schemesKey, scheme->name, 0, nullptr, 0,
		KEY_SET_VALUE, nullptr, &selectedSchemeKey, nullptr);
	if (status == ERROR_SUCCESS)
	{
		for (int i = 0; i < MAX_COLORS; ++i)
		{
			WCHAR valueName[16] = {};
			StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Color%d", i);
			DWORD color = scheme->rgb[i];
			status = RegSetValueExW(selectedSchemeKey, valueName, 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&color), sizeof(color));
			if (status != ERROR_SUCCESS) break;
		}

		// Preserve the two extended system-color entries, which aren't present
		// in the legacy Appearance\Schemes binary structure.
		for (int i = MAX_COLORS; status == ERROR_SUCCESS && i < 31; ++i)
		{
			WCHAR valueName[16] = {};
			StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Color%d", i);
			DWORD color = GetSysColor(i);
			DWORD colorSize = sizeof(color);
			if (previousSchemeKey)
				RegGetValueW(previousSchemeKey, nullptr, valueName, RRF_RT_REG_DWORD,
					nullptr, &color, &colorSize);
			status = RegSetValueExW(selectedSchemeKey, valueName, 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&color), sizeof(color));
		}
		RegCloseKey(selectedSchemeKey);
	}
	if (previousSchemeKey) RegCloseKey(previousSchemeKey);

	if (status == ERROR_SUCCESS)
	{
		status = RegSetValueExW(schemesKey, L"SelectedScheme", 0, REG_SZ,
			reinterpret_cast<const BYTE*>(scheme->name),
			static_cast<DWORD>((lstrlenW(scheme->name) + 1) * sizeof(WCHAR)));
	}
	RegCloseKey(schemesKey);
	return status;
}

static bool IsClassicSchemeCurrentlyApplied(const SCHEMEDATA* scheme)
{
	if (!scheme || !scheme->name[0]) return false;

	WCHAR selectedName[ARRAYSIZE(scheme->name)] = {};
	DWORD selectedNameSize = sizeof(selectedName);
	if (RegGetValueW(HKEY_CURRENT_USER,
		L"Control Panel\\Appearance\\ClassicSchemes", L"SelectedScheme",
		RRF_RT_REG_SZ, nullptr, selectedName, &selectedNameSize) != ERROR_SUCCESS ||
		StrCmpI(selectedName, scheme->name) != 0)
		return false;

	for (int color = 0; color < MAX_COLORS; ++color)
	{
		if ((GetSysColor(color) & 0x00FFFFFF) !=
			(scheme->rgb[color] & 0x00FFFFFF))
			return false;
	}
	return true;
}

static bool ApplyClassicSchemePalette(HWND hwnd, HWND colorCombo,
	const SCHEMEDATA* scheme, bool showErrors)
{
	if (!scheme) return false;

	LSTATUS colorSaveStatus = PersistClassicSchemeColors(scheme);
	if (colorSaveStatus != ERROR_SUCCESS && showErrors)
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_COLOR_SCHEME_SAVE_ERROR).c_str(), colorSaveStatus);
		::MessageBoxW(hwnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
	}

	LSTATUS schemeSaveStatus = colorSaveStatus == ERROR_SUCCESS
		? PersistClassicSchemeSelection(scheme) : colorSaveStatus;
	if (schemeSaveStatus != ERROR_SUCCESS && showErrors)
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_COLOR_SCHEME_RETAIN_ERROR).c_str(), schemeSaveStatus);
		::MessageBoxW(hwnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
	}

	int elements[MAX_COLORS] = {};
	for (int color = 0; color < MAX_COLORS; ++color)
		elements[color] = color;
	const bool colorsApplied = SetSysColors(MAX_COLORS, elements, scheme->rgb) != FALSE;
	LogClassicSchemeApply(L"after SetSysColors", colorCombo, scheme);
	return colorSaveStatus == ERROR_SUCCESS && schemeSaveStatus == ERROR_SUCCESS && colorsApplied;
}

static bool ApplyMsstyleColorOverrides(const SCHEMEDATA* baseline,
	const SCHEMEDATA* editedScheme)
{
	if (!baseline || !editedScheme) return false;

	int elements[MAX_COLORS] = {};
	COLORREF colors[MAX_COLORS] = {};
	int count = 0;
	for (int i = 0; i < MAX_COLORS; ++i)
	{
		if ((baseline->rgb[i] & 0x00FFFFFF) == (editedScheme->rgb[i] & 0x00FFFFFF))
			continue;
		elements[count] = i;
		colors[count] = editedScheme->rgb[i];
		++count;
	}
	return count == 0 || SetSysColors(count, elements, colors) != FALSE;
}

BOOL CAppearanceDlgProc::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	hPreviewWnd = GetDlgItem(1110);
	hThemesCombobox = GetDlgItem(1111);
	hColorCombobox = GetDlgItem(1114);
	hSizeCombobox = GetDlgItem(1116);
	size = GetClientSIZE(hPreviewWnd);

	if (!selectedTheme->fMsstyleChanged)
		RefreshActiveThemeStylePath();
	if (selectedTheme->szMsstylePath != APPEARANCE_CLASSIC_STYLE)
	{
		HANDLE activeTheme = LoadCurrentMsstyleVariant(selectedTheme->szMsstylePath.c_str());
		if (activeTheme)
		{
			CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), activeTheme);
			CleanupThemeFile(&activeTheme);
		}
		else
			selectedTheme->szMsstylePath = APPEARANCE_CLASSIC_STYLE;
	}
	classicStyleWasActive = selectedTheme->szMsstylePath == APPEARANCE_CLASSIC_STYLE;

	if (selectedTheme->newColor == NULL)
	{
		selectedTheme->useDesktopColor = true;
		selectedTheme->newColor = 0xB0000000;
	}
	currentRegistryScheme = LoadAppliedClassicScheme(classicStyleWasActive);

	WCHAR msstyledir[MAX_PATH];
	ExpandEnvironmentStrings(L"%windir%\\Resources\\Themes", msstyledir, MAX_PATH);
	LPCWSTR extensions[] = { L".msstyles" };
	EnumDir(msstyledir, extensions, ARRAYSIZE(extensions), msstyle, TRUE);

	_FilterHiddenThemes();
	if (!selectedTheme->szMsstylePath.empty() &&
		selectedTheme->szMsstylePath != APPEARANCE_CLASSIC_STYLE)
	{
		WCHAR resolvedActiveStyle[MAX_PATH] = {};
		if (ResolveMsstylePath(selectedTheme->szMsstylePath.c_str(), resolvedActiveStyle) &&
			StrCmpIW(PathFindExtensionW(resolvedActiveStyle), L".msstyles") == 0)
		{
			bool alreadyListed = false;
			for (LPWSTR stylePath : msstyle)
				if (MsstylePathsMatch(stylePath, resolvedActiveStyle))
				{
					alreadyListed = true;
					break;
				}
			if (!alreadyListed)
				msstyle.push_back(_wcsdup(resolvedActiveStyle));
			selectedTheme->szMsstylePath = resolvedActiveStyle;
		}
	}

	HKEY key;
	RegOpenKeyEx(HKEY_CURRENT_USER, L"Control Panel\\Appearance\\Schemes", 0, KEY_READ, &key);
	if (!key) return FALSE;

	RegQueryInfoKey(key, 0, 0, 0, 0, 0, 0, &mapSize, 0, 0, 0, 0);
	schemeMap = (SCHEMEDATA*)malloc(mapSize * sizeof(SCHEMEDATA));

	LSTATUS staus = ERROR_SUCCESS;
	for (DWORD i = 0; i <= mapSize; ++i)
	{
		if (staus != ERROR_SUCCESS) break;

		WCHAR value[256];
		DWORD dwType;
		DWORD dwSize = ARRAYSIZE(value);
		staus = RegEnumValue(key, i, value, &dwSize, 0, &dwType, NULL, NULL);
		if (dwType == REG_BINARY)
		{
			FillSchemeDataMap(value, i);
		}
	}
	RegCloseKey(key);

	int index = ComboBox_AddString(hThemesCombobox,
		LoadDeskString(IDS_WINDOWS_XP_STYLE).c_str());
	ComboBox_SetItemData(hThemesCombobox, index, (LPARAM)APPEARANCE_XP_STYLE);
	index = ComboBox_AddString(hThemesCombobox,
		LoadDeskString(IDS_WINDOWS_CLASSIC_STYLE).c_str());
	ComboBox_SetItemData(hThemesCombobox, index, (LPARAM)APPEARANCE_CLASSIC_STYLE);
	int selindex = _FindCurrentIndex();
	if (selindex != -1)
	{
		ComboBox_SetCurSel(hThemesCombobox, selindex);
		LPWSTR styleKind = (LPWSTR)ComboBox_GetItemData(hThemesCombobox, selindex);
		_UpdateColorBox(styleKind);
		_UpdateFontBox(StrCmpI(styleKind, APPEARANCE_CLASSIC_STYLE) == 0
			? (LPWSTR)APPEARANCE_CLASSIC_STYLE : (LPWSTR)selectedTheme->szMsstylePath.c_str());
	}

	HBITMAP ebmp;
	pWndPreview = Make<CWindowPreview>(size, wnd, (int)ARRAYSIZE(wnd), PAGETYPE::PT_APPEARANCE, nullptr, GetDpiForWindow(m_hWnd));
	pWndPreview->GetPreviewImage(&ebmp);
	SetBitmap(hPreviewWnd, ebmp);

	return 0;
}

BOOL CAppearanceDlgProc::OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	::KillTimer(m_hWnd, CLASSIC_PALETTE_REFRESH_TIMER);
	classicPaletteRefreshPending = false;
	pendingClassicPaletteValid = false;
	pWndPreview = nullptr;
	currentRegistryScheme = nullptr;
	return 0;
}

LRESULT CAppearanceDlgProc::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& bHandled)
{
	if (wParam != CLASSIC_PALETTE_REFRESH_TIMER)
	{
		bHandled = FALSE;
		return 0;
	}

	const int themeIndex = ComboBox_GetCurSel(hThemesCombobox);
	LPWSTR style = themeIndex == CB_ERR ? nullptr :
		(LPWSTR)ComboBox_GetItemData(hThemesCombobox, themeIndex);
	SCHEMEDATA* scheme = nullptr;
	if (pendingClassicPaletteValid)
		scheme = &pendingClassicPalette;
	else
	{
		const int sizeIndex = ComboBox_GetCurSel(hSizeCombobox);
		scheme = sizeIndex == CB_ERR ? selectedTheme->selectedScheme :
			(SCHEMEDATA*)ComboBox_GetItemData(hSizeCombobox, sizeIndex);
	}
	if (!classicPaletteRefreshPending || !style ||
		StrCmpI(style, APPEARANCE_CLASSIC_STYLE) != 0 || !scheme)
	{
		::KillTimer(m_hWnd, CLASSIC_PALETTE_REFRESH_TIMER);
		classicPaletteRefreshPending = false;
		pendingClassicPaletteValid = false;
		return 0;
	}

	if (!IsClassicSchemeCurrentlyApplied(scheme))
		ApplyClassicSchemePalette(m_hWnd, hColorCombobox, scheme, false);
	ApplySchemeMetrics(scheme, GetDpiForWindow(m_hWnd));

	if (++classicPaletteRefreshAttempts >= 4)
	{
		::KillTimer(m_hWnd, CLASSIC_PALETTE_REFRESH_TIMER);
		classicPaletteRefreshPending = false;
		pendingClassicPaletteValid = false;
	}
	else
	{
		::SetTimer(m_hWnd, CLASSIC_PALETTE_REFRESH_TIMER, 500, nullptr);
	}
	return 0;
}

BOOL CAppearanceDlgProc::OnAdvanced(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	static COLORREF lastUsedClr = 0xB0000000;

	SCHEMEDATA* previousScheme = selectedTheme->selectedScheme;
	const bool keepExistingBaseline = advancedAppearancePending && advancedSchemeBaselineValid;
	SCHEMEDATA baseline = {};
	if (previousScheme)
		baseline = *previousScheme;
	else
		for (int i = 0; i < MAX_COLORS; ++i)
			baseline.rgb[i] = GetSysColor(i);
	const COLORREF previousNewColor = selectedTheme->newColor;
	const bool previousCustomDesktopColorPending = selectedTheme->fCustomDesktopColorPending;
	selectedTheme->newColor = lastUsedClr;
	if (themeSelected) selectedTheme->newColor = NcGetSysColor(COLOR_BACKGROUND);

	CAppearanceDlgBox dlg;
	int ret = (int)dlg.DoModal();
	SCHEMEDATA* editedScheme = selectedTheme->selectedScheme;

	void* theme = LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str());

	// CAppearanceDlgBox returns 0 for OK and 1 for Cancel.
	if (ret == 0)
	{
		if (!keepExistingBaseline)
		{
			advancedSchemeBaseline = baseline;
			advancedSchemeBaselineValid = true;
		}
		if (previousScheme && previousScheme != editedScheme &&
			previousScheme->variant == CUSTOM_SCHEME)
			free(previousScheme);

		int i = ComboBox_GetCurSel(hThemesCombobox);
		LPWSTR style = i == CB_ERR ? nullptr : (LPWSTR)ComboBox_GetItemData(hThemesCombobox, i);
		if (editedScheme && editedScheme->variant == CUSTOM_SCHEME)
		{
			static constexpr WCHAR modifiedSuffix[] = L"";
			if (modifiedSuffix[0] && !StrStrIW(editedScheme->name, modifiedSuffix))
			{
				WCHAR baseName[ARRAYSIZE(editedScheme->name)] = {};
				StringCchCopyW(baseName, ARRAYSIZE(baseName),
					editedScheme->name[0] ? editedScheme->name : L"Custom Classic");
				const size_t suffixLength = ARRAYSIZE(modifiedSuffix) - 1;
				const size_t baseLength = wcslen(baseName);
				if (baseLength + suffixLength >= ARRAYSIZE(editedScheme->name))
					baseName[ARRAYSIZE(baseName) - suffixLength - 1] = L'\0';
				StringCchPrintfW(editedScheme->name, ARRAYSIZE(editedScheme->name),
					L"%s%s", baseName, modifiedSuffix);
			}
			// Keep the edited copy attached to the currently selected metric size;
			// OnApply consumes this item data rather than the preview-only scheme.
			if (style && StrCmpI(style, APPEARANCE_CLASSIC_STYLE) == 0)
			{
				ComboBox_ResetContent(hSizeCombobox);
				int customSize = ComboBox_AddString(hSizeCombobox,
					LoadDeskString(IDS_NORMAL_SIZE).c_str());
				ComboBox_SetItemData(hSizeCombobox, customSize, editedScheme);
				ComboBox_SetCurSel(hSizeCombobox, customSize);
			}
		}
		advancedAppearancePending = true;
		appearanceApplyPending = true;
		SetModified(TRUE);
		lastUsedClr = NcGetSysColor(COLOR_BACKGROUND);
	}
	else
	{
		if (editedScheme != previousScheme && editedScheme &&
			editedScheme->variant == CUSTOM_SCHEME)
			free(editedScheme);
		selectedTheme->selectedScheme = previousScheme;
		selectedTheme->newColor = previousNewColor;
		selectedTheme->fCustomDesktopColorPending = previousCustomDesktopColorPending;
		if (!keepExistingBaseline)
			advancedSchemeBaselineValid = false;
		lastUsedClr = NcGetSysColor(COLOR_BACKGROUND);
	}

	HBITMAP ebmp;
	pWndPreview->GetUpdatedPreviewImage(wnd, theme, &ebmp, UPDATE_SOLIDCLR | UPDATE_WINDOW);
	SetBitmap(hPreviewWnd, ebmp);
	return 0;
}

BOOL CAppearanceDlgProc::OnEffects(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	CEffectsDlg dlg;
	dlg.DoModal();
	return 0;
}

BOOL CAppearanceDlgProc::OnComboboxChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	advancedAppearancePending = false;
	advancedSchemeBaselineValid = false;
	themeSelected = TRUE;

	int i = ComboBox_GetCurSel(hThemesCombobox);
	LPWSTR styleKind = (LPWSTR)ComboBox_GetItemData(hThemesCombobox, i);
	if (StrCmpI(styleKind, APPEARANCE_XP_STYLE) == 0)
	{
		if (StrCmpI(selectedTheme->szMsstylePath.c_str(), APPEARANCE_CLASSIC_STYLE) == 0)
		{
			LPWSTR preferredStyle = nullptr;
			for (LPWSTR style : msstyle)
			{
				if (!preferredStyle) preferredStyle = style;
				if (StrCmpI(PathFindFileNameW(style), L"Luna.msstyles") == 0)
				{
					preferredStyle = style;
					break;
				}
			}
			if (preferredStyle) selectedTheme->szMsstylePath = preferredStyle;
		}
		if (selectedTheme->selectedScheme && selectedTheme->selectedScheme != currentRegistryScheme &&
			selectedTheme->selectedScheme->variant == CUSTOM_SCHEME)
			free(selectedTheme->selectedScheme);
		selectedTheme->selectedScheme = nullptr;
		_UpdateColorBox(styleKind);
		_UpdateFontBox((LPWSTR)selectedTheme->szMsstylePath.c_str());
	}
	else
	{
		selectedTheme->szMsstylePath = APPEARANCE_CLASSIC_STYLE;
		_UpdateColorBox(styleKind);
		_UpdateFontBox(styleKind);
	}
	selectedTheme->fMsstyleChanged = true;

	void* theme = LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str());

	if (StrCmpI(styleKind, APPEARANCE_CLASSIC_STYLE) != 0)
	{
		CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), theme);
		selectedTheme->newColor = NcGetSysColor(COLOR_BACKGROUND);
	}

	HBITMAP ebmp;
	pWndPreview->GetUpdatedPreviewImage(wnd, theme, &ebmp, UPDATE_SOLIDCLR | UPDATE_WINDOW);
	SetBitmap(hPreviewWnd, ebmp);

	SetModified(TRUE);
	appearanceApplyPending = true;
	return 0;
}

BOOL CAppearanceDlgProc::OnClrComboboxChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	advancedAppearancePending = false;
	advancedSchemeBaselineValid = false;
	int i = ComboBox_GetCurSel(hThemesCombobox);
	LPWSTR styleKind = (LPWSTR)ComboBox_GetItemData(hThemesCombobox, i);
	if (StrCmpI(styleKind, APPEARANCE_XP_STYLE) == 0)
	{
		int schemeIndex = ComboBox_GetCurSel(hColorCombobox);
		LPWSTR style = schemeIndex < 0 ? nullptr :
			(LPWSTR)ComboBox_GetItemData(hColorCombobox, schemeIndex);
		if (style)
		{
			selectedTheme->szMsstylePath = style;
			if (selectedTheme->selectedScheme && selectedTheme->selectedScheme != currentRegistryScheme &&
				selectedTheme->selectedScheme->variant == CUSTOM_SCHEME)
				free(selectedTheme->selectedScheme);
			selectedTheme->selectedScheme = nullptr;
			_UpdateFontBox(style);
			void* theme = LoadThemeFromFilePath(style);
			CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), theme);
			selectedTheme->newColor = NcGetSysColor(COLOR_BACKGROUND);
			selectedTheme->fMsstyleChanged = true;
			themeSelected = TRUE;
		}
	}
	else
	{
		if (selectedTheme->selectedScheme &&
			selectedTheme->selectedScheme != currentRegistryScheme &&
			(selectedTheme->selectedScheme->variant & CUSTOM_SCHEME))
		{
			free(selectedTheme->selectedScheme);
			selectedTheme->selectedScheme = nullptr;
		}
		// The active derivative may temporarily occupy the named scheme's row so
		// it can be edited. An explicit selection of that name means restore the
		// registered preset data, not keep reusing the derivative object.
		int colorIndex = ComboBox_GetCurSel(hColorCombobox);
		if (colorIndex != CB_ERR && schemeMap)
		{
			WCHAR selectedName[256] = {};
			if (ComboBox_GetLBText(hColorCombobox, colorIndex, selectedName) != CB_ERR)
			{
				SCHEMEDATA* preset = nullptr;
				for (ULONG candidate = 0; candidate < mapSize; ++candidate)
				{
					if (StrCmpI(schemeMap[candidate].name, selectedName) != 0) continue;
					if (!preset || (schemeMap[candidate].variant & HAS_NORMAL))
						preset = &schemeMap[candidate];
					if (schemeMap[candidate].variant & HAS_NORMAL) break;
				}
				if (preset)
					ComboBox_SetItemData(hColorCombobox, colorIndex, preset);
			}
		}
		_UpdateFontBox(styleKind);
	}
	LogClassicSchemeApply(L"scheme selection", hColorCombobox, selectedTheme->selectedScheme);

	HBITMAP ebmp;
	pWndPreview->GetUpdatedPreviewImage(wnd, LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str()), &ebmp, UPDATE_SOLIDCLR | UPDATE_WINDOW);
	SetBitmap(hPreviewWnd, ebmp);

	SetModified(TRUE);
	appearanceApplyPending = true;
	return 0;
}

BOOL CAppearanceDlgProc::OnFontComboboxChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	advancedAppearancePending = false;
	int i = ComboBox_GetCurSel(hSizeCombobox);
	SCHEMEDATA* data = (SCHEMEDATA*)ComboBox_GetItemData(hSizeCombobox, i);

	if (selectedTheme->selectedScheme &&
		selectedTheme->selectedScheme != currentRegistryScheme &&
		selectedTheme->selectedScheme != data)
	{
		if (selectedTheme->selectedScheme->variant & CUSTOM_SCHEME)
		{
			free(selectedTheme->selectedScheme);
			selectedTheme->selectedScheme = NULL;
		}
	}
	selectedTheme->selectedScheme = data;
	selectedTheme->newColor = NcGetSysColor(COLOR_BACKGROUND);

	HBITMAP ebmp;
	pWndPreview->GetUpdatedPreviewImage(wnd, LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str()), &ebmp, UPDATE_SOLIDCLR | UPDATE_WINDOW);
	SetBitmap(hPreviewWnd, ebmp);

	SetModified(TRUE);
	appearanceApplyPending = true;
	return 0;
}

BOOL CAppearanceDlgProc::OnSetActive()
{
	_TerminateProcess(pi);
	// Once the Themes page has applied its selection, re-read the live theme
	// rather than trusting its cached path. Preserve staged theme or Appearance
	// changes until they are applied.
	if (!selectedTheme->fMsstyleChanged && !appearanceApplyPending &&
		RefreshActiveThemeStylePath())
	{
		if (selectedTheme->szMsstylePath != APPEARANCE_CLASSIC_STYLE)
		{
			HANDLE liveTheme = LoadCurrentMsstyleVariant(selectedTheme->szMsstylePath.c_str());
			if (liveTheme)
			{
				CreateThemedMetricsScheme(GetDpiForWindow(m_hWnd), liveTheme);
				CleanupThemeFile(&liveTheme);
			}
		}
		selectedTheme->fMsstyleChanged = true;
	}
	if (!appearanceApplyPending)
		currentRegistryScheme = LoadAppliedClassicScheme(classicStyleWasActive);

	UINT flags = UPDATE_NONE;
	if (selectedTheme->newColor != 0xB0000000) flags |= UPDATE_SOLIDCLR;

	if (selectedTheme->fMsstyleChanged)
	{
		flags |= UPDATE_WINDOW;
		for (int i = 0; i < ComboBox_GetCount(hThemesCombobox); ++i)
		{
			LPWSTR data = (LPWSTR)ComboBox_GetItemData(hThemesCombobox, i);
			const bool classic = StrCmpI(selectedTheme->szMsstylePath.c_str(),
				APPEARANCE_CLASSIC_STYLE) == 0;
			if ((classic && StrCmpI(data, APPEARANCE_CLASSIC_STYLE) == 0) ||
				(!classic && StrCmpI(data, APPEARANCE_XP_STYLE) == 0))
			{
				ComboBox_SetCurSel(hThemesCombobox, i);
				_UpdateColorBox(data);
				_UpdateFontBox(classic ? (LPWSTR)APPEARANCE_CLASSIC_STYLE :
					(LPWSTR)selectedTheme->szMsstylePath.c_str());
				classicStyleWasActive = classic;
				break;
			}
		}
		selectedTheme->fMsstyleChanged = false;
	}
	if (flags != UPDATE_NONE)
	{
		HBITMAP ebmp;
		pWndPreview->GetUpdatedPreviewImage(wnd, LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str()), &ebmp, flags);
		SetBitmap(hPreviewWnd, ebmp);
	}

	return 0;
}

BOOL CAppearanceDlgProc::OnApply()
{
	// Property sheets can notify every page on Apply. Do not re-apply the
	// visual style when another page (for example, Background) is the only
	// page with pending changes.
	if (!appearanceApplyPending)
		return 0;
	::KillTimer(m_hWnd, CLASSIC_PALETTE_REFRESH_TIMER);
	classicPaletteRefreshPending = false;
	pendingClassicPaletteValid = false;

	int i = ComboBox_GetCurSel(hThemesCombobox);
	LPWSTR data = (LPWSTR)ComboBox_GetItemData(hThemesCombobox, i);
	const bool applyXpStyle = StrCmpI(data, APPEARANCE_CLASSIC_STYLE) != 0;
	const bool classicToClassic = !applyXpStyle && classicStyleWasActive;
	const bool switchingFromMsstyleToClassic = !applyXpStyle && !classicStyleWasActive;
	SCHEMEDATA classicSchemeCopy = {};
	const SCHEMEDATA* classicScheme = nullptr;
	if (advancedAppearancePending && selectedTheme->selectedScheme)
	{
		classicSchemeCopy = *selectedTheme->selectedScheme;
		classicScheme = &classicSchemeCopy;
	}
	else if (!applyXpStyle)
	{
		int sizeIndex = ComboBox_GetCurSel(hSizeCombobox);
		SCHEMEDATA* selectedSizeScheme = sizeIndex == CB_ERR ? nullptr :
			(SCHEMEDATA*)ComboBox_GetItemData(hSizeCombobox, sizeIndex);
		if (!selectedSizeScheme) selectedSizeScheme = selectedTheme->selectedScheme;
		if (selectedSizeScheme)
		{
			classicSchemeCopy = *selectedSizeScheme;
			classicScheme = &classicSchemeCopy;
		}
	}
	if (classicScheme && !classicSchemeCopy.name[0])
	{
		const int colorIndex = ComboBox_GetCurSel(hColorCombobox);
		if (colorIndex != CB_ERR)
			ComboBox_GetLBText(hColorCombobox, colorIndex, classicSchemeCopy.name);
		if (!classicSchemeCopy.name[0])
			StringCchCopyW(classicSchemeCopy.name, ARRAYSIZE(classicSchemeCopy.name),
				LoadDeskString(IDS_CUSTOM_CLASSIC).c_str());
	}
	LogClassicSchemeApply(L"before Theme Manager", hColorCombobox, classicScheme);
	HRESULT themeApplyResult = S_OK;
	if (applyXpStyle)
	{
		themeApplyResult = ApplyMsstyleWithoutChangingWallpaper(m_hWnd,
			selectedTheme->szMsstylePath.c_str());
	}
	else if (!classicToClassic)
	{
		// Keep the normal Theme Manager transition when entering Classic from
		// an msstyle; only Classic-to-Classic edits bypass that theme apply.
		themeApplyResult = ApplyClassicThemeWithoutChangingWallpaper(m_hWnd);
	}
	if (FAILED(themeApplyResult))
	{
		WCHAR message[256];
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_VISUAL_STYLE_APPLY_ERROR).c_str(), themeApplyResult);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		SetModified(TRUE);
		return PSNRET_INVALID_NOCHANGEPAGE;
	}
	classicStyleWasActive = !applyXpStyle;
	// Theme Manager applies the XP visual style. Apply registry-backed Classic
	// palette and metrics only when Classic is selected.
	if (classicScheme && (!applyXpStyle || advancedAppearancePending))
	{ 
		ApplySchemeMetrics(classicScheme, GetDpiForWindow(m_hWnd));
		// Keep named Classic palettes in their own registry-backed scheme. For
		// msstyles, classicScheme also carries the style's metrics, but applying
		// its full system-color array would replace the selected msstyles palette.
		if (!applyXpStyle)
			ApplyClassicSchemePalette(m_hWnd, hColorCombobox, classicScheme, true);

	}
	if (switchingFromMsstyleToClassic && classicScheme)
	{
		pendingClassicPalette = *classicScheme;
		pendingClassicPaletteValid = true;
		classicPaletteRefreshPending = true;
		classicPaletteRefreshAttempts = 0;
		::SetTimer(m_hWnd, CLASSIC_PALETTE_REFRESH_TIMER, 500, nullptr);
	}
	// A Classic-to-Classic Apply must not refresh Theme Manager: that refresh
	// reselects its cached scheme and resets the preview. Still ask it to snapshot
	// the working theme file, then persist the applied Classic palette/metrics.
	if (classicToClassic && classicScheme)
	{
		if (pThemeManager)
		{
			HRESULT themeUpdate = pThemeManager->UpdateCustomTheme();
			if (FAILED(themeUpdate))
			{
				WCHAR message[256] = {};
				StringCchPrintfW(message, ARRAYSIZE(message),
					LoadDeskString(IDS_CLASSIC_DERIVATIVE_SYNC_ERROR).c_str(),
					themeUpdate);
				::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONWARNING);
				SetModified(TRUE);
				return PSNRET_INVALID_NOCHANGEPAGE;
			}
		}
		WCHAR customThemePath[MAX_PATH] = {};
		if (!ExpandEnvironmentStringsW(
			L"%LOCALAPPDATA%\\Microsoft\\Windows\\Themes\\Custom.theme",
			customThemePath, ARRAYSIZE(customThemePath)) ||
			!PathFileExistsW(customThemePath))
		{
			::MessageBoxW(m_hWnd,
				LoadDeskString(IDS_CLASSIC_DERIVATIVE_MISSING).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONWARNING);
			SetModified(TRUE);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
		LSTATUS saveStatus = WriteClassicSchemeToThemeFile(customThemePath, classicScheme);
		if (saveStatus != ERROR_SUCCESS)
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_CLASSIC_DERIVATIVE_SAVE_ERROR).c_str(),
				saveStatus);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONWARNING);
			SetModified(TRUE);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
	}
	// Visual-style transitions still synchronize and refresh through Theme Manager.
	else if (pThemeManager)
	{
		HRESULT themeUpdate = pThemeManager->UpdateCustomTheme();
		if (FAILED(themeUpdate))
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_VISUAL_DERIVATIVE_SYNC_ERROR).c_str(),
				themeUpdate);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONWARNING);
			SetModified(TRUE);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
		pThemeManager->Refresh();
		ForgetSavedThemePathForCurrentTheme();
	}
	if (!classicToClassic && !applyXpStyle && classicScheme)
	{
		WCHAR customThemePath[MAX_PATH] = {};
		if (ExpandEnvironmentStringsW(
			L"%LOCALAPPDATA%\\Microsoft\\Windows\\Themes\\Custom.theme",
			customThemePath, ARRAYSIZE(customThemePath)) &&
			PathFileExistsW(customThemePath))
		{
			LSTATUS saveStatus = WriteClassicSchemeToThemeFile(customThemePath,
				classicScheme);
			if (saveStatus != ERROR_SUCCESS)
			{
				WCHAR message[256] = {};
				StringCchPrintfW(message, ARRAYSIZE(message),
					LoadDeskString(IDS_CLASSIC_DERIVATIVE_SAVE_ERROR).c_str(),
					saveStatus);
				::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(),
					MB_OK | MB_ICONWARNING);
			}
		}
	}
	// Switching from an msstyle applies the Classic theme through Theme Manager,
	// whose subsequent refresh can restore its cached non-client metrics. Reapply
	// the selected Classic metrics after all Theme Manager work is finished so
	// the first Apply leaves the chosen values active (Classic-to-Classic skips
	// that refresh, which is why a second Apply previously appeared to fix it).
	if (!applyXpStyle && classicScheme)
		ApplySchemeMetrics(classicScheme, GetDpiForWindow(m_hWnd));
	else if (applyXpStyle && selectedTheme->selectedScheme)
		ApplySchemeMetrics(selectedTheme->selectedScheme, GetDpiForWindow(m_hWnd));

	// Refresh only the applet's local Classic derivative cache. Theme Manager was
	// deliberately not refreshed on Classic-to-Classic Apply.
	if (classicToClassic && classicScheme)
	{
		currentRegistryScheme = LoadCurrentClassicSchemeFromThemeDerivative();
		if (!currentRegistryScheme)
			currentRegistryScheme = LoadCurrentClassicSchemeFromRegistry();
		if (currentRegistryScheme)
		{
			const int colorIndex = ComboBox_GetCurSel(hColorCombobox);
			if (colorIndex != CB_ERR)
				ComboBox_SetItemData(hColorCombobox, colorIndex, currentRegistryScheme);
		}
		LogClassicSchemeApply(L"after Theme Manager refresh", hColorCombobox,
			classicScheme);
		HBITMAP previewBitmap;
		pWndPreview->GetUpdatedPreviewImage(wnd,
			LoadThemeFromFilePath(selectedTheme->szMsstylePath.c_str()),
			&previewBitmap, UPDATE_SOLIDCLR | UPDATE_WINDOW);
		SetBitmap(hPreviewWnd, previewBitmap);
	}
	if (applyXpStyle && advancedAppearancePending && advancedSchemeBaselineValid && classicScheme)
		ApplyMsstyleColorOverrides(&advancedSchemeBaseline, classicScheme);
	selectedTheme->fThemePgMsstyleUpdate = true;
	advancedAppearancePending = false;
	advancedSchemeBaselineValid = false;
	appearanceApplyPending = false;
	selectedTheme->fCustomDesktopColorPending = false;
	SetModified(FALSE);
	return 0;
}

void CAppearanceDlgProc::_UpdateColorBox(LPWSTR data)
{
	ComboBox_ResetContent(hColorCombobox);
	if (StrCmpI(data, APPEARANCE_XP_STYLE) == 0)
	{
		LPCWSTR preferredStylePath = selectedTheme->szMsstylePath.c_str();
		WCHAR resolvedLiveStylePath[MAX_PATH] = {};
		WCHAR liveStylePath[MAX_PATH] = {};
		WCHAR liveColorName[MAX_PATH] = {};
		WCHAR liveSizeName[MAX_PATH] = {};
		if (!appearanceApplyPending && !selectedTheme->fMsstyleChanged &&
			SUCCEEDED(GetCurrentThemeName(liveStylePath, ARRAYSIZE(liveStylePath),
				liveColorName, ARRAYSIZE(liveColorName),
				liveSizeName, ARRAYSIZE(liveSizeName))) &&
			ResolveMsstylePath(liveStylePath, resolvedLiveStylePath) &&
			StrCmpIW(PathFindExtensionW(resolvedLiveStylePath), L".msstyles") == 0)
		{
			preferredStylePath = resolvedLiveStylePath;
			selectedTheme->szMsstylePath = resolvedLiveStylePath;
		}
		for (LPWSTR style : msstyle)
		{
			WCHAR name[MAX_PATH] = {};
			HMODULE hStyle = LoadLibraryExW(style, nullptr,
				LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (hStyle)
			{
				LoadStringW(hStyle, 101, name, ARRAYSIZE(name));
				FreeLibrary(hStyle);
			}
			if (!name[0])
			{
				StringCchCopyW(name, ARRAYSIZE(name), PathFindFileNameW(style));
				PathRemoveExtensionW(name);
			}
			if (LPCWSTR builtInName = GetBuiltInMsstyleDisplayName(style))
				StringCchCopyW(name, ARRAYSIZE(name), builtInName);
			int item = ComboBox_AddString(hColorCombobox, name);
			ComboBox_SetItemData(hColorCombobox, item, (LPARAM)style);
		}
		// This combo uses CBS_SORT, so adding a later item can shift the index
		// returned for an earlier match (e.g. Default [blue] sorts ahead of
		// Embedded Style). Resolve the preferred path against the final list.
		for (int item = 0; item < ComboBox_GetCount(hColorCombobox); ++item)
		{
			LRESULT itemData = ComboBox_GetItemData(hColorCombobox, item);
			if (itemData == CB_ERR) continue;
			LPWSTR itemStylePath = (LPWSTR)itemData;
			if (MsstylePathsMatch(itemStylePath, preferredStylePath))
			{
				ComboBox_SetCurSel(hColorCombobox, item);
				break;
			}
		}
		return;
	}
	if (StrCmpI(data, APPEARANCE_CLASSIC_STYLE) == 0)
	{
		// Theme Manager may finish refreshing the registry-backed palette after
		// this page was initialized. Read it when the Classic list is populated,
		// not from the page's potentially stale cached snapshot.
		currentRegistryScheme = LoadAppliedClassicScheme(classicStyleWasActive);
		for (ULONG j = 0; j < mapSize; ++j)
		{
			int item = ComboBox_AddString(hColorCombobox, schemeMap[j].name);
			schemeMap[j].schemeMapIndex = j;
			ComboBox_SetItemData(hColorCombobox, item, &schemeMap[j]);
		}
		_FixColorBox();
	}

	int index = -1;
	if (StrCmpI(data, APPEARANCE_CLASSIC_STYLE) == 0)
	{
		if (currentRegistryScheme)
		{
			const bool hasParentIdentity = HasClassicParentName(currentRegistryScheme);
			if (hasParentIdentity)
		{
				for (int candidate = 0; candidate < ComboBox_GetCount(hColorCombobox); ++candidate)
			{
					SCHEMEDATA* candidateScheme =
						(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, candidate);
					if (candidateScheme && (candidateScheme->variant & HAS_NORMAL) &&
						StrCmpI(candidateScheme->name, currentRegistryScheme->name) == 0)
					{
						index = candidate;
						break;
					}
				}
				if (index >= 0)
					ComboBox_SetItemData(hColorCombobox, index, currentRegistryScheme);
			}
		}
		if (index < 0 && currentRegistryScheme)
		{
			for (int candidate = 0; candidate < ComboBox_GetCount(hColorCombobox); ++candidate)
			{
				SCHEMEDATA* candidateScheme =
					(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, candidate);
				if (!candidateScheme) continue;
				bool colorsMatch = true;
				for (int color = 0; color < MAX_COLORS; ++color)
				{
					// The registry scheme binary may have non-color bits in the high
					// byte of COLORREF fields. Compare only the actual RGB channels.
					if ((candidateScheme->rgb[color] & 0x00FFFFFF) !=
						(currentRegistryScheme->rgb[color] & 0x00FFFFFF))
					{
						colorsMatch = false;
						break;
					}
				}
				if (!colorsMatch) continue;

				// Scheme variants share colors; prefer the normal-size entry.
				if (index < 0 || (candidateScheme->variant & HAS_NORMAL))
					index = candidate;
				if (candidateScheme->variant & HAS_NORMAL)
					break;
			}
		}
		if (index < 0)
		{
			// A file's DisplayName (for example "altloc") is not a Classic
			// scheme identity. Reuse a real parent row, preferring persisted
			// parent identity and otherwise finding the closest registered palette.
			if (currentRegistryScheme)
			{
				int parentIndex = CB_ERR;
				for (int candidate = 0; candidate < ComboBox_GetCount(hColorCombobox); ++candidate)
				{
					SCHEMEDATA* candidateScheme =
						(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, candidate);
					if (candidateScheme && (candidateScheme->variant & HAS_NORMAL) &&
						currentRegistryScheme->name[0] &&
						StrCmpI(candidateScheme->name, currentRegistryScheme->name) == 0)
					{
						parentIndex = candidate;
						break;
					}
				}

				if (parentIndex == CB_ERR)
				{
					ULONGLONG bestDistance = static_cast<ULONGLONG>(-1);
					for (int candidate = 0; candidate < ComboBox_GetCount(hColorCombobox); ++candidate)
					{
						SCHEMEDATA* candidateScheme =
							(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, candidate);
						if (!candidateScheme || !(candidateScheme->variant & HAS_NORMAL))
							continue;

						ULONGLONG distance = 0;
						for (int color = 0; color < MAX_COLORS; ++color)
						{
							COLORREF first = candidateScheme->rgb[color] & 0x00FFFFFF;
							COLORREF second = currentRegistryScheme->rgb[color] & 0x00FFFFFF;
							int red = static_cast<int>(GetRValue(first)) - GetRValue(second);
							int green = static_cast<int>(GetGValue(first)) - GetGValue(second);
							int blue = static_cast<int>(GetBValue(first)) - GetBValue(second);
							distance += static_cast<ULONGLONG>(red * red + green * green + blue * blue);
						}
						if (distance < bestDistance)
						{
							bestDistance = distance;
							parentIndex = candidate;
						}
					}
				}

				if (parentIndex != CB_ERR)
				{
					SCHEMEDATA* parentScheme =
						(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, parentIndex);
					StringCchCopyW(currentRegistryScheme->name,
						ARRAYSIZE(currentRegistryScheme->name), parentScheme->name);
					currentRegistryScheme->variant |= CUSTOM_SCHEME;
					ComboBox_SetItemData(hColorCombobox, parentIndex, currentRegistryScheme);
					index = parentIndex;
				}
			}
			if (index < 0)
			{
				// For an unrecognized, unedited palette, show a real named preset.
				for (int candidate = 0; candidate < ComboBox_GetCount(hColorCombobox); ++candidate)
				{
					SCHEMEDATA* candidateScheme =
						(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, candidate);
					if (candidateScheme && StrCmpI(candidateScheme->name, L"Windows Standard") == 0)
					{
						index = candidate;
						break;
					}
				}
			}
		}
		if (index != CB_ERR && index >= 0)
			ComboBox_SetCurSel(hColorCombobox, index);

		if (selectedTheme->selectedScheme && selectedTheme->selectedScheme != currentRegistryScheme)
		{
			if (selectedTheme->selectedScheme->variant == CUSTOM_SCHEME)
			{
				free(selectedTheme->selectedScheme);
				selectedTheme->selectedScheme = NULL;
			}
		}
		selectedTheme->selectedScheme = index >= 0
			? (SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, index) : nullptr;
	}
	if (index >= 0) ComboBox_SetCurSel(hColorCombobox, index);
}

void CAppearanceDlgProc::_UpdateFontBox(LPWSTR data)
{
	ComboBox_ResetContent(hSizeCombobox);
	if (lstrcmp(data, L"(classic)") == 0)
	{
		int index = ComboBox_GetCurSel(hColorCombobox);
		SCHEMEDATA* data = (SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, index);
		selectedTheme->selectedScheme = data;
		selectedTheme->newColor = NcGetSysColor(COLOR_BACKGROUND);

		if (data && (data->variant & CUSTOM_SCHEME))
		{
			int i = ComboBox_AddString(hSizeCombobox,
				LoadDeskString(IDS_NORMAL_SIZE).c_str());
			ComboBox_SetItemData(hSizeCombobox, i, data);
		}
		else if (data)
		{
			WCHAR baseName[ARRAYSIZE(data->name)] = {};
			StringCchCopyW(baseName, ARRAYSIZE(baseName), data->name);
			LPCWSTR suffixes[] = { L" (large)", L" (extra large)" };
			for (LPCWSTR suffix : suffixes)
			{
				const size_t nameLength = wcslen(baseName);
				const size_t suffixLength = wcslen(suffix);
				if (nameLength >= suffixLength &&
					StrCmpI(baseName + nameLength - suffixLength, suffix) == 0)
				{
					baseName[nameLength - suffixLength] = L'\0';
					break;
				}
			}

			auto addVariant = [&](LPCWSTR label, LPCWSTR suffix, DWORD flag)
			{
				if (!(data->variant & flag)) return;

				WCHAR variantName[ARRAYSIZE(data->name)] = {};
				if (FAILED(StringCchPrintfW(variantName, ARRAYSIZE(variantName),
					L"%s%s", baseName, suffix)))
					return;

				for (ULONG variantIndex = 0; variantIndex < mapSize; ++variantIndex)
				{
					if (StrCmpI(schemeMap[variantIndex].name, variantName) != 0) continue;

					int item = ComboBox_AddString(hSizeCombobox, label);
					ComboBox_SetItemData(hSizeCombobox, item, &schemeMap[variantIndex]);
					return;
				}
			};

			addVariant(LoadDeskString(IDS_NORMAL_SIZE).c_str(), L"", HAS_NORMAL);
			addVariant(LoadDeskString(IDS_EXTRA_LARGE_SIZE).c_str(), L" (extra large)", HAS_EXTRA_LARGE);
			addVariant(LoadDeskString(IDS_LARGE_SIZE).c_str(), L" (large)", HAS_LARGE);
			if (ComboBox_GetCount(hSizeCombobox) == 0)
			{
				int item = ComboBox_AddString(hSizeCombobox,
					LoadDeskString(IDS_NORMAL_SIZE).c_str());
				ComboBox_SetItemData(hSizeCombobox, item, data);
			}
		}
		else
		{
			ComboBox_AddString(hSizeCombobox, LoadDeskString(IDS_NORMAL_SIZE).c_str());
		}
	}
	else
	{
		HRESULT hr = S_OK;
		_THEMENAMEINFO name;
		WCHAR liveStylePath[MAX_PATH] = {};
		WCHAR liveColorName[MAX_PATH] = {};
		WCHAR liveSizeName[MAX_PATH] = {};
		const bool liveVariantMatches = !appearanceApplyPending &&
			SUCCEEDED(GetCurrentThemeName(liveStylePath, ARRAYSIZE(liveStylePath),
				liveColorName, ARRAYSIZE(liveColorName),
				liveSizeName, ARRAYSIZE(liveSizeName))) &&
			MsstylePathsMatch(data, liveStylePath);
		int liveSizeIndex = CB_ERR;

		for (int i = 0; SUCCEEDED(hr); i++)
		{
			hr = EnumThemeSize(data, NULL, i, &name);
			if (SUCCEEDED(hr))
			{
				int item = ComboBox_AddString(hSizeCombobox, name.szDisplayName);
				if (liveVariantMatches &&
					(StrCmpIW(name.szName, liveSizeName) == 0 ||
					 StrCmpIW(name.szDisplayName, liveSizeName) == 0))
					liveSizeIndex = item;
			}
		}
		ComboBox_SetCurSel(hSizeCombobox,
			liveSizeIndex != CB_ERR ? liveSizeIndex : 0);
	}
	if (lstrcmp(data, L"(classic)") == 0)
	{
		int selectedSize = CB_ERR;
		for (int candidate = 0; candidate < ComboBox_GetCount(hSizeCombobox); ++candidate)
		{
			SCHEMEDATA* candidateScheme =
				(SCHEMEDATA*)ComboBox_GetItemData(hSizeCombobox, candidate);
			if (ClassicMetricsMatch(candidateScheme, currentRegistryScheme))
			{
				selectedSize = candidate;
				break;
			}
		}
		// XP exposes only the named metric sizes here. If the active derivative's
		// metrics don't exactly match one, show the normal-size choice instead
		// of adding a synthetic "Custom" entry.
		if (selectedSize == CB_ERR && ComboBox_GetCount(hSizeCombobox) > 0)
			selectedSize = 0;
		if (selectedSize != CB_ERR)
			ComboBox_SetCurSel(hSizeCombobox, selectedSize);
		SCHEMEDATA* selectedSizeScheme = selectedSize == CB_ERR ? nullptr :
			(SCHEMEDATA*)ComboBox_GetItemData(hSizeCombobox, selectedSize);
		bool keepDerivativeMetrics = false;
		if (selectedSizeScheme && currentRegistryScheme &&
			!ClassicMetricsMatch(selectedSizeScheme, currentRegistryScheme))
		{
			const int colorIndex = ComboBox_GetCurSel(hColorCombobox);
			SCHEMEDATA* selectedColorScheme = colorIndex == CB_ERR ? nullptr :
				(SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, colorIndex);
			keepDerivativeMetrics = selectedColorScheme != nullptr;
			for (int color = 0; keepDerivativeMetrics && color < MAX_COLORS; ++color)
				keepDerivativeMetrics =
					(selectedColorScheme->rgb[color] & 0x00FFFFFF) ==
					(currentRegistryScheme->rgb[color] & 0x00FFFFFF);
		}
		// Show only standard sizes but retain the active derivative as the edit
		// baseline while its named palette remains selected. An explicit size
		// selection replaces it in the size-change handler.
		selectedTheme->selectedScheme = keepDerivativeMetrics
			? currentRegistryScheme : selectedSizeScheme;
	}
	else
		ComboBox_SetCurSel(hSizeCombobox, 0);
}

// rewrite this function
void CAppearanceDlgProc::_FixColorBox()
{
	// dont know any better way
	// note: they are in alphabetical order in the combobox
	for (int i = 0; ; ++i)
	{
		if (i >= ComboBox_GetCount(hColorCombobox)) break;

		int size = ComboBox_GetLBTextLen(hColorCombobox, i);
		WCHAR* value = new WCHAR[size + 1];
		ComboBox_GetLBText(hColorCombobox, i, value);

		SCHEMEDATA* data = (SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, i);

		// check variants
		DWORD flags = HAS_NORMAL;
		data->variant = flags;
		LPCWSTR variants[] = { L" (large)", L" (extra large)" };
		for (int j = 0; j < ARRAYSIZE(variants); ++j)
		{
			size_t totalSize = lstrlen(value) + lstrlen(variants[j]) + 1;
			LPWSTR dest = new WCHAR[totalSize];
			dest[0] = L'\0';

			StringCchCat(dest, totalSize, value);
			StringCchCat(dest, totalSize, variants[j]);

			int index = ComboBox_FindString(hColorCombobox, i, dest);
			if (index != CB_ERR)
			{
				ComboBox_DeleteString(hColorCombobox, index);
				if (j == 0) flags = flags | HAS_LARGE;
				if (j == 1) flags = flags | HAS_EXTRA_LARGE;

				// set info
				data->variant = flags;
			}
			delete[] dest;
		}

		// case where normal variant doesnt exist (Pumpkin (large))
		flags = 0;
		for (int j = 0; j < ARRAYSIZE(variants); ++j)
		{
			if (StrStrI(value, variants[j]) != NULL)
			{
				if (j == 0) flags = flags | HAS_LARGE;
				if (j == 1) flags = flags | HAS_EXTRA_LARGE;

				StrTrim(value, variants[j]);

				SCHEMEDATA* data = (SCHEMEDATA*)ComboBox_GetItemData(hColorCombobox, i);
				data->variant = flags;

				ComboBox_DeleteString(hColorCombobox, i);
				int index = ComboBox_AddString(hColorCombobox, value);
				ComboBox_SetItemData(hColorCombobox, index, data);
			}
		}

		delete[] value;
	}
}

int CAppearanceDlgProc::_FindCurrentIndex()
{
	const bool classicSelected = selectedTheme->szMsstylePath == APPEARANCE_CLASSIC_STYLE;
	const LPCWSTR wanted = classicSelected
		? APPEARANCE_CLASSIC_STYLE : APPEARANCE_XP_STYLE;
	for (int i = 0; i < ComboBox_GetCount(hThemesCombobox); ++i)
	{
		LPCWSTR data = (LPCWSTR)ComboBox_GetItemData(hThemesCombobox, i);
		if (StrCmpI(data, wanted) == 0) return i;
	}
	return -1;
}

void CAppearanceDlgProc::_FilterHiddenThemes()
{
	DWORD cbData = 0;
	DWORD type = 0;

	LONG status = RegGetValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cpls\\desk.cpl", 
		L"HideMsstyles", RRF_RT_REG_MULTI_SZ, &type, nullptr, &cbData);
	if (status != ERROR_SUCCESS) return;

	wchar_t* buffer = (wchar_t*)malloc(cbData);
	status = RegGetValue(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Control Panel\\Cpls\\desk.cpl", 
		L"HideMsstyles", RRF_RT_REG_MULTI_SZ, &type, buffer, &cbData);

	wchar_t* ptr = buffer;
	while (*ptr)
	{
		int len = lstrlen(ptr) + 1;
		wchar_t* str = (wchar_t*)malloc(sizeof(wchar_t) * len);
		StringCchPrintf(str, len, L"%s", ptr);

		WCHAR path[MAX_PATH];
		ExpandEnvironmentStrings(str, path, MAX_PATH);

		msstyle.erase(
			std::remove_if(msstyle.begin(), msstyle.end(), 
				[&](LPWSTR& lstr)
				{
					return StrCmpI(lstr, path) == 0;
				})
			,msstyle.end());

		ptr += len;
		free(str);
	}
}


VOID CAppearanceDlgProc::FillSchemeDataMap(LPCWSTR theme, int index)
{
	BYTE* value;
	DWORD dwSize;
	HRESULT hr = RegGetValue(HKEY_CURRENT_USER, L"Control Panel\\Appearance\\Schemes", theme, RRF_RT_REG_BINARY, NULL, NULL, &dwSize);

	value = (BYTE*)malloc(dwSize);
	hr = RegGetValue(HKEY_CURRENT_USER, L"Control Panel\\Appearance\\Schemes", theme, RRF_RT_REG_BINARY, NULL, value, &dwSize);

	SCHEMEDATA data = {};
	data.version = READ_AT(DWORD, value, 0);
	data.ncm = READ_AT(NONCLIENTMETRICSW_2k, value, 4);
	data.lfIconTitle = READ_AT(LOGFONTW, value, 504);
	int start = 596;
	for (int i = 0; i < MAX_COLORS; i++)
	{
		data.rgb[i] = READ_AT(COLORREF, value, start + (4 * i));
	}

	if (wcsstr(theme, L"@"))
	{
		WCHAR buffer[256];
		HRESULT hr = SHLoadIndirectString(theme, buffer, ARRAYSIZE(buffer), NULL);

		lstrcpy(data.name, SUCCEEDED(hr) ? buffer : theme);
	}
	else
	{
		lstrcpy(data.name, theme);
	}
	schemeMap[index] = data;

	free(value);
}
