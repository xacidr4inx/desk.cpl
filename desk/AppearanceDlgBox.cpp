#include "pch.h"
#include "AppearanceDlgBox.h"
#include "ColorPalette.h"
#include "cscheme.h"
#include "helper.h"
using namespace Microsoft::WRL::Details;

LRESULT CALLBACK PreviewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR dwRefData)
{
	CAppearanceDlgBox* pAppearanceDlgBox = (CAppearanceDlgBox*)dwRefData;
	if (msg == WM_LBUTTONDOWN)
	{
		POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		pAppearanceDlgBox->OnPreviewClick(pt);

		return 0;
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

// I SHALL CLEAN THIS
void CAppearanceDlgBox::OnPreviewClick(POINT pt)
{
	Microsoft::WRL::ComPtr<IWindowMetrics> pMetrics;
	pWndPreview.As(&pMetrics);
	//printf("%d,%d; x:%d, y:%d, cx:%d, cy:%d\n", pt.x, pt.y, rc.left, rc.top, rc.right, rc.bottom);

	int index = 6;

	RECT rc;
	pMetrics->GetBoundingRect(0, -1, &rc);
	if (PtInRect(&rc, pt)) index = 13;

	pMetrics->GetBoundingRect(1, -1, &rc);
	if (PtInRect(&rc, pt)) index = 2;

	pMetrics->GetBoundingRect(0, 0, &rc);
	if (PtInRect(&rc, pt)) index = 12;

	pMetrics->GetBoundingRect(1, 0, &rc);
	if (PtInRect(&rc, pt)) index = 1;

	pMetrics->GetBoundingRect(1, 6, &rc);
	if (PtInRect(&rc, pt)) index = 20;

	pMetrics->GetBoundingRect(1, 9, &rc);
	if (PtInRect(&rc, pt)) index = 17;

	pMetrics->GetBoundingRect(2, -1, &rc);
	if (PtInRect(&rc, pt)) index = 15;

	pMetrics->GetBoundingRect(2, 0, &rc);
	if (PtInRect(&rc, pt)) index = 1;

	pMetrics->GetBoundingRect(1, 8, &rc);
	if (PtInRect(&rc, pt)) index = 14;

	pMetrics->GetBoundingRect(2, 7, &rc);
	if (PtInRect(&rc, pt)) index = 0;

	for (int i = 2; i < 5; ++i)
	{
		pMetrics->GetBoundingRect(0, i, &rc);
		if (PtInRect(&rc, pt)) index = 0;

		pMetrics->GetBoundingRect(1, i, &rc);
		if (PtInRect(&rc, pt)) index = 0;

		pMetrics->GetBoundingRect(2, 2, &rc);
		if (PtInRect(&rc, pt)) index = 0;
	}

	ComboBox_SetCurSel(hElementCombobox, index);
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, index);

	_UpdateControls(tinfo);
	_RedrawColorButtons();
	_UpdateSizeItem(tinfo);
	_UpdateFont(tinfo);
}

BOOL CAppearanceDlgBox::OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
	ApplySystemDialogFont(m_hWnd);
	hElementCombobox = GetDlgItem(1126);
	hSizeUpdown = GetDlgItem(1133);
	hColor1 = GetDlgItem(1135);
	hColor2 = GetDlgItem(1141);
	hFontCmb = GetDlgItem(1129);
	hFontSize = GetDlgItem(1130);
	hFontColor = GetDlgItem(1136);
	hColor1 = InitializeThemeUiColorButton(hColor1);
	hColor2 = InitializeThemeUiColorButton(hColor2);
	hFontColor = InitializeThemeUiColorButton(hFontColor);
	hBold = GetDlgItem(1131);
	hItalic = GetDlgItem(1132);
	hPreview = GetDlgItem(1470);
	size = GetClientSIZE(hPreview);

	SetWindowSubclass(hPreview, PreviewSubclassProc, 0, (DWORD_PTR)this);

	for (int i = 1; i < 28; ++i) // 28
	{
		WCHAR szBuffer[40];
		if (LoadString(g_hThemeUI, 1400 + i, szBuffer, ARRAYSIZE(szBuffer)))
		{
			if (i == 24) continue;

			int index = ComboBox_AddString(hElementCombobox, szBuffer);
			ComboBox_SetItemData(hElementCombobox, index, &info[i-1]);
			
			if (i == 1)
			{
				ComboBox_SetCurSel(hElementCombobox, index);
				_UpdateControls(&info[i-1]);
				_RedrawColorButtons();
				_UpdateSizeItem(&info[i-1]);
			}
		}
	}

	for (UINT i = 0; i < fontInfo->cFontList; ++i)
	{
		ComboBox_AddString(hFontCmb, fontInfo->ppFontList[i]);
	}

	// create a dummy scheme
	CreateBlankScheme();

	pWndPreview = Make<CWindowPreview>(size, wnd, (int)ARRAYSIZE(wnd), PAGETYPE::PT_APPEARANCE, nullptr, GetDpiForWindow(m_hWnd));
	Microsoft::WRL::ComPtr<IWindowConfig> pConfig;
	pWndPreview.As(&pConfig);
	pConfig->SetClassicPrev(TRUE);

	HBITMAP ebmp;
	pWndPreview->GetPreviewImage(&ebmp);
	SetBitmap(hPreview, ebmp);
	return TRUE;
}

LRESULT CAppearanceDlgBox::OnOK(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled)
{
	EndDialog(0);
	return 0;
}

LRESULT CAppearanceDlgBox::OnCancel(UINT uNotifyCode, int nID, HWND hWnd, BOOL& bHandled)
{
	EndDialog(1);
	return 0;
}

BOOL CAppearanceDlgBox::OnComboboxChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	int index = ComboBox_GetCurSel(hElementCombobox);
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, index);

	_UpdateControls(tinfo);
	_RedrawColorButtons();
	_UpdateSizeItem(tinfo);
	_UpdateFont(tinfo);

	return 0;
}

BOOL CAppearanceDlgBox::OnColorPick(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	// Let the button finish its click and release capture before showing the popup.
	::PostMessageW(m_hWnd, WM_OPEN_COLOR_PALETTE, id, reinterpret_cast<LPARAM>(hWnd));
	return 0;
}

LRESULT CAppearanceDlgBox::OnDrawItem(UINT, WPARAM, LPARAM lParam, BOOL& bHandled)
{
	auto* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
	if (!drawItem || drawItem->CtlType != ODT_BUTTON)
	{
		bHandled = FALSE;
		return 0;
	}
	WORD target = 0;
	if (!_GetColorTarget(drawItem->CtlID, target))
	{
		bHandled = FALSE;
		return 0;
	}
	COLORREF color = target == COLOR_DESKTOP ? GetDeskopColor() : NcGetSysColor(target);
	DrawThemeUiColorButton(*drawItem, color);
	return TRUE;
}

bool CAppearanceDlgBox::_GetColorTarget(UINT controlId, WORD& target)
{
	if (controlId != 1135 && controlId != 1136 && controlId != 1141)
		return false;
	if (!hElementCombobox || !::IsWindow(hElementCombobox))
		return false;
	int index = ComboBox_GetCurSel(hElementCombobox);
	if (index == CB_ERR)
		return false;
	LRESULT itemData = ComboBox_GetItemData(hElementCombobox, index);
	if (itemData == CB_ERR)
		return false;
	auto* info = reinterpret_cast<SCHEMEINFO*>(itemData);
	if (controlId == 1135) target = info->color1Target;
	else if (controlId == 1136) target = info->fontColorTarget;
	else target = info->color2Target;
	return true;
}

LRESULT CAppearanceDlgBox::OnOpenColorPalette(UINT, WPARAM wParam, LPARAM lParam, BOOL&)
{
	UINT id = static_cast<UINT>(wParam);
	HWND hWnd = reinterpret_cast<HWND>(lParam);
	WORD target = 0;
	if (!::IsWindow(hWnd) || !_GetColorTarget(id, target))
		return 0;

	COLORREF color = target == COLOR_DESKTOP ? GetDeskopColor() : NcGetSysColor(target);
	if (PickThemeUiColor(hWnd, color, color))
	{
		selectedTheme->selectedScheme->rgb[target] = color;
		if (id == 1135 && target == COLOR_DESKTOP)
		{
			selectedTheme->newColor = color;
			selectedTheme->fCustomDesktopColorPending = true;
		}
		::InvalidateRect(hWnd, nullptr, TRUE);
		_UpdatePreview(TRUE);
	}
	return 0;
}

BOOL CAppearanceDlgBox::OnSpinnerChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	int index = ComboBox_GetCurSel(hElementCombobox);
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, index);

	if (tinfo && tinfo->activeButton & ACTIVE_SIZEITEM)
	{
		BOOL valid = FALSE;
		int value = GetDlgItemInt(id, &valid, FALSE);
		// EN_CHANGE also fires while the field is empty. Preserve an entered 0.
		if (!valid) return 0;
		NcUpdateSystemMetrics(tinfo->sizeTarget, value);

		if (tinfo->sizeTarget == SM_CYHSCROLL) NcUpdateSystemMetrics(SM_CXVSCROLL, value);
		if (tinfo->sizeTarget == SM_CYSIZE) NcUpdateSystemMetrics(SM_CXSIZE, value);

		_UpdatePreview(FALSE);
	}
	return 0;
}

BOOL CAppearanceDlgBox::OnSpinnerDelta(WPARAM wParam, LPNMHDR nmhdr, BOOL& bHandled)
{
	NMUPDOWN* ud = (NMUPDOWN*)nmhdr;
	ud->iDelta = -ud->iDelta;
	return 0;
}

BOOL CAppearanceDlgBox::OnFontChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, ComboBox_GetCurSel(hElementCombobox));
	LOGFONT* lf = _GetLogFontPtr(tinfo);

	int index = ComboBox_GetCurSel(hFontCmb);
	int len = ComboBox_GetLBTextLen(hFontCmb, index) + 1;
	wchar_t* szDest = (wchar_t*)malloc(len * sizeof(wchar_t));

	ComboBox_GetLBText(hFontCmb, index, szDest);
	StringCchCopy(lf->lfFaceName, ARRAYSIZE(lf->lfFaceName), szDest);

	_UpdatePreview(FALSE);
	return 0;
}

BOOL CAppearanceDlgBox::OnFontSizeChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, ComboBox_GetCurSel(hElementCombobox));
	LOGFONT* lf = _GetLogFontPtr(tinfo);

	int fontSize = 6 + ComboBox_GetCurSel(hFontSize);
	int height = -MulDiv(fontSize, 96, 72);
	int delta = -height + lf->lfHeight;

	if (tinfo->fontTarget == 0)
	{
		selectedTheme->selectedScheme->ncm.iCaptionHeight += delta;
	}
	else if (tinfo->fontTarget == 2)
	{
		selectedTheme->selectedScheme->ncm.iMenuHeight += delta;
	}

	lf->lfHeight = height;
	_UpdateSizeItem(tinfo);
	
	_UpdatePreview(FALSE);
	return 0;
}

BOOL CAppearanceDlgBox::OnFontSizeEditChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, ComboBox_GetCurSel(hElementCombobox));
	LOGFONT* lf = _GetLogFontPtr(tinfo);

	int len = ::ComboBox_GetTextLength(hFontSize) + 1;
	wchar_t* szDest = (wchar_t*)malloc(len * sizeof(wchar_t));
	::ComboBox_GetText(hFontSize, szDest, len);

	int fontSize = _wtoi(szDest);
	if (fontSize > 0)
	{
		int height = -MulDiv(fontSize, 96, 72);
		int delta = -height + lf->lfHeight;

		if (tinfo->fontTarget == 0)
		{
			selectedTheme->selectedScheme->ncm.iCaptionHeight += delta;
			lf->lfHeight = height;
		}
		else if (tinfo->fontTarget == 2)
		{
			selectedTheme->selectedScheme->ncm.iMenuHeight += delta;
			lf->lfHeight = height;
		}

		_UpdateSizeItem(tinfo);
	}

	_UpdatePreview(FALSE);
	return 0;
}

BOOL CAppearanceDlgBox::OnStyle(UINT code, UINT id, HWND hWnd, BOOL& bHandled)
{
	SCHEMEINFO* tinfo = (SCHEMEINFO*)ComboBox_GetItemData(hElementCombobox, ComboBox_GetCurSel(hElementCombobox));
	LOGFONT* lf = _GetLogFontPtr(tinfo);

	if (id == 1131)
	{
		lf->lfWeight = fIsBold ? FW_NORMAL : FW_BOLD;
		fIsBold = !fIsBold;
		Button_SetCheck(hBold, fIsBold);
	}
	if (id == 1132)
	{
		lf->lfItalic = !fIsItalic;
		fIsItalic = !fIsItalic;
		Button_SetCheck(hItalic, fIsItalic);
	}
	_UpdatePreview(FALSE);
	return 0;
}

void CAppearanceDlgBox::_UpdateControls(SCHEMEINFO* info)
{
	::EnableWindow(hSizeUpdown, info->activeButton & ACTIVE_SIZEITEM);
	::EnableWindow((HWND)SendMessage(hSizeUpdown, UDM_GETBUDDY, 0, 0), info->activeButton & ACTIVE_SIZEITEM);
	::EnableWindow(hColor1, info->activeButton & ACTIVE_COLOR1);
	::EnableWindow(hColor2, info->activeButton & ACTIVE_COLOR2);
	::EnableWindow(hFontCmb, info->activeButton & ACTIVE_FONT);
	::EnableWindow(hFontSize, info->activeButton & ACTIVE_FONT);
	::EnableWindow(hFontColor, info->activeButton & ACTIVE_FONTCOLOR);
	::EnableWindow(hBold, info->activeButton & ACTIVE_FONT);
	::EnableWindow(hItalic, info->activeButton & ACTIVE_FONT);
}

void CAppearanceDlgBox::_RedrawColorButtons()
{
	::InvalidateRect(hColor1, nullptr, TRUE);
	::InvalidateRect(hColor2, nullptr, TRUE);
	::InvalidateRect(hFontColor, nullptr, TRUE);
}

void CAppearanceDlgBox::_UpdateSizeItem(SCHEMEINFO* info)
{
	if (info->activeButton & ACTIVE_SIZEITEM)
	{
		WCHAR szText[5];
		StringCchPrintf(szText, ARRAYSIZE(szText), L"%d", NcGetSystemMetrics(info->sizeTarget));
		::SetWindowText((HWND)SendMessage(hSizeUpdown, UDM_GETBUDDY, 0, 0), szText);
	}
	else
	{
		::SetWindowText((HWND)SendMessage(hSizeUpdown, UDM_GETBUDDY, 0, 0), L"");
	}
}

void CAppearanceDlgBox::_UpdateFont(SCHEMEINFO* info)
{
	if (info->activeButton & ACTIVE_FONT)
	{
		LOGFONT* lf = _GetLogFontPtr(info);

		int index = ComboBox_FindString(hFontCmb, -1, lf->lfFaceName);
		ComboBox_SetCurSel(hFontCmb, index);

		ComboBox_ResetContent(hFontSize);
		int fontSize = -MulDiv(lf->lfHeight, 72, 96);
		for (int i = 6; i < 6 * 4; ++i)
		{
			wchar_t buffer[3];
			wsprintf(buffer, L"%d", i);
			ComboBox_AddString(hFontSize, buffer);
		}

		wchar_t buffer[3];
		wsprintf(buffer, L"%d", fontSize);
		ComboBox_SetCurSel(hFontSize, ComboBox_FindString(hFontSize, -1, buffer));

		fIsBold = lf->lfWeight == FW_BOLD;
		fIsItalic = lf->lfItalic;
		Button_SetCheck(hBold, fIsBold);
		Button_SetCheck(hItalic, fIsItalic);
	}
	else
	{
		::ComboBox_SetText(hFontCmb, L"");
		::ComboBox_SetText(hFontSize, L"");
		Button_SetCheck(hBold, 0);
		Button_SetCheck(hItalic, 0);
	}
}

void CAppearanceDlgBox::_UpdatePreview(BOOL fClr)
{
	UPDATEFLAGS flag = UPDATE_WINDOW;
	if (fClr) flag |= UPDATE_SOLIDCLR;

	HBITMAP ebmp;
	pWndPreview->GetUpdatedPreviewImage(wnd, nullptr, &ebmp, flag);
	SetBitmap(hPreview, ebmp);
}

LOGFONT* CAppearanceDlgBox::_GetLogFontPtr(SCHEMEINFO* info)
{
	LOGFONT* lf = NULL;
	switch (info->fontTarget)
	{
		case 0: lf = &selectedTheme->selectedScheme->ncm.lfCaptionFont; break;
		case 1: lf = &selectedTheme->selectedScheme->ncm.lfSmCaptionFont; break;
		case 2: lf = &selectedTheme->selectedScheme->ncm.lfMenuFont; break;
		case 3: lf = &selectedTheme->selectedScheme->ncm.lfStatusFont; break;
		case 4: lf = &selectedTheme->selectedScheme->ncm.lfMessageFont; break;
		case 5: lf = &selectedTheme->selectedScheme->lfIconTitle; break;
		default: break;
	}
	return lf;
}

void CAppearanceDlgBox::OnClose()
{
	SetBitmap(hPreview, NULL);
	RemoveWindowSubclass(hPreview, PreviewSubclassProc, 0);

	EndDialog(1);
}
