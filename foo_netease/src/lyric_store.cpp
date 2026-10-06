#include "stdafx.h"
#include "lyric_store.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "component_log.h"
#include "core/api.h"
#include "session.h"

namespace netease_lyric {

namespace {

std::mutex g_mutex;
std::unordered_map<int64_t, std::string> g_cache;   // id -> 歌词文本
std::set<int64_t> g_inflight;                       // 正在后台取的 id
std::set<int64_t> g_lrc_written;                    // 已经落过盘的 id
bool g_lrc_dir_logged = false;

std::wstring to_wide(const std::string & utf8) {
	if (utf8.empty()) return std::wstring();
	const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(need > 0 ? need : 0, L'\0');
	if (need > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), need);
	return out;
}

// 文件名里不能出现的字符换成下划线。
std::string sanitize(const std::string & in) {
	std::string out;
	out.reserve(in.size());
	for (char c : in) {
		const unsigned char u = static_cast<unsigned char>(c);
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
			c == '"' || c == '<' || c == '>' || c == '|' || u < 0x20) {
			out += '_';
		} else {
			out += c;
		}
	}
	// 去掉首尾空格与点（Windows 会拒绝以点结尾的名字）
	while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
	size_t begin = 0;
	while (begin < out.size() && out[begin] == ' ') ++begin;
	out = out.substr(begin);
	if (out.size() > 120) out.resize(120);
	return out;
}

// 只在 g_mutex 之外调用；成功后写缓存。
bool fetch_impl(int64_t id, std::string & text) {
	netease::CookieJar jar;
	jar.deserialize(netease::Session::instance().cookie_header());
	netease::NeteaseApi api(jar, 20000);

	std::string translated;
	netease::ApiCall call = api.lyrics(id, text, translated);
	if (!call.ok) {
		netease_log::write("foo_netease: 歌词获取失败 id=" + std::to_string(id) + " —— " + call.error);
		return false;
	}
	// 有翻译就拼在原文后面（不解析 LRC 时间轴，保持原始文本，交给显示端处理）。
	if (!translated.empty()) {
		text += "\r\n\r\n—— 翻译 ——\r\n";
		text += translated;
	}
	return true;
}

} // namespace

bool get_cached(int64_t id, std::string & text) {
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_cache.find(id);
	if (it == g_cache.end()) return false;
	text = it->second;
	return true;
}

bool fetch_now(int64_t id, std::string & text) {
	if (get_cached(id, text)) return true;
	if (!fetch_impl(id, text)) return false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_cache[id] = text;
	}
	return true;
}

void ensure_async(int64_t id, const std::string & path) {
	if (id <= 0) return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_cache.count(id) || g_inflight.count(id)) return;
		g_inflight.insert(id);
	}
	const std::string keep_path = path;
	fb2k::splitTask([id, keep_path] {
		std::string text;
		const bool ok = fetch_impl(id, text);
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_inflight.erase(id);
			if (ok) g_cache[id] = text;
		}
		if (!ok) return;
		netease_log::write("foo_netease: 歌词已获取 id=" + std::to_string(id) +
			"（" + std::to_string(text.size()) + " 字节）");
		if (keep_path.empty()) return;
		// 关键：dispatch_refresh() 只让界面重画，**不会重新读 file_info**，
		// 所以新出现的 %LYRICS% 标签永远进不了 metadb（歌词显示器也就看不到）。
		// 必须用 load_info_force 强制重读一次 input 的 get_info()。
		fb2k::inMainThread([keep_path] {
			metadb_handle_list handles;
			handles.add_item(metadb::get()->handle_create(keep_path.c_str(), 0));
			auto io = metadb_io_v2::get();
			if (io.is_valid()) {
				io->load_info_async(handles, metadb_io::load_info_force, nullptr,
					metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
			}
			auto legacy = metadb_io::get();
			if (legacy.is_valid()) legacy->dispatch_refresh(handles);
		});
	});
}

// ESLyric 的默认本地歌词目录就是 <profile>\lyrics（从它的 DLL 字符串
// "%fb2k_profile_path%lyrics" 得到）。写这里它无需任何配置就能找到。
std::string lrc_dir() {
	const std::string profile = netease_log::profile_dir();
	if (profile.empty()) return std::string();
	return profile + "\\lyrics";
}

bool ensure_lrc_file(int64_t id, const std::string & artist, const std::string & title,
	const std::string & text) {
	if (id <= 0 || text.empty()) return false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_lrc_written.count(id)) return true;
	}
	const std::string dir = lrc_dir();
	if (dir.empty()) return false;

	// 文件名规则来自 ESLyric 自身：
	//   模板 = $if2(%title% - ,%filename% - )$if2(%artist%,)
	// 即「标题 - 歌手」。歌手可能有多位（我们用 / 连接），而 / 不能出现在文件名里，
	// 所以写两个变体：全歌手、以及只取第一位歌手。
	const std::string t = title.empty() ? std::to_string(id) : sanitize(title);
	std::vector<std::string> stems;
	{
		std::string first = artist;
		const size_t slash = artist.find('/');
		if (slash != std::string::npos) first = artist.substr(0, slash);
		const std::string all_s = sanitize(artist);
		const std::string first_s = sanitize(first);
		if (!all_s.empty()) stems.push_back(t + " - " + all_s);
		if (!first_s.empty() && first_s != all_s) stems.push_back(t + " - " + first_s);
		if (stems.empty()) stems.push_back(t);   // 没歌手信息时至少给一个能匹配的
	}

	try {
		std::filesystem::create_directories(std::filesystem::path(to_wide(dir)));
		// UTF-8 BOM：对本地 .lrc 来说兼容性最好。
		const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
		bool wrote = false;
		for (const std::string & stem : stems) {
			std::ofstream out(to_wide(dir + "\\" + stem + ".lrc").c_str(),
				std::ios::binary | std::ios::trunc);
			if (!out) continue;
			out.write(reinterpret_cast<const char *>(bom), 3);
			out.write(text.data(), static_cast<std::streamsize>(text.size()));
			wrote = true;
		}
		if (!wrote) return false;

		std::lock_guard<std::mutex> lock(g_mutex);
		g_lrc_written.insert(id);
		if (!g_lrc_dir_logged) {
			g_lrc_dir_logged = true;
			netease_log::write("foo_netease: 歌词已写入本地歌词目录 " + dir +
				"（ESLyric 默认就找这里）；文件名 = 标题 - 歌手");
		}
		return true;
	} catch (const std::exception & ex) {
		netease_log::write(std::string("foo_netease: 写歌词文件失败 —— ") + ex.what());
		return false;
	}
}

} // namespace netease_lyric

