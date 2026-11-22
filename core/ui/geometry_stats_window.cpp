#include "geometry_stats_window.h"
#include "hw/pvr/Renderer_if.h"
#include "oslib/i18n.h"
#include <nowide/convert.hpp>
#include <array>
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

namespace geometryStatsWindow
{
namespace
{
constexpr wchar_t windowClass[] = L"FlycastGeometryStats";
constexpr UINT stopMessage = WM_APP + 1;
std::atomic<HWND> statsWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true};

struct Labels
{
	std::wstring title, waiting, vertices, polygons, bufferTitle, unavailable;
	std::wstring buffer, used, remaining, full, estimateNote, scopeNote, passes;
	std::array<std::wstring, 3> columns;
	std::array<std::wstring, 4> lists;
};

template<typename... Args>
std::wstring format(const std::wstring& pattern, Args... args)
{
	wchar_t text[512]{};
	swprintf(text, std::size(text), pattern.c_str(), args...);
	return text;
}

enum class RowType { Text, Heading, Table, TableHeader, Meter };
struct Row
{
	std::wstring text, vertices, triangles;
	RowType type = RowType::Text;
	double fraction = 0;
};

// The native window owns its message loop and all GDI resources. Only a
// mutex-protected copy of the latest render statistics crosses threads.
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
		const auto stats = rend_get_geometry_stats();
		rows.clear();
		auto text = [&](std::wstring value, RowType type = RowType::Text) {
			rows.push_back({std::move(value), {}, {}, type});
		};
		if (!stats.valid)
			text(labels.waiting);
		else
		{
			const auto total = stats.total();
			text(format(labels.vertices, total.vertices), RowType::Heading);
			text(format(labels.polygons, total.triangles), RowType::Heading);
			text(labels.bufferTitle, RowType::Heading);
			if (!stats.taBuffers.complete || stats.taBuffers.buffers.empty())
				text(labels.unavailable);
			else
				for (size_t i = 0; i < stats.taBuffers.buffers.size(); i++)
				{
					const auto& buffer = stats.taBuffers.buffers[i];
					if (stats.taBuffers.buffers.size() > 1)
						text(format(labels.buffer, (unsigned)i + 1), RowType::Heading);
					text(format(labels.used, buffer.usedBytes / 1024.0, buffer.allocation.capacityBytes / 1024.0));
					text(format(labels.remaining, buffer.remainingBytes() / 1024.0));
					rows.push_back({format(L"%.1f%%", buffer.fraction() * 100.0), {}, {}, RowType::Meter, buffer.fraction()});
					if (buffer.fraction() >= 1.0)
						text(labels.full);
				}
			rows.push_back({labels.columns[0], labels.columns[1], labels.columns[2], RowType::TableHeader});
			const GeometryCounts counts[] = {stats.opaque, stats.punchThrough, stats.translucent, stats.modifierVolumes};
			for (size_t i = 0; i < std::size(counts); i++)
				rows.push_back({labels.lists[i], std::to_wstring(counts[i].vertices),
						std::to_wstring(counts[i].triangles), RowType::Table});
			text(format(labels.passes, stats.renderPasses));
			text(labels.scopeNote);
			text(labels.estimateNote);
		}
		updateScroll();
		InvalidateRect(window, nullptr, FALSE);
	}

	// Measure and draw using the same layout so wrapped translations, small
	// windows and renders with multiple TA buffers remain fully scrollable.
	int layout(HDC dc, bool draw)
	{
		const int padding = px(16), right = std::max(padding + 1, width - padding);
		const int column1 = padding + (right - padding) * 52 / 100;
		const int column2 = padding + (right - padding) * 76 / 100;
		int y = padding;
		HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		for (const Row& row : rows)
		{
			const bool table = row.type == RowType::Table || row.type == RowType::TableHeader;
			const bool heading = row.type == RowType::Heading || row.type == RowType::TableHeader;
			SelectObject(dc, heading ? boldFont : font);
			if (heading && y != padding)
				y += px(8);
			RECT measure{padding, 0, table ? column1 - px(8) : right, 0};
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
				else if (table)
				{
					if (heading)
						FillRect(dc, &rect, GetSysColorBrush(COLOR_BTNFACE));
					RECT cell = rect;
					cell.right = column1 - px(8);
					DrawTextW(dc, row.text.c_str(), -1, &cell, DT_WORDBREAK | DT_NOPREFIX);
					cell.left = column1;
					cell.right = column2 - px(8);
					DrawTextW(dc, row.vertices.c_str(), -1, &cell, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
					cell.left = column2;
					cell.right = right;
					DrawTextW(dc, row.triangles.c_str(), -1, &cell, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
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
		ERROR_LOG(COMMON, "Cannot register geometry statistics window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 480, 620,
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
		ERROR_LOG(COMMON, "Cannot create geometry statistics window: %lu", GetLastError());
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

bool update(bool enabled)
{
	const bool userClosed = closed.exchange(false);
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
		Labels labels{L"Flycast - " + label("PowerVR2 Geometry Statistics"),
				label("Waiting for a screen render..."), label("Vertices: %u"), label("Polygons (triangles): %u"),
				label("TA vertex buffer (estimated)"), label("Buffer usage unavailable for this render."),
				label("Buffer %u"), label("Used: %.2f / %.2f KiB"), label("Remaining: %.2f KiB"),
				label("Estimated usage has reached the configured capacity."),
				label("Capacity comes from the game's TA settings. Usage and remaining space are estimates before tile clipping; OPBs and CPU-written data are not included. Actual hardware usage may differ."),
				label("Latest screen render, before culling. Background and render-to-texture excluded."),
				label("Render passes: %u"),
				{label("List"), label("Vertices"), label("Triangles")},
				{label("Opaque"), label("Punch-through"), label("Translucent"), label("Modifier volumes")}};
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
