#pragma once
// foo_netease —— 歌词获取与缓存。
//
// 为什么要有缓存：titleformat 字段（%netease_lyric%）必须在几微秒内返回，
// 绝对不能联网。所以流程是"播放/读取元数据时后台预取 -> 缓存 -> 字段直接读缓存"。
//
// 线程约定：get_cached/put 任意线程可调；ensure_async 立刻返回，网络在后台线程。

#include <cstdint>
#include <string>

namespace netease_lyric {

// 读缓存（快，任意线程）。命中返回 true。
bool get_cached(int64_t id, std::string & text);

// 增强型（逐字）歌词：转成 A2 格式的 LRC（没有逐字版权时返回 false）。
bool get_cached_enhanced(int64_t id, std::string & text);

// 网易原始逐字（yrc）文本，原样返回。
bool get_cached_raw_yrc(int64_t id, std::string & text);

// 后台取一次并缓存；已在缓存里或已有请求在飞就直接返回。
// path 用于歌词就绪后 metadb_io::dispatch_refresh()，让显示字段刷新。
// 传空 path 则不派发刷新。
void ensure_async(int64_t id, const std::string & path);

// 同步取（调用方必须自己保证不在主线程做网络）。成功时写缓存并返回 true。
bool fetch_now(int64_t id, std::string & text);

// 歌词落盘目录：<profile>\lyrics —— 这**就是 ESLyric 的默认本地歌词目录**
//（它内部写死的 "%fb2k_profile_path%lyrics"），所以不用让用户配任何东西。
std::string lrc_dir();

// 启动时扫一遍清单，删掉太久没写过的 .lrc（只删本组件写过的，见下）。
// 另外：这些 .lrc 已经写好后，<profile>\lyrics 里就有对应的文件了。
void cleanup_lrc_files();

// 把本组件写过的 .lrc 全部删掉（「清除全部数据」用；目录里别家的歌词不动）。
void purge_lrc_files();

// 把歌词写成「<歌手> - <标题>.lrc」（每个 id 只写一次）。
// 编码为带 BOM 的 UTF-8 —— 这是本地 .lrc 最被广泛接受的写法。
bool ensure_lrc_file(int64_t id, const std::string & artist, const std::string & title,
	const std::string & text);

} // namespace netease_lyric

