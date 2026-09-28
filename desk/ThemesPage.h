#pragma once
#include "pch.h"
#include "desk.h"
#include "wndprvw.h"
#include <string>
#include <vector>

class CThemeDlgProc
    : public WTL::CPropertyPageImpl<CThemeDlgProc>
{
public:
    enum { IDD = IDD_THEMEDLG };

    bool IsStartupThemeSnapshotSelected() const;
    void OnPropertySheetChanged();
    void OnPropertySheetApplyCompleted();
    void PersistModifiedStateAfterApply();

private:
    BEGIN_MSG_MAP(CThemeDlgProc)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        COMMAND_HANDLER(1101, CBN_SELCHANGE, OnThemeComboboxChange)
        COMMAND_HANDLER(1107, BN_CLICKED, OnSaveAs)
        COMMAND_HANDLER(1108, BN_CLICKED, OnDelete)
        CHAIN_MSG_MAP(WTL::CPropertyPageImpl<CThemeDlgProc>)
    END_MSG_MAP()

    BOOL OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    BOOL OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
    BOOL OnThemeComboboxChange(UINT code, UINT id, HWND hWnd, BOOL& bHandled);
    BOOL OnSaveAs(UINT code, UINT id, HWND hWnd, BOOL& bHandled);
    BOOL OnDelete(UINT code, UINT id, HWND hWnd, BOOL& bHandled);
    BOOL OnApply();
    BOOL OnSetActive();

    // custom methods
    void UpdateThemeInfo(LPWSTR ws);
    bool GetDeletableThemePath(int themeIndex, WCHAR (&path)[MAX_PATH]);
    SCHEMEDATA* GetPreviewScheme();
    int GetThemeIndexFromCombo(int comboIndex) const;
    int FindThemeComboIndex(int themeIndex) const;
    void UpdateDeleteButton();
    void PopulateThemeCombo(int selectedIndex);

    /// variables
    HWND hCombobox;
    HWND hPreview;
    SIZE size;
    Microsoft::WRL::ComPtr<IWindowPreview> pWndPreview;
    SCHEMEDATA* currentRegistryScheme = nullptr;
    SCHEMEDATA* standardPreviewScheme = nullptr;
    int lastThemeIndex = -1;
    int currentThemeIndex = -1;
    int startupThemeIndex = -1;
    GUID startupThemeId = {};
    bool startupThemeIdValid = false;
    std::wstring currentThemeParentLabel;
    std::wstring startupThemeParentLabel;
    std::wstring startupThemeSnapshotPath;
    bool startupSnapshotIsCurrent = true;
    bool currentThemeDerivativeModified = false;
    bool themeApplyPending = false;
    std::wstring pendingThemeFilePath;
    std::vector<std::wstring> savedThemePaths;

    MYWINDOWINFO wnd[1] =
    {
        {
            WT_ACTIVE,
            {33, 33, 250, 33 + 124}
        }
    };

};

bool IsMyCurrentThemeSelectedForApply();
void CompleteMyCurrentThemeApply();
void NotifyThemePagePropertySheetChanged();
void PersistThemeModifiedStateAfterApply();
