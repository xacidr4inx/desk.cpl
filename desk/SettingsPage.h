#pragma once
#include "pch.h"

typedef struct
{
	DWORD width;
	DWORD height;
	DWORD bpp;
} RESINFO;

typedef struct
{
	DWORD width;
	DWORD height;
	DWORD freq;
	DWORD bpp;
} CURRENT_RESINFO;

enum COLORMODES
{
	E4_BIT = 1 << 0,
	E8_BIT = 1 << 1,
	E16_BIT = 1 << 2,
	E24_BIT = 1 << 3,
	E32_BIT = 1 << 4,
};
DEFINE_ENUM_FLAG_OPERATORS(COLORMODES);

class CSettingsDlgProc
	: public WTL::CPropertyPageImpl<CSettingsDlgProc>
{
public:
	enum { IDD = IDD_SETTINGSDLG };

private:
	BEGIN_MSG_MAP(CSettingsDlgProc)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
		MESSAGE_HANDLER(WM_DISPLAYCHANGE, OnDisplayChange)
		MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
		MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
		MESSAGE_HANDLER(WM_HSCROLL, OnHScroll)
		COMMAND_HANDLER(1800, CBN_SELCHANGE, OnMonitorChange)
		COMMAND_HANDLER(1807, CBN_SELCHANGE, OnColorChange)
		COMMAND_HANDLER(1805, BN_CLICKED, OnExtendChange)
		COMMAND_HANDLER(1806, BN_CLICKED, OnPrimaryChange)
		COMMAND_HANDLER(1822, BN_CLICKED, OnIdentify)
		COMMAND_HANDLER(1802, BN_CLICKED, OnAdvanced)
		CHAIN_MSG_MAP(WTL::CPropertyPageImpl<CSettingsDlgProc>)
	END_MSG_MAP()

	BOOL OnInitDialog(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnDisplayChange(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnDrawItem(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnNotify(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnHScroll(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
	LRESULT OnMonitorChange(WORD code, WORD id, HWND hWnd, BOOL& bHandled);
	LRESULT OnColorChange(WORD code, WORD id, HWND hWnd, BOOL& bHandled);
	LRESULT OnExtendChange(WORD code, WORD id, HWND hWnd, BOOL& bHandled);
	LRESULT OnPrimaryChange(WORD code, WORD id, HWND hWnd, BOOL& bHandled);
	LRESULT OnIdentify(WORD code, WORD id, HWND hWnd, BOOL& bHandled);
	LRESULT OnAdvanced(WORD code, WORD id, HWND hWnd, BOOL& bHandled);
	BOOL OnApply();


	void _EnumerateActiveDisplayDevices();
	void _SelectCurrentMonitor();
	void _UpdateSelectedMonitorState();
	void _UpdatePendingDisplayState();
	bool _HasPendingDisplayChanges() const;
	std::vector<RECT> _GetPendingMonitorRects(int selected = -1) const;
	void _GetAllModes();
	void _StoreSelectedPendingMode();
	bool _GetPendingMode(LPCWSTR deviceName, RESINFO& mode) const;
	void _SetTrackbarModes(int modenum);
	void _BuildColorList();
	void _UpdateColorPreview();
	void _SelectCurrentResolution();
	void _UpdateMonitorPreview();
	void _UpdateMonitorPreviewLayout();
	void _SnapMonitorRect(int index, RECT& rect, bool force = false);
	void _RestoreMonitorDragCursorClip();
	void _FinishMonitorDrag(HWND hwnd);
	void _UpdateMonitorTooltip(POINT point);
	void _ShowMonitorTooltip(HWND hwnd);
	void _PositionMonitorTooltip(HWND hwnd);
	static BOOL CALLBACK _MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data);
	static LRESULT CALLBACK _MonitorPreviewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
	void _PaintMonitorPreview(HDC hdc, RECT client);

	struct MONITORPREVIEW
	{
		RECT rect;
		RECT originalRect;
		bool primary;
		bool originalPrimary;
		bool active;
		bool enableRequested;
		bool disableRequested;
		WCHAR monitorName[128];
		WCHAR deviceName[CCHDEVICENAME];
		WCHAR applyDeviceName[CCHDEVICENAME];
		WCHAR monitorInstanceId[512];
	};

	HWND _cmbMonitors;
	HWND _textDisplay;
	HWND _chkPrimary;
	HWND _chkExtend;
	HWND _mulMonPreview;
	std::unique_ptr<Gdiplus::Bitmap> _singleMonitorArtwork;
	std::vector<RECT> _originalModeControlRects;
	HWND _monitorTooltip = nullptr;
	std::vector<HWND> _identifyWindows;
	HWND _textCurrentRes;
	HWND _trackResolution;
	HWND _cmbColors;
	HWND _clrPreview;
	std::vector<RESINFO> _arrResInfo;
	struct PENDINGMODE
	{
		WCHAR deviceName[CCHDEVICENAME];
		RESINFO mode;
	};
	std::vector<PENDINGMODE> _pendingModes;
	std::vector<DWORD> _arrSupportedBpp;
	CURRENT_RESINFO _currentResInfo;
	WCHAR _loadedModeDeviceName[CCHDEVICENAME] = {};
	std::vector<MONITORPREVIEW> _monitors;
	std::vector<RECT> _previewPixelRects;
	std::vector<RECT> _previewDrawRects;
	std::vector<RECT> _previewHitRects;
	RECT _previewBounds{};
	int _previewScale = 0;
	int _previewDiagramLeft = 0;
	int _previewDiagramTop = 0;
	int _draggingMonitor = -1;
	POINT _dragStartPoint{};
	RECT _dragStartRect{};
	std::vector<RECT> _dragOriginalRects;
	RECT _previousCursorClip{};
	POINT _dragVisualOffset{};
	RECT _dragWorldBounds{};
	int _dragScale = 0;
	int _dragDiagramLeft = 0;
	int _dragDiagramTop = 0;
	bool _dragMoved = false;
	bool _dragHorizontalPlane = true;
	bool _monitorDragCursorClipped = false;
	bool _colorSelectionDirty = false;
	bool _applyingDisplayChanges = false;
	bool _trackingMouse = false;
	bool _tooltipActive = false;
	int _tooltipTarget = -1;
	int _previewSelection = -1;
	WCHAR _monitorTooltipText[128] = {};
	WNDPROC _oldMonitorPreviewProc = nullptr;
};
