#pragma once
// foo_netease —— 曲目元数据缓存。
//
// 为什么需要：input 组件的 get_info() 会被 foobar2000 在后台线程频繁调用，
// 而"刚刚在浏览窗口里看过的曲目"元数据我们其实已经拿到了。缓存住之后
// get_info() 命中即可立刻返回，不用再走网络——这是播放列表加载不卡的关键。
//
// 目前是进程内缓存；落盘留到 M6（计划里那一步还没做）。

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/api.h"

namespace netease {

class MetaCache {
public:
	static MetaCache & instance();

	void put(const TrackInfo & track);
	void put_all(const std::vector<TrackInfo> & tracks);
	bool get(int64_t id, TrackInfo & out) const;
	bool has(int64_t id) const;
	size_t size() const;
	void clear();

	// 全部条目快照（用于落盘）。
	std::vector<TrackInfo> all() const;

	// 落盘 / 读盘。用最简单的「每行一个 JSON 对象」，人类可读、出问题好查。
	// 写入走临时文件 + 替换，避免写一半崩溃留下坏文件。
	bool save_to_file(const std::string & path, std::string * error = nullptr) const;
	bool load_from_file(const std::string & path, std::string * error = nullptr);

private:
	MetaCache() = default;
	mutable std::mutex m_mutex;
	std::unordered_map<int64_t, TrackInfo> m_items;
};

} // namespace netease

