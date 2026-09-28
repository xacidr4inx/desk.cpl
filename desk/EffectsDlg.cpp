#include "pch.h"
#include "EffectsDlg.h"
#include "helper.h"
#include <exdisp.h>

static bool ReadLargeIconPreference(BOOL& enabled)
{
	enabled = FALSE;
	BYTE data[64] = {};
	DWORD type = 0;
	DWORD size = sizeof(data);
	LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
		L"Control Panel\\Desktop\\WindowMetrics", L"Shell Icon Size",
		RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &type, data, &size);
	if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
		return true;
	if (status != ERROR_SUCCESS)
		return false;
	DWORD iconSize = 32;
	if (type == REG_DWORD && size >= sizeof(DWORD))
		iconSize = *reinterpret_cast<const DWORD*>(data);
	else if (type == REG_SZ && size >= sizeof(WCHAR))
		iconSize = wcstoul(reinterpret_cast<const WCHAR*>(data), nullptr, 10);
	enabled = iconSize >= 48;
	return true;
}

static LSTATUS WriteLargeIconPreference(BOOL enabled)
{
	HKEY windowMetrics = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER,
		L"Control Panel\\Desktop\\WindowMetrics", 0, nullptr, 0,
		KEY_SET_VALUE, nullptr, &windowMetrics, nullptr);
	if (status != ERROR_SUCCESS)
		return status;
	const WCHAR iconSize[] = L"48";
	const WCHAR normalSize[] = L"32";
	LPCWSTR value = enabled ? iconSize : normalSize;
	status = RegSetValueExW(windowMetrics, L"Shell Icon Size", 0, REG_SZ,
		reinterpret_cast<const BYTE*>(value),
		static_cast<DWORD>((lstrlenW(value) + 1) * sizeof(WCHAR)));
	RegCloseKey(windowMetrics);
	return status;
}

static HRESULT ApplyDesktopIconSize(BOOL enabled)
{
	CComPtr<IShellWindows> shellWindows;
	HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
		IID_PPV_ARGS(&shellWindows));
	if (FAILED(hr))
		return hr;

	VARIANT location;
	VARIANT root;
	VariantInit(&location);
	VariantInit(&root);
	long desktopHwnd = 0;
	CComPtr<IDispatch> desktopDispatch;
	hr = shellWindows->FindWindowSW(&location, &root, SWC_DESKTOP, &desktopHwnd,
		SWFO_NEEDDISPATCH, &desktopDispatch);
	if (FAILED(hr))
		return hr;

	CComQIPtr<IServiceProvider> serviceProvider(desktopDispatch);
	if (!serviceProvider)
		return E_NOINTERFACE;

	CComPtr<IShellBrowser> shellBrowser;
	hr = serviceProvider->QueryService(SID_STopLevelBrowser,
		IID_PPV_ARGS(&shellBrowser));
	if (FAILED(hr))
		return hr;

	CComPtr<IShellView> shellView;
	hr = shellBrowser->QueryActiveShellView(&shellView);
	if (FAILED(hr))
		return hr;

	CComQIPtr<IFolderView2> folderView(shellView);
	if (!folderView)
		return E_NOINTERFACE;

	FOLDERVIEWMODE viewMode;
	int currentSize = 0;
	hr = folderView->GetViewModeAndIconSize(&viewMode, &currentSize);
	if (FAILED(hr))
		return hr;

	return folderView->SetViewModeAndIconSize(viewMode, enabled ? 48 : 32);
}


BOOL CEffectsDlg::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	_chkAnim = GetDlgItem(1175);
	_cmbAnim = GetDlgItem(1182);
	_chkFont = GetDlgItem(1177);
	_cmbFont = GetDlgItem(1184);
	_chkShadow = GetDlgItem(1185);
	_chkLargeIcons = GetDlgItem(1180);
	_chkDragWnd = GetDlgItem(1179);
	_chkAltIndicator = GetDlgItem(1181);

	SystemParametersInfo(SPI_GETFONTSMOOTHING, NULL, &_fSmoothingEnabled, 0);
	Button_SetCheck(_chkFont, _fSmoothingEnabled);
	::ComboBox_Enable(_cmbFont, _fSmoothingEnabled);

	// all font smoothing types
	std::wstring smoothingLabels[] = {
		LoadDeskString(IDS_STANDARD_SMOOTHING),
		LoadDeskString(IDS_CLEARTYPE_SMOOTHING),
	};
	for (int i = 0; i < _countof(smoothingLabels); i++)
	{
		ComboBox_AddString(_cmbFont, smoothingLabels[i].c_str());
	}

	SystemParametersInfo(SPI_GETFONTSMOOTHINGTYPE, 0, &_iSmoothingType, 0);
	ComboBox_SetCurSel(_cmbFont, _iSmoothingType - 1);

	BOOL fMenuAnim, fToolTipAnim;
	SystemParametersInfo(SPI_GETMENUANIMATION, 0, &fMenuAnim, 0);
	SystemParametersInfo(SPI_GETTOOLTIPANIMATION, 0, &fToolTipAnim, 0);

	if (fMenuAnim && fToolTipAnim) _iAnimEnabled = 1;
	else if (fMenuAnim || fToolTipAnim) _iAnimEnabled = 2;
	else _iAnimEnabled = 0;

	Button_SetCheck(_chkAnim, _iAnimEnabled);
	::ComboBox_Enable(_cmbAnim, _iAnimEnabled > 0 ? 1 : 0);

	// all scroll types
	std::wstring animationLabels[] = {
		LoadDeskString(IDS_FADE_EFFECT),
		LoadDeskString(IDS_SCROLL_EFFECT),
	};
	for (int i = 0; i < _countof(animationLabels); i++)
	{
		ComboBox_AddString(_cmbAnim, animationLabels[i].c_str());
	}

	BOOL fTooltipFade, fMenuFade;
	SystemParametersInfo(SPI_GETTOOLTIPFADE, 0, &fTooltipFade, 0);
	SystemParametersInfo(SPI_GETMENUFADE, 0, &fMenuFade, 0);

	int index = 0;

	// check both
	if (fToolTipAnim) index = fTooltipFade ? 0 : 1;
	if (fMenuAnim) index = fMenuFade ? 0 : 1;
	_fAnimType = index;
	ComboBox_SetCurSel(_cmbAnim, index);

	SystemParametersInfo(SPI_GETDROPSHADOW, 0, &_fDropShadows, 0);
	Button_SetCheck(_chkShadow, _fDropShadows);
	ReadLargeIconPreference(_fLargeIcons);
	Button_SetCheck(_chkLargeIcons, _fLargeIcons);

	SystemParametersInfo(SPI_GETDRAGFULLWINDOWS, 0, &_fDragWindow, 0);
	Button_SetCheck(_chkDragWnd, _fDragWindow);

	SystemParametersInfo(SPI_GETKEYBOARDCUES, 0, &_fAltIndicator, 0);
	Button_SetCheck(_chkAltIndicator, !_fAltIndicator);

	return TRUE;
}

BOOL CEffectsDlg::OnAnimChk(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	BOOL bChecked = Button_GetCheck(hWnd);
	::ComboBox_Enable(_cmbAnim, !bChecked);
	Button_SetCheck(hWnd, !bChecked);
	_iAnimEnabled = !bChecked;

	flags |= UPDATE_ANIM;
	return 0;
}

BOOL CEffectsDlg::OnAnimCmbChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	// 0- fade, 1- scroll
	int index = ComboBox_GetCurSel(hWnd);
	_fAnimType = index;

	flags |= UPDATE_ANIM;
	return 0;
}

BOOL CEffectsDlg::OnFontChk(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	BOOL bChecked = Button_GetCheck(hWnd);
	::ComboBox_Enable(_cmbFont, bChecked);
	_fSmoothingEnabled = bChecked;

	flags |= UPDATE_FONT;
	return 0;
}

BOOL CEffectsDlg::OnFontCmbChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	_iSmoothingType = ComboBox_GetCurSel(hWnd) + 1;

	flags |= UPDATE_FONT;
	return 0;
}

BOOL CEffectsDlg::OnShadowChk(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	_fDropShadows = Button_GetCheck(hWnd);

	flags |= UPDATE_DROPSHADOW;
	return 0;
}

BOOL CEffectsDlg::OnLargeIconsChk(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	_fLargeIcons = Button_GetCheck(hWnd);
	flags |= UPDATE_ICONS;
	return 0;
}

BOOL CEffectsDlg::OnWindowChk(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	_fDragWindow = Button_GetCheck(hWnd);

	flags |= UPDATE_WINDOWDRAG;
	return 0;
}

BOOL CEffectsDlg::OnAltChk(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	_fAltIndicator = !Button_GetCheck(hWnd);

	flags |= UPDATE_ALT;
	return 0;
}

LRESULT CEffectsDlg::OnOK(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled)
{
	if (flags & UPDATE_ICONS)
	{
		LSTATUS status = WriteLargeIconPreference(_fLargeIcons);
		if (status != ERROR_SUCCESS)
		{
			WCHAR message[160] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_ICON_SIZE_SAVE_ERROR).c_str(), status);
			::MessageBoxW(m_hWnd, message,
				LoadDeskString(IDS_EFFECTS_TITLE).c_str(), MB_OK | MB_ICONERROR);
			return 0;
		}
		DWORD_PTR broadcastResult = 0;
		::SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
			reinterpret_cast<LPARAM>(L"WindowMetrics"), SMTO_ABORTIFHUNG,
			1000, &broadcastResult);
		::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT,
			nullptr, nullptr);
		HRESULT applyResult = ApplyDesktopIconSize(_fLargeIcons);
		if (FAILED(applyResult))
		{
			WCHAR message[160] = {};
			StringCchPrintfW(message, ARRAYSIZE(message),
				LoadDeskString(IDS_ICON_SIZE_UPDATE_ERROR).c_str(),
				static_cast<unsigned int>(applyResult));
			::MessageBoxW(m_hWnd, message,
				LoadDeskString(IDS_EFFECTS_TITLE).c_str(), MB_OK | MB_ICONWARNING);
		}
	}

	if (flags & UPDATE_ANIM)
	{
		// set both menu and tooltip, like xp
		SystemParametersInfo(SPI_SETMENUANIMATION, 0, (PVOID)(_iAnimEnabled > 0), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
		SystemParametersInfo(SPI_SETTOOLTIPANIMATION, 0, (PVOID)(_iAnimEnabled > 0), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);

		SystemParametersInfo(SPI_SETMENUFADE, 0, (PVOID)!_fAnimType, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
		SystemParametersInfo(SPI_SETTOOLTIPFADE, 0, (PVOID)!_fAnimType, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	}

	if (flags & UPDATE_FONT)
	{
		SystemParametersInfo(SPI_SETFONTSMOOTHING, _fSmoothingEnabled, 0, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
		SystemParametersInfo(SPI_SETFONTSMOOTHINGTYPE, 0, (PVOID)_iSmoothingType, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	}

	if (flags & UPDATE_DROPSHADOW)
	{
		SystemParametersInfo(SPI_SETDROPSHADOW, 0, (PVOID)_fDropShadows, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	}

	if (flags & UPDATE_WINDOWDRAG)
	{
		SystemParametersInfo(SPI_SETDRAGFULLWINDOWS, _fDragWindow, 0, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	}

	if (flags & UPDATE_ALT)
	{
		SystemParametersInfo(SPI_SETKEYBOARDCUES, 0, (PVOID)_fAltIndicator, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
	}
	EndDialog(0);
	return 0;
}

LRESULT CEffectsDlg::OnCancel(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled)
{
	EndDialog(1);
	return 0;
}

void CEffectsDlg::OnClose()
{
	EndDialog(0);
}
