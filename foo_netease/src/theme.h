#pragma once
// foo_netease —— 跟随 foobar2000 的主题配色。
//
// 背景 / 文字 / 高亮 / 选中四项都问宿主要：ui_config_manager::query_color()。
// 用户没覆盖时按 SDK 约定的系统色回退（ui_element.cpp 里那张映射表）：
//   ui_color_text -> COLOR_WINDOWTEXT、ui_color_background -> COLOR_WINDOW、
//   ui_color_highlight -> COLOR_HOTLIGHT、ui_color_selection -> COLOR_HIGHLIGHT。
//
// 用法：面板和浏览窗口在 WM_CTLCOLOR* / 自绘里调 theme_colors()；
// 颜色变了宿主会回调 ThemeWatcher，我们广播 kMsgThemeChanged 让窗口重绘。
//
// 注意：本头文件用到 SDK 类型，必须在 stdafx.h 之后包含。

#include <windows.h>

namespace netease_ui {


struct ThemeColors {
	COLORREF background = RGB(255, 255, 255);
	COLORREF text = RGB(0, 0, 0);
	COLORREF highlight = RGB(0, 120, 215);    // 高亮（列表里「正在播放」那一条）
	COLORREF selection = RGB(0, 120, 215);    // 选中项底色
	COLORREF selection_text = RGB(255, 255, 255);
	COLORREF border = RGB(160, 160, 160);
	bool dark = false;
};

inline COLORREF theme_dim(COLORREF c) {
	// 混一点背景色，得到"弱化"的线条色（分隔线、边框）。
	return RGB(GetRValue(c) / 2 + 128, GetGValue(c) / 2 + 128, GetBValue(c) / 2 + 128);
}

// 取某一项：用户覆盖了就用覆盖值，否则用 SDK 约定的系统色。
inline COLORREF theme_query(const GUID & what, int sys_index, const char * name = "") {
	if (auto mgr = ui_config_manager::get(); mgr.is_valid()) {
		t_ui_color c = 0;
		if (mgr->query_color(what, c)) {
			return static_cast<COLORREF>(c);
		}
		const COLORREF fallback = static_cast<COLORREF>(mgr->getSysColor(sys_index));
		return fallback;
	}
	return ::GetSysColor(sys_index);
}


inline ThemeColors theme_colors() {
	ThemeColors t;
	t.background = theme_query(ui_color_background, COLOR_WINDOW, "background");
	t.text = theme_query(ui_color_text, COLOR_WINDOWTEXT, "text");
	t.highlight = theme_query(ui_color_highlight, COLOR_HOTLIGHT, "highlight");
	t.selection = theme_query(ui_color_selection, COLOR_HIGHLIGHT, "selection");
	// 选中项上的文字：按选中底色的亮度选黑/白，保证看得清。
	const int lum = (GetRValue(t.selection) * 299 + GetGValue(t.selection) * 587 +
		GetBValue(t.selection) * 114) / 1000;
	t.selection_text = (lum > 150) ? RGB(0, 0, 0) : RGB(255, 255, 255);
	t.border = theme_dim(t.background);
	if (auto mgr = ui_config_manager::get(); mgr.is_valid()) t.dark = mgr->is_dark_mode();
	return t;
}

// 颜色变了就广播，窗口自己收到后重绘（跟界面缩放那套一样）。
const UINT kMsgThemeChanged = WM_APP + 0x322;

inline BOOL CALLBACK theme_child_proc(HWND hwnd, LPARAM) {
	::SendMessageW(hwnd, kMsgThemeChanged, 0, 0);
	return TRUE;
}

inline BOOL CALLBACK theme_enum_proc(HWND hwnd, LPARAM) {
	DWORD pid = 0;
	::GetWindowThreadProcessId(hwnd, &pid);
	if (pid != ::GetCurrentProcessId()) return TRUE;
	::SendMessageW(hwnd, kMsgThemeChanged, 0, 0);
	::EnumChildWindows(hwnd, theme_child_proc, 0);
	return TRUE;
}

inline void notify_theme_changed() {
	::EnumWindows(theme_enum_proc, 0);
}

// 宿主的颜色/字体变化回调。主线程注册/注销（SDK 要求）。
class ThemeWatcher : public ui_config_callback {
public:
	void ui_colors_changed() override {
		notify_theme_changed();
	}
};

inline ThemeWatcher & theme_watcher() {
	static ThemeWatcher watcher;
	return watcher;
}

inline void theme_watch_start() {
	auto mgr = ui_config_manager::get();
	if (mgr.is_valid()) {
		mgr->add_callback(&theme_watcher());
	}
}

inline void theme_watch_stop() {
	if (auto mgr = ui_config_manager::get(); mgr.is_valid()) {
		mgr->remove_callback(&theme_watcher());
	}
}

} // namespace netease_ui

