#include "polygon_viewer_window.h"
#include "rend/polygon_preview.h"
#include "oslib/i18n.h"
#include <nowide/convert.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace polygonViewerWindow
{
namespace
{
constexpr wchar_t windowClass[] = L"FlycastPolygonViewer";
constexpr UINT stopMessage = WM_APP + 1;
std::atomic<HWND> viewerWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true};

struct Labels
{
	std::wstring title, count, note, empty, limit;
};

struct View
{
	Labels labels;
	HWND window{};
	HFONT font{};
	int dpi = 96;
	std::shared_ptr<const polygonPreview::Frame> frame;

	int px(int value) const { return MulDiv(value, dpi, 96); }
	void invalidate() { InvalidateRect(window, nullptr, FALSE); }

	void setFont()
	{
		if (font)
			DeleteObject(font);
		font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
				DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
				DEFAULT_PITCH, L"Segoe UI");
	}

	void refresh()
	{
		auto next = polygonPreview::snapshot();
		if (next != frame)
		{
			frame = std::move(next);
			invalidate();
		}
	}

	void paint()
	{
		PAINTSTRUCT paint;
		HDC target = BeginPaint(window, &paint);
		RECT client;
		GetClientRect(window, &client);
		int width = client.right, height = client.bottom;
		if (width > 0 && height > 0)
		{
			HDC dc = CreateCompatibleDC(target);
			HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			HGDIOBJ oldFont = SelectObject(dc, font);
			FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, RGB(220, 230, 220));
			int top = px(12);
			auto text = [&](const std::wstring& label) {
				RECT bounds{px(12), top, std::max<LONG>(px(13), width - px(12)), top};
				DrawTextW(dc, label.c_str(), -1, &bounds, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
				DrawTextW(dc, label.c_str(), -1, &bounds, DT_WORDBREAK | DT_NOPREFIX);
				top = bounds.bottom + px(6);
			};
			wchar_t count[128];
			swprintf(count, std::size(count), labels.count.c_str(), frame ? frame->triangles : 0u);
			text(count);
			text(labels.note);
			if (frame && frame->limited)
				text(labels.limit);
			if (!frame || frame->edges.empty())
				text(labels.empty);
			else if (width > px(24) && height > top + px(12))
			{
				const float availableWidth = float(width - px(24));
				const float availableHeight = float(height - top - px(12));
				const float w = std::min(availableWidth, availableHeight * frame->aspectRatio);
				const float h = w / frame->aspectRatio;
				const float x = (width - w) * 0.5f;
				const float y = top + (availableHeight - h) * 0.5f;
				HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1)), RGB(0, 255, 80));
				HGDIOBJ oldPen = SelectObject(dc, pen);
				// Batch independent line segments; don't connect unrelated polygons.
				std::array<POINT, 4096> points;
				std::array<DWORD, 2048> counts;
				counts.fill(2);
				for (size_t offset = 0; offset < frame->edges.size(); offset += counts.size())
				{
					size_t size = std::min(counts.size(), frame->edges.size() - offset);
					for (size_t i = 0; i < size; i++)
					{
						const auto& edge = frame->edges[offset + i];
						points[i * 2] = {LONG(x + edge.a.x * w), LONG(y + edge.a.y * h)};
						points[i * 2 + 1] = {LONG(x + edge.b.x * w), LONG(y + edge.b.y * h)};
					}
					PolyPolyline(dc, points.data(), counts.data(), (DWORD)size);
				}
				SelectObject(dc, oldPen);
				DeleteObject(pen);
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
		if (!IsIconic(window))
			view->refresh();
		return 0;
	case WM_SIZE:
		view->invalidate();
		return 0;
	case WM_DPICHANGED:
	{
		view->dpi = HIWORD(wParam);
		view->setFont();
		const RECT& bounds = *(RECT *)lParam;
		SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
				bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
		view->invalidate();
		return 0;
	}
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)lParam)->ptMinTrackSize = {view->px(360), view->px(300)};
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		view->paint();
		return 0;
	case WM_CLOSE:
		closed = true;
		DestroyWindow(window);
		return 0;
	case stopMessage:
		DestroyWindow(window);
		return 0;
	case WM_DESTROY:
		KillTimer(window, 1);
		PostQuitMessage(0); // Only the viewer thread exits.
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
	wc.lpszClassName = windowClass;
	if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
	{
		ERROR_LOG(COMMON, "Cannot register polygon viewer window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 840, 700,
			nullptr, nullptr, wc.hInstance, &view);
	viewerWindow = window;
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
		ERROR_LOG(COMMON, "Cannot create polygon viewer window: %lu", GetLastError());
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
		using namespace i18n;
		Labels labels{L"Flycast - " + nowide::widen(T("Polygon Viewer")),
				nowide::widen(T("Polygons (triangles): %u")),
				nowide::widen(T("Green wireframe includes hidden edges and sprites. Background and shadow volumes are excluded.")),
				nowide::widen(T("No polygon edges to display. Resume the game to update the view.")),
				nowide::widen(T("Display limit reached; showing the first 100,000 triangles."))};
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
