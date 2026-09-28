#pragma once
#include <windows.h>

// Let USER32 determine the bevel and glyph geometry, rather than sizing a
// Marlett font from only the button height. Recolor the result locally so
// previewing a pending Classic scheme never changes global system colors.
inline BOOL DrawPreviewFrameControl(HDC target, const RECT& bounds, UINT kind, UINT state,
	const COLORREF* previewColors)
{
	RECT nativeBounds = bounds;
	if (!previewColors)
		return ::DrawFrameControl(target, &nativeBounds, kind, state);

	constexpr int colorRoles[] = { COLOR_BTNFACE, COLOR_BTNTEXT, COLOR_BTNHIGHLIGHT,
		COLOR_BTNSHADOW, COLOR_3DDKSHADOW, COLOR_3DLIGHT, COLOR_WINDOWFRAME, COLOR_GRAYTEXT };
	DWORD sourceColors[ARRAYSIZE(colorRoles)] = {};
	DWORD destinationColors[ARRAYSIZE(colorRoles)] = {};
	bool paletteChanged = false;
	auto dibColor = [](COLORREF color) -> DWORD {
		return (static_cast<DWORD>(GetRValue(color)) << 16) |
			(static_cast<DWORD>(GetGValue(color)) << 8) | GetBValue(color);
	};
	for (size_t index = 0; index < ARRAYSIZE(colorRoles); ++index)
	{
		sourceColors[index] = dibColor(::GetSysColor(colorRoles[index]));
		destinationColors[index] = dibColor(previewColors[colorRoles[index]]);
		paletteChanged = paletteChanged || sourceColors[index] != destinationColors[index];
	}
	if (!paletteChanged)
		return ::DrawFrameControl(target, &nativeBounds, kind, state);

	const int width = bounds.right - bounds.left;
	const int height = bounds.bottom - bounds.top;
	if (width <= 0 || height <= 0) return FALSE;
	BITMAPINFO info = {};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;
	DWORD* pixels = nullptr;
	HDC buffer = ::CreateCompatibleDC(target);
	if (!buffer) return FALSE;
	HBITMAP bitmap = ::CreateDIBSection(target, &info, DIB_RGB_COLORS,
		reinterpret_cast<void**>(&pixels), nullptr, 0);
	if (!bitmap)
	{
		::DeleteDC(buffer);
		return FALSE;
	}
	HGDIOBJ previous = ::SelectObject(buffer, bitmap);
	RECT rect = { 0, 0, width, height };
	BOOL result = ::DrawFrameControl(buffer, &rect, kind, state);
	if (result)
	{
		::GdiFlush();
		const size_t count = static_cast<size_t>(width) * height;
		for (size_t pixel = 0; pixel < count; ++pixel)
		{
			const DWORD color = pixels[pixel] & 0x00ffffff;
			for (size_t role = 0; role < ARRAYSIZE(colorRoles); ++role)
			{
				if (color == sourceColors[role])
				{
					pixels[pixel] = destinationColors[role];
					break;
				}
			}
		}
		result = ::BitBlt(target, bounds.left, bounds.top, width, height, buffer, 0, 0, SRCCOPY);
	}
	::SelectObject(buffer, previous);
	::DeleteObject(bitmap);
	::DeleteDC(buffer);
	return result;
}

inline BOOL DrawPreviewCaptionButton(HDC target, const RECT& bounds, UINT state,
	const COLORREF* previewColors)
{
	return DrawPreviewFrameControl(target, bounds, DFC_CAPTION, state, previewColors);
}
