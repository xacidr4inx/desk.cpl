#pragma once
#include "framework.h"
#include "desk.h"

#define RECTWIDTH(rc)   ((rc).right-(rc).left)
#define RECTHEIGHT(rc)  ((rc).bottom-(rc).top)

#define GETSIZE(size) (size).cx, (size).cy
#define SPLIT_COLORREF(clr) GetRValue(clr), GetGValue(clr), GetBValue(clr)

#pragma comment(lib, "ntdll.lib")
// ignore warning, resolved by ntdll.lib
extern "C" NTSTATUS NTAPI NtOpenSection(
	OUT PHANDLE SectionHandle,
	IN ACCESS_MASK DesiredAccess,
	IN POBJECT_ATTRIBUTES ObjectAttributes
);


void _TerminateProcess(PROCESS_INFORMATION& hp);
// Use the current system message font for applet chrome; preview rendering
// continues to use the selected theme's own font metrics.
void ApplySystemDialogFont(HWND hwnd);
std::wstring LoadDeskString(UINT id);
COLORREF GetDeskopColor();
void EnumDir(LPCWSTR directory, LPCWSTR* extensions, int cExtensions, std::vector<LPWSTR>& vec, BOOL fEnumChildDirs);
void FreeBitmap(Gdiplus::Bitmap** bmp);
HRESULT DrawBitmapIfNotNull(Gdiplus::Bitmap* bmp, Gdiplus::Graphics* graph, Gdiplus::Rect rect);
HTHEME OpenNcThemeData(LPVOID file, LPCWSTR pszClassList);
BOOL IsClassicThemeEnabled();
bool IsWindowsClassicThemeName(LPCWSTR name);
char* trim(char* s);
wchar_t* strCut(wchar_t* s, const wchar_t* pattern);
void ScaleLogFont(LOGFONT& lf, int dpi);
void ScaleNonClientMetrics(NONCLIENTMETRICS& ncm, int dpi);
void ScaleNonClientMetrics(NONCLIENTMETRICSW_2k& ncm, int dpi);
bool ApplySchemeMetrics(const SCHEMEDATA* scheme, int dpi);
HRESULT GetSolidBtnBmp(COLORREF clr, int dpi, SIZE size, HBITMAP* pbOut);
BOOL ColorPicker(COLORREF clr, HWND hWnd, CHOOSECOLOR* clrOut, BOOL fullOpen = TRUE);
void CreateBlankScheme();
SCHEMEDATA* LoadCurrentClassicSchemeFromRegistry();
SCHEMEDATA* LoadWindowsStandardClassicScheme();
bool LoadClassicSchemeFromThemeFile(LPCWSTR path, SCHEMEDATA& scheme);
LSTATUS WriteClassicSchemeToThemeFile(LPCWSTR path, const SCHEMEDATA* scheme);
void CreateThemedMetricsScheme(int dpi, void* pTheme);
void SetBitmap(HWND hWnd, HBITMAP hBmp);
void UpdateCustomTheme();

inline SIZE GetClientSIZE(HWND _hwnd)
{
	RECT rect;
	GetClientRect(_hwnd, &rect);
	return { RECTWIDTH(rect), RECTHEIGHT(rect) };
}

