#include "core/meta_cache.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/json.h"

namespace fs = std::filesystem;

namespace netease {

MetaCache & MetaCache::instance() {
	static MetaCache inst;
	return inst;
}

void MetaCache::put(const TrackInfo & track) {
	if (track.id == 0) return;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_items[track.id] = track;
}

void MetaCache::put_all(const std::vector<TrackInfo> & tracks) {
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const TrackInfo & t : tracks) {
		if (t.id != 0) m_items[t.id] = t;
	}
}

bool MetaCache::get(int64_t id, TrackInfo & out) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_items.find(id);
	if (it == m_items.end()) return false;
	out = it->second;
	return true;
}

bool MetaCache::has(int64_t id) const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_items.find(id) != m_items.end();
}

size_t MetaCache::size() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_items.size();
}

void MetaCache::clear() {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_items.clear();
}

std::vector<TrackInfo> MetaCache::all() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<TrackInfo> out;
	out.reserve(m_items.size());
	for (const auto & kv : m_items) out.push_back(kv.second);
	return out;
}

namespace {

std::string to_json_line(const TrackInfo & t) {
	std::ostringstream out;
	out << "{\"id\":" << t.id
		<< ",\"title\":\"" << json::escape(t.title) << "\""
		<< ",\"artists\":\"" << json::escape(t.artists) << "\""
		<< ",\"album\":\"" << json::escape(t.album) << "\""
		<< ",\"duration_ms\":" << t.duration_ms
		<< ",\"fee\":" << t.fee
		// track_number 必须落盘：不存的话播放列表里的"序号"永远是问号
		// （之前只有注释说加了，字段其实没加）。cover_url 一并存，省一次网络。
		<< ",\"track_number\":" << t.track_number
		<< ",\"cover_url\":\"" << json::escape(t.cover_url) << "\"}";
	return out.str();
}

TrackInfo from_json(const json::Value & v) {
	TrackInfo t;
	if (const json::Value * x = v.find("id")) t.id = x->as_int64();
	if (const json::Value * x = v.find("title")) t.title = x->as_string();
	if (const json::Value * x = v.find("artists")) t.artists = x->as_string();
	if (const json::Value * x = v.find("album")) t.album = x->as_string();
	if (const json::Value * x = v.find("duration_ms")) t.duration_ms = x->as_int64();
	if (const json::Value * x = v.find("fee")) t.fee = x->as_int64();
	if (const json::Value * x = v.find("track_number")) t.track_number = x->as_int64();
	if (const json::Value * x = v.find("cover_url")) t.cover_url = x->as_string();
	return t;
}

} // namespace

bool MetaCache::save_to_file(const std::string & path, std::string * error) const {
	const std::vector<TrackInfo> items = all();
	const std::wstring wide_path(path.begin(), path.end());

	try {
		const fs::path target(wide_path);
		if (target.has_parent_path()) fs::create_directories(target.parent_path());
		const fs::path temp = target.wstring() + L".tmp";

		{
			std::ofstream out(temp, std::ios::binary | std::ios::trunc);
			if (!out) {
				if (error) *error = "无法写入缓存文件：" + path;
				return false;
			}
			// 版本号必须跟着字段结构走。踩过的坑：v2 的注释说加了 track_number/cover_url，
			// 但序列化里其实没有 —— 于是播放列表里漫游的"序号"永远是问号。
			// 现在真的写了这两个字段，版本升到 v3；v2 的旧缓存整份丢弃，重新拉一次。
			out << "# foo_netease meta cache v3\n";
			for (const TrackInfo & t : items) out << to_json_line(t) << "\n";
		}
		// 原子替换：先写临时文件再改名，避免中途崩溃留下半截文件。
		std::error_code ec;
		fs::rename(temp, target, ec);
		if (ec) {
			if (error) *error = "替换缓存文件失败：" + ec.message();
			return false;
		}
	} catch (const std::exception & e) {
		if (error) *error = std::string("写缓存异常：") + e.what();
		return false;
	}
	return true;
}

bool MetaCache::load_from_file(const std::string & path, std::string * error) {
	const std::wstring wide_path(path.begin(), path.end());
	std::ifstream in(wide_path, std::ios::binary);
	if (!in) {
		// 第一次运行时文件不存在是正常的，不算错误。
		return false;
	}

	size_t loaded = 0;
	size_t skipped = 0;
	bool version_ok = false;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line[0] == '#') {
			if (line.find("v3") != std::string::npos) version_ok = true;
			continue;
		}
		if (line.empty()) continue;
		if (!version_ok) { ++skipped; continue; }   // 旧格式：整份丢弃，等重新拉取
		json::Value v;
		std::string parse_error;
		if (!json::Value::parse(line, v, &parse_error)) {
			++skipped;
			continue;
		}
		const TrackInfo t = from_json(v);
		if (t.id != 0) {
			put(t);
			++loaded;
		} else {
			++skipped;
		}
	}
	if (error) {
		if (!version_ok) {
			*error = "缓存版本过旧，已整体丢弃（" + std::to_string(skipped) + " 行）";
		} else {
			*error = "载入 " + std::to_string(loaded) + " 条" +
				(skipped ? "，跳过 " + std::to_string(skipped) + " 条破损行" : "");
		}
	}
	return loaded > 0;
}

} // namespace netease

