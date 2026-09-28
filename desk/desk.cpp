#include "pch.h"
#include "theme.h"
#include "ThemesPage.h"
#include "BackgroundPage.h"
#include "ScreensaverPage.h"
#include "AppearancePage.h"
#include "SettingsPage.h"
#include "desk.h"
#include "helper.h"
#include "uxtheme.h"
#include "fms.h"
//#include "todo.h"

HINSTANCE g_hinst;
HINSTANCE g_hThemeUI;
IThemeManager2* pThemeManager = NULL;
IDesktopWallpaper* pDesktopWallpaper = NULL;
ULONG_PTR gdiplusToken;

IUnknown* currentITheme;
THEMEINFO* selectedTheme = new THEMEINFO();
BOOL selectionPicker;
PROCESS_INFORMATION pi;
FONTINFO* fontInfo = new FONTINFO();
BOOL themeSelected = FALSE;
volatile LONG g_currentSessionModified = FALSE;
volatile LONG g_themeSelectionInProgress = FALSE;

namespace
{
constexpr UINT WM_DESKN_CHECK_DEFERRED_THEME_DIRTY = WM_APP + 0x3A1;
constexpr int kApplyButtonId = 0x3021;
bool g_themeApplyInProgress = false;
bool g_themeApplySawChangedNotification = false;
bool g_ignorePostApplyDirtyNotification = false;
DWORD g_applyCompletedTick = 0;
DWORD g_lastInputTickAtApply = 0;
}

const IID IID_IThemeManager2 = { 0xc1e8c83e, 0x845d, 0x4d95, {0x81, 0xdb, 0xe2, 0x83, 0xfd, 0xff, 0xc0, 0x00} };

void ForgetSavedThemePathForCurrentTheme()
{
	if (!pThemeManager) return;
	int activeIndex = -1;
	if (FAILED(pThemeManager->GetCurrentTheme(&activeIndex)) || activeIndex < 0)
		return;

	Microsoft::WRL::ComPtr<ITheme10> theme;
	if (FAILED(pThemeManager->GetTheme(activeIndex, &theme)) || !theme)
		return;

	GUID id = {};
	WCHAR idText[64] = {};
	WCHAR keyPath[128] = {};
	if (FAILED(theme->get_ThemeId(&id)) ||
		!StringFromGUID2(id, idText, ARRAYSIZE(idText)) ||
		FAILED(StringCchPrintfW(keyPath, ARRAYSIZE(keyPath),
			L"Software\\deskn\\SavedThemes\\%s", idText)))
		return;

	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
	{
		RegDeleteValueW(key, L"Path");
		RegCloseKey(key);
	}
}

static BOOL CALLBACK ForwardThemeChangedToChild(HWND hwndChild, LPARAM)
{
	SendMessageW(hwndChild, WM_THEMECHANGED, 0, 0);
	return TRUE;
}

static LRESULT CALLBACK PropertySheetThemeSubclass(
	HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR)
{
	if (message == WM_NCDESTROY)
	{
		RemoveWindowSubclass(hwnd, PropertySheetThemeSubclass, subclassId);
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	const bool applyButtonClicked = message == WM_COMMAND &&
		LOWORD(wParam) == kApplyButtonId;
	// Capture the selected theme before the property sheet synchronously sends
	// PSN_APPLY to its pages. A page may refresh/reselect the combo during that
	// processing, so inspecting it afterward can mistake a custom-theme Apply
	// for an explicit My Current Theme Apply.
	const bool myCurrentThemeSelectedBeforeApply =
		applyButtonClicked && IsMyCurrentThemeSelectedForApply();
	if (applyButtonClicked)
	{
		g_themeApplyInProgress = true;
		g_themeApplySawChangedNotification = false;
		g_ignorePostApplyDirtyNotification = false;
	}
	LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
	if (message == PSM_CHANGED)
	{
		bool lateApplyNotification = false;
		if (g_ignorePostApplyDirtyNotification)
		{
			LASTINPUTINFO inputInfo = { sizeof(inputInfo) };
			const DWORD elapsed = GetTickCount() - g_applyCompletedTick;
			lateApplyNotification = elapsed <= 2000 &&
				GetLastInputInfo(&inputInfo) &&
				inputInfo.dwTime == g_lastInputTickAtApply;
			if (!lateApplyNotification)
				g_ignorePostApplyDirtyNotification = false;
		}
		if (g_themeApplyInProgress)
		{
			// Applying a theme can itself send PSM_CHANGED while pages refresh.
			// That notification is not a user edit and must not create a Modified row.
			g_themeApplySawChangedNotification = true;
		}
		else if (lateApplyNotification)
		{
			// Some pages report their apply-time refresh only after PSN_APPLY returns.
			// Ignore it only while no new user input has occurred.
		}
		else
		{
			InterlockedExchange(&g_currentSessionModified, TRUE);
			if (!InterlockedCompareExchange(&g_themeSelectionInProgress, FALSE, FALSE))
				PostMessageW(hwnd, WM_DESKN_CHECK_DEFERRED_THEME_DIRTY, 0, 0);
		}
	}
	else if (message == WM_DESKN_CHECK_DEFERRED_THEME_DIRTY &&
		!g_themeApplyInProgress &&
		IsWindowEnabled(GetDlgItem(hwnd, kApplyButtonId)) &&
		!InterlockedCompareExchange(&g_themeSelectionInProgress, FALSE, FALSE))
	{
		NotifyThemePagePropertySheetChanged();
	}
	const bool applyCompleted = applyButtonClicked &&
		!IsWindowEnabled(GetDlgItem(hwnd, kApplyButtonId));
	if (applyCompleted)
	{
		// Discard deferred dirty checks posted by page refreshes during PSN_APPLY.
		MSG pendingMessage = {};
		while (PeekMessageW(&pendingMessage, hwnd,
			WM_DESKN_CHECK_DEFERRED_THEME_DIRTY,
			WM_DESKN_CHECK_DEFERRED_THEME_DIRTY, PM_REMOVE))
		{
		}
		if (myCurrentThemeSelectedBeforeApply)
			CompleteMyCurrentThemeApply();
		else
			PersistThemeModifiedStateAfterApply();
		InterlockedExchange(&g_currentSessionModified, FALSE);
		LASTINPUTINFO inputInfo = { sizeof(inputInfo) };
		g_lastInputTickAtApply = GetLastInputInfo(&inputInfo)
			? inputInfo.dwTime : GetTickCount();
		g_applyCompletedTick = GetTickCount();
		g_ignorePostApplyDirtyNotification = true;
	}
	if (applyButtonClicked)
	{
		g_themeApplyInProgress = false;
		if (!applyCompleted && g_themeApplySawChangedNotification)
		{
			// A failed Apply leaves the sheet dirty; restore its deferred update.
			InterlockedExchange(&g_currentSessionModified, TRUE);
			PostMessageW(hwnd, WM_DESKN_CHECK_DEFERRED_THEME_DIRTY, 0, 0);
		}
		g_themeApplySawChangedNotification = false;
	}
	if (message == WM_THEMECHANGED)
	{
		// The system broadcasts WM_THEMECHANGED to top-level windows, but the
		// property pages and their controls are children and may keep the prior
		// visual-style handles until they receive it themselves.
		EnumChildWindows(hwnd, ForwardThemeChangedToChild, 0);
		RedrawWindow(hwnd, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}
	return result;
}

int CALLBACK DeskCallback(HWND hwnd, UINT msg, LPARAM) {
	if (msg == PSCB_INITIALIZED)
	{
		SetWindowSubclass(hwnd, PropertySheetThemeSubclass, 1, 0);
		ApplySystemDialogFont(hwnd);
	}
	return 0;
}

void FillFontList(void*)
{
	// init fms
	InitFms();

	// fill list
	GetFilteredFontFamilies(&fontInfo->cFontList, &fontInfo->ppFontList);
}

void PropertySheetMoment(LPWSTR lpCmdLine)
{
	InterlockedExchange(&g_currentSessionModified, FALSE);
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	HRESULT hr = CoCreateInstance(CLSID_ThemeManager2, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pThemeManager));
	pThemeManager->Init(ThemeInitNoFlags);

	hr = CoCreateInstance(CLSID_DesktopWallpaper, NULL, CLSCTX_ALL, IID_PPV_ARGS(&pDesktopWallpaper));
	g_hThemeUI = LoadLibraryEx(L"themeui.dll", NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);

	// initialize theme manager
	InitUxtheme();
	InitializeCurrentThemeState();

	// cache the font list
	_beginthread(FillFontList, 0, NULL);

	//_beginthread(BrowserThread, 0, 0);

	// init invisible owner window
	HWND owner = CreateWindowExW(
		0,
		L"STATIC",
		L"",
		WS_POPUP | WS_CAPTION,
		50, 50, 0, 0,
		NULL,
		NULL,
		GetModuleHandle(NULL),
		NULL
	);
	
	// init property sheet
	std::wstring sheetTitle = LoadDeskString(IDS_DISPLAY_PROPERTIES);
	WTL::CPropertySheet sheet(sheetTitle.c_str());
	sheet.m_psh.pfnCallback = DeskCallback;

	CThemeDlgProc themedlg;
	CBackgroundDlgProc backgrounddlg;
	CScrSaverDlgProc screensaverdlg;
	CAppearanceDlgProc appearancedlg;
	CSettingsDlgProc settingsdlg;

#ifndef VISTA
	sheet.AddPage(themedlg);
	sheet.AddPage(backgrounddlg);
	sheet.AddPage(screensaverdlg);
	sheet.AddPage(appearancedlg);
	sheet.AddPage(settingsdlg);
#endif

	if (lpCmdLine)
	{
		int index = _wtoi(lpCmdLine) + 1;
		if (index > 4 || index < 0) index = 4;
#ifndef VISTA
		sheet.SetActivePage(index);
#else
		LPCPROPSHEETPAGE array[5] = {themedlg, backgrounddlg, screensaverdlg, appearancedlg, settingsdlg};
		sheet.AddPage(array[index]);
#endif
	}
#ifdef VISTA
	else
	{
		sheet.AddPage(settingsdlg);
	}
#endif

	//show
	sheet.DoModal(owner);
	DestroyWindow(owner);

	// cleanup
	Gdiplus::GdiplusShutdown(gdiplusToken);
	pThemeManager->Release();
	pDesktopWallpaper->Release();
	if (currentITheme) currentITheme->Release();
	free(selectedTheme);
	_TerminateProcess(pi);
}

extern "C" LONG APIENTRY CPlApplet(
	HWND hwndCPL,			// handle of Control Panel window
	UINT uMsg,				// message
	LONG_PTR lParam1,       // first message parameter
	LONG_PTR lParam2        // second message parameter
)
{
	LPCPLINFO lpCPlInfo;
	LONG retCode = 0;

	switch (uMsg)
	{
	case CPL_INIT:
		return TRUE;

	case CPL_GETCOUNT:
		return 1L;

	case CPL_INQUIRE:
		lpCPlInfo = (LPCPLINFO)lParam2;
		lpCPlInfo->idIcon = IDI_ICON1;
		lpCPlInfo->idName = IDS_THEMESCPL;
		lpCPlInfo->idInfo = IDS_THEMESDESC;
		return (LONG)TRUE;

	case CPL_DBLCLK:
		lParam2 = 0L;
		// fall through
	case CPL_STARTWPARMS:
		PropertySheetMoment((LPWSTR)lParam2);
		return (LONG)TRUE;
	}
	return retCode;
}
