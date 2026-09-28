#include "pch.h"
#include "DeskIcons.h"
#include "desk.h"
#include "helper.h"

namespace
{
	BOOL CALLBACK FindIconListView(HWND child, LPARAM data)
	{
		WCHAR className[32] = {};
		if (::GetClassNameW(child, className, ARRAYSIZE(className)) &&
			lstrcmpiW(className, WC_LISTVIEWW) == 0)
		{
			*reinterpret_cast<HWND*>(data) = child;
			return FALSE;
		}
		return TRUE;
	}

	struct DesktopIconEntry
	{
		LPCWSTR clsid;
		UINT fallbackStringId;
	};

	const DesktopIconEntry kDesktopIcons[] =
	{
		{ L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", IDS_DESKTOP_ICON_COMPUTER },
		{ L"::{450D8FBA-AD25-11D0-98A8-0800361B1103}", IDS_DESKTOP_ICON_DOCUMENTS },
		{ L"::{208D2C60-3AEA-1069-A2D7-08002B30309D}", IDS_DESKTOP_ICON_NETWORK },
		{ L"::{645FF040-5081-101B-9F08-00AA002F954E}", IDS_DESKTOP_ICON_RECYCLE_FULL },
		{ L"::{645FF040-5081-101B-9F08-00AA002F954E}", IDS_DESKTOP_ICON_RECYCLE_EMPTY },
		{ L"::{871C5380-42A0-1069-A2EA-08002B30309D}", IDS_DESKTOP_ICON_INTERNET },
		{ L"::{21EC2020-3AEA-1069-A2DD-08002B30309D}", IDS_DESKTOP_ICON_CONTROL_PANEL },
	};

	const WCHAR* kDesktopIconClasses[] =
	{
		L"{450D8FBA-AD25-11D0-98A8-0800361B1103}", // My Documents
		L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}", // Computer
		L"{208D2C60-3AEA-1069-A2D7-08002B30309D}", // Network Places
		L"{871C5380-42A0-1069-A2EA-08002B30309D}", // Internet Explorer
	};

	void FindInternetExplorerDesktopClass(LPWSTR clsid, size_t cchClsid)
	{
		StringCchCopyW(clsid, cchClsid, kDesktopIconClasses[3]);

		IShellFolder* desktop = nullptr;
		if (FAILED(SHGetDesktopFolder(&desktop)))
			return;

		IEnumIDList* items = nullptr;
		if (SUCCEEDED(desktop->EnumObjects(nullptr,
			SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &items)))
		{
			LPITEMIDLIST item = nullptr;
			ULONG fetched = 0;
			while (items->Next(1, &item, &fetched) == S_OK)
			{
				STRRET nameResult = {};
				WCHAR name[MAX_PATH] = {};
				if (SUCCEEDED(desktop->GetDisplayNameOf(item, SHGDN_NORMAL, &nameResult)) &&
					SUCCEEDED(StrRetToBufW(&nameResult, item, name, ARRAYSIZE(name))) &&
					lstrcmpiW(name, L"Internet Explorer") == 0)
				{
					STRRET parsingResult = {};
					WCHAR parsingName[MAX_PATH] = {};
					if (SUCCEEDED(desktop->GetDisplayNameOf(item, SHGDN_FORPARSING,
						&parsingResult)) &&
						SUCCEEDED(StrRetToBufW(&parsingResult, item, parsingName,
							ARRAYSIZE(parsingName))))
					{
						WCHAR* openBrace = wcschr(parsingName, L'{');
						WCHAR* closeBrace = openBrace ? wcschr(openBrace, L'}') : nullptr;
						if (openBrace && closeBrace && closeBrace - openBrace == 37)
						{
							WCHAR candidate[64] = {};
						StringCchCopyNW(candidate, ARRAYSIZE(candidate), openBrace,
							static_cast<size_t>(closeBrace - openBrace + 1));
							CLSID parsedClass = {};
							if (SUCCEEDED(CLSIDFromString(candidate, &parsedClass)))
							{
								StringCchCopyW(clsid, cchClsid, candidate);
								if (lstrcmpiW(candidate, kDesktopIconClasses[3]) != 0)
								{
									CoTaskMemFree(item);
									break;
								}
							}
						}
					}
				}
				CoTaskMemFree(item);
				item = nullptr;
			}
			items->Release();
		}
		desktop->Release();
	}

	bool IsDesktopIconVisible(LPCWSTR clsid)
	{
		const WCHAR* panels[] = { L"NewStartPanel", L"ClassicStartMenu" };
		for (LPCWSTR panel : panels)
		{
			WCHAR path[200] = {};
			StringCchPrintfW(path, ARRAYSIZE(path),
				L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\%s", panel);
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
				continue;
			DWORD hidden = 0;
			DWORD size = sizeof(hidden);
			DWORD type = 0;
			LSTATUS status = RegQueryValueExW(key, clsid, nullptr, &type,
				reinterpret_cast<LPBYTE>(&hidden), &size);
			RegCloseKey(key);
			if (status == ERROR_SUCCESS && type == REG_DWORD)
				return hidden == 0;
		}
		// These legacy namespace icons are not enabled on this host unless Explorer
		// has an explicit visible (DWORD 0) value for them. Treat an unset value as
		// hidden so the checkboxes reflect what is actually on the desktop.
		return false;
	}

}

static int AddDesktopIconItem(HWND list, HIMAGELIST images, const DesktopIconEntry& entry)
{
	PIDLIST_ABSOLUTE pidl = nullptr;
	SHFILEINFOW info = {};
	int image = I_IMAGECALLBACK;
	WCHAR displayName[MAX_PATH] = {};
	if (SUCCEEDED(SHParseDisplayName(entry.clsid, nullptr, &pidl, 0, nullptr)))
	{
		if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &info, sizeof(info),
			SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_DISPLAYNAME))
		{
			if (info.hIcon)
			{
				image = ImageList_AddIcon(images, info.hIcon);
				DestroyIcon(info.hIcon);
			}
			StringCchCopyW(displayName, ARRAYSIZE(displayName), info.szDisplayName);
		}
		CoTaskMemFree(pidl);
	}
	if (!displayName[0])
		StringCchCopyW(displayName, ARRAYSIZE(displayName),
			LoadDeskString(entry.fallbackStringId).c_str());

	LVITEMW item = {};
	item.mask = LVIF_TEXT | LVIF_IMAGE;
	item.iItem = ListView_GetItemCount(list);
	item.pszText = displayName;
	item.iImage = image;
	return ListView_InsertItem(list, &item);
}

INT_PTR CDesktopIconsDlg::ShowModal(HWND owner)
{
	HRSRC resource = ::FindResourceExW(g_hinst, RT_DIALOG,
		MAKEINTRESOURCEW(IDD_DESKTOPICONSDLG), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
	if (!resource)
		resource = ::FindResourceExW(g_hinst, RT_DIALOG,
			MAKEINTRESOURCEW(IDD_DESKTOPICONSDLG), MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));
	if (!resource)
		resource = ::FindResourceW(g_hinst, MAKEINTRESOURCEW(IDD_DESKTOPICONSDLG), RT_DIALOG);
	if (!resource)
		return -1;
	HGLOBAL loaded = ::LoadResource(g_hinst, resource);
	if (!loaded)
		return -1;
	DLGTEMPLATE* templateData = reinterpret_cast<DLGTEMPLATE*>(::LockResource(loaded));
	if (!templateData)
		return -1;
	return ::DialogBoxIndirectParamW(g_hinst, templateData, owner, DialogProc,
		reinterpret_cast<LPARAM>(this));
}

INT_PTR CALLBACK CDesktopIconsDlg::DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	CDesktopIconsDlg* self = reinterpret_cast<CDesktopIconsDlg*>(
		::GetWindowLongPtrW(hwnd, DWLP_USER));
	if (msg == WM_INITDIALOG)
	{
		self = reinterpret_cast<CDesktopIconsDlg*>(lParam);
		::SetWindowLongPtrW(hwnd, DWLP_USER, reinterpret_cast<LONG_PTR>(self));
		if (self)
		{
			self->m_hWnd = hwnd;
			BOOL handled = TRUE;
			self->OnInitDialog(msg, wParam, lParam, handled);
			return TRUE;
		}
		return FALSE;
	}
	if (!self)
		return FALSE;
	if (msg == WM_COMMAND)
	{
		BOOL handled = TRUE;
		if (LOWORD(wParam) == IDOK)
			return self->OnOK(HIWORD(wParam), LOWORD(wParam),
				reinterpret_cast<HWND>(lParam), handled);
		if (LOWORD(wParam) == IDCANCEL)
			return self->OnCancel(HIWORD(wParam), LOWORD(wParam),
				reinterpret_cast<HWND>(lParam), handled);
	}
	else if (msg == WM_CLOSE)
	{
		self->OnClose();
		return TRUE;
	}
	return FALSE;
}

BOOL CDesktopIconsDlg::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	FindInternetExplorerDesktopClass(internetIconClass, ARRAYSIZE(internetIconClass));

	// These IDs belong to this dialog's own resource template.
	hUser = GetDlgItem(30077);
	hComputer = GetDlgItem(30078);
	hNetwork = GetDlgItem(30080);
	hInternet = GetDlgItem(30081);
	myDocumentsVisible = useInitialVisibility ? initialVisibility[0] :
		IsDesktopIconVisible(kDesktopIconClasses[0]);
	computerVisible = useInitialVisibility ? initialVisibility[1] :
		IsDesktopIconVisible(kDesktopIconClasses[1]);
	networkVisible = useInitialVisibility ? initialVisibility[2] :
		IsDesktopIconVisible(kDesktopIconClasses[2]);
	internetVisible = useInitialVisibility ? initialVisibility[3] :
		IsDesktopIconVisible(internetIconClass);
	if (hUser) Button_SetCheck(hUser, myDocumentsVisible ? BST_CHECKED : BST_UNCHECKED);
	if (hComputer) Button_SetCheck(hComputer, computerVisible ? BST_CHECKED : BST_UNCHECKED);
	if (hNetwork) Button_SetCheck(hNetwork, networkVisible ? BST_CHECKED : BST_UNCHECKED);
	if (hInternet) Button_SetCheck(hInternet, internetVisible ? BST_CHECKED : BST_UNCHECKED);
	::EnumChildWindows(m_hWnd, FindIconListView, reinterpret_cast<LPARAM>(&hIconList));
	if (hIconList)
	{
		const int iconWidth = max(32, GetSystemMetrics(SM_CXICON));
		const int iconHeight = max(32, GetSystemMetrics(SM_CYICON));
		hIconImages = ImageList_Create(iconWidth, iconHeight, ILC_COLOR32 | ILC_MASK,
			ARRAYSIZE(kDesktopIcons), 0);
		if (hIconImages)
		{
			ListView_SetImageList(hIconList, hIconImages, LVSIL_NORMAL);
			for (const auto& entry : kDesktopIcons)
				AddDesktopIconItem(hIconList, hIconImages, entry);
		}
	}

	return 0;
}

LRESULT CDesktopIconsDlg::OnOK(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled)
{
	myDocumentsVisible = hUser && Button_GetCheck(hUser) == BST_CHECKED;
	computerVisible = hComputer && Button_GetCheck(hComputer) == BST_CHECKED;
	networkVisible = hNetwork && Button_GetCheck(hNetwork) == BST_CHECKED;
	internetVisible = hInternet && Button_GetCheck(hInternet) == BST_CHECKED;
	EndDialog(IDOK);
	return 0;
}

LRESULT CDesktopIconsDlg::OnCancel(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled)
{
	EndDialog(IDCANCEL);
	return 0;
}

void CDesktopIconsDlg::OnClose()
{
	EndDialog(IDCANCEL);
}
