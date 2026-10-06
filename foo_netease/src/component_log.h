#pragma once
// foo_netease —— 组件日志。
//
// 除了写 foobar2000 控制台，还会追加到 profile 目录下的 foo_netease.log。
// 原因很实际：控制台只能人在 GUI 里看，而开发/验证阶段需要在没人盯着屏幕时
// 也能确认「组件到底加载了没有、初始化走到哪一步」。这也是 M0 验收
// 「组件能被 foobar2000 加载」唯一可程序化验证的凭据。
//
// 约束（来自 SDK/console.h）：console::print 多线程安全，但 DLL 初始化/反初始化
// 期间禁止调用——所以只在 on_init / on_quit / 运行期使用。

#include <string>

namespace netease_log {

// 在 on_init() 里调用：确定日志路径并写下启动横幅。
void init();

// 任意线程可调用：同时写控制台与日志文件。
void write(const std::string & line);

// 在 on_quit() 里调用。
void shutdown();

// 日志文件的完整路径（UTF-8）；init 之后有效，供调试输出用。
const std::string & file_path();

// foobar2000 的 profile 目录（UTF-8 本地路径，已去掉 file:// 前缀）。
// 其它模块要往 profile 里放自己的文件时用它拼路径。
std::string profile_dir();

} // namespace netease_log

