#pragma once
// foo_netease —— 界面缩放（高 DPI 自适应 + 用户可调）。
//
// 面板/浏览窗口的尺寸是像素常量。foobar2000 v2 是 DPI 感知进程，在 4K + 150%~200%
// 的机器上这些常量不会自己放大，界面就会显得又小又挤（issue #1）。
// 这里按「系统 DPI × 用户设置的额外缩放」统一换算（基准 96 DPI = 1.0）。
//
// 注意：本头文件用到 SDK 的 fb2k::configStore，必须在 stdafx.h 之后包含。

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <string>

namespace netease_ui {

// 窗口所在监视器的 DPI；拿不到就按 96 处理。
inline int dpi_of(HWND hwnd) {
	// GetDpiForWindow 要 Windows 10 1607+，用 GetProcAddress 取，避免链接期依赖。
	using Fn = UINT(WINAPI *)(HWND);
	static Fn fn = reinterpret_cast<Fn>(
		::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	if (fn && hwnd) {
		const UINT d = fn(hwnd);
		if (d >= 72 && d <= 600) return static_cast<int>(d);
	}
	if (hwnd) {
		if (HDC dc = ::GetDC(hwnd)) {
			const int d = ::GetDeviceCaps(dc, LOGPIXELSY);
			::ReleaseDC(hwnd, dc);
			if (d >= 72 && d <= 600) return d;
		}
	}
	return 96;
}

// ---- 用户设置的额外缩放（百分比；100 = 完全跟随系统 DPI）----

inline int read_scale_config() {
	auto store = fb2k::configStore::get();
	if (!store.is_valid()) return 0;
	auto value = store->getConfigString("foo_netease.ui_scale", "");
	if (!value.is_valid()) return 0;
	const int p = std::atoi(value->c_str());
	return (p >= 100 && p <= 300) ? p : 0;
}

inline void write_scale_config(int percent) {
	auto store = fb2k::configStore::get();
	if (!store.is_valid()) return;
	const std::string text = std::to_string(percent);
	store->setConfigString("foo_netease.ui_scale", text.c_str());
}

inline std::atomic<int> & scale_cache() {
	static std::atomic<int> value{ 0 };
	return value;
}

// 当前生效的缩放百分比。取不到配置时按 100 处理（且不缓存，方便稍后重试）。
inline int user_scale_percent() {
	const int cached = scale_cache().load();
	if (cached > 0) return cached;
	const int stored = read_scale_config();
	if (stored > 0) {
		scale_cache().store(stored);
		return stored;
	}
	return 100;
}

// 每次改缩放就 +1：窗口可以在自己的定时器里比对，发现变了就重排
//（停靠面板是子窗口，收不到广播，只能这样自检）。
inline int & scale_generation() {
	static int value = 0;
	return value;
}

inline void set_user_scale_percent(int percent) {
	if (percent < 100 || percent > 300) percent = 100;
	scale_cache().store(percent);
	write_scale_config(percent);
	++scale_generation();
}

// 实际生效的 DPI = 系统 DPI × 用户缩放。
inline int effective_dpi(HWND hwnd) {
	return ::MulDiv(dpi_of(hwnd), user_scale_percent(), 100);
}

// 96 DPI 下的像素值 -> 当前实际像素。
inline int scale(HWND hwnd, int px) {
	return ::MulDiv(px, effective_dpi(hwnd), 96);
}

// 缩放改了以后发给自己的窗口，让它们重排。
// 用自定义消息而不是 WM_SIZE：进程里还有别的组件窗口，别去动它们。
const UINT kMsgScaleChanged = WM_APP + 0x321;

inline BOOL CALLBACK child_notify_proc(HWND hwnd, LPARAM) {
	::SendMessageW(hwnd, kMsgScaleChanged, 0, 0);
	return TRUE;
}

inline BOOL CALLBACK notify_enum_proc(HWND hwnd, LPARAM) {
	DWORD pid = 0;
	::GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ::GetCurrentProcessId()) return TRUE;
	::SendMessageW(hwnd, kMsgScaleChanged, 0, 0);
	::EnumChildWindows(hwnd, child_notify_proc, 0);   // 停靠面板是子窗口
	return TRUE;
}

inline void notify_scale_changed() {
	::EnumWindows(notify_enum_proc, 0);
}

// 界面字体。DEFAULT_GUI_FONT 不随 DPI 放大，所以自己按实际 DPI 建一份
//（字形沿用系统消息字体，只把字号换成 9pt 的当前像素高度）。
class FontCache {
public:
	HFONT get(HWND hwnd) {
		const int dpi = effective_dpi(hwnd);
		if (m_font && m_dpi == dpi) return m_font;
		if (m_font) { ::DeleteObject(m_font); m_font = nullptr; }
		m_font = create(dpi);
		m_dpi = dpi;
		return m_font;
	}

	~FontCache() {
		if (m_font) ::DeleteObject(m_font);
	}

private:
	static HFONT create(int dpi) {
		NONCLIENTMETRICSW ncm{};
		ncm.cbSize = sizeof(ncm);
		if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
			LOGFONTW lf = ncm.lfMessageFont;
			lf.lfHeight = -::MulDiv(9, dpi, 72);   // 9pt
			return ::CreateFontIndirectW(&lf);
		}
		return nullptr;
	}

	HFONT m_font = nullptr;
	int m_dpi = 0;
};

} // namespace netease_ui

