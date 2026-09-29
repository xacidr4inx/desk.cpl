#pragma once
#include "pch.h"
#include "theme.h"

// set a static to display a bitmap
#define Static_SetBitmap(hwndCtl, hBmp)  \
	((HBITMAP)(UINT_PTR)SNDMSG((hwndCtl), STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)(HBITMAP)(hBmp)))

#define Button_SetBitmap(hwndCtl, hBmp)  \
	((HBITMAP)(UINT_PTR)SNDMSG((hwndCtl), BM_SETIMAGE , (WPARAM)IMAGE_BITMAP, (LPARAM)(HBITMAP)(hBmp)))

// 29 colors
#define MAX_COLORS (COLOR_GRADIENTINACTIVECAPTION + 1)

typedef struct tagNONCLIENTMETRICSW_2k
{
	UINT    cbSize;
	int     iBorderWidth;
	int     iScrollWidth;
	int     iScrollHeight;
	int     iCaptionWidth;
	int     iCaptionHeight;
	LOGFONTW lfCaptionFont;
	int     iSmCaptionWidth;
	int     iSmCaptionHeight;
	LOGFONTW lfSmCaptionFont;
	int     iMenuWidth;
	int     iMenuHeight;
	LOGFONTW lfMenuFont;
	LOGFONTW lfStatusFont;
	LOGFONTW lfMessageFont;
}  NONCLIENTMETRICSW_2k;

typedef struct tagSCHEMEDATA {
	// registry structure
	DWORD version;
	NONCLIENTMETRICSW_2k ncm;
	LOGFONT lfIconTitle;
	COLORREF rgb[MAX_COLORS];
	int iPaddedBorderWidth = 0;

	// custom fields
	WCHAR name[40];
	DWORD variant;
	int schemeMapIndex;
	BOOL dpiScaled;

} SCHEMEDATA;

enum WALLPAPER_TYPE {
	WT_NOWALL = 1,
	WT_PICTURE,
	WT_SLIDESHOW
};

// todo: convert this to a single class
// convenient struct with various information used by various pages
struct THEMEINFO
{
	WALLPAPER_TYPE wallpaperType;
	std::wstring wallpaperPath;
	COLORREF newColor;
	bool fCustomDesktopColorPending = false;
	bool customWallpaperSelection = false;
	int posChanged = -1;
	bool useDesktopColor = false;
	bool updateWallThemesPg = false;
	SCHEMEDATA* selectedScheme = NULL;		// address to the selected scheme data
											// NULL- use current colors

	// reduce overhead every time u load appearancepage
	std::wstring szMsstylePath;
	// Retain Classic data from a browsed theme while its msstyle is active.
	std::wstring classicSchemeSourcePath;
	bool fMsstyleChanged = false;
	bool fThemePgMsstyleUpdate = false;		// bruh
	bool fSlideshowSelection;
};

typedef struct tag_FONTINFO
{
	wchar_t** ppFontList;
	UINT cFontList;
} FONTINFO;

// global HINSTANCE for the current app
extern HINSTANCE g_hinst;

// themeui instance
extern HINSTANCE g_hThemeUI;

// ThemeManager2 interface
extern IThemeManager2* pThemeManager;

// DesktopWallpaper (8+) interface
extern IDesktopWallpaper* pDesktopWallpaper;

// current theme's ITheme interface, defined as a IUnknown for wrapper
// USAGE: ITheme* = new ITheme(currentITheme);
extern IUnknown* currentITheme;

// various theme information used across multiple pages
// defined as a struct for convenience
extern THEMEINFO* selectedTheme;

// bool to keep track if the selection in wallpaper listview is programmatically done or not
extern BOOL selectionPicker;

// process information for the loaded screen saver preview
// defined here so it can be killed by other pages
extern PROCESS_INFORMATION pi;

// font list
extern FONTINFO* fontInfo;

extern BOOL themeSelected;
extern volatile LONG g_currentSessionModified;
extern volatile LONG g_themeSelectionInProgress;
// Desktop page edits must also participate in the Themes page's apply pass.
extern bool g_desktopThemeApplyPending;

// Loads the theme and the matching color/metric scheme before any property
// page is created.  The property pages use this shared state for previews.
void InitializeCurrentThemeState();

// Drop the saved-file identity after the active theme has been modified, so
// the Themes page shows its Modified state instead of the source file name.
void ForgetSavedThemePathForCurrentTheme();
