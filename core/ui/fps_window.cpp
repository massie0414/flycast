#include "fps_window.h"
#include "types.h"
#include "oslib/i18n.h"
#include <nowide/convert.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace fpsWindow
{
namespace
{
constexpr wchar_t windowClass[] = L"FlycastFPS";
constexpr UINT stopMessage = WM_APP + 1;
std::atomic<HWND> fpsWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true};

// Count only successful game renders, never menu refreshes. Sampling on the
// window thread also reports zero FPS if the game stops producing frames.
using Clock = std::chrono::steady_clock;
std::mutex sampleMutex;
bool active{}, fastForward{};
u64 frames{};
double fps = -1;
Clock::time_point sampleStart;

struct Labels
{
	std::wstring title, measuring, paused, fastForward;
};

struct View
{
	HWND window{};
	HFONT font{}, valueFont{};
	int dpi = 96;
	Labels labels;
	std::wstring value, status;

	int px(int value) const { return MulDiv(value, dpi, 96); }

	void setFont()
	{
		if (font)
			DeleteObject(font);
		if (valueFont)
			DeleteObject(valueFont);
		auto create = [&](int size, int weight) {
			return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
					DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
					DEFAULT_PITCH, L"Segoe UI");
		};
		font = create(16, FW_NORMAL);
		valueFont = create(42, FW_SEMIBOLD);
	}

	void refresh()
	{
		double rate;
		bool running, forwarding;
		{
			std::lock_guard<std::mutex> lock(sampleMutex);
			const auto now = Clock::now();
			const double seconds = std::chrono::duration<double>(now - sampleStart).count();
			if (active && seconds >= 1.0)
			{
				fps = frames / seconds;
				frames = 0;
				sampleStart = now;
			}
			rate = fps;
			running = active;
			forwarding = fastForward;
		}
		std::wstring nextValue = L"-- FPS";
		if (running && rate >= 0)
		{
			wchar_t text[64];
			swprintf(text, std::size(text), L"%.1f FPS", rate);
			nextValue = text;
		}
		std::wstring nextStatus = !running ? labels.paused : rate < 0 ? labels.measuring
				: forwarding ? labels.fastForward : L"";
		if (value != nextValue || status != nextStatus)
		{
			value = std::move(nextValue);
			status = std::move(nextStatus);
			// Expose the current reading in the taskbar and window switcher too.
			SetWindowTextW(window, (labels.title + L" - " + value
					+ (status.empty() ? L"" : L" - " + status)).c_str());
			InvalidateRect(window, nullptr, FALSE);
		}
	}

	void paint()
	{
		PAINTSTRUCT paint;
		HDC target = BeginPaint(window, &paint);
		RECT client;
		GetClientRect(window, &client);
		if (client.right > 0 && client.bottom > 0)
		{
			HDC dc = CreateCompatibleDC(target);
			HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
			HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
			HGDIOBJ oldFont = SelectObject(dc, valueFont);
			FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
			RECT line{px(12), 0, client.right - px(12), client.bottom - px(38)};
			DrawTextW(dc, value.c_str(), -1, &line, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			SelectObject(dc, font);
			line.top = line.bottom;
			line.bottom = client.bottom - px(8);
			DrawTextW(dc, status.c_str(), -1, &line, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
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
		SetTimer(window, 1, 100, nullptr);
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
		((MINMAXINFO *)lParam)->ptMinTrackSize = {view->px(300), view->px(170)};
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
		PostQuitMessage(0); // Only this window's thread, not the game.
		return 0;
	case WM_NCDESTROY:
		fpsWindow = nullptr;
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
		ERROR_LOG(COMMON, "Cannot register FPS window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 340, 190,
			nullptr, nullptr, wc.hInstance, &view);
	fpsWindow = window;
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
		ERROR_LOG(COMMON, "Cannot create FPS window: %lu", GetLastError());
		closed = true;
	}
	if (view.font)
		DeleteObject(view.font);
	if (view.valueFont)
		DeleteObject(view.valueFont);
	UnregisterClassW(windowClass, wc.hInstance);
}

struct Worker
{
	std::thread thread;
	~Worker() { term(); }
} worker;
}

bool update(bool enabled, bool running, bool forwarding)
{
	const bool userClosed = closed.exchange(false);
	{
		std::lock_guard<std::mutex> lock(sampleMutex);
		const bool nextActive = enabled && !userClosed && running;
		if (active != nextActive)
		{
			active = nextActive;
			frames = 0;
			fps = -1;
			sampleStart = Clock::now();
		}
		fastForward = forwarding;
	}
	if (!enabled || userClosed)
	{
		stopping = true;
		if (HWND window = fpsWindow.load())
			PostMessageW(window, stopMessage, 0, 0);
		if (worker.thread.joinable() && finished)
			worker.thread.join();
		return !userClosed;
	}
	// Let SDL keep servicing synchronous focus messages while the window exits.
	if (worker.thread.joinable() && finished)
		worker.thread.join();
	if (!worker.thread.joinable())
	{
		stopping = false;
		finished = false;
		auto label = [](const char *text) { return nowide::widen(i18n::T(text)); };
		Labels labels{L"Flycast - " + label("FPS Window"), label("Measuring frame rate..."),
				label("Game paused"), label("Fast forward")};
		worker.thread = std::thread(run, std::move(labels));
	}
	return true;
}

void frameRendered()
{
	std::lock_guard<std::mutex> lock(sampleMutex);
	if (active)
		frames++;
}

void term()
{
	stopping = true;
	if (HWND window = fpsWindow.load())
		PostMessageW(window, stopMessage, 0, 0);
	if (worker.thread.joinable())
	{
		// Service cross-thread SendMessage calls until destruction completes.
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
	HWND window = fpsWindow.load();
	return window != nullptr && GetForegroundWindow() == window;
}
}
