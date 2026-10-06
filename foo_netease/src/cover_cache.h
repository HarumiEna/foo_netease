#pragma once
// foo_netease —— 封面字节的内存缓存 + 预取。
//
// 为什么需要：
//  · foobar2000 的封面面板（默认界面、ESLyric 面板等）会在切歌时同时来要图，
//    而且**切歌会取消还没完成的请求**；
//  · 实测日志里同一首歌会被请求 4 次，然后连着出现"封面下载失败 —— 已取消"，
//    结果是封面一直停在旧图上（"切歌时封面不切换"）。
//
// 所以：播放一开始就后台把封面下好放进内存缓存；面板来要时直接返回，
// 既不会被取消，也快得多。

#include <cstdint>
#include <string>

namespace netease_cover {

// 取缓存中的封面字节；命中返回 true 并填充 bytes。
bool get(int64_t id, std::string & bytes);

// 放进缓存（超过上限会淘汰最旧的）。
void put(int64_t id, const std::string & bytes);

// 后台预取：解析封面地址 + 下载 + 入缓存。同一 id 已在缓存/在下载中就跳过。
void prefetch_async(int64_t id);

// 等预取完成（最多 timeout_ms）。命中返回 true 并填充 bytes。
// 用途：封面面板的请求常常比预取先到、又会被 foobar2000 取消 ——
// 这时等一下预取就能从缓存里拿到图，而不是显示不出封面。
bool wait_for(int64_t id, std::string & bytes, int timeout_ms);

// 仅供调试/日志：当前缓存条目数。
size_t count();

// 轻推一次界面重绘。
// 为什么要这个：实测用户的封面元素**只在重绘/焦点变化时才重新取图** ——
// "切歌后不换，点一下窗口外就换"。所以新封面**真的准备好之后**要再推一次。
void nudge_ui_repaint();

} // namespace netease_cover

