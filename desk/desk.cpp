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
bool g_desktopThemeApplyPending = false;

namespace
{
constexpr UINT WM_DESKN_CHECK_DEFERRED_THEME_DIRTY = WM_APP + 0x3A1;
constexpr UINT WM_DESKN_ACTIVATE_EXISTING_SHEET = WM_APP + 0x3A2;
constexpr int kApplyButtonId = 0x3021;
constexpr UINT_PTR kRefreshStyledFrameTimer = 0xD35E;
constexpr UINT_PTR kActivateExistingSheetTimer = 0xD35F;
constexpr LPCWSTR kFrameRefreshAttempts = L"deskn.StyledFrameRefreshAttempts";
constexpr LPCWSTR kDisplayPropertiesWindow =
	L"deskn.DisplayProperties.8E7225C5-44CC-4A2E-9E75-DC5957E12D70";
bool g_themeApplyInProgress = false;
bool g_themeApplySawChangedNotification = false;
bool g_ignorePostApplyDirtyNotification = false;
DWORD g_applyCompletedTick = 0;
DWORD g_lastInputTickAtApply = 0;

BOOL CALLBACK FindExistingDisplayProperties(HWND hwnd, LPARAM context)
{
	if (!GetPropW(hwnd, kDisplayPropertiesWindow))
		return TRUE;
	*reinterpret_cast<HWND*>(context) = hwnd;
	return FALSE;
}

bool ActivateExistingDisplayProperties()
{
	HWND existing = nullptr;
	EnumWindows(FindExistingDisplayProperties,
		reinterpret_cast<LPARAM>(&existing));
	if (!existing || !IsWindow(existing))
		return false;
	DWORD processId = 0;
	GetWindowThreadProcessId(existing, &processId);
	if (processId)
		AllowSetForegroundWindow(processId);
	if (IsIconic(existing))
		ShowWindowAsync(existing, SW_RESTORE);
	SetForegroundWindow(existing);
	// Control_RunDLL can take activation back as it exits. Ask the open sheet
	// to retry after this second invocation has returned to the shell.
	PostMessageW(existing, WM_DESKN_ACTIVATE_EXISTING_SHEET, 0, 0);
	return true;
}
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
		KillTimer(hwnd, kRefreshStyledFrameTimer);
		KillTimer(hwnd, kActivateExistingSheetTimer);
		RemovePropW(hwnd, kFrameRefreshAttempts);
		RemovePropW(hwnd, kDisplayPropertiesWindow);
		RemoveWindowSubclass(hwnd, PropertySheetThemeSubclass, subclassId);
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}
	if (message == WM_DESKN_ACTIVATE_EXISTING_SHEET)
	{
		SetTimer(hwnd, kActivateExistingSheetTimer, 150, nullptr);
		return 0;
	}
	if (message == WM_TIMER && wParam == kActivateExistingSheetTimer)
	{
		KillTimer(hwnd, kActivateExistingSheetTimer);
		HWND target = GetLastActivePopup(hwnd);
		if (!target || !IsWindowVisible(target))
			target = hwnd;
		if (IsIconic(target))
			ShowWindow(target, SW_RESTORE);
		SetForegroundWindow(target);
		if (GetForegroundWindow() != target)
		{
			// Foreground locking can reject the request from rundll32. Raise
			// the sheet in z-order without leaving it permanently topmost.
			const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
			const bool wasTopmost =
				(GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
			SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, flags);
			if (!wasTopmost)
				SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
			SetForegroundWindow(target);
		}
		return 0;
	}
	if (message == WM_TIMER && wParam == kRefreshStyledFrameTimer)
	{
		UINT_PTR attempts = reinterpret_cast<UINT_PTR>(
			GetPropW(hwnd, kFrameRefreshAttempts));
		if (!selectedTheme || selectedTheme->szMsstylePath == L"(classic)" ||
			attempts >= 20)
		{
			KillTimer(hwnd, kRefreshStyledFrameTimer);
			RemovePropW(hwnd, kFrameRefreshAttempts);
			return 0;
		}
		SetPropW(hwnd, kFrameRefreshAttempts,
			reinterpret_cast<HANDLE>(attempts + 1));

		// The theme-switcher restores this process's theme hooks after the
		// Theme Manager call returns. Re-theme the live property sheet only
		// once a themed WINDOW handle can really be opened again.
		if (GetThemeAppProperties() && IsAppThemed())
		{
			HTHEME theme = OpenThemeData(hwnd, L"WINDOW");
			if (theme)
			{
				CloseThemeData(theme);
				KillTimer(hwnd, kRefreshStyledFrameTimer);
				RemovePropW(hwnd, kFrameRefreshAttempts);
				SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
				SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
					SWP_FRAMECHANGED);
				RedrawWindow(hwnd, nullptr, nullptr,
					RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
			}
		}
		return 0;
	}

	const bool applyButtonClicked = message == WM_COMMAND &&
		LOWORD(wParam) == kApplyButtonId;
	const bool wasClassicBeforeApply = applyButtonClicked &&
		GetThemeAppProperties() == 0;
	// Capture the selected theme before the property sheet synchronously sends
	// PSN_APPLY to its pages. A page may refresh/reselect the combo during that
	// processing, so inspecting it afterward can mistake a custom-theme Apply
	// for an explicit My Current Theme Apply.
	const bool myCurrentThemeSelectedBeforeApply =
		applyButtonClicked && IsMyCurrentThemeSelectedForApply();
	if (applyButtonClicked)
	{
		KillTimer(hwnd, kRefreshStyledFrameTimer);
		RemovePropW(hwnd, kFrameRefreshAttempts);
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
		if (wasClassicBeforeApply && selectedTheme &&
			selectedTheme->szMsstylePath != L"(classic)" &&
			SetTimer(hwnd, kRefreshStyledFrameTimer, 250, nullptr))
			SetPropW(hwnd, kFrameRefreshAttempts, reinterpret_cast<HANDLE>(1));
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
		// Let the page repaint after the apply message finishes. Erasing and
		// updating immediately here exposes each intermediate Classic metric set.
		RedrawWindow(hwnd, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ALLCHILDREN);
	}
	return result;
}

int CALLBACK DeskCallback(HWND hwnd, UINT msg, LPARAM) {
	if (msg == PSCB_INITIALIZED)
	{
		if (SetWindowSubclass(hwnd, PropertySheetThemeSubclass, 1, 0))
			SetPropW(hwnd, kDisplayPropertiesWindow, reinterpret_cast<HANDLE>(1));
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
	if (FAILED(hr) || !pThemeManager)
	{
		WCHAR message[160] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			L"Theme Manager could not be initialized (0x%08X).",
			static_cast<unsigned int>(hr));
		MessageBoxW(nullptr, message, L"Display Properties", MB_OK | MB_ICONERROR);
		Gdiplus::GdiplusShutdown(gdiplusToken);
		return;
	}
	hr = pThemeManager->Init(ThemeInitNoFlags);
	if (FAILED(hr))
	{
		WCHAR message[160] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			L"Theme Manager failed to start (0x%08X).",
			static_cast<unsigned int>(hr));
		MessageBoxW(nullptr, message, L"Display Properties", MB_OK | MB_ICONERROR);
		pThemeManager->Release();
		pThemeManager = nullptr;
		Gdiplus::GdiplusShutdown(gdiplusToken);
		return;
	}

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

unsigned __stdcall RunDisplayPropertiesSta(void* context)
{
	const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr))
	{
		WCHAR message[160] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			L"Display Properties could not initialize COM (0x%08X).",
			static_cast<unsigned int>(hr));
		MessageBoxW(nullptr, message, L"Display Properties", MB_OK | MB_ICONERROR);
		return 1;
	}
	LPWSTR commandLine = static_cast<LPWSTR>(context);
	PropertySheetMoment(commandLine && commandLine[0] ? commandLine : nullptr);
	CoUninitialize();
	return 0;
}

// The desktop Properties verb calls this directly through rundll32. Its
// calling thread may already be MTA, while Theme Manager requires STA.
// Keep the export alive while a fresh STA thread owns the modal sheet.
extern "C" void CALLBACK OpenDisplayPropertiesW(HWND, HINSTANCE, LPWSTR commandLine, int)
{
	if (ActivateExistingDisplayProperties())
		return;
	uintptr_t thread = _beginthreadex(nullptr, 0, RunDisplayPropertiesSta,
		commandLine, 0, nullptr);
	if (!thread)
	{
		MessageBoxW(nullptr, L"Display Properties could not start its UI thread.",
			L"Display Properties", MB_OK | MB_ICONERROR);
		return;
	}
	HANDLE handle = reinterpret_cast<HANDLE>(thread);
	WaitForSingleObject(handle, INFINITE);
	CloseHandle(handle);
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
		if (!ActivateExistingDisplayProperties())
			PropertySheetMoment((LPWSTR)lParam2);
		return (LONG)TRUE;
	}
	return retCode;
}
