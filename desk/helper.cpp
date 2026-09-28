#include "pch.h"
#include "helper.h"
#include "desk.h"
#include "uxtheme.h"

namespace
{
constexpr LPCWSTR kDialogFontProperty = L"deskn.SystemDialogFont";

BOOL CALLBACK ApplyDialogFontToChild(HWND child, LPARAM fontValue)
{
	SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(fontValue), TRUE);
	return TRUE;
}

LRESULT CALLBACK SystemDialogFontSubclass(HWND hwnd, UINT message,
	WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR)
{
	if (message == WM_NCDESTROY)
	{
		RemoveWindowSubclass(hwnd, SystemDialogFontSubclass, subclassId);
		HFONT font = static_cast<HFONT>(RemovePropW(hwnd, kDialogFontProperty));
		if (font) DeleteObject(font);
	}
	else if (message == WM_THEMECHANGED ||
		(message == WM_SETTINGCHANGE && wParam == SPI_SETNONCLIENTMETRICS))
	{
		ApplySystemDialogFont(hwnd);
	}
	return DefSubclassProc(hwnd, message, wParam, lParam);
}
}

void ApplySystemDialogFont(HWND hwnd)
{
	if (!IsWindow(hwnd)) return;
	NONCLIENTMETRICSW metrics = { sizeof(metrics) };
	UINT dpi = GetDpiForWindow(hwnd);
	if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
		&metrics, 0, dpi ? dpi : 96) &&
		!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
			&metrics, 0))
		return;

	HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
	if (!font) return;
	HFONT previous = static_cast<HFONT>(GetPropW(hwnd, kDialogFontProperty));
	if (!SetPropW(hwnd, kDialogFontProperty, font))
	{
		DeleteObject(font);
		return;
	}
	if (!SetWindowSubclass(hwnd, SystemDialogFontSubclass, 1, 0))
	{
		if (previous)
			SetPropW(hwnd, kDialogFontProperty, previous);
		else
			RemovePropW(hwnd, kDialogFontProperty);
		DeleteObject(font);
		return;
	}
	SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	EnumChildWindows(hwnd, ApplyDialogFontToChild, reinterpret_cast<LPARAM>(font));
	if (previous) DeleteObject(previous);
}

std::wstring LoadDeskString(UINT id)
{
	WCHAR text[1024] = {};
	int length = LoadStringW(g_hinst, id, text, ARRAYSIZE(text));
	return length > 0 ? std::wstring(text, length) : std::wstring();
}

VOID _TerminateProcess(PROCESS_INFORMATION& hp)
{
	if (hp.hProcess != nullptr)
	{
		TerminateProcess(hp.hProcess, 0);
		CloseHandle(hp.hThread);
		CloseHandle(hp.hProcess);
		hp.hProcess = nullptr;
		hp.hThread = nullptr;
	}
}

COLORREF GetDeskopColor()
{
	COLORREF clr;
	// colorref never has any alpha value, so a bogus value
	if (selectedTheme->newColor != 0xB0000000)
	{
		clr = selectedTheme->newColor;
	}
	else if (selectedTheme->useDesktopColor)
	{
		pDesktopWallpaper->GetBackgroundColor(&clr);
	}
	else
	{
		auto themeClass = std::make_unique<CTheme>(currentITheme);
		themeClass->GetBackgroundColor(&clr);
	}
	return clr;
}


void EnumDir(LPCWSTR directory, LPCWSTR* extensions, int cExtensions, std::vector<LPWSTR>& vec, BOOL fEnumChildDirs)
{
	WCHAR path[MAX_PATH];
	StringCchPrintf(path, ARRAYSIZE(path), L"%s\\*", directory);

	WIN32_FIND_DATAW data = { 0 };
	// FindExInfoBasic is faster?? according to msdn
	HANDLE hFind = FindFirstFileEx(path, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0);
	if (hFind == INVALID_HANDLE_VALUE) return;

	do
	{
		if (lstrcmp(data.cFileName, L"."))
		{
			if (lstrcmp(data.cFileName, L".."))
			{
				WCHAR fullPath[MAX_PATH];
				StringCchPrintf(fullPath, ARRAYSIZE(fullPath), L"%s\\%s", directory, data.cFileName);

				if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && fEnumChildDirs)
				{
					EnumDir(fullPath, extensions, cExtensions, vec, fEnumChildDirs);
				}
				else
				{
					for (int i = 0; i < cExtensions; ++i)
					{
						if (lstrcmp(PathFindExtension(data.cFileName), extensions[i]) == 0)
						{
							vec.push_back(_wcsdup(fullPath));
						}
					}
				}
			}
		}
	} while (FindNextFileW(hFind, &data));
	FindClose(hFind);
}

void FreeBitmap(Gdiplus::Bitmap** bmp)
{
	if (*bmp)
	{
		delete *bmp;
		*bmp = nullptr;
	}
}

HRESULT DrawBitmapIfNotNull(Gdiplus::Bitmap* bmp, Gdiplus::Graphics* graph, Gdiplus::Rect rect)
{
	if (bmp != nullptr)
	{
		return graph->DrawImage(bmp, rect) == Gdiplus::Ok ? S_OK : E_FAIL;
	}
	return E_FAIL;
}

HTHEME OpenNcThemeData(LPVOID file, LPCWSTR pszClassList)
{
	return file ? OpenThemeDataFromFile(file, NULL, pszClassList, 0) : OpenThemeData(NULL, pszClassList);
}

NTSTATUS _OpenThemeSection(ACCESS_MASK mask, HANDLE* hSection)
{
	DWORD sessionId;
	ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);

	WCHAR szNtSection[MAX_PATH];
	StringCchPrintf(szNtSection, ARRAYSIZE(szNtSection), L"\\Sessions\\%lu\\Windows\\ThemeSection", sessionId);

	UNICODE_STRING szNtString;
	RtlInitUnicodeString(&szNtString, szNtSection);

	OBJECT_ATTRIBUTES objAttributes;
	InitializeObjectAttributes(&objAttributes, &szNtString, OBJ_CASE_INSENSITIVE, NULL, NULL);

	return NtOpenSection(hSection, mask, &objAttributes);
}

// ugh
BOOL IsClassicThemeEnabled()
{
	if (GetThemeAppProperties() == 0) return TRUE;
	HANDLE hSection;
	if (!NT_SUCCESS(_OpenThemeSection(READ_CONTROL, &hSection))) return FALSE;

	DWORD neededSize = 0;
	GetKernelObjectSecurity(hSection, DACL_SECURITY_INFORMATION, nullptr, 0, &neededSize);
	PSECURITY_DESCRIPTOR pSD = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, neededSize);
	GetKernelObjectSecurity(hSection, DACL_SECURITY_INFORMATION, pSD, neededSize, &neededSize);

	LPWSTR sddlString = nullptr;
	ConvertSecurityDescriptorToStringSecurityDescriptor(pSD, SDDL_REVISION, DACL_SECURITY_INFORMATION, &sddlString, nullptr);

	BOOL bRet = FALSE;
	if (StrStrI(sddlString, SDDL_CREATE_CHILD) == NULL)
	{
		bRet = TRUE;
	}

	LocalFree(pSD);
	LocalFree(sddlString);
	CloseHandle(hSection);
	return bRet;
}

bool IsWindowsClassicThemeName(LPCWSTR name)
{
	if (!name || !name[0]) return false;
	if (StrCmpIW(name, L"Windows Classic") == 0 ||
		StrCmpIW(name, L"@themeui.dll,-2016") == 0)
		return true;

	WCHAR localizedName[256] = {};
	return g_hThemeUI &&
		LoadStringW(g_hThemeUI, 2016, localizedName, ARRAYSIZE(localizedName)) > 0 &&
		StrCmpIW(name, localizedName) == 0;
}

void InitializeCurrentThemeState()
{
	int current = 0;
	if (!pThemeManager || FAILED(pThemeManager->GetCurrentTheme(&current))) return;

	if (currentITheme)
	{
		currentITheme->Release();
		currentITheme = nullptr;
	}
	if (FAILED(pThemeManager->GetTheme(current, &currentITheme)) || !currentITheme) return;

	if (selectedTheme->selectedScheme)
	{
		free(selectedTheme->selectedScheme);
		selectedTheme->selectedScheme = nullptr;
	}
	Microsoft::WRL::ComPtr<ITheme10> activeThemeInfo;
	LPWSTR activeThemeName = nullptr;
	if (SUCCEEDED(pThemeManager->GetTheme(current, &activeThemeInfo)) && activeThemeInfo &&
		SUCCEEDED(activeThemeInfo->get_DisplayName(&activeThemeName)) &&
		IsWindowsClassicThemeName(activeThemeName))
	{
		selectedTheme->szMsstylePath = L"(classic)";
		CreateBlankScheme();
		return;
	}

	LPWSTR path = nullptr;
	auto theme = std::make_unique<CTheme>(currentITheme);
	if (SUCCEEDED(theme->get_VisualStyle(&path)) && path &&
		PathFileExists(path) &&
		StrCmpIW(PathFindExtensionW(path), L".msstyles") == 0)
	{
		selectedTheme->szMsstylePath = path;
		WCHAR liveStylePath[MAX_PATH] = {};
		WCHAR liveColorName[MAX_PATH] = {};
		WCHAR liveSizeName[MAX_PATH] = {};
		const bool haveLiveVariant = SUCCEEDED(GetCurrentThemeName(
			liveStylePath, ARRAYSIZE(liveStylePath),
			liveColorName, ARRAYSIZE(liveColorName),
			liveSizeName, ARRAYSIZE(liveSizeName))) &&
			liveStylePath[0] && StrCmpIW(liveStylePath, path) == 0;
		HANDLE loadedTheme = LoadThemeFromFilePath(path,
			haveLiveVariant ? liveColorName : nullptr,
			haveLiveVariant ? liveSizeName : nullptr);
		if (loadedTheme)
		{
			CreateThemedMetricsScheme(GetDpiForSystem(), loadedTheme);
			CleanupThemeFile(&loadedTheme);
		}
	}
	else
	{
		selectedTheme->szMsstylePath = L"(classic)";
		CreateBlankScheme();
	}
}

wchar_t* strCut(wchar_t* s, const wchar_t* pattern)
{
	if (wchar_t* p = wcsstr(s, pattern))
	{
		wchar_t* q = p + lstrlen(pattern);
		while (*p++ = *q++);
	}
	return s;
}


__declspec(noinline) void ScaleNonClientMetrics(NONCLIENTMETRICS& ncm, int dpi)
{
	ncm.iScrollHeight = MulDiv(ncm.iScrollHeight, dpi, 96);
	ncm.iScrollWidth = MulDiv(ncm.iScrollWidth, dpi, 96);
	ncm.iCaptionHeight = MulDiv(ncm.iCaptionHeight, dpi, 96);
	ncm.iCaptionWidth = MulDiv(ncm.iCaptionWidth, dpi, 96);

	ScaleLogFont(ncm.lfCaptionFont, dpi);
	ncm.iSmCaptionHeight = MulDiv(ncm.iSmCaptionHeight, dpi, 96);
	ncm.iSmCaptionWidth = MulDiv(ncm.iSmCaptionWidth, dpi, 96);

	ScaleLogFont(ncm.lfSmCaptionFont, dpi);
	ncm.iMenuHeight = MulDiv(ncm.iMenuHeight, dpi, 96);
	ncm.iMenuWidth = MulDiv(ncm.iMenuWidth, dpi, 96);

	ScaleLogFont(ncm.lfMenuFont, dpi);
	ScaleLogFont(ncm.lfStatusFont, dpi);
	ScaleLogFont(ncm.lfMessageFont, dpi);
}

void UnScaleNonClientMetrics(NONCLIENTMETRICSW_2k& ncm, int dpi)
{
	ncm.iScrollHeight = MulDiv(ncm.iScrollHeight, 96, dpi);
	ncm.iScrollWidth = MulDiv(ncm.iScrollWidth, 96, dpi);
	ncm.iCaptionHeight = MulDiv(ncm.iCaptionHeight, 96, dpi);
	ncm.iCaptionWidth = MulDiv(ncm.iCaptionWidth, 96, dpi);
	ncm.lfCaptionFont.lfHeight = MulDiv(ncm.lfCaptionFont.lfHeight, 96, dpi);

	ncm.iSmCaptionHeight = MulDiv(ncm.iSmCaptionHeight, 96, dpi);
	ncm.iSmCaptionWidth = MulDiv(ncm.iSmCaptionWidth, 96, dpi);
	ncm.lfSmCaptionFont.lfHeight = MulDiv(ncm.lfSmCaptionFont.lfHeight, 96, dpi);

	ncm.iMenuHeight = MulDiv(ncm.iMenuHeight, 96, dpi);
	ncm.iMenuWidth = MulDiv(ncm.iMenuWidth, 96, dpi);

	ncm.lfMenuFont.lfHeight = MulDiv(ncm.lfMenuFont.lfHeight, 96, dpi);
	ncm.lfStatusFont.lfHeight = MulDiv(ncm.lfStatusFont.lfHeight, 96, dpi);
	ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 96, dpi);
}

void ScaleNonClientMetrics(NONCLIENTMETRICSW_2k& ncm, int dpi)
{
	ncm.iScrollHeight = MulDiv(ncm.iScrollHeight, dpi, 96);
	ncm.iScrollWidth = MulDiv(ncm.iScrollWidth, dpi, 96);
	ncm.iCaptionHeight = MulDiv(ncm.iCaptionHeight, dpi, 96);
	ncm.iCaptionWidth = MulDiv(ncm.iCaptionWidth, dpi, 96);

	ncm.iSmCaptionHeight = MulDiv(ncm.iSmCaptionHeight, dpi, 96);
	ncm.iSmCaptionWidth = MulDiv(ncm.iSmCaptionWidth, dpi, 96);

	ncm.iMenuHeight = MulDiv(ncm.iMenuHeight, dpi, 96);
	ncm.iMenuWidth = MulDiv(ncm.iMenuWidth, dpi, 96);
}

void ScaleLogFont(LOGFONT& lf, int dpi)
{
	lf.lfHeight = MulDiv(lf.lfHeight, dpi, 96);
}


HRESULT GetSolidBtnBmp(COLORREF clr, int dpi, SIZE size, HBITMAP* pbOut)
{
	int i = MulDiv(10, dpi, 96);
	Gdiplus::Bitmap bmp(size.cx - i, size.cy - i);
	Gdiplus::Graphics g(&bmp);
	Gdiplus::SolidBrush brush(Gdiplus::Color(SPLIT_COLORREF(clr)));
	g.FillRectangle(&brush, 0, 0, bmp.GetWidth(), bmp.GetHeight());

	return bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0), pbOut) == Gdiplus::Ok ? S_OK : E_FAIL;
}

BOOL ColorPicker(COLORREF clr, HWND hWnd, CHOOSECOLOR* clrOut, BOOL fullOpen)
{
	static COLORREF acrCustClr[16];

	CHOOSECOLOR cc = { 0 };
	cc.lStructSize = sizeof(cc);
	cc.hwndOwner = hWnd;
	cc.lpCustColors = acrCustClr;
	cc.rgbResult = clr;
	cc.Flags = CC_RGBINIT;
	if (fullOpen)
		cc.Flags |= CC_FULLOPEN;

	BOOL out = ChooseColor(&cc);
	*clrOut = cc;
	return out;
}

// callee must free allocated SCHEMEDATA when done
void CreateBlankScheme()
{
	if (!selectedTheme->selectedScheme)
	{
		COLORREF rgb[MAX_COLORS];
		for (int i = 0; i < MAX_COLORS; ++i)
		{
			rgb[i] = GetSysColor(i);
		}

		NONCLIENTMETRICSW ncm = { sizeof(ncm) };
		SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, NULL, 96);

		NONCLIENTMETRICSW_2k ncm2k;
		memcpy(&ncm2k, &ncm, sizeof(ncm2k));

		LOGFONT lfIcon;
		SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(lfIcon), &lfIcon, NULL, 96);

		SCHEMEDATA* currentData = (SCHEMEDATA*)malloc(sizeof(SCHEMEDATA));
		if (currentData)
		{
			ZeroMemory(currentData, sizeof(*currentData));
			currentData->lfIconTitle = lfIcon;
			currentData->ncm = ncm2k;
			memcpy(currentData->rgb, rgb, sizeof(rgb));
			currentData->variant = 0x8;			// indicates custom theme
			currentData->dpiScaled = false;
			currentData->iPaddedBorderWidth = ncm.iPaddedBorderWidth;
			StringCchCopyW(currentData->name, ARRAYSIZE(currentData->name), L"Custom Classic");

			selectedTheme->selectedScheme = currentData;
		}
	}
	else
	{
		SCHEMEDATA* currentData = (SCHEMEDATA*)malloc(sizeof(SCHEMEDATA));
		if (currentData)
		{
			*currentData = *selectedTheme->selectedScheme;
			currentData->variant = 0x8;			// indicates custom theme
			if (!currentData->name[0])
				StringCchCopyW(currentData->name, ARRAYSIZE(currentData->name), L"Custom Classic");

			selectedTheme->selectedScheme = currentData;
		}
	}
}

void CreateThemedMetricsScheme(int dpi, void* pTheme)
{
	HTHEME hTheme = OpenNcThemeData(pTheme, L"Window");

	SCHEMEDATA* currentData = (SCHEMEDATA*)malloc(sizeof(SCHEMEDATA));
	if (currentData)
	{
		ZeroMemory(currentData, sizeof(SCHEMEDATA));
		currentData->dpiScaled = false;
		currentData->variant = 0x8;

		selectedTheme->selectedScheme = currentData;
	}

	for (int i = 0; i < MAX_COLORS; ++i)
	{
		selectedTheme->selectedScheme->rgb[i] = GetThemeSysColor(hTheme, i);
	}
	 
	GetThemeSysFont(hTheme, TMT_CAPTIONFONT, &selectedTheme->selectedScheme->ncm.lfCaptionFont);
	GetThemeSysFont(hTheme, TMT_SMALLCAPTIONFONT, &selectedTheme->selectedScheme->ncm.lfSmCaptionFont);
	GetThemeSysFont(hTheme, TMT_MENUFONT, &selectedTheme->selectedScheme->ncm.lfMenuFont);
	GetThemeSysFont(hTheme, TMT_STATUSFONT, &selectedTheme->selectedScheme->ncm.lfStatusFont);
	GetThemeSysFont(hTheme, TMT_MSGBOXFONT, &selectedTheme->selectedScheme->ncm.lfMessageFont);
	GetThemeSysFont(hTheme, TMT_ICONTITLEFONT, &selectedTheme->selectedScheme->lfIconTitle);

	selectedTheme->selectedScheme->ncm.iBorderWidth = GetThemeSysSize(hTheme, SM_CXBORDER);
	selectedTheme->selectedScheme->ncm.iScrollHeight = GetThemeSysSize(hTheme, SM_CYVSCROLL);
	selectedTheme->selectedScheme->ncm.iScrollWidth = GetThemeSysSize(hTheme, SM_CXVSCROLL);
	selectedTheme->selectedScheme->ncm.iCaptionHeight = GetThemeSysSize(hTheme, SM_CYSIZE);
	selectedTheme->selectedScheme->ncm.iCaptionWidth = GetThemeSysSize(hTheme, SM_CXSIZE);
	selectedTheme->selectedScheme->ncm.iSmCaptionHeight = GetThemeSysSize(hTheme, SM_CYSMSIZE);
	selectedTheme->selectedScheme->ncm.iSmCaptionWidth = GetThemeSysSize(hTheme, SM_CXSMSIZE);
	selectedTheme->selectedScheme->ncm.iMenuHeight = GetThemeSysSize(hTheme, SM_CYMENUSIZE);
	selectedTheme->selectedScheme->ncm.iMenuWidth = GetThemeSysSize(hTheme, SM_CXMENUSIZE);

	UnScaleNonClientMetrics(selectedTheme->selectedScheme->ncm, dpi);
	selectedTheme->selectedScheme->lfIconTitle.lfHeight = MulDiv(selectedTheme->selectedScheme->lfIconTitle.lfHeight, 96, dpi);

	selectedTheme->selectedScheme->iPaddedBorderWidth = GetThemeSysSize(hTheme, SM_CXPADDEDBORDER);
	CloseThemeData(hTheme);
}

bool ApplySchemeMetrics(const SCHEMEDATA* scheme, int dpi)
{
	if (!scheme || dpi <= 0)
		return false;

	NONCLIENTMETRICS metrics = {};
	memcpy(&metrics, &scheme->ncm, sizeof(NONCLIENTMETRICSW_2k));
	metrics.cbSize = sizeof(metrics);
	metrics.iPaddedBorderWidth = MulDiv(scheme->iPaddedBorderWidth, dpi, 96);
	LOGFONT iconFont = scheme->lfIconTitle;
	if (!scheme->dpiScaled)
		ScaleNonClientMetrics(metrics, dpi);
	else
	{
		ScaleLogFont(metrics.lfCaptionFont, dpi);
		ScaleLogFont(metrics.lfSmCaptionFont, dpi);
		ScaleLogFont(metrics.lfMenuFont, dpi);
		ScaleLogFont(metrics.lfStatusFont, dpi);
		ScaleLogFont(metrics.lfMessageFont, dpi);
	}
	ScaleLogFont(iconFont, dpi);

	const BOOL iconFontApplied = SystemParametersInfoW(SPI_SETICONTITLELOGFONT,
		sizeof(LOGFONT), &iconFont, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	const BOOL metricsApplied = SystemParametersInfoW(SPI_SETNONCLIENTMETRICS,
		sizeof(metrics), &metrics, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	return iconFontApplied && metricsApplied;
}

void SetBitmap(HWND hWnd, HBITMAP hBmp)
{
	HBITMAP hOld = Static_SetBitmap(hWnd, hBmp);
	DeleteBitmap(hBmp);
	DeleteBitmap(hOld);
}

void UpdateCustomTheme()
{
	WCHAR szCustomThemePath[MAX_PATH];
	ExpandEnvironmentStrings(L"%localappdata%\\Microsoft\\Windows\\Themes\\Custom.theme", szCustomThemePath, MAX_PATH);

	DeleteFile(szCustomThemePath);
	pThemeManager->Refresh();
}

SCHEMEDATA* LoadCurrentClassicSchemeFromRegistry()
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

	static SCHEMEDATA currentScheme = {};
	SCHEMEDATA* scheme = &currentScheme;
	ZeroMemory(scheme, sizeof(*scheme));

	for (int i = 0; i < MAX_COLORS; ++i)
	{
		WCHAR value[64] = {};
		DWORD size = sizeof(value);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", colorNames[i],
			RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
		{
			unsigned red, green, blue;
			if (swscanf_s(value, L"%u %u %u", &red, &green, &blue) == 3 &&
				red <= 255 && green <= 255 && blue <= 255)
			{
				scheme->rgb[i] = RGB(red, green, blue);
				continue;
			}
		}
		scheme->rgb[i] = GetSysColor(i);
	}

	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
	{
		memcpy(&scheme->ncm, &ncm, sizeof(scheme->ncm));
		scheme->iPaddedBorderWidth = ncm.iPaddedBorderWidth;
	}
	SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(scheme->lfIconTitle),
		&scheme->lfIconTitle, 0);
	DWORD selectedNameSize = sizeof(scheme->name);
	if (RegGetValueW(HKEY_CURRENT_USER,
		L"Control Panel\\Appearance\\ClassicSchemes", L"SelectedScheme",
		RRF_RT_REG_SZ, nullptr, scheme->name, &selectedNameSize) != ERROR_SUCCESS ||
		!scheme->name[0])
	{
		StringCchCopyW(scheme->name, ARRAYSIZE(scheme->name), L"Custom Classic");
	}
	scheme->ncm.cbSize = sizeof(scheme->ncm);
	scheme->variant = 0x18; // registry-backed current scheme; includes CUSTOM_SCHEME
	scheme->dpiScaled = FALSE;
	return scheme;
}

SCHEMEDATA* LoadWindowsStandardClassicScheme()
{
	static SCHEMEDATA standardScheme = {};
	ZeroMemory(&standardScheme, sizeof(standardScheme));
	StringCchCopyW(standardScheme.name, ARRAYSIZE(standardScheme.name), L"Windows Standard");
	standardScheme.variant = 0x18;
	standardScheme.dpiScaled = FALSE;

	NONCLIENTMETRICSW ncm = { sizeof(ncm) };
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
	{
		memcpy(&standardScheme.ncm, &ncm, sizeof(standardScheme.ncm));
		standardScheme.iPaddedBorderWidth = ncm.iPaddedBorderWidth;
	}
	SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(standardScheme.lfIconTitle),
		&standardScheme.lfIconTitle, 0);
	standardScheme.ncm.cbSize = sizeof(standardScheme.ncm);

	// Load the scheme's own Classic metrics/font data from the legacy binary
	// record. Scheme names on current Windows are often indirect resource IDs.
	bool loadedSchemeRecord = false;
	HKEY schemesKey = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Appearance\\Schemes",
		0, KEY_QUERY_VALUE, &schemesKey) == ERROR_SUCCESS)
	{
		for (DWORD index = 0;; ++index)
		{
			WCHAR valueName[256] = {};
			DWORD valueNameLength = ARRAYSIZE(valueName);
			DWORD type = 0;
			DWORD dataSize = 0;
			LSTATUS status = RegEnumValueW(schemesKey, index, valueName,
				&valueNameLength, nullptr, &type, nullptr, &dataSize);
			if (status == ERROR_NO_MORE_ITEMS) break;
			if (status != ERROR_SUCCESS || type != REG_BINARY ||
				dataSize < 596 + sizeof(COLORREF) * MAX_COLORS)
				continue;

			WCHAR displayName[256] = {};
			// This legacy resource is still present in the Appearance\\Schemes
			// registry, but its string was removed from newer themeui.dll builds.
			// The matching record is the system's Windows Standard Classic scheme.
			if (StrCmpI(valueName, L"@themeui.dll,-854") == 0)
				StringCchCopyW(displayName, ARRAYSIZE(displayName), L"Windows Standard");
			else if (valueName[0] == L'@' &&
				FAILED(SHLoadIndirectString(valueName, displayName, ARRAYSIZE(displayName), nullptr)))
				continue;
			if (valueName[0] != L'@')
				StringCchCopyW(displayName, ARRAYSIZE(displayName), valueName);
			if (StrCmpI(displayName, L"Windows Standard") != 0) continue;

			std::vector<BYTE> data(dataSize);
			status = RegQueryValueExW(schemesKey, valueName, nullptr, &type,
				data.data(), &dataSize);
			if (status != ERROR_SUCCESS) continue;
			standardScheme.version = *reinterpret_cast<const DWORD*>(data.data());
			memcpy(&standardScheme.ncm, data.data() + sizeof(DWORD),
				sizeof(standardScheme.ncm));
			memcpy(&standardScheme.lfIconTitle, data.data() + 504,
				sizeof(standardScheme.lfIconTitle));
			memcpy(standardScheme.rgb, data.data() + 596,
				sizeof(standardScheme.rgb));
			standardScheme.ncm.cbSize = sizeof(standardScheme.ncm);
			standardScheme.iPaddedBorderWidth = 0;
			loadedSchemeRecord = true;
			break;
		}
		RegCloseKey(schemesKey);
	}

	// Appearance's Windows Standard entry uses the legacy scheme record.
	// Use that same palette for the Themes preview/application. A saved cache
	// may contain older or edited colors; it is only a fallback if the actual
	// scheme record is unavailable, never an override of the named scheme.
	HKEY standardKey = nullptr;
	if (!loadedSchemeRecord && RegOpenKeyExW(HKEY_CURRENT_USER,
		L"Control Panel\\Appearance\\ClassicSchemes\\Windows Standard", 0,
		KEY_QUERY_VALUE, &standardKey) == ERROR_SUCCESS)
	{
		for (int i = 0; i < MAX_COLORS; ++i)
		{
			WCHAR valueName[16] = {};
			StringCchPrintfW(valueName, ARRAYSIZE(valueName), L"Color%d", i);
			DWORD color = 0;
			DWORD colorSize = sizeof(color);
			if (RegGetValueW(standardKey, nullptr, valueName, RRF_RT_REG_DWORD,
				nullptr, &color, &colorSize) == ERROR_SUCCESS)
				standardScheme.rgb[i] = color & 0x00FFFFFF;
		}
		RegCloseKey(standardKey);
	}
	return &standardScheme;
}
