/* ---------------------------------------------------------
* Background page
*
* Responsible for the "Desktop" tab
*
------------------------------------------------------------*/

#include "pch.h"
#include "BackgroundPage.h"
#include "ColorPalette.h"

#include "desk.h"
#include "helper.h"
#include "wndprvw.h"

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Details;

namespace
{
	constexpr UINT kThemeUiPaletteStartCapture = WM_APP + 0x2B;
	constexpr int kThemeUiPaletteGridId = 1205;
	constexpr int kThemeUiPaletteOtherId = 1207;
	constexpr int kThemeUiPaletteSwatchId = 1206;
	constexpr int kThemeUiPaletteColumns = 4;
	constexpr int kThemeUiPaletteMaxColors = 20;
	constexpr INT_PTR kThemeUiPaletteOtherResult = kThemeUiPaletteOtherId;
	constexpr INT_PTR kThemeUiPaletteColorResult = 1;

	struct ThemeUiPaletteState
	{
		HWND ownerButton = nullptr;
		COLORREF colors[kThemeUiPaletteMaxColors] = {};
		int colorCount = 0;
		int cellWidth = 1;
		int cellHeight = 1;
		int focusIndex = -1;
		int currentColorIndex = -1;
		COLORREF currentColor = RGB(0, 0, 0);
		INT_PTR result = 2;
		bool captureStarted = false;
		bool closing = false;
	};

	int ThemeUiPaletteColorIndex(const ThemeUiPaletteState& state, POINT point)
	{
		if (point.x < 0 || point.y < 0) return -1;
		int column = point.x / state.cellWidth;
		int row = point.y / state.cellHeight;
		if (column >= kThemeUiPaletteColumns) return -1;
		int index = row * kThemeUiPaletteColumns + column;
		return index >= 0 && index < state.colorCount ? index : -1;
	}

	void DrawThemeUiPaletteCell(HDC dc, RECT rect, COLORREF color, bool focused)
	{
		if (focused)
		{
			HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
			FrameRect(dc, &rect, black);
			InflateRect(&rect, -2, -2);
		}
		else
		{
			FrameRect(dc, &rect, GetSysColorBrush(COLOR_BTNFACE));
			InflateRect(&rect, -GetSystemMetrics(SM_CXBORDER),
				-GetSystemMetrics(SM_CYBORDER));
			DrawEdge(dc, &rect, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
		}

		HBRUSH brush = CreateSolidBrush(color);
		if (brush)
		{
			FillRect(dc, &rect, brush);
			DeleteObject(brush);
		}
		if (focused)
			DrawFocusRect(dc, &rect);
	}

	void DrawThemeUiPaletteItems(const DRAWITEMSTRUCT* drawItem, ThemeUiPaletteState& state)
	{
		if (drawItem->CtlID == kThemeUiPaletteGridId)
		{
			for (int index = 0; index < state.colorCount; ++index)
			{
				RECT cell = drawItem->rcItem;
				int column = index % kThemeUiPaletteColumns;
				int row = index / kThemeUiPaletteColumns;
				cell.left += column * state.cellWidth;
				cell.top += row * state.cellHeight;
				cell.right = min(cell.right, cell.left + state.cellWidth);
				cell.bottom = min(cell.bottom, cell.top + state.cellHeight);
				DrawThemeUiPaletteCell(drawItem->hDC, cell, state.colors[index],
					index == state.focusIndex);
			}
		}
		else if (drawItem->CtlID == kThemeUiPaletteSwatchId && state.currentColorIndex < 0)
		{
			RECT swatch = drawItem->rcItem;
			DrawThemeUiPaletteCell(drawItem->hDC, swatch, state.currentColor,
				state.focusIndex == state.colorCount);
		}
	}

	void PositionThemeUiPalette(HWND dialog, HWND ownerButton)
	{
		RECT buttonRect = {};
		RECT dialogRect = {};
		if (!GetWindowRect(ownerButton, &buttonRect) || !GetWindowRect(dialog, &dialogRect))
			return;

		MONITORINFO monitor = { sizeof(monitor) };
		HMONITOR hMonitor = MonitorFromRect(&buttonRect, MONITOR_DEFAULTTONEAREST);
		if (!GetMonitorInfoW(hMonitor, &monitor))
			return;

		int width = RECTWIDTH(dialogRect);
		int height = RECTHEIGHT(dialogRect);
		int left = buttonRect.left;
		int top = buttonRect.bottom;
		if (left + width > monitor.rcMonitor.right)
			left = monitor.rcMonitor.right - width - 1;
		if (left < monitor.rcMonitor.left)
			left = monitor.rcMonitor.left;
		if (top + height > monitor.rcMonitor.bottom)
			top = buttonRect.top - height;
		if (top < monitor.rcMonitor.top)
			top = monitor.rcMonitor.top;

		SetWindowPos(dialog, HWND_TOP, left, top, width, height,
			SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	}

	void CloseThemeUiPalette(HWND dialog, ThemeUiPaletteState& state, INT_PTR result)
	{
		if (state.closing)
			return;
		state.closing = true;
		state.result = result;
		if (GetCapture() == dialog)
			ReleaseCapture();
		DestroyWindow(dialog);
	}

	INT_PTR CALLBACK ThemeUiPaletteDialogProc(HWND dialog, UINT message,
		WPARAM wParam, LPARAM lParam)
	{
		auto* state = reinterpret_cast<ThemeUiPaletteState*>(
			GetWindowLongPtrW(dialog, DWLP_USER));
		if (message == WM_INITDIALOG)
		{
			ApplySystemDialogFont(dialog);
			state = reinterpret_cast<ThemeUiPaletteState*>(lParam);
			SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
			HWND grid = GetDlgItem(dialog, kThemeUiPaletteGridId);
			RECT gridRect = {};
			GetClientRect(grid, &gridRect);
			state->cellWidth = max(1, (RECTWIDTH(gridRect) + kThemeUiPaletteColumns - 1) /
				kThemeUiPaletteColumns);
			int rows = (state->colorCount + kThemeUiPaletteColumns - 1) /
				kThemeUiPaletteColumns;
			state->cellHeight = max(1, (RECTHEIGHT(gridRect) + rows - 1) / rows);
			HWND swatch = GetDlgItem(dialog, kThemeUiPaletteSwatchId);
			ShowWindow(swatch, state->currentColorIndex < 0 ? SW_SHOW : SW_HIDE);
			PositionThemeUiPalette(dialog, state->ownerButton);
			// Activation can send cancel/capture messages during initialization.
			// Acquire capture after the popup has been shown.
			PostMessageW(dialog, kThemeUiPaletteStartCapture, 0, 0);
			return TRUE;
		}
		if (!state)
			return FALSE;

		switch (message)
		{
		case WM_SETCURSOR:
			if (LOWORD(lParam) == HTCLIENT)
			{
				SetCursor(LoadCursorW(nullptr, IDC_ARROW));
				return TRUE;
			}
			break;

		case kThemeUiPaletteStartCapture:
			if (!state->closing)
			{
				SetCapture(dialog);
				state->captureStarted = GetCapture() == dialog;
				SetCursor(LoadCursorW(nullptr, IDC_ARROW));
			}
			return TRUE;

		case WM_DRAWITEM:
			DrawThemeUiPaletteItems(reinterpret_cast<DRAWITEMSTRUCT*>(lParam), *state);
			return TRUE;

		case WM_MOUSEMOVE:
		{
			POINT point = { static_cast<short>(LOWORD(lParam)),
				static_cast<short>(HIWORD(lParam)) };
			HWND child = ChildWindowFromPoint(dialog, point);
			int newFocus = -1;
			if (child && GetDlgCtrlID(child) == kThemeUiPaletteGridId)
			{
				MapWindowPoints(dialog, child, &point, 1);
				newFocus = ThemeUiPaletteColorIndex(*state, point);
			}
			else if (child && GetDlgCtrlID(child) == kThemeUiPaletteSwatchId &&
				state->currentColorIndex < 0)
				newFocus = state->colorCount;
			if (newFocus != state->focusIndex)
			{
				state->focusIndex = newFocus;
				InvalidateRect(GetDlgItem(dialog, kThemeUiPaletteGridId), nullptr, FALSE);
				InvalidateRect(GetDlgItem(dialog, kThemeUiPaletteSwatchId), nullptr, FALSE);
			}
			return TRUE;
		}

		case WM_LBUTTONUP:
		{
			POINT point = { static_cast<short>(LOWORD(lParam)),
				static_cast<short>(HIWORD(lParam)) };
			HWND child = ChildWindowFromPoint(dialog, point);
			int controlId = child && child != dialog ? GetDlgCtrlID(child) : 0;
			if (controlId == kThemeUiPaletteGridId)
			{
				MapWindowPoints(dialog, child, &point, 1);
				int index = ThemeUiPaletteColorIndex(*state, point);
				if (index >= 0)
				{
					state->currentColor = state->colors[index];
					CloseThemeUiPalette(dialog, *state, kThemeUiPaletteColorResult);
					return TRUE;
				}
			}
			else if (controlId == kThemeUiPaletteOtherId)
			{
				CloseThemeUiPalette(dialog, *state, kThemeUiPaletteOtherResult);
				return TRUE;
			}
			else if (controlId == kThemeUiPaletteSwatchId && state->currentColorIndex < 0)
			{
				CloseThemeUiPalette(dialog, *state, kThemeUiPaletteColorResult);
				return TRUE;
			}
			CloseThemeUiPalette(dialog, *state, 2);
			return TRUE;
		}

		case WM_COMMAND:
			if (LOWORD(wParam) == kThemeUiPaletteOtherId)
			{
				CloseThemeUiPalette(dialog, *state, kThemeUiPaletteOtherResult);
				return TRUE;
			}
			if (LOWORD(wParam) == IDCANCEL)
			{
				CloseThemeUiPalette(dialog, *state, 2);
				return TRUE;
			}
			break;

		case WM_CAPTURECHANGED:
			if (!state->closing && state->captureStarted &&
				reinterpret_cast<HWND>(lParam) != dialog)
				CloseThemeUiPalette(dialog, *state, 2);
			return TRUE;

		case WM_CANCELMODE:
			// Ignore startup cancel mode. Later cancel mode dismisses the popup.
			if (!state->closing && state->captureStarted)
				CloseThemeUiPalette(dialog, *state, 2);
			return TRUE;

		case WM_NCDESTROY:
			state->closing = true;
			if (GetCapture() == dialog)
				ReleaseCapture();
			return FALSE;
		}
		return FALSE;
	}

	bool PickThemeUiColorImpl(HWND ownerButton, COLORREF initialColor, COLORREF& chosenColor)
	{
		ThemeUiPaletteState state = {};
		state.ownerButton = ownerButton;
		state.currentColor = initialColor & 0x00ffffff;
		static constexpr COLORREF themeUiColors[] =
		{
			RGB(255, 255, 255), RGB(0, 0, 0), RGB(192, 192, 192), RGB(128, 128, 128),
			RGB(255, 0, 0), RGB(128, 0, 0), RGB(255, 255, 0), RGB(128, 128, 0),
			RGB(0, 255, 0), RGB(0, 128, 0), RGB(0, 255, 255), RGB(0, 128, 128),
			RGB(0, 0, 255), RGB(0, 0, 128), RGB(255, 0, 255), RGB(128, 0, 128)
		};
		static_assert(ARRAYSIZE(themeUiColors) == 16);
		for (COLORREF color : themeUiColors)
			state.colors[state.colorCount++] = color;

		HPALETTE defaultPalette = static_cast<HPALETTE>(GetStockObject(DEFAULT_PALETTE));
		PALETTEENTRY extraColors[4] = {};
		if (defaultPalette && GetPaletteEntries(defaultPalette, 8, ARRAYSIZE(extraColors),
			extraColors) == ARRAYSIZE(extraColors))
		{
			for (const PALETTEENTRY& entry : extraColors)
				state.colors[state.colorCount++] = RGB(entry.peRed, entry.peGreen, entry.peBlue);
		}

		for (int i = 0; i < state.colorCount; ++i)
		{
			if (state.colors[i] == state.currentColor)
			{
				state.currentColorIndex = i;
				state.focusIndex = i;
				break;
			}
		}
		if (state.currentColorIndex < 0)
			state.focusIndex = state.colorCount;

		HWND dialogOwner = GetAncestor(ownerButton, GA_ROOT);
		if (!dialogOwner)
			dialogOwner = ownerButton;
		HWND dialog = CreateDialogParamW(g_hinst,
			MAKEINTRESOURCEW(IDD_THEMEUI_COLOR_PICKER), dialogOwner,
			ThemeUiPaletteDialogProc, reinterpret_cast<LPARAM>(&state));
		if (!dialog)
			return false;
		ShowWindow(dialog, SW_SHOW);

		MSG message = {};
		while (IsWindow(dialog))
		{
			BOOL status = GetMessageW(&message, nullptr, 0, 0);
			if (status <= 0)
			{
				if (status == 0)
					PostQuitMessage(static_cast<int>(message.wParam));
				DestroyWindow(dialog);
				break;
			}
			if (!IsDialogMessageW(dialog, &message))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
		}
		INT_PTR result = state.result;
		if (result == kThemeUiPaletteColorResult)
		{
			chosenColor = state.currentColor;
			return true;
		}
		if (result != kThemeUiPaletteOtherResult)
			return false;

		CHOOSECOLOR otherColor = {};
		if (!ColorPicker(initialColor, dialogOwner, &otherColor, TRUE))
			return false;
		chosenColor = otherColor.rgbResult;
		return true;
	}

}

bool PickThemeUiColor(HWND ownerButton, COLORREF initialColor, COLORREF& chosenColor)
{
	return PickThemeUiColorImpl(ownerButton, initialColor, chosenColor);
}

static LRESULT CALLBACK ColorButtonSubclassProc(HWND button, UINT message,
	WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR)
{
	static constexpr LPCWSTR hotProperty = L"DeskCplColorButtonHot";
	if (message == WM_MOUSEMOVE && IsWindowEnabled(button))
	{
		if (!GetPropW(button, hotProperty))
		{
			SetPropW(button, hotProperty, reinterpret_cast<HANDLE>(1));
			TRACKMOUSEEVENT tracking = { sizeof(tracking), TME_LEAVE, button, 0 };
			TrackMouseEvent(&tracking);
			InvalidateRect(button, nullptr, FALSE);
		}
	}
	else if (message == WM_MOUSELEAVE || message == WM_ENABLE || message == WM_CANCELMODE)
	{
		RemovePropW(button, hotProperty);
		InvalidateRect(button, nullptr, FALSE);
	}
	else if (message == WM_THEMECHANGED)
	{
		InvalidateRect(button, nullptr, TRUE);
	}
	else if (message == WM_NCDESTROY)
	{
		RemovePropW(button, hotProperty);
		RemoveWindowSubclass(button, ColorButtonSubclassProc, subclassId);
	}
	return DefSubclassProc(button, message, wParam, lParam);
}

HWND InitializeThemeUiColorButton(HWND button)
{
	// Retain the ported ThemeUI button and palette, even with older localized
	// templates which still request bitmap buttons.
	LONG_PTR style = GetWindowLongPtrW(button, GWL_STYLE);
	style &= ~(BS_TYPEMASK | BS_BITMAP | BS_ICON);
	SetWindowLongPtrW(button, GWL_STYLE, style | BS_OWNERDRAW);
	SendMessageW(button, BM_SETSTYLE, BS_OWNERDRAW, TRUE);
	SetWindowSubclass(button, ColorButtonSubclassProc, 1, 0);
	return button;
}

// Ported from ref/themeui.dll 6.00.3790.5211:
// CAdvAppearancePage::_DrawButton (7ff5efce5e0), _DrawDownArrow (7ff5efce420).
void DrawThemeUiColorButton(const DRAWITEMSTRUCT& drawItem, COLORREF color)
{
	const UINT state = drawItem.itemState;
	const bool disabled = (state & ODS_DISABLED) != 0;
	const bool pressed = (state & ODS_SELECTED) != 0;
	const bool hot = (state & ODS_HOTLIGHT) != 0 ||
		GetPropW(drawItem.hwndItem, L"DeskCplColorButtonHot") != nullptr;
	const bool focused = (state & ODS_FOCUS) != 0 && !disabled;
	const UINT dpi = GetDpiForWindow(drawItem.hwndItem);
	const int edgeX = GetSystemMetricsForDpi(SM_CXEDGE, dpi);
	const int edgeY = GetSystemMetricsForDpi(SM_CYEDGE, dpi);
	const int dx = max(1, edgeX / 2);
	const int dy = max(1, edgeY / 2);
	const int pixel = max(1, MulDiv(1, dpi, 96));
	RECT content = drawItem.rcItem;
	bool themed = false;
	HTHEME theme = OpenThemeData(drawItem.hwndItem, L"Button");
	if (theme)
	{
		const int themeState = pressed ? PBS_PRESSED : hot ? PBS_HOT :
			disabled ? PBS_DISABLED : focused ? PBS_DEFAULTED : PBS_NORMAL;
		if (IsThemeBackgroundPartiallyTransparent(theme, BP_PUSHBUTTON, themeState))
			DrawThemeParentBackground(drawItem.hwndItem, drawItem.hDC, &drawItem.rcItem);
		if (SUCCEEDED(DrawThemeBackground(theme, drawItem.hDC, BP_PUSHBUTTON,
			themeState, &drawItem.rcItem, nullptr)))
			themed = SUCCEEDED(GetThemeBackgroundContentRect(theme, drawItem.hDC,
				BP_PUSHBUTTON, themeState, &drawItem.rcItem, &content));
		CloseThemeData(theme);
	}
	if (!themed)
	{
		content = drawItem.rcItem;
		DrawEdge(drawItem.hDC, &content, pressed ? EDGE_SUNKEN : EDGE_RAISED,
			BF_RECT | BF_ADJUST);
		if (pressed) OffsetRect(&content, pixel, pixel);
		FillRect(drawItem.hDC, &content, GetSysColorBrush(COLOR_3DFACE));
	}
	if (focused)
	{
		RECT focus = content;
		InflateRect(&focus, -dx, -dy);
		DrawFocusRect(drawItem.hDC, &focus);
	}
	InflateRect(&content, pixel - dx, -edgeY);
	content.left += edgeX;

	// Original arrow geometry, including its embossed disabled state.
	const int anchorX = content.right - edgeX;
	const int arrowY = content.top + (content.bottom - content.top) / 2 - pixel;
	if (disabled)
	{
		for (int row = 0; row < 3; ++row)
		{
			RECT line = { anchorX - (4 - row) * pixel, arrowY + (row + 1) * pixel,
				anchorX + (1 - row) * pixel, arrowY + (row + 2) * pixel };
			FillRect(drawItem.hDC, &line, GetSysColorBrush(COLOR_3DHIGHLIGHT));
		}
	}
	for (int row = 0; row < 3; ++row)
	{
		RECT line = { anchorX - (5 - row) * pixel, arrowY + row * pixel,
			anchorX - row * pixel, arrowY + (row + 1) * pixel };
		FillRect(drawItem.hDC, &line,
			GetSysColorBrush(disabled ? COLOR_3DSHADOW : COLOR_BTNTEXT));
	}
	content.right = anchorX - 5 * pixel;

	// ThemeUI places a thin etched separator before the arrow, then leaves
	// a metrics-based gap between the separator and the black swatch frame.
	InflateRect(&content, -dx, 0);
	DrawEdge(drawItem.hDC, &content, EDGE_ETCHED, BF_RIGHT);
	content.right -= edgeX * 2 + dx;
	if (disabled || content.right <= content.left || content.bottom <= content.top)
		return;
	FrameRect(drawItem.hDC, &content, GetSysColorBrush(COLOR_BTNTEXT));
	InflateRect(&content, -dx, -dy);
	if (content.right <= content.left || content.bottom <= content.top) return;
	HBRUSH brush = CreateSolidBrush(color);
	if (brush)
	{
		FillRect(drawItem.hDC, &content, brush);
		DeleteObject(brush);
	}
}

std::vector<LPWSTR> carouselWallpapers;

struct SlideshowThreadData {
	HWND wnd;
	bool* pRestoreSlideshow;
	IUnknown* iTheme;
};

void SlideshowWorkerThread(void* lpParam)
{
	SlideshowThreadData* data = (SlideshowThreadData*)lpParam;

	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	BOOL isEn = 0;
	auto themeClass = std::make_unique<CTheme>(data->iTheme);
	themeClass->IsSlideshowEnabled(&isEn);

	if (isEn)
	{
		PostMessage(data->wnd, WM_SLIDESHOW_BEGIN, 0, 0);

		// fix this: this pathway only reads single file if i add multiple files from different folders
		ComPtr<ISlideshowSettings> st;
		themeClass->get_SlideshowSettings(&st);
		ComPtr<IWallpaperCollection> wlp;
		st->GetAllMatchingWallpapers(&wlp);
		int count = wlp->GetCount();

		for (int i = 0; i < count; i++)
		{
			LPWSTR path = { 0 };
			wlp->GetWallpaperAt(i, &path);

			PostMessage(data->wnd, WM_ADD_SLIDESHOW_ITEMS, 0, (LPARAM)path);
		}
		*data->pRestoreSlideshow = false;
	}

	CoUninitialize();
}

BOOL CBackgroundDlgProc::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	hListView = GetDlgItem(1202);
	hBackPreview = GetDlgItem(1200);
	hPosCombobox = GetDlgItem(1205);
	InitializeThemeUiColorButton(GetDlgItem(1207));
	backPreviewSize = GetClientSIZE(hBackPreview);
	selCount = 0;
	fWallpaperApply = TRUE;
	selectedTheme->fSlideshowSelection = selectedTheme->wallpaperType == WT_SLIDESHOW;

	if (!currentITheme)
	{
		int cur;
		pThemeManager->GetCurrentTheme(&cur);
		pThemeManager->GetTheme(cur, &currentITheme);
	}

	RECT rect;
	::GetClientRect(hListView, &rect);
	AddColumn(hListView, rect.right - rect.left - 30);

	WCHAR szText[20];
	LoadString(g_hThemeUI, 2022, szText, ARRAYSIZE(szText));

	AddItem(hListView, 0, szText);
	HICON barrierico = LoadIcon(LoadLibraryEx(L"imageres.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_AS_DATAFILE), MAKEINTRESOURCE(1027));
	ImageList_AddIcon(hml, barrierico);
	ListView_SetImageList(hListView, hml, LVSIL_SMALL);
	DestroyIcon(barrierico);

	WCHAR wallpaperdir[MAX_PATH];
	WCHAR windowswaldir[MAX_PATH];
	ExpandEnvironmentStrings(L"%windir%\\Web\\Wallpaper", wallpaperdir, MAX_PATH);
	ExpandEnvironmentStrings(L"%windir%", windowswaldir, MAX_PATH);

	std::vector<LPWSTR> wallpapers;
	LPCWSTR extensions[] = { L".jpg",  L".png", L".bmp", L".jpeg", L".dib", L".gif" };

	// this basically combines wallpaperdir and windowswaldir
	EnumDir(wallpaperdir, extensions, ARRAYSIZE(extensions), wallpapers, TRUE);
	EnumDir(windowswaldir, extensions, ARRAYSIZE(extensions), wallpapers, FALSE);

	// this basically alphabetically sorts the wallpaper file names, for example if you have
	// a few files in windir and a few in web it will put windir files at the end and the web files on top
	// this basically fixes that issue and makes it like how it was in windows xp
	std::sort(wallpapers.begin(), wallpapers.end(), [](LPWSTR a, LPWSTR b) {
		LPCWSTR fileA = PathFindFileNameW(a);
		LPCWSTR fileB = PathFindFileNameW(b);
		int cmpResult = StrCmpLogicalW(fileA, fileB);
		return cmpResult < 0;
		});

	// start with k=1, k=0 is (none)
	int k = 1;
	for (auto path : wallpapers)
	{
		AddItem(hListView, k++, path);
	}

	HINSTANCE hThemeCpl = LoadLibraryEx(L"themecpl.dll", 0, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);

	// combobox with positions of wallpaper
	for (int i = 0; i < 6; i++)
	{
		// cool ms
		if (i == 3) i = 4;
		else if (i == 4) i = 3;

		WCHAR string[10];
		LoadString(hThemeCpl, 500 + (8 - i), string, 10);
		ComboBox_AddString(hPosCombobox, string);

		if (i == 4) i = 3;
		else if (i == 3) i = 4;
	}
	FreeLibrary(hThemeCpl);

	DESKTOP_WALLPAPER_POSITION pos;
	pDesktopWallpaper->GetPosition(&pos);
	ComboBox_SetCurSel(hPosCombobox, pos);

	if (selectedTheme->newColor == NULL)
	{
		WCHAR ws[MAX_PATH] = { 0 };
		SystemParametersInfo(SPI_GETDESKWALLPAPER, MAX_PATH, ws, 0);

		selectedTheme->wallpaperType = WT_PICTURE;
		selectedTheme->wallpaperPath = ws;

		selectedTheme->useDesktopColor = true;
		selectedTheme->newColor = 0xB0000000;
	}

	fInit = TRUE;
	AddMissingWallpapers();
	SelectCurrentWallpaper();

	_UpdateButtonBmp();
	return 0;
}

BOOL CBackgroundDlgProc::OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	pWndPreview = nullptr;
	return 0;
}

BOOL CBackgroundDlgProc::OnBgSizeChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	selectedTheme->posChanged = ComboBox_GetCurSel(hPosCombobox);
	_UpdatePreview(UPDATE_WALLPAPER);

	wallpaperApplyPending = true;
	g_desktopThemeApplyPending = true;
	SetModified(TRUE);
	return 0;
}

BOOL CBackgroundDlgProc::OnBrowse(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	LPWSTR path = NULL;
	std::wstring pictureLabel = LoadDeskString(IDS_ALL_PICTURE_FILES_LABEL);
	constexpr wchar_t picturePatterns[] = L"*.bmp;*.gif;*.jpg;*.jpeg;*.dib;*.png";
	const COMDLG_FILTERSPEC fileTypes[] = {{pictureLabel.c_str(), picturePatterns}};

	IFileDialog* pfd;
	HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
	if (SUCCEEDED(hr))
	{
		// get options
		DWORD dwFlags;
		hr = pfd->GetOptions(&dwFlags);

		// set the file types
		hr = pfd->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes);

		// the first element from the array
		hr = pfd->SetFileTypeIndex(1);

		pfd->SetTitle(LoadDeskString(IDS_BROWSE).c_str());

		// Show the dialog
		hr = pfd->Show(hWnd);

		if (SUCCEEDED(hr))
		{
			IShellItem* psiResult;
			hr = pfd->GetResult(&psiResult);
			if (SUCCEEDED(hr)) {
				hr = psiResult->GetDisplayName(SIGDN_FILESYSPATH, &path);
			}
			pfd->Release();
		}
	}
	else
	{
		wchar_t szFile[MAX_PATH];
		OPENFILENAME ofn = { sizeof(ofn) };
		ofn.hwndOwner = m_hWnd;
		ofn.lpstrFile = szFile;
		ofn.lpstrFile[0] = '\0';
		ofn.nMaxFile = sizeof(szFile);
		std::wstring filter = pictureLabel;
		filter.push_back(L'\0');
		filter += picturePatterns;
		filter.append(2, L'\0');
		ofn.lpstrFilter = filter.c_str();
		ofn.nFilterIndex = 1;
		ofn.lpstrFileTitle = NULL;
		ofn.nMaxFileTitle = 0;
		ofn.lpstrInitialDir = NULL;
		ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

		if (GetOpenFileName(&ofn) == TRUE)
		{
			path = ofn.lpstrFile;
		}
	}
	if (path && lstrlen(path) != 0)
	{
		int inde = AddItem(hListView, ListView_GetItemCount(hListView), path);
		ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED);
		ListView_SetItemState(hListView, inde, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(hListView, inde, FALSE);
	}
	return 0;
}

BOOL CBackgroundDlgProc::OnColorPick(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	// Defer the popup until the button has completed releasing its mouse capture.
	::PostMessageW(m_hWnd, WM_OPEN_COLOR_PALETTE, reinterpret_cast<WPARAM>(hWnd), 0);
	return 0;
}

LRESULT CBackgroundDlgProc::OnOpenColorPalette(UINT, WPARAM wParam, LPARAM, BOOL&)
{
	HWND ownerButton = reinterpret_cast<HWND>(wParam);
	COLORREF chosenColor = GetDeskopColor();
	if (PickThemeUiColor(ownerButton, chosenColor, chosenColor))
	{
		selectedTheme->newColor = chosenColor;
		selectedTheme->fCustomDesktopColorPending = true;

		_UpdateButtonBmp();
		_UpdatePreview(UPDATE_SOLIDCLR);

		wallpaperApplyPending = true;
		g_desktopThemeApplyPending = true;
		SetModified(TRUE);
	}
	return 0;
}

BOOL CBackgroundDlgProc::OnDeskCustomize(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	WCHAR windowsDirectory[MAX_PATH] = {};
	WCHAR rundll32Path[MAX_PATH] = {};
	WCHAR deskCplPath[MAX_PATH] = {};
	WCHAR parameters[MAX_PATH * 2] = {};
	if (!::GetWindowsDirectoryW(windowsDirectory, ARRAYSIZE(windowsDirectory)) ||
		FAILED(StringCchPrintfW(rundll32Path, ARRAYSIZE(rundll32Path),
			L"%s\\System32\\rundll32.exe", windowsDirectory)) ||
		FAILED(StringCchPrintfW(deskCplPath, ARRAYSIZE(deskCplPath),
			L"%s\\System32\\desk.cpl", windowsDirectory)) ||
		FAILED(StringCchPrintfW(parameters, ARRAYSIZE(parameters),
			L"shell32.dll,Control_RunDLL \"%s\",,0", deskCplPath)))
	{
		::MessageBoxW(m_hWnd, LoadDeskString(IDS_DESKTOP_ITEMS_OPEN_ERROR).c_str(),
			LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	HINSTANCE launchResult = ::ShellExecuteW(m_hWnd, L"open", rundll32Path,
		parameters, windowsDirectory, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(launchResult) <= 32)
	{
		WCHAR message[160] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_DESKTOP_ITEMS_OPEN_ERROR_CODE).c_str(),
			reinterpret_cast<INT_PTR>(launchResult));
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
	}
	return 0;
}

LRESULT CBackgroundDlgProc::OnDrawItem(UINT, WPARAM, LPARAM lParam, BOOL& bHandled)
{
	auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
	if (!drawItem || drawItem->CtlID != 1207 || drawItem->CtlType != ODT_BUTTON)
	{
		bHandled = FALSE;
		return 0;
	}
	COLORREF color = selectedTheme && selectedTheme->fCustomDesktopColorPending
		? selectedTheme->newColor : GetDeskopColor();
	DrawThemeUiColorButton(*drawItem, color);
	return TRUE;
}

BOOL CBackgroundDlgProc::OnWallpaperSelection(WPARAM wParam, LPNMHDR nmhdr, BOOL& bHandled)
{
	int count = ListView_GetSelectedCount(hListView);
	LPNMLISTVIEW pnmv = (LPNMLISTVIEW)nmhdr;

	LPWSTR path = GetWallpaperPath(hListView, pnmv->iItem);
	BOOL bValid = PathFileExists(path);

	if (pnmv->uChanged & LVIF_STATE && pnmv->uNewState & LVIS_SELECTED && count == 1)
	{
		::EnableWindow(hPosCombobox, bValid);
		selectedTheme->wallpaperPath = bValid ? path : L"";
		if (!selectedTheme->fSlideshowSelection)
		{
			selectedTheme->wallpaperType = bValid ? WT_PICTURE : WT_NOWALL;
		}
		selCount = 1;
		fWallpaperApply = FALSE;

		carouselWallpapers.clear();
		carouselWallpapers.push_back(path);

		selectedTheme->customWallpaperSelection = !selectionPicker;

		_UpdatePreview(UPDATE_WALLPAPER | UPDATE_SOLIDCLR);
		wallpaperApplyPending = true;
		g_desktopThemeApplyPending = true;
		SetModified(TRUE);

	}
	else if (count > 1 && pnmv->uChanged & LVIF_STATE)
	{
		selectedTheme->wallpaperType = WT_SLIDESHOW;
		fWallpaperApply = TRUE;

		if (pnmv->uNewState & LVIS_SELECTED)
		{
			carouselWallpapers.push_back(path);
			selectedTheme->wallpaperPath = bValid ? path : L"";
			selCount++;
		}
		else if (pnmv->uOldState & LVIS_SELECTED)
		{
			std::erase(carouselWallpapers, path);
			selCount--;
		}

		_UpdatePreview(UPDATE_WALLPAPER | UPDATE_SOLIDCLR);
		wallpaperApplyPending = true;
		g_desktopThemeApplyPending = true;
		SetModified(TRUE);
	}
	else if (pnmv->uOldState & LVIS_SELECTED && selCount > 1)
	{
		std::erase(carouselWallpapers, path);
		selCount--;
	}
	return 0;
}


BOOL CBackgroundDlgProc::OnSettingChange(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	if (!fWallpaperApply && !wallpaperApplyPending)
	{
		AddMissingWallpapers();
		SelectCurrentWallpaper();
	}
	return 0;
}

BOOL CBackgroundDlgProc::OnSlideshowBegin(UINT, WPARAM, LPARAM, BOOL&)
{
	ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED);
	return 0;
}

BOOL CBackgroundDlgProc::OnAddSlideshowItems(UINT, WPARAM, LPARAM lParam, BOOL&)
{
	wchar_t* path = (wchar_t*)lParam;
	// A late restore message must not overwrite a wallpaper the user has just
	// selected while the slideshow list was being populated.
	if (wallpaperApplyPending)
	{
		CoTaskMemFree(path);
		return 0;
	}

	int inde = FindItemByPath(path);
	if (inde == -1)
	{
		inde = AddItem(hListView, ListView_GetItemCount(hListView), path);
	}

	ListView_SetItemState(hListView, inde, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	ListView_EnsureVisible(hListView, inde, FALSE);

	CoTaskMemFree(path);
	fWallpaperApply = FALSE;
	wallpaperApplyPending = false;
	g_desktopThemeApplyPending = false;
	SetModified(FALSE);
	return 0;
}

BOOL CBackgroundDlgProc::OnApply()
{
	const bool themeStateChanged = wallpaperApplyPending ||
		selectedTheme->customWallpaperSelection || selectedTheme->posChanged != -1 ||
		selectedTheme->newColor != 0xB0000000 || selectedTheme->fCustomDesktopColorPending;

	if (selectedTheme->posChanged != -1)
	{
		int index = ComboBox_GetCurSel(hPosCombobox);
		pDesktopWallpaper->SetPosition((DESKTOP_WALLPAPER_POSITION)index);
		selectedTheme->posChanged = -1;
	}
	if (selectedTheme->newColor != 0xB0000000)
	{
		pDesktopWallpaper->SetBackgroundColor(selectedTheme->newColor);
		selectedTheme->newColor = 0xB0000000;
		selectedTheme->fCustomDesktopColorPending = false;
	}
	if (selectedTheme->customWallpaperSelection)
	{
		//selectedTheme->fSlideshowSelection = selectedTheme->wallpaperType == WT_SLIDESHOW;
		pDesktopWallpaper->Enable(selectedTheme->wallpaperType != WT_NOWALL);

		if (selectedTheme->wallpaperType == WT_NOWALL)
		{
			pDesktopWallpaper->SetSlideshow(0);
		}
		else if (selectedTheme->wallpaperType == WT_PICTURE)
		{
			pDesktopWallpaper->SetSlideshow(0);

			pDesktopWallpaper->SetWallpaper(NULL, selectedTheme->wallpaperPath.c_str());
			fWallpaperApply = TRUE;
		}
		else if (selectedTheme->wallpaperType == WT_SLIDESHOW)
		{
			fWallpaperApply = TRUE;

			std::vector<PIDLIST_ABSOLUTE> pidlList;

			for (LPWSTR path : carouselWallpapers)
			{
				IShellItem* pShellItem = NULL;
				PIDLIST_ABSOLUTE pidlNew = nullptr;
				SHCreateItemFromParsingName(path, NULL, IID_PPV_ARGS(&pShellItem));
				HRESULT hr = SHGetIDListFromObject(pShellItem, &pidlNew);
				if (SUCCEEDED(hr))
				{
					pidlList.push_back(pidlNew);
				}
			}

			IShellItemArray* ppNewArray;
			SHCreateShellItemArrayFromIDLists(pidlList.size(), const_cast<LPCITEMIDLIST*>(pidlList.data()), &ppNewArray);
			pDesktopWallpaper->SetSlideshow(ppNewArray);

			selectedTheme->wallpaperPath = carouselWallpapers[0];
		}
		selectedTheme->customWallpaperSelection = false;
	}

	selectedTheme->updateWallThemesPg = true;
	selectedTheme->useDesktopColor = true;

	_UpdatePreview(UPDATE_WALLPAPER | UPDATE_SOLIDCLR);
	_UpdateButtonBmp();

	// Wallpaper APIs update the live desktop directly. Ask Theme Manager to
	// snapshot that applied state too, so its Unsaved/Modified theme stays in
	// sync with the actual wallpaper and position.
	if (themeStateChanged && pThemeManager)
	{
		HRESULT themeUpdate = pThemeManager->UpdateCustomTheme();
		if (FAILED(themeUpdate))
		{
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_WALLPAPER_DERIVATIVE_SYNC_ERROR).c_str(),
				themeUpdate);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONWARNING);
			SetModified(TRUE);
			return 0;
		}
		pThemeManager->Refresh();
		ForgetSavedThemePathForCurrentTheme();
	}

	wallpaperApplyPending = false;
	g_desktopThemeApplyPending = false;
	SetModified(FALSE);
	return 0;
}

BOOL CBackgroundDlgProc::OnSetActive()
{
	selectionPicker = false;
	_UpdateButtonBmp();
	if (!selectedTheme->customWallpaperSelection && !fWallpaperApply)
	{
		AddMissingWallpapers();
		
		if (!fInit) SelectCurrentWallpaper();
		else fInit = FALSE;

		if (selectedTheme->posChanged == -1)
		{
			// update wallpaper position 
			DESKTOP_WALLPAPER_POSITION pos;
			pDesktopWallpaper->GetPosition(&pos);
			ComboBox_SetCurSel(hPosCombobox, pos);
		}
	}
	_TerminateProcess(pi);
	return 0;
}


#pragma region ListView helpers
int CBackgroundDlgProc::AddItem(HWND hListView, int rowIndex, LPCWSTR text)
{
	if (text)
	{
		// gimmick
		SHFILEINFO sh{};
		SHGetFileInfo(text, FILE_ATTRIBUTE_NORMAL, &sh, sizeof(SHFILEINFO), SHGFI_ICON);
		ImageList_AddIcon(hml, sh.hIcon);
	}

	// why is winapi so ass
	WCHAR displayName[MAX_PATH] = {};
	StringCchCopyW(displayName, ARRAYSIZE(displayName), PathFindFileNameW(text));
	PathRemoveExtensionW(displayName);

	LVITEM lvItem = { 0 };
	lvItem.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
	lvItem.iItem = rowIndex;
	lvItem.iSubItem = 0;
	lvItem.iImage = rowIndex;
	lvItem.pszText = displayName;
	lvItem.lParam = (LPARAM)StrDup(text);

	return ListView_InsertItem(hListView, &lvItem);
}

int CBackgroundDlgProc::AddColumn(HWND hListView, int width)
{
	LVCOLUMN lvc = { 0 };
	lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
	lvc.cx = width;
	lvc.pszText = (LPWSTR)L"";
	lvc.iSubItem = 0;
	return ListView_InsertColumn(hListView, 0, &lvc);
}

LPWSTR CBackgroundDlgProc::GetWallpaperPath(HWND hListView, int iIndex)
{
	LVITEM item = { 0 };
	item.iItem = iIndex;
	item.iSubItem = 0;
	item.cchTextMax = 256;
	item.mask = LVIF_PARAM;
	ListView_GetItem(hListView, &item);
	return (LPWSTR)item.lParam;
}

int CBackgroundDlgProc::FindItemByPath(LPCWSTR path)
{
	if (!path || !*path)
	{
		return -1;
	}

	for (int i = 0; i < ListView_GetItemCount(hListView); ++i)
	{
		LPCWSTR itemPath = GetWallpaperPath(hListView, i);
		if (itemPath && StrCmpIW(itemPath, path) == 0)
		{
			return i;
		}
	}

	return -1;
}
#pragma endregion

void CBackgroundDlgProc::AddMissingWallpapers()
{
	if (!selectedTheme->wallpaperPath.empty() && FindItemByPath(selectedTheme->wallpaperPath.c_str()) == -1)
	{
		if (PathFileExists(selectedTheme->wallpaperPath.c_str()))
		{
			AddItem(hListView, ListView_GetItemCount(hListView), selectedTheme->wallpaperPath.c_str());
		}
	}
}

void CBackgroundDlgProc::SelectCurrentWallpaper()
{
	::EnableWindow(hPosCombobox, selectedTheme->wallpaperType != WT_NOWALL);

	int index = 0;
	if (selectedTheme->wallpaperType != WT_NOWALL)
	{
		index = FindItemByPath(selectedTheme->wallpaperPath.c_str());
		if (index == -1)
		{
			index = 0;
		}
	}

	ListView_SetItemState(hListView, -1, 0, LVIS_SELECTED);
	ListView_SetItemState(hListView, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
	ListView_EnsureVisible(hListView, index, FALSE);
	
	if (selectedTheme->wallpaperType == WT_SLIDESHOW)
	{
		// select slideshow wallpapers on a different thread, since its expensive
		SlideshowThreadData* data = new SlideshowThreadData();
		data->wnd = m_hWnd;
		data->iTheme = currentITheme;
		data->pRestoreSlideshow = &selectedTheme->fSlideshowSelection;
		_beginthread(SlideshowWorkerThread, 0, data);
	}

	wallpaperApplyPending = false;
	g_desktopThemeApplyPending = false;
	SetModified(FALSE);
}

void CBackgroundDlgProc::_UpdateButtonBmp()
{
	::InvalidateRect(GetDlgItem(1207), nullptr, TRUE);
}

void CBackgroundDlgProc::_UpdatePreview(UINT uFlags)
{
	HBITMAP bmp;
	if (!pWndPreview)
	{
		pWndPreview = Make<CWindowPreview>(backPreviewSize, nullptr, 0, PAGETYPE::PT_BACKGROUND, nullptr, GetDpiForWindow(m_hWnd));
		pWndPreview->GetPreviewImage(&bmp);

	}
	else
	{
		pWndPreview->GetUpdatedPreviewImage(nullptr, nullptr, &bmp, uFlags);
	}
	SetBitmap(hBackPreview, bmp);
}
