#include "texture_viewer_window.h"
#include "rend/texture_preview.h"
#include "oslib/i18n.h"
#include <nowide/convert.hpp>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace textureViewerWindow
{
namespace
{
constexpr int pageSize = 24;
constexpr int filterId = 101, previousId = 102, nextId = 103, backId = 104;
constexpr UINT stopMessage = WM_APP + 1;
constexpr wchar_t windowClass[] = L"FlycastTextureViewer";
std::atomic<HWND> viewerWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true};

struct Labels
{
	std::wstring title, count, filter, empty, custom, back;
	std::wstring vram, peak, vramNote;
};

// All controls, GDI resources and view state belong to the viewer thread.
// Only immutable CPU previews cross from the renderer to this thread.
struct View
{
	Labels labels;
	HWND window{}, countLabel{}, filterLabel{}, filterEdit{}, previous{}, next{}, pageLabel{}, back{};
	HWND vramLabel{}, peakLabel{}, vramNoteLabel{};
	HFONT font{};
	int dpi = 96, width = 0, height = 0, page = 0, scroll = 0, wheel = 0;
	int contentTop = 0, meterTop = 0;
	double vramFraction = 0;
	u64 generation = 0;
	std::wstring filter;
	std::vector<std::shared_ptr<const texturePreview::Image>> images;
	std::shared_ptr<const texturePreview::Image> selected;

	int px(int value) const { return MulDiv(value, dpi, 96); }
	int top() const { return contentTop; }
	int columns() const { return std::max(1, (width - px(16)) / px(144)); }
	int visibleCount() const { return std::max(0, std::min(pageSize, (int)images.size() - page * pageSize)); }
	int pages() const { return std::max(1, ((int)images.size() + pageSize - 1) / pageSize); }

	std::wstring description(const texturePreview::Image& image) const
	{
		wchar_t value[128];
		swprintf(value, std::size(value), L"%06X  %dx%d  %hs", image.address, image.width, image.height, image.format);
		return std::wstring(value) + (image.custom ? L"  " + labels.custom : L"");
	}

	void invalidate() { InvalidateRect(window, nullptr, FALSE); }

	void updateScroll()
	{
		int rows = (visibleCount() + columns() - 1) / columns();
		SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
		info.nMax = selected ? 0 : std::max(0, rows * px(168) + px(8) - 1);
		info.nPage = std::max(1, height - top());
		info.nPos = scroll;
		// Keep the scrollbar width stable when filtering or entering detail view.
		info.fMask |= SIF_DISABLENOSCROLL;
		SetScrollInfo(window, SB_VERT, &info, TRUE);
		scroll = GetScrollPos(window, SB_VERT);
	}

	void updateControls()
	{
		wchar_t value[40];
		swprintf(value, std::size(value), L"%d / %d", page + 1, pages());
		SetWindowTextW(pageLabel, value);
		EnableWindow(previous, !selected && page > 0);
		EnableWindow(next, !selected && page + 1 < pages());
		ShowWindow(back, selected ? SW_SHOWNA : SW_HIDE);
		updateScroll();
		invalidate();
	}

	void refresh()
	{
		u64 currentGeneration = texturePreview::generation();
		bool newGeneration = currentGeneration != generation;
		if (newGeneration)
		{
			generation = currentGeneration;
			page = scroll = 0;
			selected.reset();
		}
		auto nextImages = texturePreview::snapshot();
		wchar_t count[256];
		swprintf(count, std::size(count), labels.count.c_str(), (int)nextImages.size(), (int)texturePreview::MaxEntries);
		wchar_t oldCount[256];
		GetWindowTextW(countLabel, oldCount, (int)std::size(oldCount));
		if (wcscmp(count, oldCount) != 0)
			SetWindowTextW(countLabel, count);
		const auto usage = texturePreview::vramUsage();
		constexpr double mib = 1024.0 * 1024.0;
		const double fraction = usage.totalBytes == 0 ? 0.0 : double(usage.usedBytes) / usage.totalBytes;
		auto setText = [](HWND control, const wchar_t *text) {
			wchar_t previous[512];
			GetWindowTextW(control, previous, (int)std::size(previous));
			if (wcscmp(previous, text) == 0)
				return false;
			SetWindowTextW(control, text);
			return true;
		};
		wchar_t memoryText[512];
		swprintf(memoryText, std::size(memoryText), labels.vram.c_str(),
				usage.usedBytes / mib, usage.totalBytes / mib, fraction * 100.0);
		bool usageChanged = setText(vramLabel, memoryText);
		swprintf(memoryText, std::size(memoryText), labels.peak.c_str(),
				usage.peakBytes / mib, usage.remainingBytes() / mib);
		usageChanged |= setText(peakLabel, memoryText);
		if (usageChanged || vramFraction != fraction)
		{
			vramFraction = fraction;
			layout();
		}
		nextImages.erase(std::remove_if(nextImages.begin(), nextImages.end(), [&](const auto& image) {
			std::wstring label = description(*image);
			std::transform(label.begin(), label.end(), label.begin(), towlower);
			return label.find(filter) == std::wstring::npos;
		}), nextImages.end());
		if (!newGeneration && nextImages == images)
			return;
		images = std::move(nextImages);
		page = std::min(page, pages() - 1);
		// Retain the clicked snapshot while inspecting it, even if the game evicts it.
		updateControls();
	}

	void layout()
	{
		RECT client;
		GetClientRect(window, &client);
		width = client.right;
		height = client.bottom;
		// Measure wrapped labels so the VRAM summary remains readable at narrow
		// widths and on monitors with different DPI settings.
		HDC dc = GetDC(window);
		HGDIOBJ previousFont = SelectObject(dc, font);
		int y = px(10);
		auto placeLabel = [&](HWND label) {
			wchar_t text[512];
			GetWindowTextW(label, text, (int)std::size(text));
			RECT bounds{0, 0, std::max(px(40), width - px(24)), 0};
			DrawTextW(dc, text, -1, &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
			int labelHeight = std::max(px(22), (int)bounds.bottom);
			MoveWindow(label, px(12), y, bounds.right, labelHeight, TRUE);
			y += labelHeight + px(4);
		};
		placeLabel(countLabel);
		placeLabel(vramLabel);
		meterTop = y;
		y += px(14);
		placeLabel(peakLabel);
		placeLabel(vramNoteLabel);
		SelectObject(dc, previousFont);
		ReleaseDC(window, dc);
		y += px(4);
		MoveWindow(filterLabel, px(12), y + px(4), px(80), px(24), TRUE);
		MoveWindow(filterEdit, px(96), y, std::max(px(60), width - px(108)), px(28), TRUE);
		y += px(38);
		MoveWindow(previous, px(12), y, px(36), px(26), TRUE);
		MoveWindow(pageLabel, px(56), y + px(3), px(68), px(22), TRUE);
		MoveWindow(next, px(130), y, px(36), px(26), TRUE);
		MoveWindow(back, px(180), y, px(120), px(26), TRUE);
		contentTop = y + px(35);
		updateScroll();
		invalidate();
	}

	void setFont()
	{
		HFONT old = font;
		font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				DEFAULT_PITCH, L"Segoe UI");
		for (HWND control : {countLabel, filterLabel, filterEdit, previous, next, pageLabel, back,
				vramLabel, peakLabel, vramNoteLabel})
			SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
		if (old)
			DeleteObject(old);
	}

	void createControls()
	{
		auto control = [&](const wchar_t *type, const wchar_t *text, DWORD style, int id = 0) {
			return CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
					0, 0, 0, 0, window, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
		};
		countLabel = control(L"STATIC", L"", 0);
		vramLabel = control(L"STATIC", L"", 0);
		peakLabel = control(L"STATIC", L"", 0);
		vramNoteLabel = control(L"STATIC", labels.vramNote.c_str(), 0);
		filterLabel = control(L"STATIC", labels.filter.c_str(), 0);
		filterEdit = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, filterId);
		SendMessageW(filterEdit, EM_SETLIMITTEXT, 128, 0);
		previous = control(L"BUTTON", L"<", WS_TABSTOP, previousId);
		pageLabel = control(L"STATIC", L"", SS_CENTER);
		next = control(L"BUTTON", L">", WS_TABSTOP, nextId);
		back = control(L"BUTTON", labels.back.c_str(), WS_TABSTOP, backId);
		setFont();
		updateControls();
		layout();
	}

	void drawImage(HDC dc, const texturePreview::Image& image, RECT area)
	{
		// Composite alpha onto a checkerboard before handing BGR pixels to GDI.
		std::vector<u32> pixels(image.pixels.size());
		for (int y = 0; y < image.previewHeight; y++)
			for (int x = 0; x < image.previewWidth; x++)
			{
				u32 color = image.pixels[y * image.previewWidth + x];
				u32 alpha = color >> 24;
				u32 bg = ((x / 8 + y / 8) & 1) ? 160 : 210;
				auto blend = [&](u32 c) { return (c * alpha + bg * (255 - alpha) + 127) / 255; };
				pixels[y * image.previewWidth + x] = blend(color & 255) << 16
						| blend((color >> 8) & 255) << 8 | blend((color >> 16) & 255);
			}
		float scale = std::min(float(area.right - area.left) / image.previewWidth,
				float(area.bottom - area.top) / image.previewHeight);
		int w = std::max(1, (int)(image.previewWidth * scale));
		int h = std::max(1, (int)(image.previewHeight * scale));
		BITMAPINFO bitmap{};
		bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bitmap.bmiHeader.biWidth = image.previewWidth;
		bitmap.bmiHeader.biHeight = -image.previewHeight;
		bitmap.bmiHeader.biPlanes = 1;
		bitmap.bmiHeader.biBitCount = 32;
		bitmap.bmiHeader.biCompression = BI_RGB;
		SetStretchBltMode(dc, COLORONCOLOR);
		StretchDIBits(dc, area.left + (area.right - area.left - w) / 2,
				area.top + (area.bottom - area.top - h) / 2, w, h,
				0, 0, image.previewWidth, image.previewHeight, pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
	}

	void paint()
	{
		PAINTSTRUCT paint;
		HDC target = BeginPaint(window, &paint);
		if (width > 0 && height > 0)
		{
			HDC dc = CreateCompatibleDC(target);
			HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			HGDIOBJ oldFont = SelectObject(dc, font);
			RECT all{0, 0, width, height};
			FillRect(dc, &all, GetSysColorBrush(COLOR_BTNFACE));
			RECT meter{px(12), meterTop, width - px(12), meterTop + px(8)};
			FillRect(dc, &meter, GetSysColorBrush(COLOR_3DSHADOW));
			meter.right = meter.left + (LONG)((meter.right - meter.left) * vramFraction);
			FillRect(dc, &meter, GetSysColorBrush(COLOR_HIGHLIGHT));
			SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
			SetBkMode(dc, TRANSPARENT);
			IntersectClipRect(dc, 0, top(), width, height);
			if (selected)
			{
				RECT text{px(12), top() + px(8), width - px(12), top() + px(52)};
				std::wstring label = L"VRAM: 0x" + description(*selected);
				DrawTextW(dc, label.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX);
				RECT area{px(12), top() + px(60), width - px(12), height - px(12)};
				if (area.bottom > area.top)
					drawImage(dc, *selected, area);
			}
			else if (images.empty())
			{
				RECT text{px(12), top() + px(12), width - px(12), height - px(12)};
				DrawTextW(dc, labels.empty.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX);
			}
			else
			{
				int cellWidth = (width - px(16)) / columns();
				for (int i = 0; i < visibleCount(); i++)
				{
					const auto& image = *images[page * pageSize + i];
					int x = px(8) + (i % columns()) * cellWidth;
					int y = top() + px(8) + (i / columns()) * px(168) - scroll;
					if (y + px(168) < top() || y > height)
						continue;
					RECT area{x, y, x + cellWidth - px(12), y + px(120)};
					drawImage(dc, image, area);
					wchar_t label[128];
					swprintf(label, std::size(label), L"%06X\n%dx%d %hs", image.address, image.width, image.height, image.format);
					RECT text{x, y + px(124), x + cellWidth - px(8), y + px(164)};
					DrawTextW(dc, label, -1, &text, DT_CENTER | DT_NOPREFIX);
				}
			}
			BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
			SelectObject(dc, oldFont);
			SelectObject(dc, oldBitmap);
			DeleteObject(bitmap);
			DeleteDC(dc);
		}
		EndPaint(window, &paint);
	}

	void scrollTo(int position)
	{
		scroll = position;
		updateScroll();
		invalidate();
	}
};

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
	View *view = (View *)GetWindowLongPtrW(window, GWLP_USERDATA);
	if (message == WM_NCCREATE)
	{
		view = (View *)((CREATESTRUCTW *)lParam)->lpCreateParams;
		view->window = window;
		SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)view);
	}
	if (view == nullptr)
		return DefWindowProcW(window, message, wParam, lParam);
	switch (message)
	{
	case WM_CREATE:
	{
		using GetDpi = UINT(WINAPI *)(HWND);
		auto getDpi = (GetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
		if (getDpi)
			view->dpi = getDpi(window);
		view->createControls();
		view->refresh();
		SetTimer(window, 1, 100, nullptr);
		return 0;
	}
	case WM_TIMER:
		view->refresh();
		return 0;
	case WM_SIZE:
		view->layout();
		return 0;
	case WM_DPICHANGED:
	{
		view->dpi = HIWORD(wParam);
		view->setFont();
		const RECT& bounds = *(RECT *)lParam;
		SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
				bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
		view->layout();
		return 0;
	}
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lParam)->ptMinTrackSize = {view->px(360), view->px(440)};
		return 0;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case filterId:
			if (HIWORD(wParam) == EN_CHANGE)
			{
				wchar_t text[129];
				GetWindowTextW(view->filterEdit, text, (int)std::size(text));
				view->filter = text;
				std::transform(view->filter.begin(), view->filter.end(), view->filter.begin(), towlower);
				view->page = view->scroll = 0;
				view->selected.reset();
				view->refresh();
				view->updateControls();
			}
			break;
		case previousId:
			view->page = std::max(0, view->page - 1);
			view->scroll = 0;
			view->updateControls();
			break;
		case nextId:
			view->page = std::min(view->pages() - 1, view->page + 1);
			view->scroll = 0;
			view->updateControls();
			break;
		case backId:
			view->selected.reset();
			view->updateControls();
			break;
		}
		return 0;
	case WM_LBUTTONUP:
	{
		int x = GET_X_LPARAM(lParam) - view->px(8);
		int y = GET_Y_LPARAM(lParam) - view->top() - view->px(8) + view->scroll;
		if (!view->selected && x >= 0 && y >= 0 && GET_Y_LPARAM(lParam) >= view->top())
		{
			int column = x / ((view->width - view->px(16)) / view->columns());
			int index = (y / view->px(168)) * view->columns() + column;
			if (column < view->columns() && index < view->visibleCount())
			{
				view->selected = view->images[view->page * pageSize + index];
				view->updateControls();
			}
		}
		return 0;
	}
	case WM_VSCROLL:
	{
		SCROLLINFO info{sizeof(info), SIF_ALL};
		GetScrollInfo(window, SB_VERT, &info);
		int position = info.nPos;
		switch (LOWORD(wParam))
		{
		case SB_LINEUP: position -= view->px(40); break;
		case SB_LINEDOWN: position += view->px(40); break;
		case SB_PAGEUP: position -= info.nPage; break;
		case SB_PAGEDOWN: position += info.nPage; break;
		case SB_THUMBTRACK: position = info.nTrackPos; break;
		case SB_TOP: position = 0; break;
		case SB_BOTTOM: position = info.nMax; break;
		}
		view->scrollTo(position);
		return 0;
	}
	case WM_MOUSEWHEEL:
		view->wheel += GET_WHEEL_DELTA_WPARAM(wParam);
		view->scrollTo(view->scroll - (view->wheel / WHEEL_DELTA) * view->px(80));
		view->wheel %= WHEEL_DELTA;
		return 0;
	case WM_PAINT:
		view->paint();
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_CLOSE:
		closed = true;
		DestroyWindow(window);
		return 0;
	case stopMessage:
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		KillTimer(window, 1);
		PostQuitMessage(0); // This thread's queue only; never SDL's game window.
		return 0;
	case WM_NCDESTROY:
		viewerWindow = nullptr;
		SetWindowLongPtrW(window, GWLP_USERDATA, 0);
		break;
	}
	return DefWindowProcW(window, message, wParam, lParam);
}

void run(Labels labels)
{
	struct Completion
	{
		~Completion() { finished = true; }
	} completion;
	WNDCLASSW wc{};
	wc.lpfnWndProc = windowProc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
	wc.lpszClassName = windowClass;
	if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		ERROR_LOG(COMMON, "Cannot register texture viewer window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
			CW_USEDEFAULT, CW_USEDEFAULT, 640, 720, nullptr, nullptr, wc.hInstance, &view);
	viewerWindow = window;
	if (window)
	{
		if (!stopping)
			ShowWindow(window, SW_SHOWNOACTIVATE);
		else
			PostMessageW(window, stopMessage, 0, 0);
		MSG message;
		while (GetMessageW(&message, nullptr, 0, 0) > 0)
			if (!IsDialogMessageW(window, &message))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
	}
	else
	{
		ERROR_LOG(COMMON, "Cannot create texture viewer window: %lu", GetLastError());
		closed = true;
	}
	if (view.font)
		DeleteObject(view.font);
	UnregisterClassW(windowClass, wc.hInstance);
}

// The destructor also covers shutdown paths that bypass gui_term().
struct Worker
{
	std::thread thread;
	~Worker() { term(); }
} worker;
}

bool update(bool enabled)
{
	if (!enabled || closed)
	{
		term();
		return !closed.exchange(false);
	}
	if (!worker.thread.joinable())
	{
		stopping = false;
		finished = false;
		using namespace i18n;
		Labels labels{L"Flycast - " + nowide::widen(T("Texture Viewer")),
				nowide::widen(T("Loaded textures: %d (up to %d)")), nowide::widen(T("Filter")),
				nowide::widen(T("No textures to display. Resume the game to load textures, or clear the filter.")),
				nowide::widen(T("Custom")), nowide::widen(T("Back")),
				nowide::widen(T("Texture VRAM (estimated): %.2f / %.2f MiB (%.1f%%)")),
				nowide::widen(T("Peak since opening: %.2f MiB | Remaining (upper bound): %.2f MiB")),
				nowide::widen(T("Framebuffers and other allocations are not counted. Remaining is not actual free VRAM."))};
		worker.thread = std::thread(run, std::move(labels));
	}
	return true;
}

void term()
{
	stopping = true;
	if (HWND window = viewerWindow.load())
		PostMessageW(window, stopMessage, 0, 0);
	if (worker.thread.joinable())
	{
		// Window destruction can synchronously send focus messages to the game
		// thread. Service them before joining to avoid a shutdown deadlock.
		while (!finished)
		{
			MSG message;
			PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
			MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_SENDMESSAGE);
		}
		worker.thread.join();
	}
}

bool hasFocus()
{
	HWND window = viewerWindow.load();
	return window != nullptr && GetForegroundWindow() == window;
}
}
