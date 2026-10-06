#include "stdafx.h"
#include "meta_store.h"

#include "component_log.h"
#include "core/meta_cache.h"

namespace netease_app {

std::string meta_cache_path() {
	const std::string dir = netease_log::profile_dir();
	return dir.empty() ? std::string() : dir + "\\foo_netease_meta.jsonl";
}

bool load_meta_cache() {
	const std::string path = meta_cache_path();
	if (path.empty()) return false;

	std::string detail;
	if (netease::MetaCache::instance().load_from_file(path, &detail)) {
		netease_log::write("foo_netease: 元数据缓存已载入 —— " + detail);
		return true;
	}
	if (!detail.empty()) netease_log::write("foo_netease: 元数据缓存未载入 —— " + detail);
	return false;
}

bool save_meta_cache(std::string * error) {
	const std::string path = meta_cache_path();
	if (path.empty()) {
		if (error) *error = "取不到 profile 目录";
		return false;
	}

	const size_t count = netease::MetaCache::instance().size();
	std::string local_error;
	if (!netease::MetaCache::instance().save_to_file(path, &local_error)) {
		if (error) *error = local_error;
		netease_log::write("foo_netease: 元数据缓存保存失败 —— " + local_error);
		return false;
	}
	netease_log::write("foo_netease: 元数据缓存已保存（" + std::to_string(count) + " 条）");
	return true;
}

} // namespace netease_app

