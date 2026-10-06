#pragma once
// foo_netease —— 歌单浏览窗口。
//
// 只在主线程调用；打开后就常驻（关闭只是隐藏），避免工作线程回调悬垂。

namespace netease_ui {

void show_browse_window();

} // namespace netease_ui

