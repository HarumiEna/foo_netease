#pragma once
// foo_netease —— 元数据缓存的落盘入口。
//
// 单独抽出来是因为它需要同时被两处使用：
//  · 生命周期（on_init 载入 / on_quit 保存）；
//  · 浏览窗口（每次拉完一批曲目就保存一次）。
//
// 为什么不能只在退出时保存：实测发现 foobar2000 退出时如果有上千项的播放列表要写盘，
// 整个退出过程可能超过 10 秒；用户（或脚本）等不及就会强杀进程，缓存直接丢掉。
// 所以改成"拿到数据就存"，退出时再存一次兜底。

#include <string>

namespace netease_app {

// 缓存文件的完整路径（UTF-8）；profile 目录不可用时返回空串。
std::string meta_cache_path();

// 启动时载入，返回是否真的读到了条目。
bool load_meta_cache();

// 保存；返回是否成功。条目为空时也会写出（表示"确实没有缓存"）。
bool save_meta_cache(std::string * error = nullptr);

} // namespace netease_app

