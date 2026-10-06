#pragma once
// 网易云音乐可停靠面板（ui_element，子类 utility）。
//
// 与「浏览歌单」模态窗口的区别：它是常驻的，可以作为分栏 / 标签页留在布局里，
// 不需要每次弹出。实现见 panel_ui.cpp；注册由文件内的 ui_element_impl_withpopup
// 工厂完成，因此「视图 → 布局 → 添加 → 网易云音乐」里会出现它。

namespace netease_ui {

// 以独立窗口打开面板。布局里也能添加它；这里额外给一个确定性入口
//（设置页的「打开面板」按钮），便于快速查看与自动化验证。
void open_panel_window();

} // namespace netease_ui

