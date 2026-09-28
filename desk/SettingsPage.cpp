#include "pch.h"
#include "SettingsPage.h"
#include "MonitorArrangement.h"

#pragma comment(lib, "Msimg32.lib")
#include "helper.h"
#include "desk.h"
#include "ThemeChngDlg.h"

static int CALLBACK AdvancedSheetFontCallback(HWND hwnd, UINT message, LPARAM)
{
	if (message == PSCB_INITIALIZED)
		ApplySystemDialogFont(hwnd);
	return 0;
}

#define WM_CUSTOM_DPI_CHANGED (WM_APP + 0x341)
#define MONITOR_TOOLTIP_TIMER 0x4D31
#define MONITOR_IDENTIFY_TIMER 0x4D32

// This is a deskn preference, not a change to Windows' global mode pruning.
// EnumDisplaySettingsExW already distinguishes monitor-compatible modes from
// all modes reported by the display driver through EDS_RAWMODE.
static constexpr WCHAR kDisplayModePreferencesKey[] = L"Software\\deskn\\DisplayModes";

static void GetDisplayModePreferenceName(LPCWSTR deviceName, LPCWSTR monitorInstanceId,
	WCHAR (&name)[32])
{
	// Include the monitor identity when available so moving a different display
	// onto the same adapter does not inherit an unsafe choice accidentally.
	const LPCWSTR identity = monitorInstanceId && monitorInstanceId[0] ?
		monitorInstanceId : deviceName;
	ULONGLONG hash = 14695981039346656037ULL;
	for (const WCHAR* character = identity ? identity : L""; *character; ++character)
	{
		WCHAR normalized = *character >= L'a' && *character <= L'z' ?
			*character - (L'a' - L'A') : *character;
		hash = (hash ^ normalized) * 1099511628211ULL;
	}
	StringCchPrintfW(name, ARRAYSIZE(name), L"Hide_%016I64X", hash);
}

static bool ReadHideUnsupportedModes(LPCWSTR deviceName, LPCWSTR monitorInstanceId)
{
	if (!deviceName || !deviceName[0]) return true;
	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kDisplayModePreferencesKey, 0,
		KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return true;
	WCHAR name[32] = {};
	GetDisplayModePreferenceName(deviceName, monitorInstanceId, name);
	DWORD value = 1;
	DWORD type = 0;
	DWORD size = sizeof(value);
	const LSTATUS status = RegQueryValueExW(key, name, nullptr, &type,
		reinterpret_cast<LPBYTE>(&value), &size);
	RegCloseKey(key);
	return status != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value) || value != 0;
}

static LSTATUS WriteHideUnsupportedModes(LPCWSTR deviceName,
	LPCWSTR monitorInstanceId, bool hide)
{
	if (!deviceName || !deviceName[0]) return ERROR_INVALID_PARAMETER;
	HKEY key = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kDisplayModePreferencesKey,
		0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (status != ERROR_SUCCESS) return status;
	WCHAR name[32] = {};
	GetDisplayModePreferenceName(deviceName, monitorInstanceId, name);
	const DWORD value = hide ? 1 : 0;
	status = RegSetValueExW(key, name, 0, REG_DWORD,
		reinterpret_cast<const BYTE*>(&value), sizeof(value));
	RegCloseKey(key);
	return status;
}

// Make room for XP's taller single-monitor illustration without changing
// the multi-monitor arrangement layout or the bottom action buttons.
static constexpr int kSingleMonitorModeControls[] =
{ 1820, 1800, 1811, 1818, 1808, 1815, 1816, 1814, 1817, 1807, 1813 };

static void EnsureMonitorDisplayName(LPWSTR name, size_t count)
{
	if (!name || count == 0) return;
	for (LPCWSTR character = name; *character; ++character)
		if (*character != L' ' && *character != L'\t' && *character != L'\r' && *character != L'\n')
			return; // Keep the actual Windows/EDID name whenever available.
	// Virtual displays and monitors without EDID can have no child monitor
	// device at all. Use XP's existing localized fallback, not a made-up model.
	StringCchCopyW(name, count, LoadDeskString(IDS_DEFAULT_MONITOR).c_str());
}

static constexpr int kWindowsDpiScaleSteps[] =
	{ 100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500 };

static constexpr COLORREF MONITOR_IDENTIFY_TRANSPARENT = RGB(255, 0, 255);
static const WCHAR MONITOR_IDENTIFY_CLASS[] = L"DeskCplMonitorIdentifyOverlay";

static bool GetDeviceInstanceIdForMonitorInterface(LPCWSTR interfacePath,
	LPWSTR instanceId, size_t instanceIdCount)
{
	if (!interfacePath || !interfacePath[0] || !instanceId || instanceIdCount == 0)
		return false;
	instanceId[0] = L'\0';
	HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MONITOR,
		nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devices == INVALID_HANDLE_VALUE)
		return false;
	SP_DEVICE_INTERFACE_DATA interfaceData = {};
	interfaceData.cbSize = sizeof(interfaceData);
	bool success = false;
	if (SetupDiOpenDeviceInterfaceW(devices, interfacePath, 0, &interfaceData))
	{
		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0,
			&requiredSize, nullptr);
		if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && requiredSize >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
		{
			std::vector<BYTE> detailBuffer(requiredSize);
			auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
			detail->cbSize = sizeof(*detail);
			SP_DEVINFO_DATA deviceInfo = {};
			deviceInfo.cbSize = sizeof(deviceInfo);
			if (SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail,
					requiredSize, nullptr, &deviceInfo))
			{
				DWORD chars = 0;
				success = SetupDiGetDeviceInstanceIdW(devices, &deviceInfo,
					instanceId, static_cast<DWORD>(instanceIdCount), &chars) != FALSE;
			}
		}
	}
	SetupDiDestroyDeviceInfoList(devices);
	return success;
}

static bool GetDeviceInstanceIdForDisplayAdapter(LPCWSTR displayDeviceName,
	LPWSTR instanceId, size_t instanceIdCount)
{
	if (!displayDeviceName || !displayDeviceName[0] || !instanceId || instanceIdCount == 0)
		return false;
	instanceId[0] = L'\0';
	WCHAR pnpId[512] = {};
	WCHAR adapterDescription[512] = {};
	for (DWORD i = 0;; ++i)
	{
		DISPLAY_DEVICEW display = {};
		display.cb = sizeof(display);
		if (!EnumDisplayDevicesW(nullptr, i, &display, 0))
			break;
		if (StrCmpI(display.DeviceName, displayDeviceName) == 0 && display.DeviceID[0])
		{
			StringCchCopyW(pnpId, ARRAYSIZE(pnpId), display.DeviceID);
			StringCchCopyW(adapterDescription, ARRAYSIZE(adapterDescription), display.DeviceString);
			break;
		}
	}
	if (!pnpId[0])
		return false;
	HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr,
		DIGCF_PRESENT | DIGCF_ALLCLASSES);
	if (devices == INVALID_HANDLE_VALUE)
		return false;
	SP_DEVINFO_DATA deviceInfo = {};
	deviceInfo.cbSize = sizeof(deviceInfo);
	bool success = SetupDiOpenDeviceInfoW(devices, pnpId, nullptr, 0, &deviceInfo) &&
		SetupDiGetDeviceInstanceIdW(devices, &deviceInfo, instanceId,
			static_cast<DWORD>(instanceIdCount), nullptr) != FALSE;
	if (!success)
	{
		// Some display drivers expose the hardware ID in DISPLAY_DEVICE.DeviceID
		// rather than the full PnP instance ID. Resolve that ID against the
		// present devnodes, preferring a matching device description and refusing
		// an ambiguous hardware-ID-only match.
		SP_DEVINFO_DATA match = {};
		bool foundMatch = false;
		bool ambiguousMatch = false;
		for (DWORD index = 0;; ++index)
		{
			SP_DEVINFO_DATA candidate = {};
			candidate.cbSize = sizeof(candidate);
			if (!SetupDiEnumDeviceInfo(devices, index, &candidate)) break;
			BYTE hardwareIds[4096] = {};
			DWORD propertyType = 0, required = 0;
			if (!SetupDiGetDeviceRegistryPropertyW(devices, &candidate, SPDRP_HARDWAREID,
					&propertyType, hardwareIds, sizeof(hardwareIds), &required) ||
				propertyType != REG_MULTI_SZ)
				continue;
			bool hardwareIdMatches = false;
			for (LPCWSTR hardwareId = reinterpret_cast<LPCWSTR>(hardwareIds);
				*hardwareId; hardwareId += lstrlenW(hardwareId) + 1)
			{
				if (StrCmpIW(hardwareId, pnpId) == 0)
				{
					hardwareIdMatches = true;
					break;
				}
			}
			if (!hardwareIdMatches) continue;

			WCHAR description[512] = {};
			if (SetupDiGetDeviceRegistryPropertyW(devices, &candidate, SPDRP_DEVICEDESC,
					&propertyType, reinterpret_cast<PBYTE>(description), sizeof(description), nullptr) &&
				StrCmpIW(description, adapterDescription) == 0)
			{
				match = candidate;
				foundMatch = true;
				ambiguousMatch = false;
				break;
			}
			if (foundMatch)
				ambiguousMatch = true;
			else
			{
				match = candidate;
				foundMatch = true;
			}
		}
		if (foundMatch && !ambiguousMatch)
			success = SetupDiGetDeviceInstanceIdW(devices, &match, instanceId,
				static_cast<DWORD>(instanceIdCount), nullptr) != FALSE;
	}
	SetupDiDestroyDeviceInfoList(devices);
	return success;
}

static bool ShowDeviceManagerProperties(HWND owner, LPCWSTR instanceId)
{
	if (!instanceId || !instanceId[0])
		return false;
	std::wstring parameters = L"devmgr.dll,DeviceProperties_RunDLL /DeviceID \"";
	parameters += instanceId;
	parameters += L"\"";
	HINSTANCE result = ShellExecuteW(owner, L"open", L"rundll32.exe",
		parameters.c_str(), nullptr, SW_SHOWNORMAL);
	return reinterpret_cast<INT_PTR>(result) > 32;
}

static LRESULT CALLBACK MonitorIdentifyWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (message == WM_NCCREATE)
	{
		const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	if (message == WM_PAINT)
	{
		PAINTSTRUCT paint = {};
		HDC dc = ::BeginPaint(hwnd, &paint);
		RECT client = {};
		::GetClientRect(hwnd, &client);
		HBRUSH keyBrush = ::CreateSolidBrush(MONITOR_IDENTIFY_TRANSPARENT);
		::FillRect(dc, &client, keyBrush);
		::DeleteObject(keyBrush);

		WCHAR number[16] = {};
		StringCchPrintfW(number, ARRAYSIZE(number), L"%Id",
			static_cast<INT_PTR>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA)));
		const int width = max(1L, client.right - client.left);
		const int height = max(1L, client.bottom - client.top);
		const int digits = static_cast<int>(wcslen(number));
		const int fontHeight = -max(32, min(MulDiv(height, 84, 100),
			MulDiv(width, 150, max(1, digits) * 100)));
		LOGFONTW fontInfo = {};
		fontInfo.lfHeight = fontHeight;
		fontInfo.lfWeight = FW_BOLD;
		fontInfo.lfQuality = ANTIALIASED_QUALITY;
		HFONT font = ::CreateFontIndirectW(&fontInfo);
		HGDIOBJ oldFont = font ? ::SelectObject(dc, font) : nullptr;
		::SetBkMode(dc, TRANSPARENT);
		RECT text = client;
		::SetTextColor(dc, GetSysColor(COLOR_WINDOWFRAME));
		for (int y = -2; y <= 2; ++y)
		{
			for (int x = -2; x <= 2; ++x)
			{
				if (abs(x) != 2 && abs(y) != 2) continue;
				RECT outline = text;
				::OffsetRect(&outline, x, y);
				::DrawTextW(dc, number, -1, &outline,
					DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			}
		}
	::SetTextColor(dc, RGB(255, 255, 255));
		::DrawTextW(dc, number, -1, &text,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
		if (oldFont) ::SelectObject(dc, oldFont);
		if (font) ::DeleteObject(font);
		::EndPaint(hwnd, &paint);
		return 0;
	}
	if (message == WM_ERASEBKGND)
	{
		RECT client = {};
		::GetClientRect(hwnd, &client);
		HBRUSH keyBrush = ::CreateSolidBrush(MONITOR_IDENTIFY_TRANSPARENT);
		::FillRect(reinterpret_cast<HDC>(wParam), &client, keyBrush);
		::DeleteObject(keyBrush);
		return 1;
	}
	if (message == WM_TIMER && wParam == MONITOR_IDENTIFY_TIMER)
	{
		::KillTimer(hwnd, MONITOR_IDENTIFY_TIMER);
		::DestroyWindow(hwnd);
		return 0;
	}
	if (message == WM_NCHITTEST) return HTTRANSPARENT;
	if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
	return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

static HWND CreateMonitorIdentifyOverlay(const RECT& monitor, int number)
{
	static ATOM windowClass = 0;
	if (!windowClass)
	{
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = MonitorIdentifyWndProc;
		wc.hInstance = g_hinst;
		wc.lpszClassName = MONITOR_IDENTIFY_CLASS;
		windowClass = ::RegisterClassExW(&wc);
		if (!windowClass && ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
			windowClass = 1;
	}
	if (!windowClass) return nullptr;
	HWND hwnd = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
		MONITOR_IDENTIFY_CLASS, L"", WS_POPUP,
		monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
		nullptr, nullptr, g_hinst, reinterpret_cast<LPVOID>(static_cast<INT_PTR>(number)));
	if (!hwnd) return nullptr;
	if (!::SetLayeredWindowAttributes(hwnd, MONITOR_IDENTIFY_TRANSPARENT, 0, LWA_COLORKEY) ||
		!::SetTimer(hwnd, MONITOR_IDENTIFY_TIMER, 5000, nullptr))
	{
		::DestroyWindow(hwnd);
		return nullptr;
	}
	::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
	::UpdateWindow(hwnd);
	return hwnd;
}

class CCustomDpiDlg : public ATL::CDialogImpl<CCustomDpiDlg>
{
public:
	enum { IDD = IDD_CUSTOMDPIDLG };
	int minPercent = 100;
	int maxPercent = 200;
	int percent = 100;
	int dpiX = 96;
	int dpiY = 96;
	int sampleFontPercent = -1;
	std::vector<int> supportedPercents;
	HFONT sampleFont = nullptr;
	HFONT rulerFont = nullptr;

	~CCustomDpiDlg()
	{
		if (sampleFont) ::DeleteObject(sampleFont);
		if (rulerFont) ::DeleteObject(rulerFont);
	}

	BEGIN_MSG_MAP(CCustomDpiDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CUSTOM_DPI_CHANGED, OnDpiChanged)
		COMMAND_HANDLER(IDC_CUSTOMDPIPERCENT, CBN_SELCHANGE, OnPercentChanged)
		COMMAND_ID_HANDLER(IDOK, OnClose)
		COMMAND_ID_HANDLER(IDCANCEL, OnClose)
	END_MSG_MAP()

	static LRESULT CALLBACK RulerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		CCustomDpiDlg* self = reinterpret_cast<CCustomDpiDlg*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
		if (msg == WM_PAINT)
		{
			PAINTSTRUCT ps = {};
			HDC dc = ::BeginPaint(hwnd, &ps);
			RECT rc = {};
			::GetClientRect(hwnd, &rc);
			FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
			const int width = max(1L, rc.right - rc.left);
			const int height = max(1L, rc.bottom - rc.top);
			const int rulerLeft = 1;
			const int rulerRight = max(rulerLeft + 1, width - 2);
			const int rulerWidth = rulerRight - rulerLeft;
			const int pixelsPerInch = max(1, MulDiv(self->percent, self->dpiX, 100));
			HPEN pen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_3DSHADOW));
			HPEN oldPen = (HPEN)SelectObject(dc, pen);
			MoveToEx(dc, rulerLeft, 0, nullptr);
			LineTo(dc, rulerRight, 0);
			for (int step = 0;; ++step)
			{
				int x = rulerLeft + MulDiv(step, pixelsPerInch, 4);
				if (x > rulerRight) break;
				int tickHeight = (step % 4 == 0) ? min(25, height * 42 / 100) :
					(step % 2 == 0 ? min(15, height * 26 / 100) : min(8, height * 14 / 100));
				MoveToEx(dc, x, 1, nullptr);
				LineTo(dc, x, tickHeight);
			}
			SelectObject(dc, oldPen);
			DeleteObject(pen);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
			HFONT oldFont = self->rulerFont ? (HFONT)SelectObject(dc, self->rulerFont) : nullptr;
			for (int i = 0;; ++i)
			{
				int x = rulerLeft + i * pixelsPerInch;
				if (x > rulerRight) break;
				WCHAR label[2] = { (WCHAR)(L'0' + i), 0 };
				RECT text = { x - 8, height - 17, x + 8, height - 1 };
				DrawTextW(dc, label, 1, &text, DT_CENTER | DT_TOP | DT_SINGLELINE);
			}
			if (oldFont) SelectObject(dc, oldFont);
			::EndPaint(hwnd, &ps);
			return 0;
		}
		if (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE)
		{
			if (msg == WM_LBUTTONDOWN || (wParam & MK_LBUTTON))
			{
				RECT rc = {};
				::GetClientRect(hwnd, &rc);
				int x = GET_X_LPARAM(lParam);
				int width = max(1L, rc.right - rc.left - 10);
				if (self->supportedPercents.empty()) return 0;
				int index = MulDiv(max(0, min(width, x - 5)),
					static_cast<int>(self->supportedPercents.size()) - 1, width);
				index = max(0, min(static_cast<int>(self->supportedPercents.size()) - 1, index));
				self->percent = self->supportedPercents[index];
				::SendMessageW(self->m_hWnd, WM_CUSTOM_DPI_CHANGED, self->percent, 0);
				::InvalidateRect(hwnd, nullptr, FALSE);
				if (msg == WM_LBUTTONDOWN) ::SetCapture(hwnd);
				return 0;
			}
		}
		if (msg == WM_LBUTTONUP)
		{
			if (::GetCapture() == hwnd) ::ReleaseCapture();
			return 0;
		}
		return ::DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
	{
		ApplySystemDialogFont(m_hWnd);
		HWND combo = ::GetDlgItem(m_hWnd, IDC_CUSTOMDPIPERCENT);
		for (int value : kWindowsDpiScaleSteps)
		{
			if (value < minPercent || value > maxPercent)
				continue;
			supportedPercents.push_back(value);
			WCHAR text[16] = {};
			StringCchPrintfW(text, ARRAYSIZE(text), L"%d%%", value);
			int index = ComboBox_AddString(combo, text);
			ComboBox_SetItemData(combo, index, value);
		}
		if (supportedPercents.empty())
		{
			EndDialog(IDCANCEL);
			return TRUE;
		}
		percent = NearestSupportedPercent(percent);
		SelectPercent(percent);
		HWND ruler = ::GetDlgItem(m_hWnd, IDC_CUSTOMDPIRULER);
		::SetWindowLongPtrW(ruler, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
		::SetWindowLongPtrW(ruler, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RulerProc));
		HDC screen = ::GetDC(nullptr);
		dpiX = screen ? max(96, GetDeviceCaps(screen, LOGPIXELSX)) : 96;
		dpiY = screen ? max(96, GetDeviceCaps(screen, LOGPIXELSY)) : 96;
		if (screen) ::ReleaseDC(nullptr, screen);
		rulerFont = ::CreateFontW(-MulDiv(8, dpiY, 72), 0, 0, 0, FW_NORMAL,
			FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
			LoadDeskString(IDS_DPI_SAMPLE_FONT).c_str());
		UpdateSample();
		auto sx = [this](int value) { return MulDiv(value, dpiX, 96); };
		auto sy = [this](int value) { return MulDiv(value, dpiY, 96); };
		::SetWindowPos(m_hWnd, nullptr, 0, 0, sx(383), sy(276),
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		auto place = [&](int id, int x, int y, int width, int height)
		{
			::SetWindowPos(::GetDlgItem(m_hWnd, id), nullptr, sx(x), sy(y),
				sx(width), sy(height), SWP_NOZORDER | SWP_NOACTIVATE);
		};
		place(IDC_CUSTOMDPIDESC, 22, 10, 340, 30);
		place(IDC_CUSTOMDPISCALELABEL, 22, 43, 180, 14);
		place(IDC_CUSTOMDPIPERCENT, 221, 43, 66, 21);
		place(IDC_CUSTOMDPIRULER, 23, 70, 327, 58);
		place(IDC_CUSTOMDPISAMPLE, 23, 136, 340, 64);
		place(IDOK, 214, 210, 73, 21);
		place(IDCANCEL, 294, 210, 73, 21);
		CenterWindow(::GetParent(m_hWnd));
		return TRUE;
	}

	LRESULT OnPercentChanged(WORD, WORD, HWND combo, BOOL&)
	{
		int index = ComboBox_GetCurSel(combo);
		if (index >= 0)
		{
			percent = (int)ComboBox_GetItemData(combo, index);
			UpdateSample();
			::InvalidateRect(::GetDlgItem(m_hWnd, IDC_CUSTOMDPIRULER), nullptr, FALSE);
		}
		return 0;
	}

	LRESULT OnDpiChanged(UINT, WPARAM value, LPARAM, BOOL&)
	{
		percent = (int)value;
		SelectPercent(percent);
		UpdateSample();
		::InvalidateRect(::GetDlgItem(m_hWnd, IDC_CUSTOMDPIRULER), nullptr, FALSE);
		return 0;
	}

	LRESULT OnClose(WORD, WORD id, HWND, BOOL&)
	{
		EndDialog(id);
		return 0;
	}

private:
	int NearestSupportedPercent(int value) const
	{
		int nearest = supportedPercents.front();
		int nearestDistance = abs(value - nearest);
		for (int candidate : supportedPercents)
		{
			const int distance = abs(value - candidate);
			if (distance < nearestDistance)
			{
				nearest = candidate;
				nearestDistance = distance;
			}
		}
		return nearest;
	}

	void SelectPercent(int value)
	{
		HWND combo = ::GetDlgItem(m_hWnd, IDC_CUSTOMDPIPERCENT);
		for (int i = 0; i < ComboBox_GetCount(combo); ++i)
		{
			if ((int)ComboBox_GetItemData(combo, i) == value)
			{
				ComboBox_SetCurSel(combo, i);
				return;
			}
		}
		percent = NearestSupportedPercent(value);
		for (int i = 0; i < ComboBox_GetCount(combo); ++i)
		{
			if ((int)ComboBox_GetItemData(combo, i) == percent)
			{
				ComboBox_SetCurSel(combo, i);
				return;
			}
		}
	}

	void UpdateSample()
	{
		if (sampleFontPercent != percent)
		{
			HFONT newFont = ::CreateFontW(-MulDiv(10 * percent, dpiY, 7200), 0, 0, 0,
				FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
				LoadDeskString(IDS_DPI_SAMPLE_FONT).c_str());
			if (newFont)
			{
				HFONT oldFont = sampleFont;
				sampleFont = newFont;
				sampleFontPercent = percent;
				::SendMessageW(::GetDlgItem(m_hWnd, IDC_CUSTOMDPISAMPLE), WM_SETFONT,
					reinterpret_cast<WPARAM>(sampleFont), TRUE);
				if (oldFont) ::DeleteObject(oldFont);
			}
		}
		WCHAR text[80] = {};
		StringCchPrintfW(text, ARRAYSIZE(text),
			LoadDeskString(IDS_DPI_SAMPLE_FORMAT).c_str(),
			LoadDeskString(IDS_DPI_SAMPLE_FONT).c_str(), MulDiv(96, percent, 100));
		::SetDlgItemTextW(m_hWnd, IDC_CUSTOMDPISAMPLE, text);
	}
};

class CGeneralPage : public WTL::CPropertyPageImpl<CGeneralPage>
{
public:
	 enum { IDD = IDD_GENERALDLG };
	 int dpiPercent = 100;
	 int appliedDpiPercent = 100;
	 bool dpiSettingPending = false;

	 BEGIN_MSG_MAP(CGeneralPage)
		 MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		 COMMAND_HANDLER(2031, CBN_SELCHANGE, OnDpiSelection)
		 CHAIN_MSG_MAP(WTL::CPropertyPageImpl<CGeneralPage>)
	 END_MSG_MAP()

	LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
	{
		ApplySystemDialogFont(m_hWnd);
		 HWND dpi = ::GetDlgItem(m_hWnd, 2031);
		LSTATUS dpiReadStatus = ERROR_SUCCESS;
		LPCWSTR dpiReadFailureStage = L"DPI setting read";
		if (!ReadDpiPercent(dpiPercent, dpiReadStatus, &dpiReadFailureStage))
		{
			::EnableWindow(dpi, FALSE);
			WCHAR message[256] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_DPI_READ_ERROR).c_str(),
				dpiReadFailureStage, dpiReadStatus);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return TRUE;
		}
		int normalIndex = ComboBox_AddString(dpi,
			LoadDeskString(IDS_NORMAL_DPI).c_str());
		ComboBox_SetItemData(dpi, normalIndex, 100);
		int largeIndex = ComboBox_AddString(dpi,
			LoadDeskString(IDS_LARGE_DPI).c_str());
		ComboBox_SetItemData(dpi, largeIndex, 125);
		int customIndex = ComboBox_AddString(dpi,
			LoadDeskString(IDS_CUSTOM_DPI).c_str());
		ComboBox_SetItemData(dpi, customIndex, 0);
		appliedDpiPercent = dpiPercent;
		SelectDpiChoice();
		UpdateDpiDescription();
		 ::CheckDlgButton(m_hWnd, 2034, BST_CHECKED);
		 return TRUE;
	 }

	 LRESULT OnDpiSelection(WORD, WORD, HWND combo, BOOL&)
	 {
		const int selection = ComboBox_GetCurSel(combo);
		const int selectedPercent = selection >= 0 ?
			static_cast<int>(ComboBox_GetItemData(combo, selection)) : 0;
		if (selectedPercent == 0)
		{
			CCustomDpiDlg dialog;
			dialog.minPercent = 100;
			dialog.maxPercent = 500;
			dialog.percent = dpiPercent;
			if (dialog.DoModal(m_hWnd) == IDOK)
			{
				dpiPercent = dialog.percent;
			}
			else
			{
				SelectDpiChoice();
				return 0;
			}
		}
		else
		{
			dpiPercent = selectedPercent;
		}

		UpdateDpiDescription();
		// Stage the new value on this Advanced property sheet. PersistDpiSetting
		// runs only when the user presses this sheet's Apply button.
		dpiSettingPending = dpiPercent != appliedDpiPercent ||
			!IsDpiSettingPersisted(dpiPercent);
		SetModified(dpiSettingPending ? TRUE : FALSE);
		if (dpiSettingPending)
		{
			::MessageBoxW(m_hWnd,
				LoadDeskString(IDS_DPI_RESTART_NOTICE).c_str(),
				LoadDeskString(IDS_CHANGE_DPI_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
		}
		return 0;
	 }

	 int OnApply()
	 {
		if (!dpiSettingPending)
		{
			return PSNRET_NOERROR;
		}

		LSTATUS status = ERROR_SUCCESS;
		if (!PersistDpiSetting(dpiPercent, status))
			return ShowApplyError(status);
		appliedDpiPercent = dpiPercent;
		dpiSettingPending = false;
		SetModified(FALSE);
		return PSNRET_NOERROR;
	 }

private:
	static LSTATUS ReadDesktopDword(HKEY key, LPCWSTR valueName, DWORD& value,
		bool& exists)
	{
		DWORD type = 0, size = sizeof(value);
		LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type,
			reinterpret_cast<BYTE*>(&value), &size);
		if (status == ERROR_FILE_NOT_FOUND)
		{
			exists = false;
			return ERROR_SUCCESS;
		}
		if (status != ERROR_SUCCESS)
			return status;
		if (type != REG_DWORD || size != sizeof(value))
			return ERROR_INVALID_DATA;
		exists = true;
		return ERROR_SUCCESS;
	}

	static int NearestDpiPercent(int percent)
	{
		int nearest = kWindowsDpiScaleSteps[0];
		int distance = abs(percent - nearest);
		for (int candidate : kWindowsDpiScaleSteps)
		{
			const int candidateDistance = abs(percent - candidate);
			if (candidateDistance < distance)
			{
				nearest = candidate;
				distance = candidateDistance;
			}
		}
		return nearest;
	}

	static bool ReadDpiPercent(int& percent, LSTATUS& status,
		LPCWSTR* failedStage = nullptr)
	{
		static const WCHAR desktopPath[] = L"Control Panel\\Desktop";
		HKEY desktopKey = nullptr;
		if (failedStage) *failedStage = L"Desktop registry open";
		status = RegOpenKeyExW(HKEY_CURRENT_USER, desktopPath, 0,
			KEY_QUERY_VALUE, &desktopKey);
		if (status != ERROR_SUCCESS)
			return false;

		DWORD globalScaling = 0, logPixels = 0;
		bool hasGlobalScaling = false, hasLogPixels = false;
		if (failedStage) *failedStage = L"Win8DpiScaling registry read";
		status = ReadDesktopDword(desktopKey, L"Win8DpiScaling", globalScaling,
			hasGlobalScaling);
		if (status == ERROR_SUCCESS)
		{
			if (failedStage) *failedStage = L"LogPixels registry read";
			status = ReadDesktopDword(desktopKey, L"LogPixels", logPixels, hasLogPixels);
		}
		RegCloseKey(desktopKey);
		if (status != ERROR_SUCCESS)
			return false;

		int dpi = 0;
		if (hasGlobalScaling && globalScaling == 1)
		{
			if (!hasLogPixels || logPixels < 96 || logPixels > 480)
			{
				if (failedStage) *failedStage = L"global LogPixels validation";
				status = ERROR_INVALID_DATA;
				return false;
			}
			dpi = static_cast<int>(logPixels);
		}
		else
			dpi = static_cast<int>(::GetDpiForSystem());

		if (dpi <= 0)
		{
			if (failedStage) *failedStage = L"system DPI query";
			status = ERROR_INVALID_DATA;
			return false;
		}
		percent = NearestDpiPercent(MulDiv(dpi, 100, 96));
		status = ERROR_SUCCESS;
		return true;
	}

	static bool IsDpiSettingPersisted(int percent)
	{
		HKEY desktopKey = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0,
			KEY_QUERY_VALUE, &desktopKey) != ERROR_SUCCESS)
			return false;
		DWORD globalScaling = 0, logPixels = 0;
		bool hasGlobalScaling = false, hasLogPixels = false;
		const LSTATUS scalingStatus = ReadDesktopDword(desktopKey,
			L"Win8DpiScaling", globalScaling, hasGlobalScaling);
		const LSTATUS pixelsStatus = ReadDesktopDword(desktopKey,
			L"LogPixels", logPixels, hasLogPixels);
		RegCloseKey(desktopKey);
		return scalingStatus == ERROR_SUCCESS && pixelsStatus == ERROR_SUCCESS &&
			hasGlobalScaling && globalScaling == 1 && hasLogPixels &&
			logPixels == static_cast<DWORD>(MulDiv(96, percent, 100));
	}

	static bool PersistDpiSetting(int percent, LSTATUS& status)
	{
		if (percent < 100 || percent > 500)
		{
			status = ERROR_INVALID_PARAMETER;
			return false;
		}
		HKEY desktopKey = nullptr;
		status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0,
			nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &desktopKey, nullptr);
		if (status != ERROR_SUCCESS)
			return false;

		DWORD oldGlobalScaling = 0, oldLogPixels = 0;
		bool hadGlobalScaling = false, hadLogPixels = false;
		status = ReadDesktopDword(desktopKey, L"Win8DpiScaling",
			oldGlobalScaling, hadGlobalScaling);
		if (status == ERROR_SUCCESS)
			status = ReadDesktopDword(desktopKey, L"LogPixels", oldLogPixels, hadLogPixels);
		const DWORD newLogPixels = static_cast<DWORD>(MulDiv(96, percent, 100));
		if (status == ERROR_SUCCESS)
			status = RegSetValueExW(desktopKey, L"LogPixels", 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&newLogPixels), sizeof(newLogPixels));
		if (status == ERROR_SUCCESS)
		{
			const DWORD enableGlobalScaling = 1;
			status = RegSetValueExW(desktopKey, L"Win8DpiScaling", 0, REG_DWORD,
				reinterpret_cast<const BYTE*>(&enableGlobalScaling), sizeof(enableGlobalScaling));
		}
		if (status != ERROR_SUCCESS)
		{
			if (hadLogPixels)
				RegSetValueExW(desktopKey, L"LogPixels", 0, REG_DWORD,
					reinterpret_cast<const BYTE*>(&oldLogPixels), sizeof(oldLogPixels));
			else
				RegDeleteValueW(desktopKey, L"LogPixels");
			if (hadGlobalScaling)
				RegSetValueExW(desktopKey, L"Win8DpiScaling", 0, REG_DWORD,
					reinterpret_cast<const BYTE*>(&oldGlobalScaling), sizeof(oldGlobalScaling));
			else
				RegDeleteValueW(desktopKey, L"Win8DpiScaling");
			RegCloseKey(desktopKey);
			return false;
		}

		RegFlushKey(desktopKey);
		RegCloseKey(desktopKey);
		return true;
	}

	void SelectDpiChoice()
	{
		HWND dpi = ::GetDlgItem(m_hWnd, 2031);
		int customIndex = -1;
		for (int i = 0; i < ComboBox_GetCount(dpi); ++i)
		{
			const int itemPercent = static_cast<int>(ComboBox_GetItemData(dpi, i));
			if (itemPercent == dpiPercent)
			{
				ComboBox_SetCurSel(dpi, i);
				return;
			}
			if (itemPercent == 0)
				customIndex = i;
		}
		if (customIndex >= 0)
			ComboBox_SetCurSel(dpi, customIndex);
	}

	void UpdateDpiDescription()
	{
		WCHAR text[64] = {};
		const int dpi = MulDiv(96, dpiPercent, 100);
		StringCchPrintfW(text, ARRAYSIZE(text), LoadDeskString(IDS_DPI_DESCRIPTION_FORMAT).c_str(),
			dpiPercent, dpi);
		::SetDlgItemTextW(m_hWnd, 2038, text);
	}

	int ShowApplyError(LSTATUS status)
	{
		WCHAR message[128] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_DPI_SAVE_ERROR).c_str(), status);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return PSNRET_INVALID_NOCHANGEPAGE;
	}
};

struct GRAPHICS_PROFILE_REGVALUE
{
	bool exists = false;
	DWORD type = REG_NONE;
	std::vector<BYTE> data;
};

static bool ReadGraphicsProfileValue(HKEY root, LPCWSTR subkey, LPCWSTR valueName,
	GRAPHICS_PROFILE_REGVALUE& value, LSTATUS& status)
{
	value = {};
	HKEY key = nullptr;
	status = RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key);
	if (status == ERROR_FILE_NOT_FOUND)
	{
		status = ERROR_SUCCESS;
		return true;
	}
	if (status != ERROR_SUCCESS) return false;
	DWORD bytes = 0;
	status = RegQueryValueExW(key, valueName, nullptr, &value.type, nullptr, &bytes);
	if (status == ERROR_FILE_NOT_FOUND)
	{
		RegCloseKey(key);
		status = ERROR_SUCCESS;
		return true;
	}
	if (status != ERROR_SUCCESS)
	{
		RegCloseKey(key);
		return false;
	}
	value.data.resize(bytes);
	if (bytes)
		status = RegQueryValueExW(key, valueName, nullptr, &value.type,
			value.data.data(), &bytes);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS) return false;
	value.data.resize(bytes);
	value.exists = true;
	return true;
}

static bool WriteGraphicsProfileValue(HKEY root, LPCWSTR subkey, LPCWSTR valueName,
	DWORD type, const BYTE* data, DWORD bytes, LSTATUS& status)
{
	HKEY key = nullptr;
	status = RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE,
		nullptr, &key, nullptr);
	if (status != ERROR_SUCCESS) return false;
	status = RegSetValueExW(key, valueName, 0, type, data, bytes);
	RegCloseKey(key);
	return status == ERROR_SUCCESS;
}

static bool RestoreGraphicsProfileValue(HKEY root, LPCWSTR subkey, LPCWSTR valueName,
	const GRAPHICS_PROFILE_REGVALUE& value, LSTATUS& status)
{
	if (value.exists)
		return WriteGraphicsProfileValue(root, subkey, valueName, value.type,
			value.data.data(), static_cast<DWORD>(value.data.size()), status);
	HKEY key = nullptr;
	status = RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key);
	if (status == ERROR_FILE_NOT_FOUND)
	{
		status = ERROR_SUCCESS;
		return true;
	}
	if (status != ERROR_SUCCESS) return false;
	status = RegDeleteValueW(key, valueName);
	if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
	RegCloseKey(key);
	return status == ERROR_SUCCESS;
}

static bool ReadVrrPreference(bool& enabled, GRAPHICS_PROFILE_REGVALUE& original,
	LSTATUS& status)
{
	static const WCHAR path[] = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
	if (!ReadGraphicsProfileValue(HKEY_CURRENT_USER, path,
		L"DirectXUserGlobalSettings", original, status)) return false;
	enabled = false;
	if (!original.exists) return true;
	if (original.type != REG_SZ || original.data.size() < sizeof(WCHAR) ||
		(original.data.size() % sizeof(WCHAR)) != 0)
	{
		status = ERROR_INVALID_DATATYPE;
		return false;
	}
	std::wstring settings(reinterpret_cast<const WCHAR*>(original.data.data()),
		original.data.size() / sizeof(WCHAR));
	const size_t nul = settings.find(L'\0');
	if (nul != std::wstring::npos) settings.resize(nul);
	for (size_t start = 0; start < settings.size();)
	{
		size_t end = settings.find(L';', start);
		if (end == std::wstring::npos) end = settings.size();
		const std::wstring token = settings.substr(start, end - start);
		if (_wcsnicmp(token.c_str(), L"VRROptimizeEnable=", 18) == 0)
			enabled = token.size() > 18 && token[18] == L'1';
		start = end + 1;
	}
	return true;
}

static std::wstring BuildVrrPreference(const GRAPHICS_PROFILE_REGVALUE& original, bool enabled)
{
	std::wstring settings;
	if (original.exists && original.type == REG_SZ && !original.data.empty())
	{
		settings.assign(reinterpret_cast<const WCHAR*>(original.data.data()),
			original.data.size() / sizeof(WCHAR));
		const size_t nul = settings.find(L'\0');
		if (nul != std::wstring::npos) settings.resize(nul);
	}
	std::wstring preserved;
	for (size_t start = 0; start < settings.size();)
	{
		size_t end = settings.find(L';', start);
		if (end == std::wstring::npos) end = settings.size();
		const std::wstring token = settings.substr(start, end - start);
		if (!token.empty() && _wcsnicmp(token.c_str(), L"VRROptimizeEnable=", 18) != 0)
		{
			preserved += token;
			preserved += L';';
		}
		start = end + 1;
	}
	preserved += enabled ? L"VRROptimizeEnable=1;" : L"VRROptimizeEnable=0;";
	return preserved;
}

static bool IsKnownVirtualGraphicsAdapter(LPCWSTR adapter)
{
	if (!adapter || !adapter[0]) return false;
	static const LPCWSTR virtualNames[] = {
		L"VMware", L"VirtualBox", L"Microsoft Basic Display", L"Microsoft Remote Display",
		L"Remote Display Adapter", L"Parallels Display", L"QXL"
	};
	for (LPCWSTR name : virtualNames)
		if (StrStrIW(adapter, name)) return true;
	return false;
}

class CTroubleshootPage : public WTL::CPropertyPageImpl<CTroubleshootPage>
{
public:
	enum { IDD = IDD_TROUBLESHOOTDLG };
	LPCWSTR adapter = L"";

	 BEGIN_MSG_MAP(CTroubleshootPage)
		 MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		 MESSAGE_HANDLER(WM_HSCROLL, OnSliderChanged)
		 CHAIN_MSG_MAP(WTL::CPropertyPageImpl<CTroubleshootPage>)
	 END_MSG_MAP()

	 LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
	 {
		 ApplySystemDialogFont(m_hWnd);
		 HWND slider = ::GetDlgItem(m_hWnd, 2041);
		 const bool hagsRead = ReadGraphicsProfileValue(HKEY_LOCAL_MACHINE,
			 L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode",
			 _hagsOriginal, _status);
		 bool vrrRead = false;
		 if (hagsRead)
			 vrrRead = ReadVrrPreference(_vrrEnabled, _vrrOriginal, _status);
		 if (!hagsRead || !vrrRead)
		 {
			 ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(0, 0));
			 ::SendMessageW(slider, TBM_SETPOS, TRUE, 0);
			 ::EnableWindow(slider, FALSE);
			 ::SetDlgItemTextW(m_hWnd, 2048,
				 LoadDeskString(IDS_GRAPHICS_SETTINGS_READ_ERROR).c_str());
			 return TRUE;
		 }
		 if (_hagsOriginal.exists && _hagsOriginal.type == REG_DWORD &&
			 _hagsOriginal.data.size() >= sizeof(DWORD))
		 {
			 DWORD mode = *reinterpret_cast<const DWORD*>(_hagsOriginal.data.data());
			 _hagsEnabled = mode == 2;
		 }
		 DWORD build = 0;
		 HKEY versionKey = nullptr;
		 if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
			 L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
			 KEY_QUERY_VALUE, &versionKey) == ERROR_SUCCESS)
		 {
			 WCHAR buildText[32] = {};
			 DWORD type = 0, bytes = sizeof(buildText);
			 if (RegQueryValueExW(versionKey, L"CurrentBuildNumber", nullptr,
				 &type, reinterpret_cast<LPBYTE>(buildText), &bytes) == ERROR_SUCCESS)
				 build = wcstoul(buildText, nullptr, 10);
			 RegCloseKey(versionKey);
		 }
		 const bool virtualAdapter = IsKnownVirtualGraphicsAdapter(adapter);
		 _vrrSupported = build >= 18362 && !virtualAdapter;
		 _hagsSupported = build >= 19041 && !virtualAdapter;
		 int maxProfile = _hagsSupported ? 2 : (_vrrSupported ? 1 : 0);
		 ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELONG(0, maxProfile));
		 ::SendMessageW(slider, TBM_SETTICFREQ, 1, 0);
		 int initial = _hagsEnabled ? 2 : (_vrrEnabled ? 1 : 0);
		 if (initial > maxProfile) initial = maxProfile;
		 _lastPosition = initial;
		 ::SendMessageW(slider, TBM_SETPOS, TRUE, initial);
		if (!_vrrSupported && !_hagsSupported)
			::EnableWindow(slider, FALSE);
		else if (!_hagsSupported)
			::EnableWindow(::GetDlgItem(m_hWnd, 2046), FALSE);
		UpdateAccelerationDescription(initial);
		 _lastPosition = static_cast<int>(::SendMessageW(slider, TBM_GETPOS, 0, 0));
		 return TRUE;
	 }

	 LRESULT OnSliderChanged(UINT, WPARAM, LPARAM lParam, BOOL&)
	 {
		 if (reinterpret_cast<HWND>(lParam) != ::GetDlgItem(m_hWnd, 2041)) return 0;
		 const int position = static_cast<int>(::SendDlgItemMessageW(m_hWnd, 2041, TBM_GETPOS, 0, 0));
		 if (position != _lastPosition)
		 {
			 _lastPosition = position;
			 UpdateAccelerationDescription(position);
			 _userChanged = true;
			 SetModified(TRUE);
		 }
		 return 0;
	 }

	 int OnApply()
	 {
		 if (!_userChanged) return PSNRET_NOERROR;
		 const int profile = static_cast<int>(::SendDlgItemMessageW(m_hWnd, 2041, TBM_GETPOS, 0, 0));
		 const bool wantVrr = profile >= 1;
		 const bool wantHags = profile >= 2;
		 const bool vrrChanged = wantVrr != _vrrEnabled;
		 const bool hagsChanged = wantHags != _hagsEnabled;
		 if (wantVrr && !_vrrSupported)
			 return ShowVrrPreferenceError(ERROR_NOT_SUPPORTED, true);
		 if (wantHags && !_hagsSupported)
		 {
			 ::MessageBoxW(m_hWnd,
				 LoadDeskString(IDS_GRAPHICS_PROFILE_UNSUPPORTED).c_str(),
				 LoadDeskString(IDS_GRAPHICS_TITLE).c_str(), MB_OK | MB_ICONWARNING);
			 return PSNRET_INVALID_NOCHANGEPAGE;
		 }

		 GRAPHICS_PROFILE_REGVALUE hagsBefore, vrrBefore;
		 LSTATUS status = ERROR_SUCCESS;
		 if (!ReadGraphicsProfileValue(HKEY_LOCAL_MACHINE,
			 L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode",
			 hagsBefore, status) ||
			 !ReadGraphicsProfileValue(HKEY_CURRENT_USER,
			 L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
			 L"DirectXUserGlobalSettings", vrrBefore, status))
			 return vrrChanged ? ShowVrrPreferenceError(status, wantVrr) :
				ShowGraphicsProfileError(status);

		 const std::wstring vrrSettings = BuildVrrPreference(vrrBefore, wantVrr);
		 if (!WriteGraphicsProfileValue(HKEY_CURRENT_USER,
			 L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
			 L"DirectXUserGlobalSettings", REG_SZ,
			 reinterpret_cast<const BYTE*>(vrrSettings.c_str()),
			 static_cast<DWORD>((vrrSettings.size() + 1) * sizeof(WCHAR)), status))
			 return vrrChanged ? ShowVrrPreferenceError(status, wantVrr) :
				ShowGraphicsProfileError(status);

		 DWORD hagsMode = wantHags ? 2 : 1;
		 if ((_hagsSupported || !wantHags) && !WriteGraphicsProfileValue(HKEY_LOCAL_MACHINE,
			 L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode",
			 REG_DWORD, reinterpret_cast<const BYTE*>(&hagsMode), sizeof(hagsMode), status))
		{
			 LSTATUS restoreStatus = ERROR_SUCCESS;
			 RestoreGraphicsProfileValue(HKEY_CURRENT_USER,
				 L"Software\\Microsoft\\DirectX\\UserGpuPreferences",
				 L"DirectXUserGlobalSettings", vrrBefore, restoreStatus);
			 return ShowGraphicsProfileError(status);
		}
		 _vrrEnabled = wantVrr;
		 _hagsEnabled = wantHags;
		 _vrrOriginal = {};
		 _hagsOriginal = {};
		 _userChanged = false;
		 SetModified(FALSE);
		 ::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
			 reinterpret_cast<LPARAM>(L"Software\\Microsoft\\DirectX\\UserGpuPreferences"),
			 SMTO_ABORTIFHUNG, 1000, nullptr);
		 if (hagsChanged)
			 ::MessageBoxW(m_hWnd,
				 LoadDeskString(IDS_GPU_SCHEDULING_RESTART).c_str(),
				 LoadDeskString(IDS_GRAPHICS_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
		 else
			 ::MessageBoxW(m_hWnd,
				 wantVrr ? LoadDeskString(IDS_VRR_ENABLED).c_str() :
					LoadDeskString(IDS_VRR_DISABLED).c_str(),
				 LoadDeskString(IDS_GRAPHICS_TITLE).c_str(), MB_OK | MB_ICONINFORMATION);
		 return PSNRET_NOERROR;
	 }

private:
	void UpdateAccelerationDescription(int position)
	{
		const UINT stringId = position <= 0 ? IDS_ACCEL_NONE :
			(position >= 2 ? IDS_ACCEL_FULL : IDS_ACCEL_REDUCED);
		WCHAR description[256] = {};
		if (::LoadStringW(g_hinst, stringId, description, ARRAYSIZE(description)))
			::SetDlgItemTextW(m_hWnd, 2048, description);
	}

	int ShowGraphicsProfileError(LSTATUS status)
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_GRAPHICS_PROFILE_SAVE_ERROR).c_str(), status);
		::MessageBoxW(m_hWnd, message,
			LoadDeskString(IDS_GRAPHICS_TITLE).c_str(), MB_OK | MB_ICONERROR);
		return PSNRET_INVALID_NOCHANGEPAGE;
	}

	int ShowVrrPreferenceError(LSTATUS status, bool enable)
	{
		WCHAR message[256] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			enable ? LoadDeskString(IDS_VRR_ENABLE_ERROR).c_str() :
				LoadDeskString(IDS_VRR_DISABLE_ERROR).c_str(),
			static_cast<unsigned int>(status));
		::MessageBoxW(m_hWnd, message,
			LoadDeskString(IDS_GRAPHICS_TITLE).c_str(), MB_OK | MB_ICONERROR);
		return PSNRET_INVALID_NOCHANGEPAGE;
	}

	GRAPHICS_PROFILE_REGVALUE _hagsOriginal;
	GRAPHICS_PROFILE_REGVALUE _vrrOriginal;
	LSTATUS _status = ERROR_SUCCESS;
	int _lastPosition = 2;
	bool _hagsEnabled = false;
	bool _vrrEnabled = false;
	bool _hagsSupported = false;
	bool _vrrSupported = false;
	bool _userChanged = false;
};

static bool ShowListAllModesDialog(HWND owner, LPCWSTR deviceName, bool hideUnsupportedModes);

class CAdapterPage : public WTL::CPropertyPageImpl<CAdapterPage>
{
public:
	enum { IDD = IDD_ADAPTERDLG };
	LPCWSTR adapter = L"";
	LPCWSTR deviceName = L"";
	LPCWSTR deviceInstanceId = L"";
	const bool* hideUnsupportedModes = nullptr;
	DWORD width = 0;
	DWORD height = 0;
	DWORD bpp = 0;
	DWORD frequency = 0;

	BEGIN_MSG_MAP(CAdapterPage)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		COMMAND_ID_HANDLER(2003, OnProperties)
		COMMAND_ID_HANDLER(2009, OnListAllModes)
		CHAIN_MSG_MAP(WTL::CPropertyPageImpl<CAdapterPage>)
	END_MSG_MAP()

	LRESULT OnProperties(WORD, WORD, HWND, BOOL&)
	{
		if (!ShowDeviceManagerProperties(m_hWnd, deviceInstanceId))
			::MessageBoxW(m_hWnd, LoadDeskString(IDS_ADAPTER_PROPERTIES_ERROR).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	LRESULT OnListAllModes(WORD, WORD, HWND, BOOL&)
	{
		if (!ShowListAllModesDialog(m_hWnd, deviceName,
			!hideUnsupportedModes || *hideUnsupportedModes))
			::MessageBoxW(m_hWnd, LoadDeskString(IDS_ALL_MODES_OPEN_ERROR).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
	{
		ApplySystemDialogFont(m_hWnd);
		WCHAR adapterString[256] = {};
		WCHAR chipType[256] = {};
		WCHAR dacType[256] = {};
		WCHAR memorySize[64] = {};
		WCHAR biosString[256] = {};
		std::wstring unavailable = LoadDeskString(IDS_NOT_AVAILABLE);
		StringCchCopyW(chipType, ARRAYSIZE(chipType), unavailable.c_str());
		StringCchCopyW(dacType, ARRAYSIZE(dacType), unavailable.c_str());
		StringCchCopyW(memorySize, ARRAYSIZE(memorySize), unavailable.c_str());
		StringCchCopyW(biosString, ARRAYSIZE(biosString), unavailable.c_str());
		StringCchCopyW(adapterString, ARRAYSIZE(adapterString), adapter);
		DISPLAY_DEVICEW display = {};
		display.cb = sizeof(display);
		for (DWORD i = 0; deviceName[0] && EnumDisplayDevicesW(nullptr, i, &display, 0); ++i)
		{
			if (StrCmpI(display.DeviceName, deviceName) == 0)
			{
				if (display.DeviceString[0])
					StringCchCopyW(adapterString, ARRAYSIZE(adapterString), display.DeviceString);
				const WCHAR registryPrefix[] = L"\\Registry\\Machine\\";
				LPCWSTR registryPath = display.DeviceKey;
				if (StrCmpNI(registryPath, registryPrefix, ARRAYSIZE(registryPrefix) - 1) == 0)
					registryPath += ARRAYSIZE(registryPrefix) - 1;
				HKEY key = nullptr;
				if (registryPath[0] && RegOpenKeyExW(HKEY_LOCAL_MACHINE, registryPath,
					0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
				{
					auto readHardwareString = [&](LPCWSTR valueName, LPWSTR output, size_t outputCount)
					{
						BYTE data[1024] = {};
						DWORD type = 0;
						DWORD bytes = sizeof(data);
						if (RegQueryValueExW(key, valueName, nullptr, &type, data, &bytes) != ERROR_SUCCESS ||
							(type != REG_SZ && type != REG_EXPAND_SZ && type != REG_BINARY) || bytes == 0)
							return false;
						output[0] = L'\0';
						if ((type == REG_SZ || type == REG_EXPAND_SZ ||
							(bytes >= sizeof(WCHAR) && (data[1] == 0 || (bytes >= 4 && data[3] == 0)))) &&
							bytes >= sizeof(WCHAR))
						{
							size_t chars = min(bytes / sizeof(WCHAR), outputCount - 1);
							memcpy(output, data, chars * sizeof(WCHAR));
							output[chars] = L'\0';
						}
						else
						{
							int chars = MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCSTR>(data),
								(int)min(bytes, (DWORD)INT_MAX), output, (int)outputCount - 1);
							if (chars <= 0) return false;
							output[chars] = L'\0';
						}
						return output[0] != L'\0';
				};
				readHardwareString(L"HardwareInformation.ChipType", chipType, ARRAYSIZE(chipType));
				readHardwareString(L"HardwareInformation.DacType", dacType, ARRAYSIZE(dacType));
				readHardwareString(L"HardwareInformation.AdapterString", adapterString, ARRAYSIZE(adapterString));
				readHardwareString(L"HardwareInformation.BiosString", biosString, ARRAYSIZE(biosString));
				ULONGLONG memoryValue = 0;
				DWORD type = 0;
				DWORD bytes = sizeof(memoryValue);
				if (RegQueryValueExW(key, L"HardwareInformation.MemorySize", nullptr,
					&type, reinterpret_cast<LPBYTE>(&memoryValue), &bytes) == ERROR_SUCCESS &&
					bytes >= sizeof(DWORD) &&
					(type == REG_DWORD || type == REG_QWORD || type == REG_BINARY))
				{
					if (bytes < sizeof(memoryValue))
						memoryValue = *reinterpret_cast<const DWORD*>(&memoryValue);
					// Older drivers commonly store this value as bytes, while newer
					// miniports report the documented value directly in megabytes.
					ULONGLONG memoryMb = memoryValue >= (1024ULL * 1024ULL) ?
						memoryValue / (1024ULL * 1024ULL) : memoryValue;
					StringCchPrintfW(memorySize, ARRAYSIZE(memorySize),
						LoadDeskString(IDS_ADAPTER_MEMORY_FORMAT).c_str(), memoryMb);
				}
				RegCloseKey(key);
				}
				break;
			}
			display = {};
			display.cb = sizeof(display);
		}
		::SetDlgItemTextW(m_hWnd, 2002, adapterString);
		::SetDlgItemTextW(m_hWnd, 2012, chipType);
		::SetDlgItemTextW(m_hWnd, 2013, dacType);
		::SetDlgItemTextW(m_hWnd, 2014, memorySize);
		::SetDlgItemTextW(m_hWnd, 2015, adapterString);
		::SetDlgItemTextW(m_hWnd, 2016, biosString);
		return TRUE;
	}
};

class CMonitorPage : public WTL::CPropertyPageImpl<CMonitorPage>

{
public:
	enum { IDD = IDD_MONITORDLG };
	LPCWSTR monitor = L"";
	LPCWSTR deviceName = L"";
	LPCWSTR deviceInstanceId = L"";
	DWORD width = 0;
	DWORD height = 0;
	DWORD frequency = 0;
	bool hideUnsupportedModes = true;
	void SetHideUnsupportedModes(bool hide)
	{
		hideUnsupportedModes = hide;
		_appliedHideUnsupportedModes = hide;
	}

	BEGIN_MSG_MAP(CMonitorPage)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		COMMAND_ID_HANDLER(2023, OnProperties)
		COMMAND_HANDLER(2026, CBN_SELCHANGE, OnRefreshChange)
		COMMAND_HANDLER(2027, BN_CLICKED, OnHideModesChange)
		CHAIN_MSG_MAP(WTL::CPropertyPageImpl<CMonitorPage>)
	END_MSG_MAP()

	LRESULT OnProperties(WORD, WORD, HWND, BOOL&)
	{
		if (!ShowDeviceManagerProperties(m_hWnd, deviceInstanceId))
			::MessageBoxW(m_hWnd, LoadDeskString(IDS_MONITOR_PROPERTIES_ERROR).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return 0;
	}

	LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
	{
		ApplySystemDialogFont(m_hWnd);
		::SetDlgItemTextW(m_hWnd, 2022, monitor);
		HWND checkbox = ::GetDlgItem(m_hWnd, 2027);
		LONG_PTR style = ::GetWindowLongPtrW(checkbox, GWL_STYLE);
		::SetWindowLongPtrW(checkbox, GWL_STYLE,
			(style & ~BS_TYPEMASK) | BS_AUTOCHECKBOX);
		Button_SetCheck(checkbox, hideUnsupportedModes ? BST_CHECKED : BST_UNCHECKED);
		::EnableWindow(checkbox, deviceName && deviceName[0]);
		PopulateRefreshRates();
		return TRUE;
	}

	LRESULT OnHideModesChange(WORD, WORD, HWND, BOOL&)
	{
		hideUnsupportedModes = Button_GetCheck(::GetDlgItem(m_hWnd, 2027)) == BST_CHECKED;
		PopulateRefreshRates();
		SetModified(hideUnsupportedModes != _appliedHideUnsupportedModes ||
			_pendingFrequency != frequency);
		return 0;
	}

	LRESULT OnRefreshChange(WORD, WORD, HWND, BOOL&)
	{
		HWND combo = ::GetDlgItem(m_hWnd, 2026);
		int selected = ComboBox_GetCurSel(combo);
		if (selected != CB_ERR)
		{
			_pendingFrequency = static_cast<DWORD>(ComboBox_GetItemData(combo, selected));
			SetModified(_pendingFrequency != frequency ||
				hideUnsupportedModes != _appliedHideUnsupportedModes);
		}
		return 0;
	}

	int OnApply()
	{
		const bool hideChanged = hideUnsupportedModes != _appliedHideUnsupportedModes;
		if (_pendingFrequency == frequency && !hideChanged)
			return PSNRET_NOERROR;
		if (_pendingFrequency != frequency)
		{
			DEVMODEW original = {};
			original.dmSize = sizeof(original);
			if (!deviceName[0] || !EnumDisplaySettingsExW(deviceName, ENUM_CURRENT_SETTINGS, &original, 0))
			{
				::MessageBoxW(m_hWnd, LoadDeskString(IDS_MONITOR_CONFIG_READ_ERROR).c_str(),
					LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
				return PSNRET_INVALID_NOCHANGEPAGE;
			}
			DEVMODEW updated = original;
			updated.dmDisplayFrequency = _pendingFrequency;
			updated.dmFields |= DM_DISPLAYFREQUENCY;
			LONG result = ChangeDisplaySettingsExW(deviceName, &updated, nullptr, CDS_TEST, nullptr);
			if (result == DISP_CHANGE_SUCCESSFUL)
				result = ChangeDisplaySettingsExW(deviceName, &updated, nullptr, CDS_UPDATEREGISTRY, nullptr);
			if (result != DISP_CHANGE_SUCCESSFUL)
				return ShowRefreshError(result);

			CThemeChngDlg confirmation;
			if (confirmation.DoModal() != CThemeChngDlg::KeepChanges)
			{
				result = ChangeDisplaySettingsExW(deviceName, &original, nullptr, CDS_UPDATEREGISTRY, nullptr);
				if (result != DISP_CHANGE_SUCCESSFUL)
					return ShowRefreshError(result);
			}
		}
		if (hideChanged)
		{
			const LSTATUS status = WriteHideUnsupportedModes(deviceName,
				deviceInstanceId, hideUnsupportedModes);
			if (status != ERROR_SUCCESS)
			{
				WCHAR message[160] = {};
				StringCchPrintfW(message, ARRAYSIZE(message),
					LoadDeskString(IDS_MODE_FILTER_SAVE_ERROR).c_str(), status);
				::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
				return PSNRET_INVALID_NOCHANGEPAGE;
			}
			_appliedHideUnsupportedModes = hideUnsupportedModes;
		}
		PopulateRefreshRates();
		SetModified(FALSE);
		return PSNRET_NOERROR;
	}

private:
	DWORD _pendingFrequency = 0;
	bool _appliedHideUnsupportedModes = true;

	int ShowRefreshError(LONG result)
	{
		WCHAR message[160] = {};
		StringCchPrintfW(message, ARRAYSIZE(message),
			LoadDeskString(IDS_REFRESH_RATE_APPLY_ERROR).c_str(), result);
		::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return PSNRET_INVALID_NOCHANGEPAGE;
	}

	void PopulateRefreshRates()
	{
		HWND combo = ::GetDlgItem(m_hWnd, 2026);
		ComboBox_ResetContent(combo);
		DEVMODEW current = {};
		current.dmSize = sizeof(current);
		const bool haveCurrent = deviceName[0] &&
			EnumDisplaySettingsExW(deviceName, ENUM_CURRENT_SETTINGS, &current, 0);
		std::vector<DWORD> rates;
		if (haveCurrent)
		{
			frequency = current.dmDisplayFrequency;
			for (DWORD index = 0;; ++index)
			{
				DEVMODEW mode = {};
				mode.dmSize = sizeof(mode);
				if (!EnumDisplaySettingsExW(deviceName, index, &mode,
					hideUnsupportedModes ? 0 : EDS_RAWMODE)) break;
				if (mode.dmPelsWidth == current.dmPelsWidth && mode.dmPelsHeight == current.dmPelsHeight &&
					mode.dmBitsPerPel == current.dmBitsPerPel && mode.dmDisplayFlags == current.dmDisplayFlags &&
					std::find(rates.begin(), rates.end(), mode.dmDisplayFrequency) == rates.end())
					rates.push_back(mode.dmDisplayFrequency);
			}
		}
		if (std::find(rates.begin(), rates.end(), frequency) == rates.end())
			rates.push_back(frequency);
		std::sort(rates.begin(), rates.end());
		for (DWORD rate : rates)
		{
			WCHAR label[32] = {};
			if (rate > 1)
				StringCchPrintfW(label, ARRAYSIZE(label),
					LoadDeskString(IDS_HERTZ_FORMAT).c_str(), rate);
			else
				StringCchCopyW(label, ARRAYSIZE(label),
					LoadDeskString(IDS_DEFAULT_REFRESH_RATE).c_str());
			int item = ComboBox_AddString(combo, label);
			if (item == CB_ERR || item == CB_ERRSPACE) continue;
			ComboBox_SetItemData(combo, item, rate);
			if (rate == frequency) ComboBox_SetCurSel(combo, item);
		}
		_pendingFrequency = frequency;
		::EnableWindow(combo, haveCurrent && rates.size() > 1);
	}
};

class CAdvancedDisplayDlg : public ATL::CDialogImpl<CAdvancedDisplayDlg>
{
public:
	enum { IDD = IDD_ADVANCEDDLG };
	LPCWSTR adapter = L"";
	LPCWSTR monitor = L"";
	DWORD width = 0;
	DWORD height = 0;
	DWORD frequency = 0;

	static INT_PTR CALLBACK IndirectProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		CAdvancedDisplayDlg* self = reinterpret_cast<CAdvancedDisplayDlg*>(::GetWindowLongPtr(hwnd, DWLP_USER));
		if (msg == WM_INITDIALOG)
		{
			ApplySystemDialogFont(hwnd);
			self = reinterpret_cast<CAdvancedDisplayDlg*>(lParam);
			::SetWindowLongPtr(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
			if (self)
			{
				WCHAR title[256] = {};
				StringCchPrintfW(title, ARRAYSIZE(title),
					LoadDeskString(IDS_ADVANCED_DISPLAY_TITLE_FORMAT).c_str(),
					self->monitor, self->adapter);
				::SetWindowTextW(hwnd, title);
				WCHAR mode[64] = {};
				StringCchPrintf(mode, ARRAYSIZE(mode),
					LoadDeskString(IDS_DISPLAY_MODE_FORMAT).c_str(),
					self->width, self->height, self->frequency);
				::SetDlgItemTextW(hwnd, 1903, self->adapter);
				::SetDlgItemTextW(hwnd, 1905,
					LoadDeskString(IDS_DISPLAY_MEMORY_PLACEHOLDER).c_str());
				::SetDlgItemTextW(hwnd, 1907, mode);
				::SetDlgItemTextW(hwnd, 1910, self->monitor);
				HWND tabs = ::GetDlgItem(hwnd, 1900);
				std::wstring labels[] = {
					LoadDeskString(IDS_SETTINGS_GENERAL),
					LoadDeskString(IDS_SETTINGS_ADAPTER),
					LoadDeskString(IDS_SETTINGS_MONITOR),
					LoadDeskString(IDS_SETTINGS_TROUBLESHOOT),
				};
				for (int i = 0; i < ARRAYSIZE(labels); ++i)
				{
					TCITEMW item = { TCIF_TEXT, 0, 0, labels[i].data(), 0, 0 };
					TabCtrl_InsertItem(tabs, i, &item);
				}
			}
			return TRUE;
		}
		if (msg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
		{
			::EndDialog(hwnd, LOWORD(wParam));
			return TRUE;
		}
		return FALSE;
	}

	INT_PTR ShowIndirect(HWND owner)
	{
		HINSTANCE instance = _AtlBaseModule.GetResourceInstance();
	// FindResourceW is intercepted by the host's shell-fix hook on this build and
	// can return NULL even though the dialog is present in our image.  Resolve the
	// English resource explicitly through the Ex form, then retain the normal
	// lookup as a fallback for non-English builds.
	HRSRC resource = ::FindResourceExW(instance, RT_DIALOG,
		MAKEINTRESOURCEW(IDD_ADVANCEDDLG), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
	if (!resource)
		resource = ::FindResourceExW(instance, RT_DIALOG,
			MAKEINTRESOURCEW(IDD_ADVANCEDDLG), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
	if (!resource)
		resource = ::FindResourceW(instance, MAKEINTRESOURCEW(IDD_ADVANCEDDLG), RT_DIALOG);
	if (!resource)
		return -1;
	HGLOBAL loaded = ::LoadResource(instance, resource);
	DLGTEMPLATE* templateData = reinterpret_cast<DLGTEMPLATE*>(::LockResource(loaded));
	if (!templateData)
		return -1;
	return ::DialogBoxIndirectParamW(instance, templateData, owner, IndirectProc, reinterpret_cast<LPARAM>(this));
	}

	BEGIN_MSG_MAP(CAdvancedDisplayDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		COMMAND_ID_HANDLER(IDOK, OnClose)
		COMMAND_ID_HANDLER(IDCANCEL, OnClose)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&)
	{
		ApplySystemDialogFont(m_hWnd);
		WCHAR mode[64] = {};
		StringCchPrintf(mode, ARRAYSIZE(mode),
			LoadDeskString(IDS_DISPLAY_MODE_FORMAT).c_str(), width, height, frequency);
		::SetDlgItemTextW(m_hWnd, 1903, adapter);
		::SetDlgItemTextW(m_hWnd, 1905,
			LoadDeskString(IDS_DISPLAY_MEMORY_PLACEHOLDER).c_str());
		::SetDlgItemTextW(m_hWnd, 1907, mode);
		::SetDlgItemTextW(m_hWnd, 1910, monitor);
		return TRUE;
	}

	LRESULT OnClose(WORD, WORD id, HWND, BOOL&)
	{
		EndDialog(id);
		return 0;
	}
};

BOOL CSettingsDlgProc::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	_cmbMonitors = GetDlgItem(1800);
	_mulMonPreview = GetDlgItem(1801);
	HWND monitorIllustration = GetDlgItem(1812);
	LONG_PTR illustrationStyle = ::GetWindowLongPtrW(monitorIllustration, GWL_STYLE);
	::SetWindowLongPtrW(monitorIllustration, GWL_STYLE,
		(illustrationStyle & ~(SS_TYPEMASK | SS_CENTERIMAGE)) | SS_OWNERDRAW);
	_singleMonitorArtwork.reset(Gdiplus::Bitmap::FromResource(g_hinst, MAKEINTRESOURCEW(IDB_BITMAP1)));
	for (int id : kSingleMonitorModeControls)
	{
		RECT rect = {};
		::GetWindowRect(GetDlgItem(id), &rect);
		::MapWindowPoints(nullptr, m_hWnd, reinterpret_cast<POINT*>(&rect), 2);
		_originalModeControlRects.push_back(rect);
	}
	_textDisplay = GetDlgItem(1811);
	_chkPrimary = GetDlgItem(1806);
	LONG_PTR primaryStyle = ::GetWindowLongPtrW(_chkPrimary, GWL_STYLE);
	::SetWindowLongPtrW(_chkPrimary, GWL_STYLE,
		(primaryStyle & ~BS_TYPEMASK) | BS_AUTOCHECKBOX);
	_chkExtend = GetDlgItem(1805);
	// Installed MUI resources may still use BS_CHECKBOX, which does not
	// toggle itself before BN_CLICKED. Normalize those templates as well.
	LONG_PTR extendStyle = ::GetWindowLongPtrW(_chkExtend, GWL_STYLE);
	::SetWindowLongPtrW(_chkExtend, GWL_STYLE,
		(extendStyle & ~BS_TYPEMASK) | BS_AUTOCHECKBOX);
	_textCurrentRes = GetDlgItem(1814);
	_trackResolution = GetDlgItem(1808);
	_cmbColors = GetDlgItem(1807);
	_clrPreview = GetDlgItem(1813);
	_monitorTooltip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
		WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
		CW_USEDEFAULT, CW_USEDEFAULT, m_hWnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
	if (_monitorTooltip)
	{
		TOOLINFOW tool = {};
		tool.cbSize = sizeof(tool);
		tool.uFlags = TTF_TRACK | TTF_ABSOLUTE | TTF_CENTERTIP;
		tool.hwnd = _mulMonPreview;
		tool.uId = 1;
		tool.lpszText = _monitorTooltipText;
		::SendMessageW(_monitorTooltip, TTM_ADDTOOLW, 0, (LPARAM)&tool);
		::SendMessageW(_monitorTooltip, TTM_SETMAXTIPWIDTH, 0, 320);
	}
	::SetWindowLongPtr(_mulMonPreview, GWLP_USERDATA, (LONG_PTR)this);
	_oldMonitorPreviewProc = (WNDPROC)::SetWindowLongPtrW(_mulMonPreview, GWLP_WNDPROC,
		(LONG_PTR)_MonitorPreviewProc);
	
	_UpdateMonitorPreview();
	_EnumerateActiveDisplayDevices();
	_SelectCurrentMonitor();
	_GetAllModes();
	_UpdateMonitorPreview();
	return 0;
}

LRESULT CSettingsDlgProc::OnDestroy(UINT, WPARAM, LPARAM, BOOL&)
{
	// PropertySheetMoment shuts down GDI+ before the page objects leave scope.
	// Release the artwork while the dialog (and GDI+ session) still exists.
	_singleMonitorArtwork.reset();
	for (HWND identifyWindow : _identifyWindows)
	{
		if (::IsWindow(identifyWindow)) ::DestroyWindow(identifyWindow);
	}
	_identifyWindows.clear();
	_RestoreMonitorDragCursorClip();
	::KillTimer(_mulMonPreview, MONITOR_TOOLTIP_TIMER);
	if (_mulMonPreview && _oldMonitorPreviewProc)
	{
		::SetWindowLongPtrW(_mulMonPreview, GWLP_WNDPROC,
			(LONG_PTR)_oldMonitorPreviewProc);
		::SetWindowLongPtrW(_mulMonPreview, GWLP_USERDATA, 0);
		_oldMonitorPreviewProc = nullptr;
	}
	if (_monitorTooltip)
	{
		::DestroyWindow(_monitorTooltip);
		_monitorTooltip = nullptr;
	}
	return 0;
}

LRESULT CSettingsDlgProc::OnDisplayChange(UINT, WPARAM, LPARAM, BOOL&)
{
	// Applying a batch broadcasts this message before confirmation. Do not
	// replace the queued layout halfway through the transaction (or a drag).
	if (_applyingDisplayChanges || _draggingMonitor >= 0 || _HasPendingDisplayChanges()) return 0;
	_UpdateMonitorPreview();
	_EnumerateActiveDisplayDevices();
	_SelectCurrentMonitor();
	_GetAllModes();
	return 0;
}

LRESULT CSettingsDlgProc::OnDrawItem(UINT, WPARAM, LPARAM lParam, BOOL& handled)
{
	const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
	if (!item || item->CtlType != ODT_STATIC || item->CtlID != 1812)
	{
		handled = FALSE;
		return 0;
	}
	::FillRect(item->hDC, &item->rcItem, ::GetSysColorBrush(COLOR_3DFACE));
	if (::IsThemeActive()) ::DrawThemeParentBackground(item->hwndItem, item->hDC, &item->rcItem);
	if (_singleMonitorArtwork && _singleMonitorArtwork->GetLastStatus() == Gdiplus::Ok)
	{
		const int availableWidth = item->rcItem.right - item->rcItem.left;
		const int availableHeight = item->rcItem.bottom - item->rcItem.top;
		const int sourceWidth = static_cast<int>(_singleMonitorArtwork->GetWidth());
		const int sourceHeight = static_cast<int>(_singleMonitorArtwork->GetHeight());
		if (availableWidth > 0 && availableHeight > 0 && sourceWidth > 0 && sourceHeight > 0)
		{
			int width = min(availableWidth, MulDiv(sourceWidth, GetDpiForWindow(m_hWnd), 96));
			int height = MulDiv(width, sourceHeight, sourceWidth);
			if (height > availableHeight)
			{
				height = availableHeight;
				width = MulDiv(height, sourceWidth, sourceHeight);
			}
			Gdiplus::Graphics graphics(item->hDC);
			Gdiplus::ImageAttributes attributes;
			Gdiplus::Color transparent(255, 255, 0, 255);
			attributes.SetColorKey(transparent, transparent, Gdiplus::ColorAdjustTypeBitmap);
			Gdiplus::Rect destination(item->rcItem.left + (availableWidth - width) / 2,
				item->rcItem.top + (availableHeight - height) / 2, width, height);
			graphics.DrawImage(_singleMonitorArtwork.get(), destination, 0, 0,
				sourceWidth, sourceHeight, Gdiplus::UnitPixel, &attributes);

			// ThemeUI::_SetPreviewScreenSize resamples the entire screen inside
			// the fixed monitor bitmap. Lower selected modes magnify a top-left
			// source crop; higher modes draw the full source into a smaller area.
			if (_currentResInfo.width && _currentResInfo.height && !_arrResInfo.empty())
			{
				const int slider = max(0, min(static_cast<int>(::SendMessageW(
					_trackResolution, TBM_GETPOS, 0, 0)), static_cast<int>(_arrResInfo.size()) - 1));
				const RESINFO selectedMode = _arrResInfo[slider];
				if (selectedMode.width && selectedMode.height)
				{
					constexpr int screenX = 16;
					constexpr int screenY = 17;
					constexpr int screenWidth = 152;
					constexpr int screenHeight = 104;
					auto mapX = [&](int x) { return destination.X + MulDiv(x, width, sourceWidth); };
					auto mapY = [&](int y) { return destination.Y + MulDiv(y, height, sourceHeight); };
					int destinationWidth = screenWidth;
					int sourceCropWidth = screenWidth;
					if (selectedMode.width < _currentResInfo.width)
						sourceCropWidth = MulDiv(screenWidth, selectedMode.width, _currentResInfo.width);
					else if (selectedMode.width > _currentResInfo.width)
						destinationWidth = MulDiv(screenWidth, _currentResInfo.width, selectedMode.width);

					int destinationHeight = screenHeight;
					int sourceCropHeight = screenHeight;
					if (selectedMode.height < _currentResInfo.height)
						sourceCropHeight = MulDiv(screenHeight, selectedMode.height, _currentResInfo.height);
					else if (selectedMode.height > _currentResInfo.height)
						destinationHeight = MulDiv(screenHeight, _currentResInfo.height, selectedMode.height);

					const int screenRight = mapX(screenX + screenWidth);
					const int screenBottom = mapY(screenY + screenHeight);
					Gdiplus::Rect fullScreen(mapX(screenX), mapY(screenY),
						screenRight - mapX(screenX), screenBottom - mapY(screenY));
					if (destinationWidth < screenWidth || destinationHeight < screenHeight)
					{
						Gdiplus::Color screenBackground;
						if (_singleMonitorArtwork->GetPixel(screenX + 1, screenY + 1, &screenBackground) == Gdiplus::Ok)
						{
							Gdiplus::SolidBrush backgroundBrush(screenBackground);
							graphics.FillRectangle(&backgroundBrush, fullScreen);
						}
					}

					Gdiplus::Rect resizedScreen(mapX(screenX), mapY(screenY),
						mapX(screenX + destinationWidth) - mapX(screenX),
						mapY(screenY + destinationHeight) - mapY(screenY));
					graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
					graphics.DrawImage(_singleMonitorArtwork.get(), resizedScreen,
						screenX, screenY, sourceCropWidth, sourceCropHeight,
						Gdiplus::UnitPixel, &attributes);

				}
			}
	}
	}
	return TRUE;
}

void CSettingsDlgProc::_UpdateMonitorPreviewLayout()
{
	const bool singleMonitor = _monitors.size() == 1;
	::ShowWindow(GetDlgItem(1812), singleMonitor ? SW_SHOW : SW_HIDE);
	::ShowWindow(_mulMonPreview, singleMonitor ? SW_HIDE : SW_SHOW);
	::ShowWindow(GetDlgItem(1819), singleMonitor ? SW_HIDE : SW_SHOW);
	::ShowWindow(GetDlgItem(1822), singleMonitor ? SW_HIDE : SW_SHOW);
	RECT offset = { 0, 0, 0, 16 };
	::MapDialogRect(m_hWnd, &offset);
	for (size_t index = 0; index < _originalModeControlRects.size(); ++index)
	{
		const RECT& original = _originalModeControlRects[index];
		::SetWindowPos(GetDlgItem(kSingleMonitorModeControls[index]), nullptr,
			original.left, original.top + (singleMonitor ? offset.bottom : 0), 0, 0,
			SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	if (singleMonitor)
	{
		_previewPixelRects.clear();
		_previewDrawRects.clear();
		_previewHitRects.clear();
		::InvalidateRect(GetDlgItem(1812), nullptr, TRUE);
	}
	::InvalidateRect(m_hWnd, nullptr, TRUE);
}

LRESULT CSettingsDlgProc::OnNotify(UINT, WPARAM, LPARAM lParam, BOOL& bHandled)
{
	NMHDR* notification = reinterpret_cast<NMHDR*>(lParam);
	if (notification && notification->hwndFrom == _monitorTooltip &&
		notification->code == TTN_SHOW)
	{
		_PositionMonitorTooltip(_mulMonPreview);
		if (!::AnimateWindow(_monitorTooltip, 180, AW_BLEND | AW_ACTIVATE))
			::ShowWindow(_monitorTooltip, SW_SHOWNOACTIVATE);
		bHandled = TRUE;
		return TRUE;
	}
	bHandled = FALSE;
	return 0;
}

LRESULT CSettingsDlgProc::OnHScroll(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	HWND wnd = (HWND)lParam;
	if (wnd == _trackResolution)
	{
		int interactionType = LOWORD(wParam);
		int pos = 0;
		if (interactionType == TB_THUMBPOSITION || interactionType == TB_THUMBTRACK)
		{
			pos = HIWORD(wParam);
		}
		else
		{
			pos = (int)SendMessage(wnd, TBM_GETPOS, 0, 0);
		}

		_SetTrackbarModes(pos);
		_StoreSelectedPendingMode();
		::InvalidateRect(GetDlgItem(1812), nullptr, FALSE);
		::InvalidateRect(_mulMonPreview, nullptr, FALSE);
		_UpdatePendingDisplayState();
	}
	return 0;
}

LRESULT CSettingsDlgProc::OnMonitorChange(WORD, WORD, HWND, BOOL&)
{
	_StoreSelectedPendingMode();
	_previewSelection = -1;
	_GetAllModes();
	_UpdateSelectedMonitorState();
	::InvalidateRect(_mulMonPreview, nullptr, FALSE);
	return 0;
}

LRESULT CSettingsDlgProc::OnColorChange(WORD, WORD, HWND, BOOL&)
{
	if (ComboBox_GetCurSel(_cmbColors) < 0) return 0;
	_colorSelectionDirty = true;
	_UpdateColorPreview();
	_StoreSelectedPendingMode();
	_UpdatePendingDisplayState();
	return 0;
}

LRESULT CSettingsDlgProc::OnExtendChange(WORD, WORD, HWND, BOOL&)
{
	const int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	LPARAM itemData = selectedItem < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, selectedItem);
	LPCWSTR selectedDevice = itemData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(itemData);
	if (!selectedDevice) return 0;
	_StoreSelectedPendingMode();
	for (MONITORPREVIEW& monitor : _monitors)
	{
		if (StrCmpI(selectedDevice, monitor.deviceName) != 0) continue;
		if (monitor.primary)
		{
			_UpdateSelectedMonitorState();
			return 0;
		}
		const bool extend = Button_GetCheck(_chkExtend) == BST_CHECKED;
		if (monitor.active)
		{
			monitor.disableRequested = !extend;
		}
		else
		{
			monitor.enableRequested = extend;
		}
		_UpdateSelectedMonitorState();
		_UpdatePendingDisplayState();
		::InvalidateRect(_mulMonPreview, nullptr, FALSE);
		break;
	}
	return 0;
}

void CSettingsDlgProc::_UpdatePendingDisplayState()
{
	SetModified(_HasPendingDisplayChanges());
}

bool CSettingsDlgProc::_HasPendingDisplayChanges() const
{
	bool changed = !_pendingModes.empty();
	for (const MONITORPREVIEW& monitor : _monitors)
		changed = changed || monitor.enableRequested || monitor.disableRequested ||
			monitor.primary != monitor.originalPrimary ||
			(monitor.active && (monitor.rect.left != monitor.originalRect.left || monitor.rect.top != monitor.originalRect.top));
	return changed;
}

LRESULT CSettingsDlgProc::OnPrimaryChange(WORD, WORD, HWND, BOOL&)
{
	const int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	LPARAM data = selectedItem < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, selectedItem);
	LPCWSTR device = data == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(data);
	int target = -1;
	for (int index = 0; device && index < static_cast<int>(_monitors.size()); ++index)
		if (StrCmpI(device, _monitors[index].deviceName) == 0) { target = index; break; }
	if (target < 0 || !_monitors[target].active || _monitors[target].disableRequested ||
		_monitors[target].primary || Button_GetCheck(_chkPrimary) != BST_CHECKED)
	{
		_UpdateSelectedMonitorState();
		return 0;
	}
	_StoreSelectedPendingMode();
	const POINT origin = { _monitors[target].rect.left, _monitors[target].rect.top };
	// The pending primary is the desktop origin. Translate every preview by
	// the same amount, preserving relative placement and any arrangement edits.
	for (int index = 0; index < static_cast<int>(_monitors.size()); ++index)
	{
		_monitors[index].primary = index == target;
		::OffsetRect(&_monitors[index].rect, -origin.x, -origin.y);
	}
	_UpdateSelectedMonitorState();
	_UpdatePendingDisplayState();
	::InvalidateRect(_mulMonPreview, nullptr, FALSE);
	return 0;
}

LRESULT CSettingsDlgProc::OnIdentify(WORD, WORD, HWND, BOOL&)
{
	for (HWND identifyWindow : _identifyWindows)
	{
		if (::IsWindow(identifyWindow)) ::DestroyWindow(identifyWindow);
	}
	_identifyWindows.clear();
	int number = 1;
	for (const MONITORPREVIEW& monitor : _monitors)
	{
		if (!monitor.active) continue;
		HWND overlay = CreateMonitorIdentifyOverlay(monitor.originalRect, number++);
		if (overlay) _identifyWindows.push_back(overlay);
	}
	return 0;
}

LRESULT CSettingsDlgProc::OnAdvanced(WORD, WORD, HWND, BOOL&)
{
	CAdapterPage adapterPage;
	CMonitorPage monitorPage;
	CGeneralPage generalPage;
	CTroubleshootPage troubleshootPage;
	int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	if (selectedItem < 0 && ComboBox_GetCount(_cmbMonitors) > 0) selectedItem = 0;
	WCHAR adapterModel[256] = {};
	WCHAR monitorName[128] = {};
	WCHAR adapterDeviceName[CCHDEVICENAME] = {};
	WCHAR adapterInstanceId[512] = {};
	WCHAR monitorInstanceId[512] = {};
	LPARAM selectedData = selectedItem < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, selectedItem);
	LPCWSTR selectedDevice = selectedData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(selectedData);
	WCHAR selectedDeviceName[CCHDEVICENAME] = {};
	if (selectedDevice)
		StringCchCopyW(selectedDeviceName, ARRAYSIZE(selectedDeviceName), selectedDevice);
	if (selectedDevice)
	{
		for (const MONITORPREVIEW& monitor : _monitors)
		{
			if (StrCmpI(selectedDevice, monitor.deviceName) == 0)
			{
				StringCchCopyW(monitorName, ARRAYSIZE(monitorName), monitor.monitorName);
				StringCchCopyW(adapterDeviceName, ARRAYSIZE(adapterDeviceName),
					monitor.applyDeviceName[0] ? monitor.applyDeviceName : monitor.deviceName);
				StringCchCopyW(monitorInstanceId, ARRAYSIZE(monitorInstanceId),
					monitor.monitorInstanceId);
				break;
			}
		}
	}
	for (DWORD i = 0; adapterDeviceName[0]; ++i)
	{
		DISPLAY_DEVICEW device = {};
		device.cb = sizeof(device);
		if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) break;
		if (StrCmpI(device.DeviceName, adapterDeviceName) == 0)
		{
			StringCchCopyW(adapterModel, ARRAYSIZE(adapterModel), device.DeviceString);
			break;
		}
	}
	GetDeviceInstanceIdForDisplayAdapter(adapterDeviceName, adapterInstanceId,
		ARRAYSIZE(adapterInstanceId));
	EnsureMonitorDisplayName(monitorName, ARRAYSIZE(monitorName));
	adapterPage.adapter = adapterModel;
	adapterPage.deviceName = adapterDeviceName;
	adapterPage.deviceInstanceId = adapterInstanceId;
	adapterPage.hideUnsupportedModes = &monitorPage.hideUnsupportedModes;
	adapterPage.width = _currentResInfo.width;
	adapterPage.height = _currentResInfo.height;
	adapterPage.frequency = _currentResInfo.freq;
	troubleshootPage.adapter = adapterModel;
	monitorPage.monitor = monitorName;
	monitorPage.deviceName = adapterDeviceName;
	monitorPage.deviceInstanceId = monitorInstanceId;
	const bool initialHideUnsupportedModes = ReadHideUnsupportedModes(
		adapterDeviceName, monitorInstanceId);
	monitorPage.SetHideUnsupportedModes(initialHideUnsupportedModes);
	monitorPage.width = _currentResInfo.width;
	monitorPage.height = _currentResInfo.height;
	monitorPage.frequency = _currentResInfo.freq;
	// The settings page is a child property page.  Use the Display Properties
	// sheet as the owner so the advanced sheet is created in the same modal
	// ownership chain as the XP implementation.
	HWND owner = ::GetParent(m_hWnd);
	if (!owner)
		owner = m_hWnd;
	WCHAR title[256] = {};
	StringCchPrintfW(title, ARRAYSIZE(title),
		LoadDeskString(IDS_ADVANCED_DISPLAY_TITLE_FORMAT).c_str(), monitorName, adapterModel);
	WTL::CPropertySheet sheet(title);
	sheet.m_psh.pfnCallback = AdvancedSheetFontCallback;
	BOOL generalAdded = sheet.AddPage(generalPage);
	BOOL adapterAdded = sheet.AddPage(adapterPage);
	BOOL monitorAdded = sheet.AddPage(monitorPage);
	BOOL troubleshootAdded = sheet.AddPage(troubleshootPage);
	sheet.SetActivePage(0);
	if (!generalAdded || !adapterAdded || !monitorAdded || !troubleshootAdded)
	{
		CAdvancedDisplayDlg fallback;
		fallback.adapter = adapterModel;
		fallback.monitor = monitorName;
		fallback.frequency = _currentResInfo.freq;
		fallback.DoModal(owner);
		return 0;
	}
	INT_PTR result = sheet.DoModal(owner);
	const bool hideUnsupportedModes = ReadHideUnsupportedModes(adapterDeviceName,
		monitorInstanceId);
	if (result != -1 && adapterDeviceName[0] &&
		hideUnsupportedModes != initialHideUnsupportedModes)
	{
		_StoreSelectedPendingMode();
		if (hideUnsupportedModes && selectedDeviceName[0])
		{
			// Re-check an already queued raw resolution before it can reach Apply.
			_pendingModes.erase(std::remove_if(_pendingModes.begin(), _pendingModes.end(),
				[&](const PENDINGMODE& pending)
				{
					if (StrCmpI(pending.deviceName, selectedDeviceName) != 0) return false;
					for (DWORD index = 0;; ++index)
					{
						DEVMODEW mode = {};
						mode.dmSize = sizeof(mode);
						if (!EnumDisplaySettingsExW(adapterDeviceName, index, &mode, 0)) break;
						if (mode.dmPelsWidth == pending.mode.width &&
							mode.dmPelsHeight == pending.mode.height &&
							(!pending.mode.bpp || mode.dmBitsPerPel == pending.mode.bpp))
							return false;
					}
					return true;
				}), _pendingModes.end());
		}
		_GetAllModes();
		_UpdatePendingDisplayState();
	}
	WCHAR trace[128] = {};
	StringCchPrintf(trace, ARRAYSIZE(trace), L"deskn: Advanced property sheet result=%Id error=%lu\n", result, ::GetLastError());
	::OutputDebugStringW(trace);
	return 0;
}

static bool GetWindowsMonitorNameForAdapter(LPCWSTR adapterName, LPWSTR monitorName, size_t monitorNameCount)
{
	if (!adapterName || !adapterName[0] || !monitorName || monitorNameCount == 0)
		return false;
	monitorName[0] = L'\0';
	for (DWORD adapterIndex = 0;; ++adapterIndex)
	{
		DISPLAY_DEVICEW adapter = {};
		adapter.cb = sizeof(adapter);
		if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0))
			return false;
		if (StrCmpI(adapter.DeviceName, adapterName) != 0)
			continue;

		WCHAR firstReportedName[128] = {};
		for (DWORD childIndex = 0;; ++childIndex)
		{
			DISPLAY_DEVICEW child = {};
			child.cb = sizeof(child);
			if (!EnumDisplayDevicesW(adapter.DeviceName, childIndex, &child,
				EDD_GET_DEVICE_INTERFACE_NAME))
				break;
			if (!child.DeviceString[0])
				continue;
			if (!firstReportedName[0])
				StringCchCopyW(firstReportedName, ARRAYSIZE(firstReportedName), child.DeviceString);
			if (child.StateFlags & (DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
			{
				StringCchCopyW(monitorName, monitorNameCount, child.DeviceString);
				return true;
			}
		}
		if (firstReportedName[0])
		{
			StringCchCopyW(monitorName, monitorNameCount, firstReportedName);
			return true;
		}
		return false;
	}
}

static bool GetMonitorNameFromEdid(LPCWSTR monitorInstanceId, LPWSTR monitorName,
	size_t monitorNameCount)
{
	if (!monitorInstanceId || !monitorInstanceId[0] || !monitorName || !monitorNameCount)
		return false;

	WCHAR keyPath[1024] = {};
	if (FAILED(StringCchPrintfW(keyPath, ARRAYSIZE(keyPath),
		L"SYSTEM\\CurrentControlSet\\Enum\\%s\\Device Parameters", monitorInstanceId)))
		return false;

	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
		return false;
	DWORD type = 0;
	DWORD byteCount = 0;
	LSTATUS status = RegQueryValueExW(key, L"EDID", nullptr, &type, nullptr, &byteCount);
	if (status != ERROR_SUCCESS || type != REG_BINARY || byteCount < 128)
	{
		RegCloseKey(key);
		return false;
	}
	std::vector<BYTE> edid(byteCount);
	status = RegQueryValueExW(key, L"EDID", nullptr, &type, edid.data(), &byteCount);
	RegCloseKey(key);
	if (status != ERROR_SUCCESS || type != REG_BINARY || byteCount < 128)
		return false;

	for (size_t offset = 54; offset + 18 <= 126; offset += 18)
	{
		const BYTE* descriptor = edid.data() + offset;
		if (descriptor[0] != 0 || descriptor[1] != 0 || descriptor[2] != 0 ||
			descriptor[3] != 0xFC || descriptor[4] != 0)
			continue;

		WCHAR decodedName[14] = {};
		size_t length = 0;
		for (; length < 13; ++length)
		{
			BYTE character = descriptor[5 + length];
			if (character == 0 || character == '\r' || character == '\n')
				break;
			if (character < 0x20 || character > 0x7e)
				return false;
			decodedName[length] = static_cast<WCHAR>(character);
		}
		while (length > 0 && decodedName[length - 1] == L' ')
			decodedName[--length] = L'\0';
		if (length == 0)
			return false;
		return SUCCEEDED(StringCchCopyW(monitorName, monitorNameCount, decodedName));
	}
	return false;
}

BOOL CALLBACK CSettingsDlgProc::_MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
	CSettingsDlgProc* self = reinterpret_cast<CSettingsDlgProc*>(data);
	MONITORINFOEX mi = { sizeof(mi) };
	if (GetMonitorInfo(monitor, &mi))
	{
		MONITORPREVIEW preview = {};
		preview.rect = mi.rcMonitor;
		DEVMODEW mode = {};
		mode.dmSize = sizeof(mode);
		if (::EnumDisplaySettingsExW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0))
			preview.rect = { mode.dmPosition.x, mode.dmPosition.y,
				mode.dmPosition.x + static_cast<LONG>(mode.dmPelsWidth),
				mode.dmPosition.y + static_cast<LONG>(mode.dmPelsHeight) };
		preview.originalRect = preview.rect;
		preview.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
		preview.originalPrimary = preview.primary;
		preview.active = true;
		StringCchCopyW(preview.deviceName, ARRAYSIZE(preview.deviceName), mi.szDevice);
		StringCchCopyW(preview.applyDeviceName, ARRAYSIZE(preview.applyDeviceName), mi.szDevice);
		GetWindowsMonitorNameForAdapter(mi.szDevice, preview.monitorName,
			ARRAYSIZE(preview.monitorName));
		for (DWORD adapterIndex = 0;; ++adapterIndex)
		{
			DISPLAY_DEVICEW adapter = {};
			adapter.cb = sizeof(adapter);
			if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0)) break;
			if (StrCmpI(adapter.DeviceName, mi.szDevice) != 0) continue;
			for (DWORD childIndex = 0;; ++childIndex)
			{
				DISPLAY_DEVICEW child = {};
				child.cb = sizeof(child);
				if (!EnumDisplayDevicesW(adapter.DeviceName, childIndex, &child,
					EDD_GET_DEVICE_INTERFACE_NAME)) break;
				if (child.DeviceID[0] && (child.StateFlags &
					(DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)))
				{
					GetDeviceInstanceIdForMonitorInterface(child.DeviceID,
						preview.monitorInstanceId, ARRAYSIZE(preview.monitorInstanceId));
					GetMonitorNameFromEdid(preview.monitorInstanceId, preview.monitorName,
						ARRAYSIZE(preview.monitorName));
					break;
				}
			}
			break;
		}
		self->_monitors.push_back(preview);
	}
	return TRUE;
}

LRESULT CALLBACK CSettingsDlgProc::_MonitorPreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CSettingsDlgProc* self = reinterpret_cast<CSettingsDlgProc*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));
	if (self && msg == WM_PAINT)
	{
		PAINTSTRUCT ps;
		HDC hdc = ::BeginPaint(hwnd, &ps);
		RECT rc;
		::GetClientRect(hwnd, &rc);
		const int width = rc.right - rc.left;
		const int height = rc.bottom - rc.top;
		HDC backBuffer = width > 0 && height > 0 ? ::CreateCompatibleDC(hdc) : nullptr;
		HBITMAP bitmap = backBuffer ? ::CreateCompatibleBitmap(hdc, width, height) : nullptr;
		HGDIOBJ oldBitmap = bitmap ? ::SelectObject(backBuffer, bitmap) : nullptr;
		if (oldBitmap)
		{
			self->_PaintMonitorPreview(backBuffer, rc);
			::BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
				ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
				backBuffer, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
			::SelectObject(backBuffer, oldBitmap);
			::DeleteObject(bitmap);
			::DeleteDC(backBuffer);
		}
		else
		{
			self->_PaintMonitorPreview(hdc, rc);
			if (backBuffer) ::DeleteDC(backBuffer);
			if (bitmap) ::DeleteObject(bitmap);
		}
		::EndPaint(hwnd, &ps);
		return 0;
	}
	if (self && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK))
	{
		POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		int hit = -1;
		for (int i = (int)self->_previewHitRects.size() - 1; i >= 0; --i)
		{
			if (PtInRect(&self->_previewHitRects[i], point))
			{
				hit = i;
				break;
			}
		}
		if (hit >= 0 && hit < (int)self->_monitors.size())
		{
			self->_previewSelection = hit;
			for (int item = 0; item < ComboBox_GetCount(self->_cmbMonitors); ++item)
			{
				LPARAM data = ComboBox_GetItemData(self->_cmbMonitors, item);
				LPCWSTR name = data == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(data);
				if (name && StrCmpI(name, self->_monitors[hit].deviceName) == 0)
				{
					self->_StoreSelectedPendingMode();
					if (ComboBox_GetCurSel(self->_cmbMonitors) != item)
						ComboBox_SetCurSel(self->_cmbMonitors, item);
					self->_GetAllModes();
					self->_UpdateSelectedMonitorState();
					break;
				}
			}
			::InvalidateRect(hwnd, nullptr, FALSE);
			::UpdateWindow(hwnd);
			if (msg == WM_LBUTTONDBLCLK)
			{
				// Use the same selected-device path as the Advanced button, but
				// do not begin a second drag or clip the cursor around a modal sheet.
				BOOL handled = FALSE;
				self->OnAdvanced(BN_CLICKED, 1802, ::GetDlgItem(self->m_hWnd, 1802), handled);
				return 0;
			}
			// Freeze the visible pending mode geometry before a drag. Otherwise
			// paint-only resize shifts can be applied again after moving a display.
			self->_dragOriginalRects.clear();
			for (size_t index = 0; index < self->_monitors.size(); ++index)
			{
				self->_dragOriginalRects.push_back(self->_monitors[index].rect);
				self->_monitors[index].rect = self->_previewPixelRects[index];
			}
			self->_draggingMonitor = hit;
			self->_dragStartPoint = point;
			self->_dragStartRect = self->_previewPixelRects[hit];
			self->_dragWorldBounds = self->_previewBounds;
			self->_dragScale = max(1, self->_previewScale);
			self->_dragDiagramLeft = self->_previewDiagramLeft;
			self->_dragDiagramTop = self->_previewDiagramTop;
			self->_dragVisualOffset = {
				self->_previewHitRects[hit].left - self->_previewDrawRects[hit].left,
				self->_previewHitRects[hit].top - self->_previewDrawRects[hit].top
			};
			self->_dragMoved = false;
			self->_dragHorizontalPlane = true;
			RECT clip = {};
			POINT clipTopLeft = {};
			POINT clipBottomRight = {};
			if (::GetClipCursor(&self->_previousCursorClip) &&
				::GetClientRect(hwnd, &clip))
			{
				clipTopLeft = { clip.left, clip.top };
				clipBottomRight = { clip.right, clip.bottom };
				if (::ClientToScreen(hwnd, &clipTopLeft) &&
					::ClientToScreen(hwnd, &clipBottomRight))
				{
					clip = { clipTopLeft.x, clipTopLeft.y,
						clipBottomRight.x, clipBottomRight.y };
					self->_monitorDragCursorClipped = ::ClipCursor(&clip) != FALSE;
				}
			}
			::SetCapture(hwnd);
			self->_UpdateMonitorTooltip(point);
			return 0;
		}
	}
	if (self && msg == WM_TIMER && wParam == MONITOR_TOOLTIP_TIMER)
	{
		self->_ShowMonitorTooltip(hwnd);
		return 0;
	}
	if (self && msg == WM_MOUSEMOVE && self->_draggingMonitor >= 0 && ::GetCapture() == hwnd)
	{
		POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		LONG dx = MulDiv(point.x - self->_dragStartPoint.x, 1000, self->_dragScale);
		LONG dy = MulDiv(point.y - self->_dragStartPoint.y, 1000, self->_dragScale);
		RECT moved = self->_dragStartRect;
		OffsetRect(&moved, dx, dy);
		self->_dragMoved = self->_dragMoved || dx != 0 || dy != 0;
		if (dx != 0 || dy != 0)
			self->_dragHorizontalPlane = abs(dx) >= abs(dy);
		if (::GetKeyState(VK_CONTROL) >= 0)
			self->_SnapMonitorRect(self->_draggingMonitor, moved);
		if (self->_dragMoved)
			self->_monitors[self->_draggingMonitor].rect = moved;
		::InvalidateRect(hwnd, nullptr, FALSE);
		self->_UpdateMonitorTooltip(point);
		return 0;
	}
	if (self && msg == WM_MOUSEMOVE)
	{
		POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		self->_UpdateMonitorTooltip(point);
	}
	if (self && msg == WM_MOUSELEAVE)
	{
		self->_trackingMouse = false;
		if (self->_draggingMonitor >= 0) return 0;
		::KillTimer(hwnd, MONITOR_TOOLTIP_TIMER);
		if (self->_tooltipActive && self->_monitorTooltip)
		{
			TOOLINFOW tool = {};
			tool.cbSize = sizeof(tool);
			tool.hwnd = hwnd;
			tool.uId = 1;
			::SendMessageW(self->_monitorTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
			self->_tooltipActive = false;
		}
		self->_tooltipTarget = -1;
		return 0;
	}
	if (self && msg == WM_LBUTTONUP && self->_draggingMonitor >= 0)
	{
		if (::GetCapture() == hwnd)
		{
			POINT point = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			LONG dx = MulDiv(point.x - self->_dragStartPoint.x, 1000, self->_dragScale);
			LONG dy = MulDiv(point.y - self->_dragStartPoint.y, 1000, self->_dragScale);
			RECT moved = self->_dragStartRect;
			OffsetRect(&moved, dx, dy);
			self->_dragMoved = self->_dragMoved || dx != 0 || dy != 0;
			if (dx != 0 || dy != 0)
				self->_dragHorizontalPlane = abs(dx) >= abs(dy);
			if (self->_dragMoved)
				self->_monitors[self->_draggingMonitor].rect = moved;
			::InvalidateRect(hwnd, nullptr, FALSE);
			::UpdateWindow(hwnd);
			self->_FinishMonitorDrag(hwnd);
			self->_UpdateMonitorTooltip(point);
		}
		return 0;
	}
	if (self && (msg == WM_CANCELMODE || msg == WM_CAPTURECHANGED) && self->_draggingMonitor >= 0)
	{
		for (size_t index = 0; index < self->_dragOriginalRects.size(); ++index)
			self->_monitors[index].rect = self->_dragOriginalRects[index];
		self->_dragOriginalRects.clear();
		self->_RestoreMonitorDragCursorClip();
		self->_draggingMonitor = -1;
		self->_dragScale = 0;
		self->_dragMoved = false;
		self->_dragHorizontalPlane = true;
		::KillTimer(hwnd, MONITOR_TOOLTIP_TIMER);
		if (self->_tooltipActive && self->_monitorTooltip)
		{
			TOOLINFOW tool = {};
			tool.cbSize = sizeof(tool);
			tool.hwnd = hwnd;
			tool.uId = 1;
			::SendMessageW(self->_monitorTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
			self->_tooltipActive = false;
		}
		self->_tooltipTarget = -1;
		if (msg == WM_CANCELMODE && ::GetCapture() == hwnd) ::ReleaseCapture();
		::InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}
	if (self && msg == WM_ERASEBKGND) return 1;
	if (self && self->_oldMonitorPreviewProc)
		return ::CallWindowProcW(self->_oldMonitorPreviewProc, hwnd, msg, wParam, lParam);
	return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CSettingsDlgProc::_RestoreMonitorDragCursorClip()
{
	if (!_monitorDragCursorClipped) return;
	::ClipCursor(&_previousCursorClip);
	_monitorDragCursorClipped = false;
}

void CSettingsDlgProc::_SnapMonitorRect(int index, RECT& rect, bool force)
{
	if (index < 0 || index >= (int)_monitors.size() || _dragScale <= 0) return;
	const LONG tolerance = 6; // ThemeUI's SnapMonitorRect uses inclusive +/-6 preview pixels.
	auto toPreviewX = [&](LONG x) { return MulDiv(x - _dragWorldBounds.left, _dragScale, 1000); };
	auto toPreviewY = [&](LONG y) { return MulDiv(y - _dragWorldBounds.top, _dragScale, 1000); };
	if (force)
	{
		// On release, attach to an actual shared edge. Preserve the dropped
		// offset along it, except when nearby parallel edges can align exactly.
		// If there is no overlap, shift only enough to create a shared segment.
		const LONG width = rect.right - rect.left;
		const LONG height = rect.bottom - rect.top;
		RECT nearest = rect;
		LONGLONG nearestDistance = LLONG_MAX;
		bool found = false;
		bool nearestSideAttachment = false;
		auto intersectsAnother = [&](const RECT& candidate)
		{
			for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
			{
				if (otherIndex == index) continue;
				RECT intersection = {};
				if (IntersectRect(&intersection, &candidate, &_monitors[otherIndex].rect))
					return true;
			}
			return false;
		};
		auto considerAdjacent = [&](RECT candidate, const RECT& neighbor, bool sideAttachment)
		{
			RECT aligned = SnapAttachedMonitorAlignment(candidate, neighbor, sideAttachment, _dragScale, tolerance);
			if (!intersectsAnother(aligned)) candidate = aligned;
			if (intersectsAnother(candidate)) return;
			LONGLONG dx = toPreviewX(candidate.left) - toPreviewX(rect.left);
			LONGLONG dy = toPreviewY(candidate.top) - toPreviewY(rect.top);
			LONGLONG distance = dx * dx + dy * dy;
			// Choose the nearest valid edge from the actual dropped position,
			// independent of drag direction. If distances tie, prefer a vertical
			// side so the monitor keeps the closest possible Y to where it landed.
			if (!found || distance < nearestDistance ||
				(distance == nearestDistance && sideAttachment && !nearestSideAttachment))
			{
				nearest = candidate;
				nearestDistance = distance;
				found = true;
				nearestSideAttachment = sideAttachment;
			}
		};
		for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
		{
			if (otherIndex == index) continue;
			const RECT& other = _monitors[otherIndex].rect;
			const LONG nearestTop = max(other.top - height + 1,
				min(rect.top, other.bottom - 1));
			const LONG nearestLeft = max(other.left - width + 1,
				min(rect.left, other.right - 1));
			considerAdjacent({ other.left - width, nearestTop,
				other.left, nearestTop + height }, other, true);
			considerAdjacent({ other.right, nearestTop,
				other.right + width, nearestTop + height }, other, true);
			considerAdjacent({ nearestLeft, other.top - height,
				nearestLeft + width, other.top }, other, false);
			considerAdjacent({ nearestLeft, other.bottom,
				nearestLeft + width, other.bottom + height }, other, false);
		}
		if (found) rect = nearest;
		return;
	}
	const bool horizontalDrag = _dragHorizontalPlane;
	if (horizontalDrag)
	{
		// During an X-plane drag, seek a true left/right attachment first. Keep
		// the dropped Y if it already overlaps the neighbor; otherwise shift it
		// only enough to make the edges share a positive-length segment.
		RECT nearest = rect;
		LONGLONG nearestDistance = LLONG_MAX;
		bool found = false;
		bool nearestSideAttachment = false;
		auto intersectsAnother = [&](const RECT& candidate)
		{
			for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
			{
				if (otherIndex == index) continue;
				RECT intersection = {};
				if (IntersectRect(&intersection, &candidate, &_monitors[otherIndex].rect))
					return true;
			}
			return false;
		};
		auto considerSide = [&](const RECT& other, bool attachLeft)
		{
			const LONG width = rect.right - rect.left;
			const LONG height = rect.bottom - rect.top;
			const LONG targetLeft = attachLeft ? other.left - width : other.right;
			const LONG movingEdge = attachLeft ? rect.right : rect.left;
			const LONG edgeDistance = abs(toPreviewX(targetLeft) - toPreviewX(movingEdge));
			if (edgeDistance > tolerance)
				return;
			const LONG nearestTop = max(other.top - height + 1,
				min(rect.top, other.bottom - 1));
			RECT candidate = { targetLeft, nearestTop,
				targetLeft + width, nearestTop + height };
			RECT aligned = SnapAttachedMonitorAlignment(candidate, other, true, _dragScale, tolerance);
			if (!intersectsAnother(aligned)) candidate = aligned;
			if (intersectsAnother(candidate)) return;
			const LONGLONG dx = toPreviewX(candidate.left) - toPreviewX(rect.left);
			const LONGLONG dy = toPreviewY(candidate.top) - toPreviewY(rect.top);
			const LONGLONG distance = dx * dx + dy * dy;
			if (!found || distance < nearestDistance ||
				(distance == nearestDistance && !nearestSideAttachment))
			{
				nearest = candidate;
				nearestDistance = distance;
				found = true;
				nearestSideAttachment = true;
			}
		};
		for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
		{
			if (otherIndex == index) continue;
			const RECT& other = _monitors[otherIndex].rect;
			considerSide(other, true);
			considerSide(other, false);
		}
		// Active monitors can also align their top edge to the virtual desktop's
		// Y=0 origin while dragging, just as they can align their X origin.
		const LONG originYDistance = abs(toPreviewY(rect.top) - toPreviewY(0));
		if (_monitors[index].active && originYDistance <= tolerance)
		{
			RECT origin = rect;
			OffsetRect(&origin, 0, -origin.top);
			if (!intersectsAnother(origin))
			{
				const LONGLONG dy = toPreviewY(origin.top) - toPreviewY(rect.top);
				const LONGLONG distance = dy * dy;
				if (!found || distance < nearestDistance)
				{
					nearest = origin;
					nearestDistance = distance;
					found = true;
					nearestSideAttachment = false;
				}
			}
		}
		if (found) rect = nearest;
		else if (abs(toPreviewX(rect.left) - toPreviewX(0)) <= tolerance)
		{
			RECT origin = rect;
			OffsetRect(&origin, -origin.left, 0);
			if (!intersectsAnother(origin)) rect = origin;
		}
		return;
	}
	LONG bestX = LONG_MAX;
	LONG bestXDistance = LONG_MAX;
	LONG bestY = LONG_MAX;
	LONG bestYDistance = LONG_MAX;
	auto considerX = [&](LONG movingEdge, LONG targetEdge)
	{
		LONG distance = abs(toPreviewX(targetEdge) - toPreviewX(movingEdge));
		if ((force || distance <= tolerance) && distance < bestXDistance)
		{
			bestXDistance = distance;
			bestX = targetEdge - movingEdge;
		}
	};
	auto considerY = [&](LONG movingEdge, LONG targetEdge)
	{
		LONG distance = abs(toPreviewY(targetEdge) - toPreviewY(movingEdge));
		if ((force || distance <= tolerance) && distance < bestYDistance)
		{
			bestYDistance = distance;
			bestY = targetEdge - movingEdge;
		}
	};

	RECT preview = rect;
	preview.left = toPreviewX(rect.left);
	preview.right = toPreviewX(rect.right);
	preview.top = toPreviewY(rect.top);
	preview.bottom = toPreviewY(rect.bottom);
	for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
	{
		if (otherIndex == index) continue;
		const RECT& other = _monitors[otherIndex].rect;
		RECT otherPreview = {
			toPreviewX(other.left), toPreviewY(other.top),
			toPreviewX(other.right), toPreviewY(other.bottom)
		};
		// Top/bottom edge snaps may line up screens while they are separated
		// horizontally; horizontal drags use the side-attachment path above.
		const bool nearY = preview.top < otherPreview.bottom + tolerance &&
			preview.bottom > otherPreview.top - tolerance;
		if (nearY) considerX(rect.right, other.left);
		considerY(rect.bottom, other.top);
		considerY(rect.bottom, other.bottom);
		considerY(rect.top, other.top);
		considerY(rect.top, other.bottom);
	}
	// XP additionally lets the dragged display settle onto the virtual desktop
	// origin; preserve the monitor's existing visual offset but store exact zero.
	const LONG originXDistance = abs(toPreviewX(rect.left) - toPreviewX(0));
	if ((force || originXDistance <= tolerance) && originXDistance < bestXDistance)
	{
		bestX = -rect.left;
		bestXDistance = originXDistance;
	}
	const LONG originYDistance = abs(toPreviewY(rect.top) - toPreviewY(0));
	if ((force || originYDistance <= tolerance) && originYDistance < bestYDistance)
	{
		bestY = -rect.top;
		bestYDistance = originYDistance;
	}
	// Snap to the nearest single edge, not both axes independently. A vertical
	// side attachment changes X and preserves the dragged Y; a top/bottom
	// attachment changes Y and preserves X. This prevents corner-only placement.
	if (bestX != LONG_MAX && bestY != LONG_MAX)
	{
		// An already-aligned Y edge is not a reason to discard a nearby vertical
		// side snap; preserve Y unless a top/bottom correction is actually nearer.
		if (bestYDistance == 0 || bestXDistance <= bestYDistance)
			bestY = LONG_MAX;
		else
			bestX = LONG_MAX;
	}
	if (bestX != LONG_MAX) OffsetRect(&rect, bestX, 0);
	if (bestY != LONG_MAX) OffsetRect(&rect, 0, bestY);

	// Snapping individual edges can still leave the dragged monitor overlapping
	// another one (especially when it is released well inside a monitor). If so,
	// move it to the nearest collision-free border. Each candidate changes only
	// one axis, preserving the user's offset along that border.
	auto overlapsMonitor = [&](const RECT& candidate)
	{
		for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
		{
			if (otherIndex == index) continue;
			RECT intersection = {};
			if (IntersectRect(&intersection, &candidate, &_monitors[otherIndex].rect))
				return true;
		}
		return false;
	};
	if (overlapsMonitor(rect))
	{
		RECT nearest = rect;
		LONG nearestDistance = LONG_MAX;
		bool found = false;
		auto considerBorder = [&](LONG dx, LONG dy)
		{
			RECT candidate = rect;
			OffsetRect(&candidate, dx, dy);
			if (overlapsMonitor(candidate)) return;
			LONG distanceX = abs(toPreviewX(rect.left + dx) - toPreviewX(rect.left));
			LONG distanceY = abs(toPreviewY(rect.top + dy) - toPreviewY(rect.top));
			LONG distance = max(distanceX, distanceY);
			if (!found || distance < nearestDistance)
			{
				nearest = candidate;
				nearestDistance = distance;
				found = true;
			}
		};
		for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
		{
			if (otherIndex == index) continue;
			const RECT& other = _monitors[otherIndex].rect;
			considerBorder(other.left - rect.right, 0);  // left edge
			considerBorder(other.right - rect.left, 0); // right edge
			considerBorder(0, other.top - rect.bottom); // top edge
			considerBorder(0, other.bottom - rect.top);// bottom edge
		}
		if (found) rect = nearest;
	}
	// Top/bottom attachments also align X during a drag, not just on release.
	for (int otherIndex = 0; otherIndex < (int)_monitors.size(); ++otherIndex)
	{
		if (otherIndex == index) continue;
		const RECT& other = _monitors[otherIndex].rect;
		if ((rect.bottom == other.top || rect.top == other.bottom) &&
			max(rect.left, other.left) < min(rect.right, other.right))
		{
			RECT aligned = SnapAttachedMonitorAlignment(rect, other, false, _dragScale, tolerance);
			if (!overlapsMonitor(aligned)) rect = aligned;
		}
	}
}

void CSettingsDlgProc::_FinishMonitorDrag(HWND hwnd)
{
	const int index = _draggingMonitor;
	if (index < 0 || index >= (int)_monitors.size()) return;
	const bool inactive = !_monitors[index].active;
	const RECT dropped = _monitors[index].rect;
	const bool moved = _dragMoved;
	if (!moved)
		for (size_t restore = 0; restore < _dragOriginalRects.size(); ++restore)
			_monitors[restore].rect = _dragOriginalRects[restore];
	_dragOriginalRects.clear();
	_draggingMonitor = -1;
	_dragMoved = false;
	_RestoreMonitorDragCursorClip();
	if (::GetCapture() == hwnd) ::ReleaseCapture();
	if (moved && inactive)
	{
		WCHAR title[64] = {};
		StringCchPrintfW(title, ARRAYSIZE(title),
			LoadDeskString(IDS_MONITOR_NUMBER_FORMAT).c_str(), index + 1);
		int answer = ::MessageBoxW(m_hWnd,
			LoadDeskString(IDS_MONITOR_ENABLE_PROMPT).c_str(),
			title, MB_YESNO | MB_ICONINFORMATION);
		// Keep the dropped position visible while the prompt is shown; once the
		// user answers, return it to the nearest collision-free adjacent edge.
		RECT snapped = dropped;
		_SnapMonitorRect(index, snapped, true);
		_monitors[index].rect = snapped;
		if (answer == IDYES)
		{
			_monitors[index].enableRequested = true;
		}
		else
		{
			// Keep the preview at its snapped edge, but declining activation must
			// not mark a system arrangement change as pending.
			_monitors[index].enableRequested = false;
		}
	}
	else if (moved)
	{
		RECT snapped = dropped;
		// Active displays also settle onto an adjacent edge when dragging ends.
		_SnapMonitorRect(index, snapped, true);
		_monitors[index].rect = snapped;
	}
	_UpdatePendingDisplayState();
	_dragScale = 0;
	_dragHorizontalPlane = true;
	::InvalidateRect(hwnd, nullptr, FALSE);
}

void CSettingsDlgProc::_UpdateMonitorTooltip(POINT point)
{
	if (!_monitorTooltip) return;
	if (!_trackingMouse)
	{
		TRACKMOUSEEVENT tracking = { sizeof(tracking), TME_LEAVE, _mulMonPreview, 0 };
		_trackingMouse = ::TrackMouseEvent(&tracking) != FALSE;
	}

	int index = _draggingMonitor;
	if (index < 0 || index >= (int)_monitors.size())
	{
		index = -1;
		for (int i = (int)_previewHitRects.size() - 1; i >= 0; --i)
		{
			if (PtInRect(&_previewHitRects[i], point))
			{
				index = i;
				break;
			}
		}
	}
	if (index < 0 || index >= (int)_monitors.size())
	{
		::KillTimer(_mulMonPreview, MONITOR_TOOLTIP_TIMER);
		if (_tooltipActive)
		{
			TOOLINFOW tool = {};
			tool.cbSize = sizeof(tool);
			tool.hwnd = _mulMonPreview;
			tool.uId = 1;
			::SendMessageW(_monitorTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
			_tooltipActive = false;
		}
		_tooltipTarget = -1;
		return;
	}

	int primary = -1;
	for (int i = 0; i < (int)_monitors.size(); ++i)
	{
		if (_monitors[i].primary)
		{
			primary = i;
			break;
		}
	}
	if (primary < 0 && !_monitors.empty()) primary = 0;
	LONG x = primary >= 0 && index < (int)_previewPixelRects.size() &&
		primary < (int)_previewPixelRects.size() ?
		_previewPixelRects[index].left - _previewPixelRects[primary].left : 0;
	LONG y = primary >= 0 && index < (int)_previewPixelRects.size() &&
		primary < (int)_previewPixelRects.size() ?
		_previewPixelRects[index].top - _previewPixelRects[primary].top : 0;
	WCHAR newText[ARRAYSIZE(_monitorTooltipText)] = {};
	if (!_monitors[index].active && _draggingMonitor < 0)
		StringCchCopyW(newText, ARRAYSIZE(newText),
			LoadDeskString(IDS_NOT_ACTIVE_LABEL).c_str());
	else if (_monitors[index].primary && _draggingMonitor < 0)
		StringCchCopyW(newText, ARRAYSIZE(newText),
			LoadDeskString(IDS_PRIMARY_DISPLAY_LABEL).c_str());
	else
		StringCchPrintfW(newText, ARRAYSIZE(newText), L"%ld, %ld", x, y);

	TOOLINFOW tool = {};
	tool.cbSize = sizeof(tool);
	tool.hwnd = _mulMonPreview;
	tool.uId = 1;
	tool.lpszText = _monitorTooltipText;
	if (index != _tooltipTarget)
	{
		::KillTimer(_mulMonPreview, MONITOR_TOOLTIP_TIMER);
		if (_tooltipActive)
		{
			::SendMessageW(_monitorTooltip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&tool);
			_tooltipActive = false;
		}
		_tooltipTarget = index;
		::SetTimer(_mulMonPreview, MONITOR_TOOLTIP_TIMER, 550, nullptr);
	}
	if (StrCmpW(_monitorTooltipText, newText) != 0)
	{
		StringCchCopyW(_monitorTooltipText, ARRAYSIZE(_monitorTooltipText), newText);
		::SendMessageW(_monitorTooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&tool);
	}
	if (_tooltipActive && _draggingMonitor >= 0)
		_PositionMonitorTooltip(_mulMonPreview);
}

void CSettingsDlgProc::_PositionMonitorTooltip(HWND hwnd)
{
	if (!_monitorTooltip || _tooltipTarget < 0 ||
		_tooltipTarget >= (int)_previewHitRects.size()) return;
	const RECT& monitor = _previewHitRects[_tooltipTarget];
	POINT position = {};
	if (_draggingMonitor >= 0)
	{
		// Do not ask the tooltip control for TTM_GETBUBBLESIZE here. That message
		// re-enters the active tracking tooltip while it is handling TTN_SHOW;
		// on systems with tooltip WndProc hooks this can reach the text painter
		// with a null layout object. Measure our short, single-line coordinate
		// label locally instead.
		LONG bubbleWidth = 0;
		HDC dc = ::GetDC(_monitorTooltip);
		if (dc)
		{
			HFONT font = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
			HGDIOBJ oldFont = font ? ::SelectObject(dc, font) : nullptr;
			SIZE textSize = {};
			if (::GetTextExtentPoint32W(dc, _monitorTooltipText,
				lstrlenW(_monitorTooltipText), &textSize))
				bubbleWidth = textSize.cx + 12; // text padding and tooltip border
			if (oldFont) ::SelectObject(dc, oldFont);
			::ReleaseDC(_monitorTooltip, dc);
		}
		if (bubbleWidth <= 0)
			bubbleWidth = static_cast<LONG>(lstrlenW(_monitorTooltipText) * 8 + 12);
		// With TTF_CENTERTIP, center its anchor by half the measured bubble
		// width so the tooltip's left edge starts at the monitor's top-left.
		position = { monitor.left + bubbleWidth / 2, monitor.top };
	}
	else
	{
		position = {
			(monitor.left + monitor.right) / 2,
			monitor.bottom + 3
		};
	}
	::ClientToScreen(hwnd, &position);
	::SendMessageW(_monitorTooltip, TTM_TRACKPOSITION, 0,
		MAKELONG((short)position.x, (short)position.y));
}

void CSettingsDlgProc::_ShowMonitorTooltip(HWND hwnd)
{
	::KillTimer(hwnd, MONITOR_TOOLTIP_TIMER);
	if (_tooltipActive || _tooltipTarget < 0 ||
		_tooltipTarget >= (int)_monitors.size()) return;
	TOOLINFOW tool = {};
	tool.cbSize = sizeof(tool);
	tool.hwnd = hwnd;
	tool.uId = 1;
	tool.lpszText = _monitorTooltipText;
	_PositionMonitorTooltip(hwnd);
	::SendMessageW(_monitorTooltip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&tool);
	_tooltipActive = true;
	_PositionMonitorTooltip(hwnd);
}

std::vector<RECT> CSettingsDlgProc::_GetPendingMonitorRects(int selected) const
{
	std::vector<RECT> before, after;
	size_t primary = 0;
	for (size_t index = 0; index < _monitors.size(); ++index)
	{
		const MONITORPREVIEW& monitor = _monitors[index];
		before.push_back(monitor.rect);
		RECT rect = monitor.rect;
		RESINFO mode = {};
		bool hasPendingMode = _GetPendingMode(monitor.deviceName, mode);
		if (!hasPendingMode && !monitor.active && monitor.enableRequested)
		{
			// A disabled monitor's small glyph is only a placeholder. Once queued
			// for extension, show and snap its real saved mode dimensions.
			DEVMODEW saved = {};
			saved.dmSize = sizeof(saved);
			if (::EnumDisplaySettingsExW(monitor.applyDeviceName, ENUM_REGISTRY_SETTINGS, &saved, 0))
			{
				mode.width = saved.dmPelsWidth;
				mode.height = saved.dmPelsHeight;
				hasPendingMode = mode.width != 0 && mode.height != 0;
			}
		}
		if (monitor.active && !hasPendingMode)
		{
			// A mode restored to its original slider position must also undo
			// dimensions frozen by an earlier drag.
			rect.right = rect.left + monitor.originalRect.right - monitor.originalRect.left;
			rect.bottom = rect.top + monitor.originalRect.bottom - monitor.originalRect.top;
		}
		if (hasPendingMode)
		{
			rect.right = rect.left + static_cast<LONG>(mode.width);
			rect.bottom = rect.top + static_cast<LONG>(mode.height);
		}
		if (static_cast<int>(index) == selected && !_arrResInfo.empty())
		{
			const int slider = max(0, min(static_cast<int>(::SendMessageW(_trackResolution,
				TBM_GETPOS, 0, 0)), static_cast<int>(_arrResInfo.size()) - 1));
			rect.right = rect.left + static_cast<LONG>(_arrResInfo[slider].width);
			rect.bottom = rect.top + static_cast<LONG>(_arrResInfo[slider].height);
		}
		after.push_back(rect);
		if (monitor.primary) primary = index;
	}
	return ResizeMonitorArrangement(before, after, primary);
}

void CSettingsDlgProc::_PaintMonitorPreview(HDC hdc, RECT client)
{
	// On XP, COLOR_APPWORKSPACE resolves to this gray in the monitor preview.
	// The host OS resolves that role to a lighter color, so keep the XP value.
	constexpr COLORREF monitorWorkspace = RGB(128, 128, 128);
	HBRUSH background = CreateSolidBrush(monitorWorkspace);
	FillRect(hdc, &client, background);
	DeleteObject(background);
	if (_monitors.empty()) return;

	int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	LPCWSTR selectedDevice = nullptr;
	if (selectedItem >= 0)
	{
		LPARAM itemData = ComboBox_GetItemData(_cmbMonitors, selectedItem);
		if (itemData != CB_ERR) selectedDevice = reinterpret_cast<LPCWSTR>(itemData);
	}
	int selected = -1;
	for (int i = 0; i < (int)_monitors.size(); ++i)
	{
		if (selectedDevice && *selectedDevice &&
			StrCmpI(selectedDevice, _monitors[i].deviceName) == 0)
		{
			selected = i;
			break;
		}
	}
	if (selected < 0)
	{
		for (int i = 0; i < (int)_monitors.size(); ++i)
		{
			if (_monitors[i].primary) { selected = i; break; }
		}
	}
	if (selected < 0) selected = 0;
	// The combo box and resolution slider describe the mode being edited. A
	// click in the arrangement preview only changes the visual highlight; it
	// must never cause that monitor to inherit another display's resolution.
	const int highlighted = (_previewSelection >= 0 &&
		_previewSelection < (int)_monitors.size()) ? _previewSelection : selected;
	const std::vector<RECT> previewRects = _GetPendingMonitorRects(selected);
	auto overlaps = [](LONG a0, LONG a1, LONG b0, LONG b1)
	{
		return max(a0, b0) < min(a1, b1);
	};

	RECT bounds = previewRects[0];
	for (const RECT& monitor : previewRects) UnionRect(&bounds, &bounds, &monitor);
	if (_draggingMonitor >= 0 && _dragScale > 0)
		bounds = _dragWorldBounds;
	LONG virtualWidth = max(1L, bounds.right - bounds.left);
	LONG virtualHeight = max(1L, bounds.bottom - bounds.top);

	// _CleanupRects in ThemeUI fits the current aligned desktop into the client
	// area with an 8-pixel inset, then applies its characteristic 2/3 scale.
	// Importantly this uses the current mode, never the largest slider mode.
	LONG clientWidth = max(1L, client.right - client.left);
	LONG clientHeight = max(1L, client.bottom - client.top);
	int scale = _draggingMonitor >= 0 && _dragScale > 0 ? _dragScale : 0;
	int diagramLeft = _draggingMonitor >= 0 ? _dragDiagramLeft : 0;
	int diagramTop = _draggingMonitor >= 0 ? _dragDiagramTop : 0;
	if (scale == 0)
	{
		int fitX = MulDiv(max(1L, clientWidth - 16), 1000, virtualWidth);
		int fitY = MulDiv(max(1L, clientHeight - 16), 1000, virtualHeight);
		scale = max(1, min(fitX, fitY) * 2 / 3);
		int diagramWidth = MulDiv(virtualWidth, scale, 1000);
		int diagramHeight = MulDiv(virtualHeight, scale, 1000);
		diagramLeft = client.left + ((client.right - client.left) - diagramWidth) / 2;
		diagramTop = client.top + ((client.bottom - client.top) - diagramHeight) / 2;
	}
	_previewBounds = bounds;
	_previewScale = scale;
	_previewDiagramLeft = diagramLeft;
	_previewDiagramTop = diagramTop;
	_previewPixelRects = previewRects;
	// The XP reference shows an 8-pixel gutter between touching monitor glyphs.
	// Keep that presentation offset separate from desktop-coordinate rectangles
	// used for snapping and applying the real arrangement.
	const LONG monitorGap = 8;
	std::vector<POINT> visualOffsets(_monitors.size());
	std::vector<bool> hasVisualOffset(_monitors.size(), false);
	int gapRoot = 0;
	for (int i = 0; i < (int)_monitors.size(); ++i)
	{
		if (_monitors[i].primary) { gapRoot = i; break; }
	}
	if (!_monitors.empty()) hasVisualOffset[gapRoot] = true;
	for (size_t pass = 0; pass < _monitors.size(); ++pass)
	{
		bool changed = false;
		for (int i = 0; i < (int)_monitors.size(); ++i)
		{
			if (!hasVisualOffset[i]) continue;
			for (int j = 0; j < (int)_monitors.size(); ++j)
			{
				if (hasVisualOffset[j] || i == j) continue;
				const RECT& a = previewRects[i];
				const RECT& b = previewRects[j];
				if (overlaps(a.top, a.bottom, b.top, b.bottom))
				{
					if (a.right == b.left)
						visualOffsets[j] = { visualOffsets[i].x + monitorGap, visualOffsets[i].y };
					else if (a.left == b.right)
						visualOffsets[j] = { visualOffsets[i].x - monitorGap, visualOffsets[i].y };
					else
						continue;
					hasVisualOffset[j] = true;
					changed = true;
				}
				else if (overlaps(a.left, a.right, b.left, b.right))
				{
					if (a.bottom == b.top)
						visualOffsets[j] = { visualOffsets[i].x, visualOffsets[i].y + monitorGap };
					else if (a.top == b.bottom)
						visualOffsets[j] = { visualOffsets[i].x, visualOffsets[i].y - monitorGap };
					else
						continue;
					hasVisualOffset[j] = true;
					changed = true;
				}
			}
		}
		if (!changed) break;
	}
	if (_draggingMonitor >= 0 && _draggingMonitor < (int)visualOffsets.size())
		visualOffsets[_draggingMonitor] = _dragVisualOffset;
	_previewDrawRects.resize(_monitors.size());
	_previewHitRects.resize(_monitors.size());
	RECT selectionRect = {};
	if (highlighted >= 0 && highlighted < (int)_monitors.size())
	{
		RECT selectedGeometry = previewRects[highlighted];
		selectionRect = {
			diagramLeft + MulDiv(selectedGeometry.left - bounds.left, scale, 1000),
			diagramTop + MulDiv(selectedGeometry.top - bounds.top, scale, 1000),
			diagramLeft + MulDiv(selectedGeometry.right - bounds.left, scale, 1000),
			diagramTop + MulDiv(selectedGeometry.bottom - bounds.top, scale, 1000)
		};
		OffsetRect(&selectionRect, visualOffsets[highlighted].x, visualOffsets[highlighted].y);
		InflateRect(&selectionRect, 4, 4);
	}
	// Composite in three layers: inactive glyphs, the blue selection surround,
	// then active glyphs. Thus the surround overlays inactive boxes but active
	// displays (including the selected one) remain visually in front of it.
	std::vector<int> monitorPaintOrder;
	monitorPaintOrder.reserve(_monitors.size() + 2);
	for (int i = 0; i < (int)_monitors.size(); ++i)
		if (!_monitors[i].active) monitorPaintOrder.push_back(i);
	monitorPaintOrder.push_back(-2); // disabled stipple is part of the bitmap
	monitorPaintOrder.push_back(-1); // image-list blend over the stippled bitmap
	for (int i = 0; i < (int)_monitors.size(); ++i)
		if (_monitors[i].active && i != highlighted) monitorPaintOrder.push_back(i);
	if (highlighted >= 0 && _monitors[highlighted].active)
		monitorPaintOrder.push_back(highlighted);
	for (int paintOrder = 0; paintOrder < (int)monitorPaintOrder.size(); ++paintOrder)
	{
		int i = monitorPaintOrder[paintOrder];
		if (i == -1)
		{
			if (highlighted >= 0)
			{
				// ILD_BLEND50 is a one-pixel checker blend with COLOR_HIGHLIGHT,
				// not a smooth AlphaBlend. The inactive bitmap's alternate pixels
				// remain visible between these highlight-colored pixels.
				const COLORREF highlightColor = GetSysColor(COLOR_HIGHLIGHT);
				for (LONG y = selectionRect.top; y < selectionRect.bottom; ++y)
				{
					for (LONG x = selectionRect.left + ((y - selectionRect.top) & 1);
						x < selectionRect.right; x += 2)
						SetPixelV(hdc, x, y, highlightColor);
				}
			}
			continue;
		}
		if (i == -2)
		{
			// ThemeUI's disabled stipple is in the image before ILD_BLEND50 is
			// applied, so put it below the selected-image blend.
			for (int inactive = 0; inactive < (int)_monitors.size(); ++inactive)
			{
				if (_monitors[inactive].active) continue;
				const RECT& stipple = _previewHitRects[inactive];
				for (LONG y = stipple.top; y < stipple.bottom; ++y)
				{
					for (LONG x = stipple.left + ((y - stipple.top) & 1);
						x < stipple.right; x += 2)
						SetPixelV(hdc, x, y, monitorWorkspace);
				}
			}
			continue;
		}
		RECT source = previewRects[i];
		RECT geometry = {
			diagramLeft + MulDiv(source.left - bounds.left, scale, 1000),
			diagramTop + MulDiv(source.top - bounds.top, scale, 1000),
			diagramLeft + MulDiv(source.right - bounds.left, scale, 1000),
			diagramTop + MulDiv(source.bottom - bounds.top, scale, 1000)
		};
		_previewDrawRects[i] = geometry;
		RECT draw = geometry;
		OffsetRect(&draw, visualOffsets[i].x, visualOffsets[i].y);
		_previewHitRects[i] = draw;
		// MakeMonitorBitmap builds a layered glyph: COLOR_APPWORKSPACE outside,
		// a four-pixel COLOR_BTNHIGHLIGHT surround, then a one-pixel inset and
		// COLOR_BACKGROUND screen. A selected glyph substitutes COLOR_HIGHLIGHT
		// for the outer layer; that surround lies outside monitor geometry.
		const COLORREF monitorFace = GetSysColor(COLOR_BACKGROUND);
		const bool isHighlighted = i == highlighted;
		RECT outer = isHighlighted ? selectionRect : draw;
		const COLORREF windowFrame = GetSysColor(COLOR_WINDOWFRAME);
		const COLORREF fallbackColor = windowFrame != monitorWorkspace
			? windowFrame : (monitorWorkspace ^ RGB(255, 255, 255));
		COLORREF outerColor = isHighlighted
			? GetSysColor(COLOR_HIGHLIGHT) : monitorWorkspace;
		if (isHighlighted && outerColor == monitorWorkspace)
			outerColor = fallbackColor;
		HBRUSH outerBrush = CreateSolidBrush(outerColor);
		FillRect(hdc, &outer, outerBrush);
		DeleteObject(outerBrush);
		RECT edge = outer;
		InflateRect(&edge, -4, -4);
		// MakeMonitorBitmap insets the button-highlight layer by four pixels;
		// its screen face is inset one additional pixel from that layer.
		COLORREF edgeColor = GetSysColor(COLOR_BTNHIGHLIGHT);
		if (edgeColor == monitorWorkspace) edgeColor = fallbackColor;
		if (edge.right > edge.left && edge.bottom > edge.top)
		{
			HBRUSH edgeBrush = CreateSolidBrush(edgeColor);
			FillRect(hdc, &edge, edgeBrush);
			DeleteObject(edgeBrush);
		}
		InflateRect(&edge, -1, -1);
		if (edge.right > edge.left && edge.bottom > edge.top)
		{
			HBRUSH faceBrush = CreateSolidBrush(monitorFace);
			FillRect(hdc, &edge, faceBrush);
			DeleteObject(faceBrush);
		}
		WCHAR number[8];
		StringCchPrintf(number, ARRAYSIZE(number), L"%d", i + 1);

		// DrawMonitorNum renders to an off-screen monitor bitmap. Translate that
		// bitmap-local baseline into the monitor's coordinates on this page DC.
		const int monitorWidth = max(1L, draw.right - draw.left);
		const int monitorHeight = max(1L, draw.bottom - draw.top);
		RECT numberBounds = { 0, 0, monitorWidth, monitorHeight };
		int32_t negativeHeight = -17 * monitorHeight;
		int32_t dy = (int32_t)(((int64_t)negativeHeight * 0x60606061LL) >> 32);
		dy >>= 6;
		dy += (int32_t)((uint32_t)dy >> 31);
		int32_t negativeWidth = -16 * monitorWidth;
		int32_t dx = (int32_t)(((int64_t)negativeWidth * (int32_t)0xB21642C9) >> 32);
		dx += negativeWidth;
		dx >>= 7;
		dx += (int32_t)((uint32_t)dx >> 31);
		dx >>= 1;
		InflateRect(&numberBounds, dx, dy);

		LOGFONTW lf = {};
		lf.lfHeight = numberBounds.bottom - numberBounds.top;
		lf.lfWeight = 0x320;
		lf.lfOutPrecision = 7;
		lf.lfPitchAndFamily = 0x20;
		COLORREF numberText = GetSysColor(COLOR_CAPTIONTEXT);
		const COLORREF numberBackground = GetSysColor(COLOR_BACKGROUND);
		if (numberText == numberBackground)
		{
			numberText = GetSysColor(COLOR_WINDOWTEXT);
			if (numberText == numberBackground)
				numberText ^= RGB(255, 255, 255);
		}
		HFONT font = CreateFontIndirectW(&lf);
		HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;
		int saved = SaveDC(hdc);
		IntersectClipRect(hdc, draw.left, draw.top, draw.right, draw.bottom);
		SetTextAlign(hdc, TA_CENTER);
		SetBkMode(hdc, TRANSPARENT);
		SetTextColor(hdc, numberText);
		ExtTextOutW(hdc,
			draw.left + (numberBounds.left + numberBounds.right) / 2,
			draw.top + numberBounds.top, 0, nullptr, number,
			(UINT)lstrlenW(number), nullptr);
		if (saved) RestoreDC(hdc, saved);
		if (oldFont) SelectObject(hdc, oldFont);
		if (font) DeleteObject(font);
	}
}

void CSettingsDlgProc::_UpdateMonitorPreview()
{
	_previewSelection = -1;
	_monitors.clear();
	EnumDisplayMonitors(nullptr, nullptr, _MonitorEnumProc, (LPARAM)this);

	// EnumDisplayMonitors returns active desktop outputs. Discover additional
	// outputs only when their monitor-interface PnP device is currently present;
	// EnumDisplayDevices' top-level list alone includes stale adapter entries.
	RECT activeBounds = {};
	if (!_monitors.empty())
	{
		activeBounds = _monitors[0].rect;
		for (const MONITORPREVIEW& monitor : _monitors)
			UnionRect(&activeBounds, &activeBounds, &monitor.rect);
	}
	RECT desktopBounds = activeBounds;
	if (_monitors.empty())
	{
		desktopBounds = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		activeBounds = desktopBounds;
	}
	// ThemeUI gives unattached displays XP's fixed 640x480 preview geometry;
	// _PaintMonitorPreview applies the same virtual-desktop scale used by active
	// displays rather than estimating an inactive size from the current desktop.
	const LONG inactiveWidth = 640;
	const LONG inactiveHeight = 480;
	HDEVINFO presentMonitors = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MONITOR,
		nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	std::vector<std::wstring> seenMonitorInterfaces;
	UINT inactiveOrdinal = 1;
	for (DWORD adapterIndex = 0; presentMonitors != INVALID_HANDLE_VALUE; ++adapterIndex)
	{
		DISPLAY_DEVICEW adapter = {};
		adapter.cb = sizeof(adapter);
		if (!EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0)) break;
		if (!adapter.DeviceName[0] || (adapter.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) continue;

		for (DWORD childIndex = 0;; ++childIndex)
		{
			DISPLAY_DEVICEW child = {};
			child.cb = sizeof(child);
			if (!EnumDisplayDevicesW(adapter.DeviceName, childIndex, &child,
				EDD_GET_DEVICE_INTERFACE_NAME)) break;
			if (!child.DeviceID[0] ||
				(child.StateFlags & (DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_MIRRORING_DRIVER)))
				continue;

			SP_DEVICE_INTERFACE_DATA interfaceData = {};
			interfaceData.cbSize = sizeof(interfaceData);
			if (!SetupDiOpenDeviceInterfaceW(presentMonitors, child.DeviceID, 0, &interfaceData))
				continue;

			bool duplicate = false;
			for (const std::wstring& seen : seenMonitorInterfaces)
			{
				if (StrCmpIW(seen.c_str(), child.DeviceID) == 0)
				{
					duplicate = true;
					break;
				}
			}
			if (duplicate) continue;
			seenMonitorInterfaces.emplace_back(child.DeviceID);

			RECT rect = { activeBounds.right, desktopBounds.top,
				activeBounds.right + inactiveWidth, desktopBounds.top + inactiveHeight };

			MONITORPREVIEW preview = {};
			preview.rect = rect;
			preview.originalRect = rect;
			preview.active = false;
			StringCchCopyW(preview.monitorName, ARRAYSIZE(preview.monitorName), child.DeviceString);
			GetDeviceInstanceIdForMonitorInterface(child.DeviceID,
				preview.monitorInstanceId, ARRAYSIZE(preview.monitorInstanceId));
			GetMonitorNameFromEdid(preview.monitorInstanceId, preview.monitorName,
				ARRAYSIZE(preview.monitorName));
			StringCchPrintfW(preview.deviceName, ARRAYSIZE(preview.deviceName),
				L"#inactive%u", inactiveOrdinal++);
			StringCchCopyW(preview.applyDeviceName, ARRAYSIZE(preview.applyDeviceName), adapter.DeviceName);
			_monitors.push_back(preview);
			UnionRect(&activeBounds, &activeBounds, &preview.rect);
		}
	}
	if (presentMonitors != INVALID_HANDLE_VALUE)
		SetupDiDestroyDeviceInfoList(presentMonitors);

	// Normalize every detected output, including inactive and virtual ones.
	// These names feed the selector, single-monitor label and Advanced pages.
	for (MONITORPREVIEW& monitor : _monitors)
		EnsureMonitorDisplayName(monitor.monitorName, ARRAYSIZE(monitor.monitorName));
	_UpdateMonitorPreviewLayout();
	::InvalidateRect(_mulMonPreview, nullptr, FALSE);
}

BOOL CSettingsDlgProc::OnApply()
{
	struct DISPLAYUPDATE
	{
		WCHAR deviceName[CCHDEVICENAME];
		DEVMODEW original;
		DEVMODEW updated;
		bool originalPrimary;
		bool primary;
	};
	std::vector<DISPLAYUPDATE> updates;
	_StoreSelectedPendingMode();
	if (!_HasPendingDisplayChanges())
	{
		SetModified(FALSE);
		return PSNRET_NOERROR;
	}
	for (const MONITORPREVIEW& monitor : _monitors)
	{
		if (!monitor.active) continue;
		DEVMODEW live = {};
		live.dmSize = sizeof(live);
		if (!::EnumDisplaySettingsExW(monitor.applyDeviceName, ENUM_CURRENT_SETTINGS, &live, 0) ||
			live.dmPosition.x != monitor.originalRect.left || live.dmPosition.y != monitor.originalRect.top ||
			live.dmPelsWidth != static_cast<DWORD>(monitor.originalRect.right - monitor.originalRect.left) ||
			live.dmPelsHeight != static_cast<DWORD>(monitor.originalRect.bottom - monitor.originalRect.top))
		{
			// Preserve pending edits on notifications, but never apply an old
			// layout over a real driver/topology change that happened meanwhile.
			_pendingModes.clear();
			_UpdateMonitorPreview();
			_EnumerateActiveDisplayDevices();
			_SelectCurrentMonitor();
			_GetAllModes();
			SetModified(FALSE);
			::MessageBoxW(m_hWnd,
				LoadDeskString(IDS_MONITOR_CONFIGURATION_CHANGED).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONINFORMATION);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
	}
	size_t primary = 0;
	for (size_t index = 0; index < _monitors.size(); ++index)
	{
		const MONITORPREVIEW& monitor = _monitors[index];
		if (monitor.primary && monitor.active && !monitor.disableRequested)
		{
			primary = index;
			break;
		}
	}
	// Use exactly the desktop-pixel layout drawn by the arrangement control,
	// including pending resolution shifts. Windows requires the primary at 0,0.
	const std::vector<RECT> applyRects = NormalizeMonitorArrangement(_GetPendingMonitorRects(), primary);

	for (int i = 0; i < (int)_monitors.size(); ++i)
	{
		const MONITORPREVIEW& monitor = _monitors[i];
		RESINFO pendingMode = {};
		const bool hasPendingMode = _GetPendingMode(monitor.deviceName, pendingMode);
		// Mode edits for an inactive output need the explicit Extend checkbox;
		// otherwise applying them would implicitly activate that display.
		if (!monitor.active && !monitor.enableRequested && hasPendingMode)
		{
			::MessageBoxW(m_hWnd,
				LoadDeskString(IDS_EXTEND_MONITOR_FIRST).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONINFORMATION);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
		if (!monitor.active && !monitor.enableRequested) continue;
		if (!monitor.applyDeviceName[0])
		{
			::MessageBoxW(m_hWnd, LoadDeskString(IDS_MONITOR_DEVICE_MISSING).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
		bool duplicate = false;
		for (const DISPLAYUPDATE& update : updates)
		{
			if (StrCmpI(update.deviceName, monitor.applyDeviceName) == 0)
			{
				duplicate = true;
				break;
			}
		}
		if (duplicate)
		{
			::MessageBoxW(m_hWnd, LoadDeskString(IDS_MONITORS_SHARE_DEVICE).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}

		const bool positionChanged = applyRects[i].left != monitor.originalRect.left ||
			applyRects[i].top != monitor.originalRect.top;
		const bool primaryChanged = monitor.primary != monitor.originalPrimary;
		if (!positionChanged && !primaryChanged && !hasPendingMode && !monitor.enableRequested && !monitor.disableRequested) continue;

		DISPLAYUPDATE update = {};
		update.originalPrimary = monitor.originalPrimary;
		update.primary = monitor.primary;
		StringCchCopyW(update.deviceName, ARRAYSIZE(update.deviceName), monitor.applyDeviceName);
		update.original.dmSize = sizeof(update.original);
		DWORD mode = monitor.active ? ENUM_CURRENT_SETTINGS : ENUM_REGISTRY_SETTINGS;
		if (!EnumDisplaySettingsExW(update.deviceName, mode, &update.original, 0))
		{
			if (monitor.active || monitor.enableRequested)
			{
				::MessageBoxW(m_hWnd, monitor.enableRequested ?
					LoadDeskString(IDS_SAVED_MONITOR_CONFIG_READ_ERROR).c_str() :
					LoadDeskString(IDS_CURRENT_DISPLAY_CONFIG_READ_ERROR).c_str(),
					LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
				return PSNRET_INVALID_NOCHANGEPAGE;
			}
			continue;
		}
		update.updated = update.original;
		if (!monitor.active)
		{
			// The registry mode supplies the dimensions for enabling this output,
			// but a rollback must restore its detached state, not enable it again.
			update.original.dmPelsWidth = 0;
			update.original.dmPelsHeight = 0;
			update.original.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
		}
		if (monitor.disableRequested)
		{
			update.updated.dmPelsWidth = 0;
			update.updated.dmPelsHeight = 0;
			update.updated.dmFields |= DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
		}
		else
		{
			update.updated.dmPosition.x = applyRects[i].left;
			update.updated.dmPosition.y = applyRects[i].top;
			update.updated.dmFields |= DM_POSITION;
		}
		if (!monitor.disableRequested && hasPendingMode && (pendingMode.width != update.original.dmPelsWidth ||
			pendingMode.height != update.original.dmPelsHeight))
		{
			update.updated.dmPelsWidth = pendingMode.width;
			update.updated.dmPelsHeight = pendingMode.height;
			update.updated.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
		}
		if (!monitor.disableRequested && hasPendingMode && pendingMode.bpp &&
			pendingMode.bpp != update.original.dmBitsPerPel)
		{
			update.updated.dmBitsPerPel = pendingMode.bpp;
			update.updated.dmFields |= DM_BITSPERPEL;
		}
		if (!positionChanged && !primaryChanged && !hasPendingMode && !monitor.enableRequested && !monitor.disableRequested)
			continue;
		updates.push_back(update);
	}

	if (updates.empty())
	{
		SetModified(FALSE);
		return PSNRET_NOERROR;
	}
	struct ApplyGuard
	{
		bool& applying;
		ApplyGuard(bool& value) : applying(value) { applying = true; }
		~ApplyGuard() { applying = false; }
	} applyGuard(_applyingDisplayChanges);

	for (const DISPLAYUPDATE& update : updates)
	{
		LONG result = ChangeDisplaySettingsExW(update.deviceName,
			(DEVMODEW*)&update.updated, nullptr, CDS_TEST, nullptr);
		if (result != DISP_CHANGE_SUCCESSFUL)
		{
			WCHAR message[160] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_MONITOR_ARRANGEMENT_REJECTED).c_str(), result);
			::MessageBoxW(m_hWnd, message, LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
	}

	std::vector<size_t> staged;
	for (size_t i = 0; i < updates.size(); ++i)
	{
		LONG result = ChangeDisplaySettingsExW(updates[i].deviceName,
			&updates[i].updated, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET |
			(updates[i].primary ? CDS_SET_PRIMARY : 0), nullptr);
		if (result != DISP_CHANGE_SUCCESSFUL)
		{
			for (size_t applied : staged)
				ChangeDisplaySettingsExW(updates[applied].deviceName,
					&updates[applied].original, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET |
					(updates[applied].originalPrimary ? CDS_SET_PRIMARY : 0), nullptr);
			// The new primary may have been staged before the old primary's
			// update failed. Restore the original designation even if that old
			// display was not yet included in the staged list.
			for (const DISPLAYUPDATE& update : updates)
				if (update.originalPrimary)
					ChangeDisplaySettingsExW(update.deviceName, (DEVMODEW*)&update.original,
						nullptr, CDS_UPDATEREGISTRY | CDS_NORESET | CDS_SET_PRIMARY, nullptr);
			::MessageBoxW(m_hWnd, LoadDeskString(IDS_MONITOR_ARRANGEMENT_SAVE_ERROR).c_str(),
				LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
			return PSNRET_INVALID_NOCHANGEPAGE;
		}
		staged.push_back(i);
	}

	LONG applyResult = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
	if (applyResult != DISP_CHANGE_SUCCESSFUL)
	{
		for (const DISPLAYUPDATE& update : updates)
			ChangeDisplaySettingsExW(update.deviceName, (DEVMODEW*)&update.original,
				nullptr, CDS_UPDATEREGISTRY | CDS_NORESET |
				(update.originalPrimary ? CDS_SET_PRIMARY : 0), nullptr);
		ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
		::MessageBoxW(m_hWnd, LoadDeskString(IDS_MONITOR_ARRANGEMENT_APPLY_ERROR).c_str(),
			LoadDeskString(IDS_DISPLAY_PROPERTIES).c_str(), MB_OK | MB_ICONERROR);
		return PSNRET_INVALID_NOCHANGEPAGE;
	}

	CThemeChngDlg dlg;
	if (dlg.DoModal(m_hWnd) != CThemeChngDlg::KeepChanges)
	{
		for (const DISPLAYUPDATE& update : updates)
			ChangeDisplaySettingsExW(update.deviceName, (DEVMODEW*)&update.original,
				nullptr, CDS_UPDATEREGISTRY | CDS_NORESET |
				(update.originalPrimary ? CDS_SET_PRIMARY : 0), nullptr);
		ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
	}
	_pendingModes.clear();
	SetModified(FALSE);
	_UpdateMonitorPreview();
	_EnumerateActiveDisplayDevices();
	_SelectCurrentMonitor();
	_GetAllModes();
	return PSNRET_NOERROR;
}

void CSettingsDlgProc::_SelectCurrentMonitor()
{
	POINT pt;
	GetCursorPos(&pt);
	HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	
	MONITORINFOEX mi = { sizeof(mi) };
	GetMonitorInfo(hMon, &mi);

	int count = ComboBox_GetCount(_cmbMonitors);
	if (count <= 0) return;
	ComboBox_SetCurSel(_cmbMonitors, 0);
	const bool hasMultipleMonitors = _monitors.size() > 1 || count > 1;
	if (!hasMultipleMonitors)
	{
		::ShowWindow(_cmbMonitors, SW_HIDE);
		::ShowWindow(_chkExtend, SW_HIDE);
		::ShowWindow(_chkPrimary, SW_HIDE);
		::ShowWindow(_textDisplay, SW_SHOW);

		ComboBox_SetCurSel(_cmbMonitors, 0);
		WCHAR name[256] = {};
		ComboBox_GetLBText(_cmbMonitors, 0, name);

		WCHAR finalName[256] = {};
		StringCchCopyW(finalName, ARRAYSIZE(finalName), name + min(3, (int)wcslen(name)));
		::SetWindowText(_textDisplay, finalName);
	}
	else
	{
		::ShowWindow(_cmbMonitors, SW_SHOW);
		::ShowWindow(_chkExtend, SW_SHOW);
		::ShowWindow(_chkPrimary, SW_SHOW);
		::ShowWindow(_textDisplay, SW_HIDE);

		for (int i = 0; i < count; ++i)
		{
			LPARAM itemData = ComboBox_GetItemData(_cmbMonitors, i);
			LPCWSTR data = itemData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(itemData);
			if (data && StrCmpI(data, mi.szDevice) == 0)
			{
				ComboBox_SetCurSel(_cmbMonitors, i);
				break;
			}
		}

		_UpdateSelectedMonitorState();
	}
}

void CSettingsDlgProc::_UpdateSelectedMonitorState()
{
	const int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	LPARAM selectedData = selectedItem < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, selectedItem);
	LPCWSTR selectedDevice = selectedData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(selectedData);
	bool selectedPrimary = false;
	bool selectedActive = false;
	if (selectedDevice)
	{
		for (const MONITORPREVIEW& monitor : _monitors)
		{
			if (StrCmpI(selectedDevice, monitor.deviceName) == 0)
			{
				selectedPrimary = monitor.primary;
				selectedActive = monitor.active;
				break;
			}
		}
	}
	Button_SetCheck(_chkPrimary, selectedPrimary ? BST_CHECKED : BST_UNCHECKED);
	bool selectedExtended = selectedActive;
	if (selectedDevice)
	{
		for (const MONITORPREVIEW& monitor : _monitors)
		{
			if (StrCmpI(selectedDevice, monitor.deviceName) == 0)
			{
				selectedExtended = monitor.active ? !monitor.disableRequested : monitor.enableRequested;
				break;
			}
		}
	}
	Button_SetCheck(_chkExtend, selectedExtended ? BST_CHECKED : BST_UNCHECKED);
	::EnableWindow(_chkPrimary, selectedActive && selectedExtended && !selectedPrimary);
	::EnableWindow(_chkExtend, !selectedPrimary);
}

void CSettingsDlgProc::_EnumerateActiveDisplayDevices()
{
	int oldCount = ComboBox_GetCount(_cmbMonitors);
	for (int i = oldCount - 1; i >= 0; --i)
	{
		LPARAM itemData = ComboBox_GetItemData(_cmbMonitors, i);
		if (itemData != CB_ERR && itemData != 0)
			LocalFree(reinterpret_cast<HLOCAL>(itemData));
	}
	ComboBox_ResetContent(_cmbMonitors);

	int ordinal = 1;
	for (const MONITORPREVIEW& monitor : _monitors)
	{
		if (!monitor.deviceName[0]) continue;

		WCHAR adapterName[128] = {};
		for (DWORD i = 0;; ++i)
		{
			DISPLAY_DEVICEW device = {};
			device.cb = sizeof(device);
			if (!EnumDisplayDevicesW(nullptr, i, &device, 0)) break;
			LPCWSTR displayName = monitor.active ? monitor.deviceName : monitor.applyDeviceName;
			if (displayName[0] && StrCmpI(device.DeviceName, displayName) == 0)
			{
				StringCchCopyW(adapterName, ARRAYSIZE(adapterName),
					device.DeviceString[0] ? device.DeviceString : device.DeviceName);
				break;
			}
		}
		WCHAR label[256] = {};
		StringCchPrintfW(label, ARRAYSIZE(label),
			LoadDeskString(IDS_DISPLAY_LIST_FORMAT).c_str(),
			ordinal, monitor.monitorName, adapterName);
		int item = ComboBox_AddString(_cmbMonitors, label);
		if (item == CB_ERR || item == CB_ERRSPACE) continue;

		LPWSTR name = StrDupW(monitor.deviceName);
		if (!name || ComboBox_SetItemData(_cmbMonitors, item, (LPARAM)name) == CB_ERR)
		{
			if (name) LocalFree(name);
			ComboBox_DeleteString(_cmbMonitors, item);
			continue;
		}
		++ordinal;
	}
	if (ComboBox_GetCount(_cmbMonitors) > 0)
		ComboBox_SetCurSel(_cmbMonitors, 0);
}

bool Compare(const RESINFO& a, const RESINFO& b)
{
	if (a.width != b.width)
	{
		return a.width < b.width;
	}
	return a.height < b.height;
}

struct ALLMODES_DIALOG_CONTEXT
{
	LPCWSTR deviceName;
	bool hideUnsupportedModes;
};

struct DISPLAY_MODE_ENTRY
{
	DWORD width;
	DWORD height;
	DWORD bitsPerPixel;
	DWORD frequency;
};

static bool DisplayModeEntryLess(const DISPLAY_MODE_ENTRY& a, const DISPLAY_MODE_ENTRY& b)
{
	if (a.width != b.width) return a.width < b.width;
	if (a.height != b.height) return a.height < b.height;
	if (a.bitsPerPixel != b.bitsPerPixel) return a.bitsPerPixel < b.bitsPerPixel;
	return a.frequency < b.frequency;
}

static INT_PTR CALLBACK ListAllModesDialogProc(HWND hwnd, UINT message,
	WPARAM wParam, LPARAM lParam)
{
	if (message == WM_INITDIALOG)
	{
		ApplySystemDialogFont(hwnd);
		auto context = reinterpret_cast<ALLMODES_DIALOG_CONTEXT*>(lParam);
		::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
		HWND list = ::GetDlgItem(hwnd, 802);
		if (!context || !context->deviceName || !context->deviceName[0] || !list)
			return TRUE;

		std::vector<DISPLAY_MODE_ENTRY> modes;
		for (DWORD index = 0;; ++index)
		{
			DEVMODEW mode = {};
			mode.dmSize = sizeof(mode);
			if (!::EnumDisplaySettingsExW(context->deviceName, index, &mode,
				context->hideUnsupportedModes ? 0 : EDS_RAWMODE))
				break;
			DISPLAY_MODE_ENTRY entry = {
				mode.dmPelsWidth, mode.dmPelsHeight,
				mode.dmBitsPerPel, mode.dmDisplayFrequency };
			if (std::find_if(modes.begin(), modes.end(), [&entry](const DISPLAY_MODE_ENTRY& existing)
				{
					return existing.width == entry.width && existing.height == entry.height &&
						existing.bitsPerPixel == entry.bitsPerPixel && existing.frequency == entry.frequency;
				}) == modes.end())
				modes.push_back(entry);
		}
		std::sort(modes.begin(), modes.end(), DisplayModeEntryLess);

		for (const DISPLAY_MODE_ENTRY& mode : modes)
		{
			WCHAR colorDepth[48] = {};
			switch (mode.bitsPerPixel)
			{
			case 4: StringCchCopyW(colorDepth, ARRAYSIZE(colorDepth), LoadDeskString(IDS_COLOR_DEPTH_4).c_str()); break;
			case 8: StringCchCopyW(colorDepth, ARRAYSIZE(colorDepth), LoadDeskString(IDS_COLOR_DEPTH_8).c_str()); break;
			case 16: StringCchCopyW(colorDepth, ARRAYSIZE(colorDepth), LoadDeskString(IDS_COLOR_DEPTH_16).c_str()); break;
			case 24: StringCchCopyW(colorDepth, ARRAYSIZE(colorDepth), LoadDeskString(IDS_COLOR_DEPTH_24).c_str()); break;
			case 32: StringCchCopyW(colorDepth, ARRAYSIZE(colorDepth), LoadDeskString(IDS_COLOR_DEPTH_32).c_str()); break;
			default:
				StringCchPrintfW(colorDepth, ARRAYSIZE(colorDepth),
					LoadDeskString(IDS_COLOR_DEPTH_GENERIC_FORMAT).c_str(), mode.bitsPerPixel);
				break;
			}

			WCHAR line[160] = {};
			if (mode.frequency > 1)
				StringCchPrintfW(line, ARRAYSIZE(line),
					LoadDeskString(IDS_MODE_LINE_FORMAT).c_str(),
					mode.width, mode.height, colorDepth, mode.frequency);
			else
				StringCchPrintfW(line, ARRAYSIZE(line),
					LoadDeskString(IDS_MODE_LINE_DEFAULT_FORMAT).c_str(),
					mode.width, mode.height, colorDepth);
			::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
		}
		if (modes.empty())
			::SendMessageW(list, LB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(LoadDeskString(IDS_NO_VALID_MODES).c_str()));
		return TRUE;
	}
	if (message == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
	{
		::EndDialog(hwnd, LOWORD(wParam));
		return TRUE;
	}
	return FALSE;
}

static bool ShowListAllModesDialog(HWND owner, LPCWSTR deviceName,
	bool hideUnsupportedModes)
{
	WCHAR systemDirectory[MAX_PATH] = {};
	UINT length = ::GetSystemDirectoryW(systemDirectory, ARRAYSIZE(systemDirectory));
	if (!length || length >= ARRAYSIZE(systemDirectory) ||
		FAILED(StringCchCatW(systemDirectory, ARRAYSIZE(systemDirectory), L"\\deskadp.dll")))
		return false;
	HMODULE module = ::LoadLibraryW(systemDirectory);
	if (!module)
		return false;
	ALLMODES_DIALOG_CONTEXT context = { deviceName, hideUnsupportedModes };
	INT_PTR result = ::DialogBoxParamW(module, MAKEINTRESOURCEW(800), owner,
		ListAllModesDialogProc, reinterpret_cast<LPARAM>(&context));
	::FreeLibrary(module);
	return result != -1;
}

void CSettingsDlgProc::_GetAllModes()
{
	_arrResInfo.clear();
	_arrSupportedBpp.clear();

	int index = ComboBox_GetCurSel(_cmbMonitors);
	if (index < 0 && ComboBox_GetCount(_cmbMonitors) > 0)
	{
		index = 0;
		ComboBox_SetCurSel(_cmbMonitors, index);
	}
	LPARAM itemData = index < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, index);
	LPCWSTR selectedDevice = itemData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(itemData);
	_loadedModeDeviceName[0] = L'\0';
	const MONITORPREVIEW* selectedMonitor = nullptr;
	if (selectedDevice)
	{
		for (const MONITORPREVIEW& monitor : _monitors)
		{
			if (StrCmpI(selectedDevice, monitor.deviceName) == 0)
			{
				selectedMonitor = &monitor;
				break;
			}
		}
	}
	LPCWSTR modeDevice = selectedMonitor ?
		(selectedMonitor->active ? selectedMonitor->deviceName : selectedMonitor->applyDeviceName) : selectedDevice;
	const DWORD enumerationFlags = ReadHideUnsupportedModes(modeDevice,
		selectedMonitor ? selectedMonitor->monitorInstanceId : nullptr) ? 0 : EDS_RAWMODE;
	const DWORD modeType = selectedMonitor && !selectedMonitor->active ?
		ENUM_REGISTRY_SETTINGS : ENUM_CURRENT_SETTINGS;
	if (selectedDevice && selectedMonitor)
		StringCchCopyW(_loadedModeDeviceName, ARRAYSIZE(_loadedModeDeviceName), selectedDevice);
	if (!modeDevice || !modeDevice[0])
	{
		_currentResInfo = {};
		::SetWindowTextW(_textCurrentRes, L"");
		::EnableWindow(_trackResolution, FALSE);
		::EnableWindow(_cmbColors, FALSE);
		SendMessage(_trackResolution, TBM_SETRANGE, TRUE, MAKELPARAM(0, 0));
		_BuildColorList();
		return;
	}
	::EnableWindow(_trackResolution, TRUE);
	::EnableWindow(_cmbColors, TRUE);

	DEVMODE devMode = {};
	devMode.dmSize = sizeof(devMode);
	if (!EnumDisplaySettingsExW(modeDevice, modeType, &devMode, 0))
	{
		_currentResInfo = {};
		::SetWindowTextW(_textCurrentRes, L"");
		::EnableWindow(_trackResolution, FALSE);
		::EnableWindow(_cmbColors, FALSE);
		SendMessage(_trackResolution, TBM_SETRANGE, TRUE, MAKELPARAM(0, 0));
		_BuildColorList();
		return;
	}
	_currentResInfo.width = devMode.dmPelsWidth;
	_currentResInfo.height = devMode.dmPelsHeight;
	_currentResInfo.bpp = devMode.dmBitsPerPel;
	_currentResInfo.freq = devMode.dmDisplayFrequency;

	// Enumerate this output's supported modes. Inactive outputs use their saved
	// registry mode until the user enables them with the Extend checkbox.
	for (DWORD i = 0; EnumDisplaySettingsExW(modeDevice, i, &devMode,
		enumerationFlags); ++i)
	{
		BOOL exists = FALSE;
		for (int j = 0; j < _arrResInfo.size(); j++)
		{
			if (_arrResInfo[j].width == devMode.dmPelsWidth &&
				_arrResInfo[j].height == devMode.dmPelsHeight)
			{
				exists = TRUE;
				break;
			}
		}

		if (!exists)
		{
			_arrResInfo.push_back({ devMode.dmPelsWidth, devMode.dmPelsHeight });
		}

		if (std::find(_arrSupportedBpp.begin(), _arrSupportedBpp.end(), devMode.dmBitsPerPel) == _arrSupportedBpp.end())
		{
			_arrSupportedBpp.push_back(devMode.dmBitsPerPel);
		}
	}

	std::sort(_arrResInfo.begin(), _arrResInfo.end(), Compare);
	if (_arrResInfo.empty())
	{
		_arrResInfo.push_back({ _currentResInfo.width, _currentResInfo.height });
	}
	if (_arrSupportedBpp.empty() && _currentResInfo.bpp)
		_arrSupportedBpp.push_back(_currentResInfo.bpp);
	SendMessage(_trackResolution, TBM_SETRANGE, TRUE, MAKELPARAM(0, (int)_arrResInfo.size() - 1));

	_SelectCurrentResolution();
	_BuildColorList();
}

bool CSettingsDlgProc::_GetPendingMode(LPCWSTR deviceName, RESINFO& mode) const
{
	if (!deviceName || !*deviceName) return false;
	for (const PENDINGMODE& pending : _pendingModes)
	{
		if (StrCmpI(pending.deviceName, deviceName) == 0)
		{
			mode = pending.mode;
			return true;
		}
	}
	return false;
}

void CSettingsDlgProc::_StoreSelectedPendingMode()
{
	if (_arrResInfo.empty()) return;
	LPCWSTR deviceName = _loadedModeDeviceName;
	if (!*deviceName) return;
	const int pos = max(0, min((int)SendMessage(_trackResolution, TBM_GETPOS, 0, 0),
		(int)_arrResInfo.size() - 1));
	RESINFO mode = _arrResInfo[pos];
	const int colorIndex = ComboBox_GetCurSel(_cmbColors);
	if (_colorSelectionDirty && colorIndex >= 0)
		mode.bpp = (DWORD)ComboBox_GetItemData(_cmbColors, colorIndex);
	else
	{
		RESINFO existingMode = {};
		mode.bpp = _GetPendingMode(deviceName, existingMode) && existingMode.bpp ?
			existingMode.bpp : _currentResInfo.bpp;
	}
	_colorSelectionDirty = false;
	size_t existing = _pendingModes.size();
	for (size_t i = 0; i < _pendingModes.size(); ++i)
	{
		if (StrCmpI(_pendingModes[i].deviceName, deviceName) == 0)
		{
			existing = i;
			break;
		}
	}
	if (mode.width == _currentResInfo.width && mode.height == _currentResInfo.height &&
		(!mode.bpp || mode.bpp == _currentResInfo.bpp))
	{
		if (existing < _pendingModes.size())
			_pendingModes.erase(_pendingModes.begin() + existing);
		return;
	}
	if (existing == _pendingModes.size())
	{
		_pendingModes.push_back({});
		existing = _pendingModes.size() - 1;
		StringCchCopyW(_pendingModes[existing].deviceName,
			ARRAYSIZE(_pendingModes[existing].deviceName), deviceName);
	}
	_pendingModes[existing].mode = mode;
}

void CSettingsDlgProc::_SetTrackbarModes(int modenum)
{
	if (_arrResInfo.empty())
	{
		::SetWindowText(_textCurrentRes, L"");
		return;
	}
	modenum = max(0, min(modenum, (int)_arrResInfo.size() - 1));
	SendMessage(_trackResolution, TBM_SETPOS, TRUE, (LPARAM)modenum);

	WCHAR str[64];
	StringCchPrintf(str, ARRAYSIZE(str),
		LoadDeskString(IDS_RESOLUTION_FORMAT).c_str(),
		_arrResInfo[modenum].width, _arrResInfo[modenum].height);
	::SetWindowText(_textCurrentRes, str);
}

void CSettingsDlgProc::_BuildColorList()
{
	ComboBox_ResetContent(_cmbColors);
	std::sort(_arrSupportedBpp.begin(), _arrSupportedBpp.end());

	COLORMODES modes{};
	for (int i = 0; i < _arrSupportedBpp.size(); ++i)
	{
		if (_arrSupportedBpp[i] == 4)
		{
			int index = ComboBox_AddString(_cmbColors,
				LoadDeskString(IDS_LOWEST_COLOR_QUALITY).c_str());
			ComboBox_SetItemData(_cmbColors, index, 4);
		}
		if (_arrSupportedBpp[i] == 8)
		{
			int index = ComboBox_AddString(_cmbColors,
				LoadDeskString(IDS_LOW_COLOR_QUALITY).c_str());
			ComboBox_SetItemData(_cmbColors, index, 8);
		}
		if (_arrSupportedBpp[i] == 16)
		{
			int index = ComboBox_AddString(_cmbColors,
				LoadDeskString(IDS_MEDIUM_COLOR_QUALITY).c_str());
			ComboBox_SetItemData(_cmbColors, index, 16);
		}
		if (_arrSupportedBpp[i] == 24)
		{
			int index = ComboBox_AddString(_cmbColors,
				LoadDeskString(IDS_HIGH_COLOR_QUALITY).c_str());
			ComboBox_SetItemData(_cmbColors, index, 24);
		}
		if (_arrSupportedBpp[i] == 32)
		{
			int index = ComboBox_AddString(_cmbColors,
				LoadDeskString(IDS_HIGHEST_COLOR_QUALITY).c_str());
			ComboBox_SetItemData(_cmbColors, index, 32);
		}
	}

	RESINFO pending = {};
	int targetBpp = 32;
	int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	LPARAM selectedData = selectedItem < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, selectedItem);
	LPCWSTR selectedDevice = selectedData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(selectedData);
	if (_GetPendingMode(selectedDevice, pending) && pending.bpp)
		targetBpp = (int)pending.bpp;
	else if (std::find(_arrSupportedBpp.begin(), _arrSupportedBpp.end(), (DWORD)targetBpp) ==
		_arrSupportedBpp.end())
		targetBpp = _currentResInfo.bpp;
	if (std::find(_arrSupportedBpp.begin(), _arrSupportedBpp.end(), (DWORD)targetBpp) ==
		_arrSupportedBpp.end() && !_arrSupportedBpp.empty())
		targetBpp = (int)_arrSupportedBpp.back();
	for (int i = 0; i < ComboBox_GetCount(_cmbColors); ++i)
	{
		int bpp = (int)ComboBox_GetItemData(_cmbColors, i);
		if (bpp == targetBpp)
		{
			ComboBox_SetCurSel(_cmbColors, i);
		}
	}
	_UpdateColorPreview();
}

void CSettingsDlgProc::_UpdateColorPreview()
{
	int index = ComboBox_GetCurSel(_cmbColors);
	if (index < 0) return;
	int bpp = (int)ComboBox_GetItemData(_cmbColors, index);
	
	int img = 120;
	switch (bpp)
	{
	case 8:
		img += 1;
		break;
	case 16:
		img += 3;
		break;
	case 24:
		img += 4;
		break;
	case 32:
		img += 5;
		break;
	default:
		break;
	}

	SIZE size = GetClientSIZE(_clrPreview);
	HBITMAP bmp = (HBITMAP)LoadImage(g_hinst, MAKEINTRESOURCE(img), IMAGE_BITMAP, size.cx, 0, LR_DEFAULTCOLOR);
	Static_SetBitmap(_clrPreview, bmp);
}

void CSettingsDlgProc::_SelectCurrentResolution()
{
	if (_arrResInfo.empty()) return;
	int selectedItem = ComboBox_GetCurSel(_cmbMonitors);
	LPARAM selectedData = selectedItem < 0 ? CB_ERR : ComboBox_GetItemData(_cmbMonitors, selectedItem);
	LPCWSTR selectedDevice = selectedData == CB_ERR ? nullptr : reinterpret_cast<LPCWSTR>(selectedData);
	RESINFO pending = {};
	if (_GetPendingMode(selectedDevice, pending))
	{
		for (int i = 0; i < (int)_arrResInfo.size(); ++i)
		{
			if (_arrResInfo[i].width == pending.width && _arrResInfo[i].height == pending.height)
			{
				_SetTrackbarModes(i);
				return;
			}
		}
	}
	for (int i = 0; i < _arrResInfo.size(); ++i)
	{
		if (_arrResInfo[i].width == _currentResInfo.width
			&& _arrResInfo[i].height == _currentResInfo.height)
		{
			_SetTrackbarModes(i);
			return;
		}
	}
	for (int i = 0; i < (int)_arrResInfo.size(); ++i)
	{
		if (_arrResInfo[i].width == 800 && _arrResInfo[i].height == 600)
		{
			_SetTrackbarModes(i);
			return;
		}
	}
	// The trackbar keeps its old position when its range changes. Clamp to
	// the lowest supported mode explicitly instead of accidentally retaining
	// the maximum resolution when switching to an inactive display.
	_SetTrackbarModes(0);
}
