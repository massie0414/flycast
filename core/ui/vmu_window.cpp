#include "vmu_window.h"
#include "rend/osd.h"
#include "oslib/i18n.h"
#include <nowide/convert.hpp>
#include <algorithm>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace vmuWindow
{
namespace
{
constexpr wchar_t windowClass[] = L"FlycastVmuWindow";
constexpr UINT stopMessage = WM_APP + 1;
std::atomic<HWND> lcdWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true};

struct Labels
{
	std::wstring title, empty;
};

// Window state and GDI objects belong exclusively to this thread. The LCD
// snapshot is copied under a lock, never read directly from the emulator.
struct View
{
	HWND window{};
	HFONT font{};
	int dpi = 96;
	Labels labels;
	std::array<VmuScreen, 8> screens{};

	int px(int value) const { return MulDiv(value, dpi, 96); }

	void setFont()
	{
		if (font)
			DeleteObject(font);
		font = CreateFontW(-px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				DEFAULT_PITCH, L"Segoe UI");
	}

	void refresh()
	{
		auto next = snapshotVmuScreens();
		bool changed = false;
		for (size_t i = 0; i < screens.size(); i++)
			changed |= next[i].active != screens[i].active
					|| (next[i].active && next[i].pixels != screens[i].pixels);
		if (changed)
		{
			screens = std::move(next);
			InvalidateRect(window, nullptr, FALSE);
		}
	}

	void paint()
	{
		PAINTSTRUCT paint;
		HDC target = BeginPaint(window, &paint);
		RECT client;
		GetClientRect(window, &client);
		const int width = client.right, height = client.bottom;
		if (width > 0 && height > 0)
		{
			HDC dc = CreateCompatibleDC(target);
			HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			HGDIOBJ oldFont = SelectObject(dc, font);
			FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
			SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
			SetBkMode(dc, TRANSPARENT);
			int count = 0;
			for (const auto& screen : screens)
				count += screen.active;
			const int padding = px(12), labelHeight = px(24);
			if (count == 0)
			{
				RECT text{padding, padding, width - padding, height - padding};
				DrawTextW(dc, labels.empty.c_str(), -1, &text, DT_WORDBREAK | DT_NOPREFIX);
			}
			else
			{
				// Choose a grid that gives the LCDs the largest integer scale at
				// this window size. Preserve the original 48:32 pixel aspect ratio.
				int columns = 1;
				float bestScale = 0;
				for (int c = 1; c <= count; c++)
				{
					int rows = (count + c - 1) / c;
					float candidate = std::min((width / c - 2 * padding) / 48.f,
							(height / rows - 2 * padding - labelHeight) / 32.f);
					if (candidate > bestScale)
					{
						bestScale = candidate;
						columns = c;
					}
				}
				const int scale = std::max(1, (int)bestScale);
				const int rows = (count + columns - 1) / columns;
				const int cellWidth = width / columns, cellHeight = height / rows;
				BITMAPINFO info{};
				info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				info.bmiHeader.biWidth = 48;
				// The emulator stores LCD rows bottom-up, matching a positive DIB height.
				info.bmiHeader.biHeight = 32;
				info.bmiHeader.biPlanes = 1;
				info.bmiHeader.biBitCount = 32;
				info.bmiHeader.biCompression = BI_RGB;
				SetStretchBltMode(dc, COLORONCOLOR);
				int position = 0;
				for (size_t i = 0; i < screens.size(); i++)
				{
					if (!screens[i].active)
						continue;
					int x = position % columns * cellWidth + (cellWidth - 48 * scale) / 2;
					int y = position / columns * cellHeight + (cellHeight - 32 * scale - labelHeight) / 2;
					wchar_t label[32];
					swprintf(label, std::size(label), L"VMU %lc%u", L'A' + (int)i / 2, (unsigned)i % 2 + 1);
					RECT text{x, y, x + 48 * scale, y + labelHeight};
					DrawTextW(dc, label, -1, &text, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
					StretchDIBits(dc, x, y + labelHeight, 48 * scale, 32 * scale,
							0, 0, 48, 32, screens[i].pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
					position++;
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
		view->setFont();
		view->refresh();
		SetTimer(window, 1, 33, nullptr);
		return 0;
	}
	case WM_TIMER:
		view->refresh();
		return 0;
	case WM_SIZE:
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	case WM_DPICHANGED:
	{
		view->dpi = HIWORD(wParam);
		view->setFont();
		const RECT& bounds = *(RECT *)lParam;
		SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
				bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lParam)->ptMinTrackSize = {view->px(240), view->px(200)};
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
		PostQuitMessage(0); // Only the VMU thread; keep the game running.
		return 0;
	case WM_NCDESTROY:
		lcdWindow = nullptr;
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
	wc.lpszClassName = windowClass;
	if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		ERROR_LOG(COMMON, "Cannot register VMU window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 660, 360,
			nullptr, nullptr, wc.hInstance, &view);
	lcdWindow = window;
	if (window)
	{
		if (!stopping)
			ShowWindow(window, SW_SHOWNOACTIVATE);
		else
			PostMessageW(window, stopMessage, 0, 0);
		MSG message;
		while (GetMessageW(&message, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	else
	{
		ERROR_LOG(COMMON, "Cannot create VMU window: %lu", GetLastError());
		closed = true;
	}
	if (view.font)
		DeleteObject(view.font);
	UnregisterClassW(windowClass, wc.hInstance);
}

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
		Labels labels{L"Flycast - " + nowide::widen(i18n::T("VMU Window")),
				nowide::widen(i18n::T("Waiting for VMU screen data. Connect a VMU in Settings > Controls and resume the game."))};
		worker.thread = std::thread(run, std::move(labels));
	}
	return true;
}

void term()
{
	stopping = true;
	if (HWND window = lcdWindow.load())
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
	HWND window = lcdWindow.load();
	return window != nullptr && GetForegroundWindow() == window;
}
}
