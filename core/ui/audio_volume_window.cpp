#include "audio_volume_window.h"
#include "types.h"
#include "audio/level_meter.h"
#include <algorithm>
#include <cmath>
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

namespace audioVolumeWindow
{
namespace
{
constexpr wchar_t windowClass[] = L"FlycastAudioVolume";
constexpr UINT stopMessage = WM_APP + 1;
std::atomic<HWND> volumeWindow{};
std::atomic<bool> stopping{}, closed{}, finished{true};
std::atomic<bool> playing{};
std::atomic<int> volumePercent{100};

struct Labels
{
	std::wstring title, volume, live, paused, note;
	std::array<std::wstring, audioMeter::SourceCount> sources;
};

struct View
{
	HWND window{};
	HFONT font{}, boldFont{};
	int dpi = 96, width = 0, height = 0, scroll = 0, wheel = 0;
	Labels labels;
	audioMeter::Levels levels{};
	int volume = 100;
	bool running = false;

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
		levels = audioMeter::consume();
		volume = volumePercent.load();
		running = playing.load();
		if (!running)
			levels = {};
		updateScroll();
		InvalidateRect(window, nullptr, FALSE);
	}

	// Measure and draw the same wrapped layout, including at small sizes/high DPI.
	int layout(HDC dc, bool draw)
	{
		const int padding = px(16), right = std::max(padding + 1, width - padding);
		int y = padding;
		HGDIOBJ oldFont = GetCurrentObject(dc, OBJ_FONT);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
		auto text = [&](const std::wstring& value, bool bold = false) {
			SelectObject(dc, bold ? boldFont : font);
			RECT measure{padding, 0, right, 0};
			DrawTextW(dc, value.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
			const int rowHeight = std::max(px(24), int(measure.bottom) + px(6));
			if (draw)
			{
				RECT line{padding, y - scroll, right, y + rowHeight - scroll};
				DrawTextW(dc, value.c_str(), -1, &line, DT_WORDBREAK | DT_NOPREFIX);
			}
			y += rowHeight;
		};
		text(labels.volume + L": " + std::to_wstring(volume) + L"%", true);
		text(running ? labels.live : labels.paused);
		for (unsigned source = 0; source < audioMeter::SourceCount; source++)
		{
			y += px(10);
			text(labels.sources[source], true);
			SelectObject(dc, font);
			for (unsigned channel = 0; channel < 2; channel++)
			{
				const float peak = levels[source][channel];
				const float db = peak > 0 ? 20.f * std::log10(peak) : -100.f;
				if (draw)
				{
					RECT side{padding, y - scroll, padding + px(20), y + px(24) - scroll};
					DrawTextW(dc, channel == 0 ? L"L" : L"R", -1, &side, DT_SINGLELINE | DT_VCENTER);
					RECT track{side.right + px(4), y + px(4) - scroll, right - px(105), y + px(20) - scroll};
					FillRect(dc, &track, GetSysColorBrush(COLOR_BTNFACE));
					RECT fill = track;
					fill.right = fill.left + int((track.right - track.left) * std::clamp((db + 60.f) / 60.f, 0.f, 1.f));
					const COLORREF color = db >= -0.1f ? RGB(220, 65, 65)
							: db >= -6.f ? RGB(210, 155, 25) : RGB(40, 160, 105);
					HBRUSH brush = CreateSolidBrush(color);
					FillRect(dc, &fill, brush);
					DeleteObject(brush);
					wchar_t value[48];
					if (peak > 0)
						swprintf(value, std::size(value), L"%.1f dBFS", db);
					else
						wcscpy(value, L"-\x221e dBFS");
					RECT reading{track.right + px(6), y - scroll, right, y + px(24) - scroll};
					DrawTextW(dc, value, -1, &reading, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
				}
				y += px(24);
			}
		}
		y += px(16);
		text(labels.note);
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
		SetTimer(window, 1, 50, nullptr);
		return 0;
	}
	case WM_TIMER:
		view->refresh(); // Consume peaks even while minimized; do not retain stale audio.
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
		((MINMAXINFO *)lParam)->ptMinTrackSize = {view->px(420), view->px(220)};
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
		PostQuitMessage(0); // Only the audio panel thread; keep the game running.
		return 0;
	case WM_NCDESTROY:
		volumeWindow = nullptr;
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
		ERROR_LOG(COMMON, "Cannot register audio volume window: %lu", GetLastError());
		closed = true;
		return;
	}
	View view;
	view.labels = std::move(labels);
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, windowClass, view.labels.title.c_str(),
			WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 560, 690,
			nullptr, nullptr, wc.hInstance, &view);
	volumeWindow = window;
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
		ERROR_LOG(COMMON, "Cannot create audio volume window: %lu", GetLastError());
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

bool update(bool enabled, bool running, int volume)
{
	const bool userClosed = closed.exchange(false);
	playing = enabled && !userClosed && running;
	volumePercent = volume;
	audioMeter::setEnabled(playing.load());
	if (!enabled || userClosed)
	{
		stopping = true;
		if (HWND window = volumeWindow.load())
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
		Labels labels{L"Flycast - " + label("Audio Volume Panel"), label("Volume Level"),
				label("Live peak levels (dBFS)"), label("Paused / audio muted"),
				label("BGM and sound effects share AICA channels and cannot be identified separately. Source meters include volume settings, before final clipping. Output is the final mix; system volume is not included. Scale: -60 to 0 dBFS."),
				{label("Output"), label("AICA (BGM / sound effects)"), label("CD-DA"), label("DSP effects"), label("VMU beep")}};
		worker.thread = std::thread(run, std::move(labels));
	}
	return true;
}

void term()
{
	audioMeter::setEnabled(false);
	playing = false;
	stopping = true;
	if (HWND window = volumeWindow.load())
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
	HWND window = volumeWindow.load();
	return window != nullptr && GetForegroundWindow() == window;
}
}
