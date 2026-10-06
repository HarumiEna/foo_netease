#pragma once
// foo_netease —— 歌词显示。
//
// 两条输出路径：
//  1) titleformat 字段 @@%netease_lyric%@（由 metadb_display_field_provider 提供，
//     任何能显示 titleformat 的面板都能用）；
//  2) 一个独立的歌词窗口（面板上的「歌词」按钮打开）。

#include <cstdint>
#include <string>

namespace netease_lyric_ui {

// 打开（或复用）歌词窗口；缓存未命中会在后台取。
void show_for(int64_t id, const std::string & title);

} // namespace netease_lyric_ui

