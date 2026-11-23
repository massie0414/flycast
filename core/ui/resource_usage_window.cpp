#include "resource_usage_window.h"
#include "profiler/resource_usage.h"
#include "oslib/i18n.h"
#include <nowide/convert.hpp>
#include "types.h"
#include <algorithm>
#include <iterator>
#include <vector>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace resourceUsageWindow
{
namespace
{
constexpr wchar_t windowClass[] = L"FlycastResourceUsage";
constexpr UINT stopMessage = WM_APP + 1;
std::atomic<HWND> statsWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true}, paused{};

struct Labels
{
	std::wstring title, waiting, cpu, main, sound, memory, cpuNote, memoryNote, paused;
};

template<typename... Args>
std::wstring format(const std::wstring& pattern, Args... args)
{
	wchar_t text[512]{};
	swprintf(text, std::size(text), pattern.c_str(), args...);
	return text;
}

enum class RowType { Text, Heading, Meter };
struct Row
{
	std::wstring text;
	RowType type = RowType::Text;
	double fraction = 0;
};

// The native window owns its message loop and all GDI resources. Only a
// mutex-protected copy of the latest usage statistics crosses threads.
struct View
{
	HWND window{};
	HFONT font{}, boldFont{};
	int dpi = 96, width = 0, height = 0, scroll = 0, wheel = 0;
	Labels labels;
	std::vector<Row> rows;

	int px(int value) const { return MulDiv(value, dpi, 96); }

	void setFont()
	{
		if (font)
			DeleteObject(font);
		if (boldFont)
			DeleteObject(boldFont);
		auto create = [&](int weight) {
			return CreateFontW(-px(16), 0, 0, 0, weight, FALSE, FALSE, FALSE,
					DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
					DEFAULT_PITCH, L"Segoe UI");
		};
		font = create(FW_NORMAL);
		boldFont = create(FW_SEMIBOLD);
	}

	void refresh()
	{
		const auto stats = resourceUsage::snapshot();
		rows.clear();
		auto text = [&](std::wstring value, RowType type = RowType::Text) {
			rows.push_back({std::move(value), type});
		};
		auto meter = [&](double fraction) {
			rows.push_back({format(L"%.1f%%", fraction * 100.0), RowType::Meter, fraction});
		};
		if (paused)
			text(labels.paused);
		if (!stats.valid)
			text(labels.waiting);
		else
		{
			text(labels.cpu, RowType::Heading);
			meter(stats.cpu);
			text(labels.cpuNote);
			auto memory = [&](const std::wstring& title, const resourceUsage::Memory& memory) {
				text(title, RowType::Heading);
				meter(memory.fraction());
				text(format(labels.memory, memory.occupied / 1048576.0, memory.capacity / 1048576.0));
			};
			memory(labels.main, stats.main);
			memory(labels.sound, stats.sound);
			text(labels.memoryNote);
		}
		updateScroll();
		InvalidateRect(window, nullptr, FALSE);
	}

	// Measure and draw using the same layout so wrapped translations in small
	// windows remain fully scrollable.
	int layout(HDC dc, bool draw)
	{
		const int padding = px(16), right = std::max(padding + 1, width - padding);
		int y = padding;
		HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		for (const Row& row : rows)
		{
			const bool heading = row.type == RowType::Heading;
			SelectObject(dc, heading ? boldFont : font);
			if (heading && y != padding)
				y += px(8);
			RECT measure{padding, 0, right, 0};
			DrawTextW(dc, row.text.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
			const int rowHeight = row.type == RowType::Meter ? px(30) : std::max(px(24), (int)measure.bottom + px(6));
			if (draw)
			{
				RECT rect{padding, y - scroll, right, y + rowHeight - scroll};
				if (row.type == RowType::Meter)
				{
					rect.bottom -= px(8);
					FillRect(dc, &rect, GetSysColorBrush(COLOR_BTNFACE));
					RECT fill = rect;
					fill.right = fill.left + (int)((fill.right - fill.left) * std::clamp(row.fraction, 0.0, 1.0));
					const COLORREF color = row.fraction >= 1.0 ? RGB(230, 105, 95)
							: row.fraction >= 0.8 ? RGB(245, 195, 65) : RGB(110, 195, 140);
					HBRUSH brush = CreateSolidBrush(color);
					FillRect(dc, &fill, brush);
					DeleteObject(brush);
					DrawTextW(dc, row.text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
				}
				else
					DrawTextW(dc, row.text.c_str(), -1, &rect, DT_WORDBREAK | DT_NOPREFIX);
			}
			y += rowHeight;
		}
		SelectObject(dc, oldFont);
		return y + padding;
	}

	void updateScroll()
	{
		HDC dc = GetDC(window);
		SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
		info.nMax = layout(dc, false) - 1;
		info.nPage = std::max(1, height);
		info.nPos = scroll;
		ReleaseDC(window, dc);
		SetScrollInfo(window, SB_VERT, &info, TRUE);
		scroll = GetScrollPos(window, SB_VERT);
	}

	void scrollTo(int position)
	{
		SetScrollPos(window, SB_VERT, position, TRUE);
		scroll = GetScrollPos(window, SB_VERT);
		InvalidateRect(window, nullptr, FALSE);
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
			RECT client{0, 0, width, height};
			FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
			layout(dc, true);
			BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
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
		SetTimer(window, 1, 100, nullptr);
		return 0;
	}
	case WM_TIMER:
		if (!IsIconic(window))
			view->refresh();
		return 0;
	case WM_SIZE:
	{
		RECT client;
		GetClientRect(window, &client);
		view->width = client.right;
		view->height = client.bottom;
		view->updateScroll();
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}
	case WM_DPICHANGED:
	{
		view->dpi = HIWORD(wParam);
		view->setFont();
		const RECT& bounds = *(RECT *)lParam;
		SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
				bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
		view->updateScroll();
		InvalidateRect(window, nullptr, FALSE);
		return 0;
	}
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lParam)->ptMinTrackSize = {view->px(380), view->px(220)};
		return 0;
	case WM_VSCROLL:
	{
		SCROLLINFO info{sizeof(info), SIF_ALL};
		GetScrollInfo(window, SB_VERT, &info);
		int position = info.nPos;
		switch (LOWORD(wParam))
		{
		case SB_LINEUP: position -= view->px(32); break;
		case SB_LINEDOWN: position += view->px(32); break;
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
		PostQuitMessage(0); // Only the statistics thread; keep the game running.
		return 0;
	case WM_NCDESTROY:
		statsWindow = nullptr;
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
		ERROR_LOG(COMMON, "Cannot register resource usage window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 540, 560,
			nullptr, nullptr, wc.hInstance, &view);
	statsWindow = window;
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
		ERROR_LOG(COMMON, "Cannot create resource usage window: %lu", GetLastError());
		closed = true;
	}
	if (view.font)
		DeleteObject(view.font);
	if (view.boldFont)
		DeleteObject(view.boldFont);
	UnregisterClassW(windowClass, wc.hInstance);
}

struct Worker
{
	std::thread thread;
	~Worker() { term(); }
} worker;
}

bool update(bool enabled, bool running)
{
	const bool userClosed = closed.exchange(false);
	paused = !running;
	resourceUsage::setEnabled(enabled && !userClosed);
	if (!enabled || userClosed)
	{
		stopping = true;
		if (HWND window = statsWindow.load())
			PostMessageW(window, stopMessage, 0, 0);
		if (worker.thread.joinable() && finished)
			worker.thread.join();
		return !userClosed;
	}
	// DestroyWindow can send focus messages synchronously to the game thread.
	// Keep processing SDL events until the native worker has actually exited.
	if (worker.thread.joinable() && finished)
		worker.thread.join();
	if (!worker.thread.joinable())
	{
		stopping = false;
		finished = false;
		// Resolve translations on the main thread, as in the other native viewers.
		auto label = [](const char *text) { return nowide::widen(i18n::T(text)); };
		Labels labels{L"Flycast - " + label("CPU / Memory Usage"),
				label("Waiting for usage data. Resume the game to measure."),
				label("CPU usage (SH-4, estimated)"),
				label("Main memory usage (estimated)"), label("Sound memory usage (AICA, estimated)"),
				label("Occupied: %.2f / %.2f MiB"),
				label("Emulated CPU time outside SLEEP. Busy-wait loops count as active. Updated every 0.5 seconds of emulated time."),
				label("Memory estimates count 4 KiB blocks containing nonzero data. Zero-filled allocations and freed data cannot be distinguished; this is not the game's allocated memory or free space."),
				label("Paused - showing the last sample.")};
		worker.thread = std::thread(run, std::move(labels));
	}
	return true;
}

void term()
{
	stopping = true;
	if (HWND window = statsWindow.load())
		PostMessageW(window, stopMessage, 0, 0);
	if (worker.thread.joinable())
	{
		// During application shutdown, service cross-thread SendMessage calls
		// without consuming SDL's queued messages while waiting for destruction.
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
	HWND window = statsWindow.load();
	return window != nullptr && GetForegroundWindow() == window;
}
}
