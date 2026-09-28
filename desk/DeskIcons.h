#pragma once
#include "pch.h"


class CDesktopIconsDlg :
	public ATL::CDialogImpl<CDesktopIconsDlg>
{
public:
	enum { IDD = IDD_DESKTOPICONSDLG };

	bool myDocumentsVisible = false;
	bool computerVisible = false;
	bool networkVisible = false;
	bool internetVisible = false;
	WCHAR internetIconClass[64] = L"{871C5380-42A0-1069-A2EA-08002B30309D}";
	bool useInitialVisibility = false;
	bool initialVisibility[4] = {};

	INT_PTR ShowModal(HWND owner);

	~CDesktopIconsDlg()
	{
		if (hIconImages) ImageList_Destroy(hIconImages);
	}

private:
	static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	BEGIN_MSG_MAP(CDesktopIconsDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MSG_WM_CLOSE(OnClose)
		COMMAND_ID_HANDLER(IDOK, OnOK)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
	END_MSG_MAP()

	BOOL OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnOK(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled);
	LRESULT OnCancel(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled);
	void OnClose();

	HWND hComputer = nullptr;
	HWND hUser = nullptr;
	HWND hNetwork = nullptr;
	HWND hInternet = nullptr;
	HWND hIconList = nullptr;
	HIMAGELIST hIconImages = nullptr;
};

