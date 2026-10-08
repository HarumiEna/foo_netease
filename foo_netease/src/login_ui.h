#pragma once
// foo_netease —— 登录相关界面。
//
// 三种登录方式各一个模态对话框，只在主线程调用：
//  · 扫码（默认，最安全）
//  · 手机号 + 密码

namespace netease_ui {

void show_login_dialog(fb2k::hwnd_t parent);

} // namespace netease_ui

