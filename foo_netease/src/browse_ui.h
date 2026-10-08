#pragma once
// foo_netease —— 歌单浏览窗口。
//
// 只在主线程调用；打开后就常驻（关闭只是隐藏），避免工作线程回调悬垂。

#include <string>

namespace netease_ui {

// parent 一般传设置页或主窗口，作为新窗口的 owner（否则会被模态窗口压住）。
void show_browse_window(fb2k::hwnd_t parent = nullptr);

// 打开歌单窗口并直接定位到某个歌单（面板右键「显示歌单」用）。
void show_browse_window_for(int64_t playlist_id, fb2k::hwnd_t parent = nullptr);

// 打开歌单窗口，并把关键字的搜索结果直接显示在右侧曲目列表（面板左键「搜索」用）。
void show_browse_window_search(const std::string & keyword, fb2k::hwnd_t parent = nullptr);
// 面板「更多」：让搜索结果面板再加载一页（追加）。面板没开着就返回 false。
bool show_browse_window_search_more();

} // namespace netease_ui

