#include "stdafx.h"
#include "lyric_store.h"

#include <windows.h>

#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "component_log.h"
#include "core/api.h"
#include "core/meta_cache.h"
#include "session.h"

namespace netease_lyric {

namespace {

std::mutex g_mutex;
std::unordered_map<int64_t, std::string> g_cache;   // id -> 歌词文本（普通 LRC，含翻译段）
std::unordered_map<int64_t, std::string> g_cache_yrc;        // id -> 原始逐字（yrc）
std::unordered_map<int64_t, std::string> g_cache_enhanced;   // id -> 增强型 LRC（A2）
std::set<int64_t> g_inflight;                       // 正在后台取的 id
std::set<int64_t> g_lrc_written;                    // 已经落过盘的 id
// 我们写过的 .lrc 文件名（不含后缀）→ 最后写入时间（unix 秒）。
// 清理时**只动这里记着的文件**：<profile>\lyrics 是 ESLyric 的默认歌词目录，
// 里面还可能有它自己下载的歌词，不能见到 .lrc 就删。
std::map<std::string, int64_t> g_lrc_files;
bool g_lrc_manifest_dirty = false;
bool g_lrc_dir_logged = false;
// 多久没再写过就清掉。"不需要时再清理"的默认策略。
const int64_t kLrcKeepSeconds = 7 * 24 * 60 * 60;

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

// 毫秒 -> [mm:ss.xx] 里的 mm:ss.xx
std::string ms_to_lrc(int64_t ms) {
	if (ms < 0) ms = 0;
	const int64_t total_cs = ms / 10;
	const int64_t cs = total_cs % 100;
	const int64_t total_s = total_cs / 100;
	char buf[32];
	// 用**三位毫秒**：网易原文和多数歌词文件都是 [mm:ss.xxx]，
	// ESLyric 的增强型解析按这个位数匹配（两位会被当成普通文本）。
	std::snprintf(buf, sizeof(buf), "%02lld:%02lld.%03lld",
		static_cast<long long>(total_s / 60), static_cast<long long>(total_s % 60),
		static_cast<long long>(cs * 10));
	return buf;
}

// 网易逐字格式 yrc：
//   [行开始,行时长,0](词开始,词时长,0)词(词开始,词时长,0)词…
// 转成通用的「增强型 LRC」(A2)：
//   [mm:ss.xx]词<mm:ss.xx>词<mm:ss.xx>…
// <> 里是**该词的结束时间** —— 逐字着色就靠它。没逐字版权的歌这里会是空串。
std::string yrc_to_enhanced(const std::string & yrc) {
	std::string out;
	size_t pos = 0;
	while (pos < yrc.size()) {
		size_t end = yrc.find('\n', pos);
		if (end == std::string::npos) end = yrc.size();
		std::string line = yrc.substr(pos, end - pos);
		pos = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
		if (line.size() < 3 || line[0] != '[') continue;
		const size_t line_close = line.find(']');
		if (line_close == std::string::npos) continue;
		const int64_t line_start = std::strtoll(line.c_str() + 1, nullptr, 10);
		std::string words;
		size_t p = line_close + 1;
		while (p < line.size()) {
			if (line[p] != '(') { ++p; continue; }
			const size_t close = line.find(')', p);
			if (close == std::string::npos) break;
			const std::string stamp = line.substr(p + 1, close - p - 1);
			const size_t c1 = stamp.find(',');
			if (c1 == std::string::npos) break;
			const int64_t ws = std::strtoll(stamp.c_str(), nullptr, 10);
			const int64_t wd = std::strtoll(stamp.c_str() + c1 + 1, nullptr, 10);
			const size_t word_begin = close + 1;
			const size_t word_end = line.find('(', word_begin);
			const size_t stop = (word_end == std::string::npos) ? line.size() : word_end;
			// 标准「增强型 LRC」(A2)：时间戳放在**词前面**，是这个词的开始时间。
			//   [00:12.00]<00:12.00>Hello <00:12.50>world
			// ESLyric 的「显示增强型歌词」认的就是这个形式（词尾标注它不认）。
			words += "<" + ms_to_lrc(ws) + ">";
			words += line.substr(word_begin, stop - word_begin);
			(void)wd;
			p = stop;
		}
		if (words.empty()) continue;
		out += "[" + ms_to_lrc(line_start) + "]" + words + "\r\n";
	}
	return out;
}

// 把逐行翻译按顺序插到增强型歌词里：用**同一行的时间戳**再写一行。
// 播放器（ESLyric 等）会把"同一时间戳的第二行"当成翻译显示，这就是双语歌词的通用做法。
// 从 "[mm:ss.xxx]…" 里取毫秒；取不到返回 false。
bool lrc_time_ms(const std::string & line, int64_t & out) {
	const size_t open = line.find('[');
	if (open == std::string::npos) return false;
	const size_t close = line.find(']', open);
	if (close == std::string::npos) return false;
	const std::string stamp = line.substr(open + 1, close - open - 1);
	int minutes = 0, seconds = 0, frac = 0;
	const int n = sscanf_s(stamp.c_str(), "%d:%d.%d", &minutes, &seconds, &frac);
	if (n < 2) return false;
	// 小数位数决定单位：1~2 位是百分秒，3 位是毫秒
	int ms = 0;
	if (n >= 3) {
		if (frac < 10) ms = frac * 100;
		else if (frac < 100) ms = frac * 10;
		else ms = frac;
	}
	out = (static_cast<int64_t>(minutes) * 60 + seconds) * 1000 + ms;
	return true;
}

// 去掉空白，用于按文本匹配（yrc 与 lrc 时间轴不同，但文字是一样的）。
std::string lrc_norm(const std::string & s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		const char c = s[i];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
		out += c;
	}
	return out;
}

// 把一行 "[mm:ss.xxx]文本" 拆成时间和文本。
bool split_lrc_line(const std::string & line, int64_t & when, std::string & text) {
	if (!lrc_time_ms(line, when)) return false;
	const size_t close = line.find(']');
	if (close == std::string::npos) return false;
	text = line.substr(close + 1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
	return !text.empty();
}

// 把逐行翻译插到增强型歌词里。
//
// 注意：**不能按时间戳配对** —— yrc 的时间是"这个字唱出来的时刻"，
// 而 tlyric 的时间跟着普通 lrc 走，两者的行边界和数值都不一样（实测差一整句）。
// 所以按**文本**配对：先用普通 lrc 把"原文 <-> 翻译"配好，再拿增强型每行的
// 文字去 lrc 里找同一条，把翻译贴上去。
std::string merge_translation(const std::string & enhanced,
	const std::string & lrc_plain, const std::string & translated_lrc) {
	// 1) 翻译条目
	struct Tr { int64_t when; std::string text; bool used; };
	std::vector<Tr> tr;
	{
		size_t pos = 0;
		while (pos < translated_lrc.size()) {
			size_t end = translated_lrc.find('\n', pos);
			if (end == std::string::npos) end = translated_lrc.size();
			std::string line = translated_lrc.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			int64_t when = 0;
			std::string text;
			if (split_lrc_line(line, when, text)) tr.push_back(Tr{ when, text, false });
		}
	}
	if (tr.empty()) return enhanced;

	// 2) 普通 lrc 行 -> 翻译（优先同一时间戳，退一步找最近的）
	struct LrcLine { int64_t when; std::string norm; const std::string * tr; };
	std::vector<LrcLine> lrc;
	{
		size_t pos = 0;
		while (pos < lrc_plain.size()) {
			size_t end = lrc_plain.find('\n', pos);
			if (end == std::string::npos) end = lrc_plain.size();
			std::string line = lrc_plain.substr(pos, end - pos);
			pos = end + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			int64_t when = 0;
			std::string text;
			if (!split_lrc_line(line, when, text)) continue;
			const std::string norm = lrc_norm(text);
			// 同一时间戳（±300ms）优先，其次按顺序取下一个没用过的
			// 单调就近匹配：两边都是按时间递增的，找窗口内最近的、还没用过的。
			// 之前"时间对不上就按顺序硬配"会把整段翻译顶错一句，已去掉。
			const std::string * matched = nullptr;
			{
				// **精确**时间戳配对：网易的 tlyric 就是从同一份歌词生成的，
				// 被翻译的那几行时间戳和 lrc 完全相同。
				// 模糊窗口会把翻译贴到"本来没有翻译"的句子上（用户报的错位就是它）。
				size_t best = tr.size();
				for (size_t i = 0; i < tr.size(); ++i) {
					if (tr[i].used) continue;
					const int64_t delta = tr[i].when > when ? tr[i].when - when : when - tr[i].when;
					if (delta <= 30) { best = i; break; }
				}
				if (best < tr.size()) { tr[best].used = true; matched = &tr[best].text; }
			}
			lrc.push_back(LrcLine{ when, norm, matched });
		}
	}
	if (lrc.empty()) return enhanced;

	// 3) 逐行增强型：按文字找 lrc 行，贴翻译
	std::string out;
	size_t pos = 0;
	std::vector<bool> lrc_used(lrc.size(), false);
	while (pos <= enhanced.size()) {
		size_t end = enhanced.find('\n', pos);
		if (end == std::string::npos) end = enhanced.size();
		std::string line = enhanced.substr(pos, end - pos);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) {
			out += line;
			out += "\r\n";
			const size_t close = line.find(']');
			// 整行文字 = 去掉行首 [..] 和所有 <..> 词标签之后剩下的部分。
			// （之前只取了最后一个词标签之后的内容，导致文字匹配全部失败。）
			std::string body;
			for (size_t k = 0; k < line.size(); ++k) {
				if (line[k] == '[' || line[k] == '<') {
					const char stop = (line[k] == '[') ? ']' : '>';
					const size_t j = line.find(stop, k);
					if (j == std::string::npos) break;
					k = j;
					continue;
				}
				body += line[k];
			}
			const std::string norm = lrc_norm(body);
			const std::string * matched = nullptr;
			for (size_t i = 0; i < lrc.size(); ++i) {
				if (lrc_used[i]) continue;
				if (lrc[i].norm == norm && lrc[i].tr) { lrc_used[i] = true; matched = lrc[i].tr; break; }
			}
			if (!matched) {
				// 文字对不上就**不贴** —— 硬配会比不配更糟（整段顶错一句）。
				// 之前这里"按顺序取下一个"，就是错位的元凶之一。
			}
			if (matched && close != std::string::npos) {
				out += line.substr(0, close + 1);
				out += *matched;
				out += "\r\n";
			}
		}
		if (end >= enhanced.size()) break;
		pos = end + 1;
	}
	return out;
}

// 只在 g_mutex 之外调用；成功后写缓存。
bool fetch_impl(int64_t id, std::string & text) {
	netease::CookieJar jar;
	jar.deserialize(netease::Session::instance().cookie_header());
	netease::NeteaseApi api(jar, 20000);

	std::string translated, yrc, yrc_tr;
	netease::ApiCall call = api.lyrics(id, text, translated, &yrc, &yrc_tr);
	if (!call.ok) {
		netease_log::write("foo_netease: 歌词获取失败 id=" + std::to_string(id) + " —— " + call.error);
		return false;
	}
	// 逐字（增强型）：转成 A2 形式缓存起来，供 %netease_lyric_enhanced% 和本地 .lrc 用。
	if (!yrc.empty()) {
		std::string enhanced = yrc_to_enhanced(yrc);
		// 翻译：优先用逐行翻译（tlyric），按行插到原文后面 —— 之前"翻译没了"
		// 就是因为换成增强型后只带了原文。
		if (!translated.empty()) enhanced = merge_translation(enhanced, text, translated);
		else if (!yrc_tr.empty()) enhanced = merge_translation(enhanced, text, yrc_tr);
		std::lock_guard<std::mutex> lock(g_mutex);
		g_cache_yrc[id] = yrc;
		g_cache_enhanced[id] = enhanced;
		netease_log::write("foo_netease: 逐字歌词已获取 id=" + std::to_string(id) +
			"（增强型 " + std::to_string(enhanced.size()) + " 字节）");
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

bool get_cached_enhanced(int64_t id, std::string & text) {
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_cache_enhanced.find(id);
	if (it == g_cache_enhanced.end() || it->second.empty()) return false;
	text = it->second;
	return true;
}

bool get_cached_raw_yrc(int64_t id, std::string & text) {
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_cache_yrc.find(id);
	if (it == g_cache_yrc.end() || it->second.empty()) return false;
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
		// 文件就在这儿写：不依赖宿主重读元数据，播放时它已经躺在 <profile>\lyrics 里
		//（ESLyric 的「本地歌词」来源默认就找这个目录，无需任何配置）。
		{
			netease::TrackInfo t;
			if (netease::MetaCache::instance().get(id, t)) {
				std::string enhanced;
				const std::string body = get_cached_enhanced(id, enhanced) ? enhanced : text;
				ensure_lrc_file(id, t.artists, t.title, body);
			}
		}
		netease_log::write("foo_netease: 歌词已获取 id=" + std::to_string(id) +
			"（" + std::to_string(text.size()) + " 字节）");
		if (keep_path.empty()) return;
		// 顺手让宿主重读一次元数据：input 的 get_info() 里会兜底补写一次 .lrc
		//（万一上面 MetaCache 还没缓存到这首歌的标题/歌手，就凑不出文件名）。
		// dispatch_refresh() 只让界面重画、**不会重新读 file_info**，所以要 load_info_force。
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

namespace {

std::string lrc_manifest_path() {
	const std::string profile = netease_log::profile_dir();
	if (profile.empty()) return std::string();
	return profile + "\\foo_netease_lrc_files.txt";
}

// 把"我们写过哪些 .lrc"落盘。清理只认这份清单。
void save_lrc_manifest() {
	std::map<std::string, int64_t> snapshot;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_lrc_manifest_dirty) return;
		snapshot = g_lrc_files;
		g_lrc_manifest_dirty = false;
	}
	const std::string path = lrc_manifest_path();
	if (path.empty()) return;
	std::ofstream out(to_wide(path).c_str(), std::ios::trunc);
	if (!out) return;
	out << "# foo_netease —— 本组件写在 <profile>\\lyrics 里的 .lrc（文件名 <TAB> unix 秒）。"
		"\n# 清理时只删清单里列出的这些，不动目录里别家的歌词。\n";
	for (const auto & kv : snapshot) out << kv.first << "\t" << kv.second << "\n";
}

} // namespace

void cleanup_lrc_files() {
	const std::string path = lrc_manifest_path();
	const std::string dir = lrc_dir();
	if (path.empty() || dir.empty()) return;

	std::map<std::string, int64_t> files;
	{
		std::ifstream in(to_wide(path).c_str());
		std::string line;
		while (std::getline(in, line)) {
			if (line.empty() || line[0] == '#') continue;
			const size_t tab = line.find('\t');
			if (tab == std::string::npos || tab == 0) continue;
			files[line.substr(0, tab)] = std::strtoll(line.c_str() + tab + 1, nullptr, 10);
		}
	}
	const int64_t now = static_cast<int64_t>(::time(nullptr));
	std::map<std::string, int64_t> keep;
	std::error_code ec;
	size_t removed = 0;
	for (const auto & kv : files) {
		if (kv.second > 0 && now - kv.second < kLrcKeepSeconds) {
			keep.insert(kv);
			continue;
		}
		std::filesystem::remove(std::filesystem::path(to_wide(dir + "\\" + kv.first + ".lrc")), ec);
		if (!ec) ++removed;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_lrc_files = keep;
		g_lrc_manifest_dirty = true;
	}
	save_lrc_manifest();
	if (removed > 0) {
		netease_log::write("foo_netease: 本地歌词已清理 " + std::to_string(removed) +
			" 份（超过 " + std::to_string(kLrcKeepSeconds / 86400) + " 天没再写过的）");
	}
}

void purge_lrc_files() {
	const std::string dir = lrc_dir();
	std::map<std::string, int64_t> files;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		files.swap(g_lrc_files);
		g_lrc_written.clear();   // 放行：以后播放还会重新写
		g_lrc_manifest_dirty = true;
	}
	size_t removed = 0;
	std::error_code ec;
	if (!dir.empty()) {
		for (const auto & kv : files) {
			std::filesystem::remove(std::filesystem::path(to_wide(dir + "\\" + kv.first + ".lrc")), ec);
			if (!ec) ++removed;
		}
	}
	save_lrc_manifest();
	netease_log::write("foo_netease: 已删除组件生成的本地歌词 " + std::to_string(removed) + " 份");
}

bool ensure_lrc_file(int64_t id, const std::string & artist, const std::string & title,
	const std::string & text) {
	// 有逐字就写增强型 LRC —— ESLyric 这类歌词显示器看到 <mm:ss.xx> 就会逐字着色。
	std::string enhanced;
	const bool use_enhanced = get_cached_enhanced(id, enhanced);
	const std::string & body = use_enhanced ? enhanced : text;
	if (id <= 0 || body.empty()) return false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		// 已经有逐字版时**允许覆盖**：之前可能写的是普通版（旧版本留下的），
		// 不覆盖的话 ESLyric 会一直读那份普通歌词，"显示增强型歌词"也就不生效。
		if (g_lrc_written.count(id) && !use_enhanced) return true;
	}
	const std::string dir = lrc_dir();
	if (dir.empty()) return false;

	// 文件名规则：ESLyric 的「本地歌词」是按它自己的模板去这个目录里找同名文件的，
	// 而模板方向随版本/设置而变 —— 这台机器的配置里是 [%artist% - ]%title%，
	// 也就是「歌手 - 标题」；另一处默认又是「标题 - 歌手」。**两个方向都写一份**，
	// 免得因为方向反了死活匹配不上（之前就是这个原因：文件写了但歌词显示器读不到）。
	// 歌手可能有多位（缓存里用 / 连接），所以每个方向还要覆盖几种写法：
	//   · 全歌手（/ 换成 _）
	//   · 只取第一位歌手
	//   · foobar2000 里 %artist% 的常规呈现（多值用 ", " 连接）
	const std::string t = title.empty() ? std::to_string(id) : sanitize(title);
	std::vector<std::string> artists;
	{
		std::string first = artist;
		const size_t slash = artist.find('/');
		if (slash != std::string::npos) first = artist.substr(0, slash);
		const std::string all_s = sanitize(artist);
		const std::string first_s = sanitize(first);
		if (!all_s.empty()) artists.push_back(all_s);
		if (!first_s.empty() && first_s != all_s) artists.push_back(first_s);
		if (slash != std::string::npos) {
			std::string comma;
			size_t pos = 0;
			while (pos <= artist.size()) {
				const size_t next = artist.find('/', pos);
				const std::string part = artist.substr(pos,
					next == std::string::npos ? std::string::npos : next - pos);
				if (!comma.empty()) comma += ", ";
				comma += part;
				if (next == std::string::npos) break;
				pos = next + 1;
			}
			const std::string comma_s = sanitize(comma);
			if (!comma_s.empty() && comma_s != all_s && comma_s != first_s) {
				artists.push_back(comma_s);
			}
		}
	}
	std::vector<std::string> stems;
	for (const std::string & a : artists) {
		const std::string s1 = t + " - " + a;   // 标题 - 歌手
		const std::string s2 = a + " - " + t;   // 歌手 - 标题
		if (std::find(stems.begin(), stems.end(), s1) == stems.end()) stems.push_back(s1);
		if (std::find(stems.begin(), stems.end(), s2) == stems.end()) stems.push_back(s2);
	}
	// 没歌手信息时至少给一个能匹配的（模板里歌手那一段本来就可省）。
	if (stems.empty()) stems.push_back(t);

	try {
		std::filesystem::create_directories(std::filesystem::path(to_wide(dir)));
		// UTF-8 BOM：对本地 .lrc 来说兼容性最好。
		const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
		bool wrote = false;
		const int64_t now = static_cast<int64_t>(::time(nullptr));
		for (const std::string & stem : stems) {
			std::ofstream out(to_wide(dir + "\\" + stem + ".lrc").c_str(),
				std::ios::binary | std::ios::trunc);
			if (!out) continue;
			out.write(reinterpret_cast<const char *>(bom), 3);
			out.write(body.data(), static_cast<std::streamsize>(body.size()));
			wrote = true;
			// 记进清单：以后「不需要时再清理」只删这里记着的文件。
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				g_lrc_files[stem] = now;
				g_lrc_manifest_dirty = true;
			}
		}
		if (!wrote) return false;
		save_lrc_manifest();

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

