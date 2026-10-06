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

// 后台取一次并缓存；已在缓存里或已有请求在飞就直接返回。
// path 用于歌词就绪后 metadb_io::dispatch_refresh()，让显示字段刷新。
// 传空 path 则不派发刷新。
void ensure_async(int64_t id, const std::string & path);

// 同步取（调用方必须自己保证不在主线程做网络）。成功时写缓存并返回 true。
bool fetch_now(int64_t id, std::string & text);

// 歌词落盘目录：<profile>\foo_netease_lyrics
// 把它填进歌词显示器的「本地歌词文件夹」，就能显示我们抓到的歌词。
std::string lrc_dir();

// 把歌词写成「<歌手> - <标题>.lrc」（每个 id 只写一次）。
// 编码为带 BOM 的 UTF-8 —— 这是本地 .lrc 最被广泛接受的写法。
bool ensure_lrc_file(int64_t id, const std::string & artist, const std::string & title,
	const std::string & text);

} // namespace netease_lyric

