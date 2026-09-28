#pragma once

#include <windows.h>
#include <commctrl.h>

// Shows the compact ThemeUI-style palette anchored to a color button.
// Returns false if dismissed without choosing a color.
bool PickThemeUiColor(HWND ownerButton, COLORREF initialColor, COLORREF& chosenColor);

// Initializes the ported ThemeUI button with visual-style hover tracking.
HWND InitializeThemeUiColorButton(HWND button);

// Paints the ThemeUI color button with the active v6 theme's border and arrow.
void DrawThemeUiColorButton(const DRAWITEMSTRUCT& drawItem, COLORREF color);
