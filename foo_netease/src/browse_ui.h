#pragma once
// foo_netease —— 歌单浏览窗口。
//
// 只在主线程调用；打开后就常驻（关闭只是隐藏），避免工作线程回调悬垂。

namespace netease_ui {

// parent 一般传设置页或主窗口，作为新窗口的 owner（否则会被模态窗口压住）。
void show_browse_window(fb2k::hwnd_t parent = nullptr);

} // namespace netease_ui

