#include "stdafx.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "component_log.h"
#include "core/api.h"
#include "core/meta_cache.h"
#include "cover_cache.h"
#include "http_file.h"
#include "lyric_store.h"
#include "netease_data.h"
#include "session.h"

// 输入组件：接管 netease:// 路径。
//
// 依据：
//  · input_entry::is_our_path(p_full_path, p_extension) 拿到的是**完整路径**
//    （sdk/foobar2000/SDK/input.h:242），所以按前缀认领即可，不需要伪扩展名；
//  · 实现类的静态函数由 input_entry_impl_t 转发（input_impl.h:452-453）；
//  · 注册用 input_singletrack_factory_t<T>（input_impl.h:517-519）；
//  · 委托给内置 HTTP 输入用 input_entry::g_open_for_decoding（input.h:283）。
//
// 播放策略：我们不自己解码，但**自己拉流**。open() 里把 netease:// 解析成带时效签名的
// CDN 直链，然后用我们自己的 HTTP 客户端把它包成一个 SDK file 对象，作为 p_filehint
// 交给 foobar2000 的输入系统去解码和 seek。
//
// 为什么不直接把直链交给内置 HTTP 输入：实测云盘 / Hi-Res 曲目走内置那条路会报
// 「不支持的格式或文件损坏」，而我们自己发请求能 100% 正确拿到数据（见 http_file.h）。
// 这样一来请求头、URL 编码、超时、Range、abort 全部在我们手里。

namespace {

// 技术信息（采样率 / 声道 / 位深 / 码率）只有解码器知道，而 get_info() 有两种场景：
//   · 播放中：m_decoder 有效，可以直接问解码器；
//   · 只读元数据（播放列表加载、歌词到位后的强制重读）：**没有解码器**，
//     这时如果什么都不写，就会把状态栏左下角的 kbps/Hz 冲成问号。
// 所以播放中学到的技术信息留一份缓存，以后任何一次 get_info 都把它带上。
struct TechInfo {
	int samplerate = 0;
	int channels = 0;
	int bits = 0;
	int bitrate = 0;
	std::string codec;      // 「格式」列看的就是它（FLAC / MP3 …）
	std::string encoding;
};

std::mutex g_tech_mutex;
std::unordered_map<int64_t, TechInfo> g_tech_by_id;
std::set<std::string> g_tech_refreshed_paths;

void store_tech(int64_t id, const file_info & tech) {
	if (id <= 0) return;
	auto to_int = [&tech](const char * name) -> int {
		const char * v = tech.info_get(name);
		return (v && *v) ? std::atoi(v) : 0;
	};
	TechInfo t;
	t.samplerate = to_int("samplerate");
	t.channels = to_int("channels");
	t.bits = to_int("bitspersample");
	t.bitrate = to_int("bitrate");
	auto to_str = [&tech](const char * name) -> std::string {
		const char * v = tech.info_get(name);
		return (v && *v) ? std::string(v) : std::string();
	};
	t.codec = to_str("codec");
	t.encoding = to_str("encoding");
	if (t.samplerate <= 0 && t.bitrate <= 0 && t.codec.empty() && t.encoding.empty()) return;
	std::lock_guard<std::mutex> lock(g_tech_mutex);
	TechInfo & slot = g_tech_by_id[id];
	// 只补空的：解码器给的值优先
	if (slot.codec.empty()) slot.codec = t.codec;
	if (slot.encoding.empty()) slot.encoding = t.encoding;
	if (t.samplerate > 0) slot.samplerate = t.samplerate;
	if (t.channels > 0) slot.channels = t.channels;
	if (t.bits > 0) slot.bits = t.bits;
	if (t.bitrate > 0) slot.bitrate = t.bitrate;
}

// 解码前我们就知道实际格式（解析直链时响应里的 type），
// 万一解码器不报 codec，用它兜底 —— 「格式」列就不会是问号。
void store_codec_fallback(int64_t id, const std::string & fmt) {
	if (id <= 0 || fmt.empty()) return;
	std::string up;
	for (char c : fmt) up += static_cast<char>(::toupper(static_cast<unsigned char>(c)));
	std::lock_guard<std::mutex> lock(g_tech_mutex);
	TechInfo & slot = g_tech_by_id[id];
	if (slot.codec.empty()) slot.codec = up;
}

// 把缓存里的技术字段补进这次的 file_info（只补缺失的，绝不覆盖解码器的实时值）。
bool apply_cached_tech(int64_t id, file_info & info) {
	TechInfo t;
	{
		std::lock_guard<std::mutex> lock(g_tech_mutex);
		auto it = g_tech_by_id.find(id);
		if (it == g_tech_by_id.end()) return false;
		t = it->second;
	}
	int filled = 0;
	auto set_if_missing = [&info, &filled](const char * name, int value) {
		if (value <= 0) return;
		const char * cur = info.info_get(name);
		if (cur && *cur) return;
		info.info_set(name, std::to_string(value).c_str());
		++filled;
	};
	set_if_missing("samplerate", t.samplerate);
	set_if_missing("channels", t.channels);
	set_if_missing("bitspersample", t.bits);
	set_if_missing("bitrate", t.bitrate);

	auto set_str_if_missing = [&info, &filled](const char * name, const std::string & value) {
		if (value.empty()) return;
		const char * cur = info.info_get(name);
		if (cur && *cur) return;
		info.info_set(name, value.c_str());
		++filled;
	};
	set_str_if_missing("codec", t.codec);
	set_str_if_missing("encoding", t.encoding);

	if (filled > 0) {
		static std::atomic<int> logged{ 0 };
		if (logged.fetch_add(1) < 5) {
			netease_log::write("foo_netease [input] 只读元数据：用缓存补上技术信息 id=" +
				std::to_string(id) + " codec=" + (t.codec.empty() ? "?" : t.codec) +
				" samplerate=" + std::to_string(t.samplerate) +
				" bits=" + std::to_string(t.bits) + " bitrate=" + std::to_string(t.bitrate) +
				"（不补的话状态栏/格式列就是问号）");
		}
	}
	return true;
}

// 第一次学到技术信息后，让 metadb 把这一条重读一次。重读本身走"只读元数据"实例，
// 但技术信息已经进了缓存，所以状态栏立刻就能拿到。每条路径只补一次。
void refresh_info_deferred(const std::string & path) {
	{
		std::lock_guard<std::mutex> lock(g_tech_mutex);
		if (!g_tech_refreshed_paths.insert(path).second) return;
	}
	fb2k::splitTask([path] {
		fb2k::inMainThread([path] {
			metadb_handle_list handles;
			handles.add_item(metadb::get()->handle_create(path.c_str(), 0));
			auto io = metadb_io_v2::get();
			if (io.is_valid()) {
				io->load_info_async(handles, metadb_io::load_info_force, nullptr,
					metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
			}
		});
	});
}

const char * kPathPrefix = "netease://song/";


// 解析 netease://song/<id>[?level=<档位>]
bool parse_song_path(const char * path, int64_t & id, std::string & level, int64_t & playlist_no) {
	id = 0;
	level.clear();
	playlist_no = 0;
	if (!path) return false;
	const size_t prefix_len = std::strlen(kPathPrefix);
	if (std::strncmp(path, kPathPrefix, prefix_len) != 0) return false;

	const char * rest = path + prefix_len;
	const char * query = std::strchr(rest, '?');
	const size_t id_len = query ? static_cast<size_t>(query - rest) : std::strlen(rest);
	if (id_len == 0) return false;

	const std::string id_text(rest, id_len);
	id = std::strtoll(id_text.c_str(), nullptr, 10);
	if (id <= 0) return false;

	// 解析查询串。no 是"在源歌单里排第几"——它写进路径而不是写进按 id 共享的缓存，
	// 因为同一首歌在不同歌单里的序号不同；放进路径才能跟播放列表条目一一对应，
	// 本地按标题排序也不会改动它（这正是用户要的"绑定在歌曲上"）。
	if (query) {
		const char * level_pos = std::strstr(query, "level=");
		if (level_pos) {
			const char * value = level_pos + 6;
			const char * end = std::strchr(value, '&');
			level.assign(value, end ? static_cast<size_t>(end - value) : std::strlen(value));
		}
		const char * no_pos = std::strstr(query, "no=");
		if (no_pos) {
			playlist_no = std::strtoll(no_pos + 3, nullptr, 10);
		}
	}
	return true;
}

// 取一个"以当前登录身份"构造的 API 对象。
void make_api(netease::CookieJar & jar, std::unique_ptr<netease::NeteaseApi> & holder,
	abort_callback & abort) {
	jar.deserialize(netease::Session::instance().cookie_header());
	holder = std::make_unique<netease::NeteaseApi>(jar, 20000);
	// 把 foobar2000 的 abort_callback 接到我们的 HTTP 层：
	// 用户取消播放时，正在进行的请求会在下一个数据块前中断，而不是等满 20 秒超时。
	holder->set_abort([&abort] { return abort.is_aborting(); });
}

// 把响应里的 type 字段（"flac"/"mp3"/"FLAC"…）归一化成扩展名。
std::string normalize_extension(const std::string & type) {
	std::string ext;
	for (char c : type) {
		if (c >= 'a' && c <= 'z') ext += c;
		else if (c >= 'A' && c <= 'Z') ext += static_cast<char>(c - 'A' + 'a');
		else if (c >= '0' && c <= '9') ext += c;
	}
	return ext;
}

// 去掉 URL 的查询串。框架按路径末尾的扩展名认格式，查询串（vuutv 签名里含 + / =）
// 会把扩展名弄脏，所以选解码器时用干净的路径。
std::string strip_query(const std::string & url) {
	const size_t q = url.find('?');
	return q == std::string::npos ? url : url.substr(0, q);
}

// 解析 CDN 直链。失败时把原因写进 error 并写日志——因为最终会被 foobar2000
// 显示成"无法打开"，用户需要能看出到底是无版权、要会员还是网络问题。
bool resolve_cdn_url(int64_t id, const std::string & level_hint, std::string & url,
	std::string & out_type, std::string & error, abort_callback & abort) {
	netease::CookieJar jar;
	std::unique_ptr<netease::NeteaseApi> holder;
	make_api(jar, holder, abort);

	const std::string level = level_hint.empty() ? netease::Session::instance().quality() : level_hint;
	netease::SongUrlInfo info = holder->song_url_with_fallback(id, level);
	if (!info.url.empty()) {
		url = info.url;
		out_type = info.type;
		// 直链带时效签名，把有效期记下来便于排查（实测 expi=1200 秒）。
		netease_log::write("foo_netease: 直链解析成功 id=" + std::to_string(id) +
			" 档位=" + info.level + " 码率=" + std::to_string(info.br) +
			" 格式=" + info.type + " fee=" + std::to_string(info.fee) +
			" 有效期=" + std::to_string(info.expi) + "s" +
			(info.has_free_trial ? "（仅试听片段）" : ""));
		// 完整 URL 都记下来：出现「解析成功但解码失败」时必须能拿它去实测。
		// 这是本地日志，且直链 20 分钟就失效，留存风险可接受。
		netease_log::write("foo_netease:   直链 = " + info.url);
		return true;
	}

	error = info.error.empty() ? "拿不到播放直链" : info.error;
	out_type.clear();
	if (info.has_free_trial) {
		error = "该曲目当前只能试听片段（未登录或不是会员）";
	}
	netease_log::write("foo_netease: 直链解析失败 id=" + std::to_string(id) + " —— " + error);
	return false;
}

// 诊断计数：只在日志里打出前若干次调用，避免刷屏。
std::atomic<int> g_get_info_calls{ 0 };
std::atomic<int> g_open_calls{ 0 };

class netease_input : public input_stubs {
public:
	// input_singletrack_impl 列出的必须实现项（见 sdk/foobar2000/SDK/input_impl.h:125-181）
	void open(service_ptr_t<file> p_filehint, const char * p_path, t_input_open_reason p_reason,
		abort_callback & p_abort) {
		(void)p_filehint;
		if (p_reason == input_open_info_write) throw exception_tagging_unsupported();

		int64_t id = 0;
		std::string level;
		int64_t playlist_no = 0;
		if (!parse_song_path(p_path, id, level, playlist_no)) {
			throw exception_io_data("不是有效的 netease://song/<id> 路径");
		}
		m_id = id;
		m_level = level;
		m_playlist_no = playlist_no;
		m_path = p_path;

		if (const int n = g_open_calls.fetch_add(1); n < 6) {
			netease_log::write("foo_netease [input] open #" + std::to_string(n) + " 原因=" +
				std::to_string(static_cast<int>(p_reason)) + " path=" + (p_path ? p_path : "(null)") +
				" 解析出的 id=" + std::to_string(id));
		}

		if (p_reason == input_open_decode) {
			std::string url, format_type, error;
			if (!resolve_cdn_url(id, level, url, format_type, error, p_abort)) {
				throw exception_io_data(error.c_str());
			}
			// 关键一步：把**我们自己的 file** 作为 p_filehint 传进去，数据就不再走
			// foobar2000 内置的下载路径（那条路在云盘 / Hi-Res 上拿不到数据）。
			netease::CookieJar stream_jar;
			stream_jar.deserialize(netease::Session::instance().cookie_header());
			service_ptr_t<file> stream = netease::open_http_stream(url, stream_jar);

			// 歌词：播放时后台预取一份，供 %netease_lyric% 与歌词窗口直接读缓存。
			// 只在这里触发（而不是 get_info）——get_info 会被播放列表批量调用，
			// 那样会一次打出上百个歌词请求。
			netease_lyric::ensure_async(id, std::string(m_path));

			// **必须在这里**就记下"正在播放"，不能只在 play_callback 里记：
			// 打开解码器发生在任何 play_callback 通知之前，而封面加载器
			// （另一个 play_callback）可能在我们的回调之前就去要图 ——
			// 那样它拿到的还是上一首的路径，封面就"不会自动切换"，
			// 非要再点一下界面（触发一次重新请求）才更新。
			netease_data::set_now_playing_path(std::string(m_path.c_str()));
			// 封面也在这里就开始预取，比 play_callback 更早。
			netease_cover::prefetch_async(id);
			{
				static std::atomic<int> now_logged{ 0 };
				if (now_logged.fetch_add(1) < 6) {
					netease_log::write("foo_netease [input] 打开解码器：已更新「正在播放」id=" +
						std::to_string(id) + "（封面加载器此时来要图就能拿到新封面）");
				}
			}

			// 但仅仅给 filehint 还不够：**解码器必须按实际格式选**。
			// 实测（2026-10-05）：路径是 http:// 时，框架会落到「foobar2000 MPEG 解码器」
			// ——mp3 正好蒙对，FLAC / 云盘 / Hi-Res 一律解不了，
			// 表现就是"无法打开用于播放的项目(不支持的格式或文件损坏)"。
			// 所以这里按响应里的 type 字段（flac/mp3/…）挑对应的输入组件，直接调用它。
			const std::string clean_url = strip_query(url);
			const std::string ext = normalize_extension(format_type);
			// 记下实际格式（FLAC/MP3/…）：get_info 里当 codec 的兜底值。
			store_codec_fallback(id, format_type);
			service_ptr_t<input_entry> chosen;
			bool chosen_ok = false;
			if (!ext.empty()) {
				const std::string service_path = std::string("foo_netease_stream.") + ext;
				chosen_ok = input_entry::g_find_service_by_path(chosen, service_path.c_str()) && chosen.is_valid();
			}
			const bool verbose = g_open_calls.load() <= 12;
			try {
				if (chosen_ok) {
					if (verbose) {
						netease_log::write(std::string("foo_netease [input] 按格式「") + ext +
							"」选定解码器「" + (chosen->get_name_() ? chosen->get_name_() : "?") +
							"」（数据走我们提供的 file）");
					}
					chosen->open_for_decoding(m_decoder, stream, clean_url.c_str(), p_abort);
				} else {
					if (verbose) {
						netease_log::write("foo_netease [input] 未能按格式选定解码器，回退让框架按路径选择（格式=" +
							(format_type.empty() ? std::string("未知") : format_type) + "）");
					}
					input_entry::g_open_for_decoding(m_decoder, stream, url.c_str(), p_abort);
				}
			} catch (const std::exception & ex) {
				// 把"打开解码器"这一步的失败原因留下来——不然用户只会看到一句
				// 通用的"无法打开用于播放的项目"。
				netease_log::write(std::string("foo_netease [input] 打开解码器失败：") + ex.what());
				throw;
			}
		}
	}

	void get_info(file_info & p_info, abort_callback & p_abort) {
		const int call = g_get_info_calls.fetch_add(1);
		const bool log_this = call < 6;

		netease::TrackInfo track;
		const bool cache_hit = netease::MetaCache::instance().get(m_id, track);
		if (log_this) {
			netease_log::write("foo_netease [input] get_info #" + std::to_string(call) +
				" path=" + m_path.c_str() + " 缓存命中=" + (cache_hit ? "是" : "否") +
				" 标题=「" + track.title + "」");
		}
		if (!cache_hit) {
			// 缓存未命中（例如用户直接拖入 netease:// 链接）：同步取一次。
			netease::CookieJar jar;
			std::unique_ptr<netease::NeteaseApi> holder;
			make_api(jar, holder, p_abort);
			std::vector<int64_t> ids{ m_id };
			std::vector<netease::TrackInfo> out;
			netease::ApiCall call = holder->song_details(ids, out);
			if (call.ok && !out.empty()) {
				track = out[0];
				netease::MetaCache::instance().put(track);
			}
		}
		if (p_abort.is_aborting()) return;   // 被取消就别再往下写元数据了

		if (log_this) {
			netease_log::write("foo_netease [input]   → 写入 title=「" + track.title +
				"」artist=「" + track.artists + "」album=「" + track.album +
				"」时长ms=" + std::to_string(track.duration_ms));
		}
		if (!track.title.empty()) p_info.meta_set("title", track.title.c_str());
		if (!track.artists.empty()) p_info.meta_set("artist", track.artists.c_str());
		if (!track.album.empty()) p_info.meta_set("album", track.album.c_str());
		if (track.duration_ms > 0) p_info.set_length(static_cast<double>(track.duration_ms) / 1000.0);
		// 音轨号：不写的话播放列表那一列就是问号，也没法按序号排序。
		if (track.track_number > 0) {
			p_info.meta_set("tracknumber", std::to_string(track.track_number).c_str());
		} else {
			p_info.meta_set("tracknumber", "");   // 明确置空，避免显示成未知
		}
		if (track.fee == 0) p_info.info_set("netease_fee", "free");

		// 自定义字段留给 metadb_display_field_provider / titleformat 用。
		p_info.meta_set("netease_id", std::to_string(m_id).c_str());
		// 源歌单内序号：加一列 %netease_no% 就能在本地乱序后按它一键还原歌单原顺序。
		if (m_playlist_no > 0) {
			p_info.meta_set("netease_no", std::to_string(m_playlist_no).c_str());
			// 顺便补上 foobar2000 自己的序号字段，方便直接用现成的列
			if (track.track_number <= 0) {
				p_info.meta_set("tracknumber", std::to_string(m_playlist_no).c_str());
			}
		}
		const std::string level = m_level.empty() ? netease::Session::instance().quality() : m_level;
		p_info.meta_set("netease_level", level.c_str());

		// 歌词：以**标准标签名**提供，这样 foobar2000 的歌词显示器
		//（例如 ESLyric 的「内嵌歌词」来源）能直接读到。
		// 播放时已后台预取；取到后会 dispatch_refresh，于是这里会被再调一次。
		{
			std::string lyric;
			if (netease_lyric::get_cached(m_id, lyric) && !lyric.empty()) {
				// ESLyric 的「内嵌歌词」读的是 %LYRICS%（从它的 DLL 字符串确认），
				// 另外两个名字是别家歌词组件常用的，一并给出。
				p_info.meta_set("LYRICS", lyric.c_str());
				p_info.meta_set("LYRIC", lyric.c_str());
				p_info.meta_set("UNSYNCEDLYRICS", lyric.c_str());
				// 同时落一份 .lrc，供歌词显示器的「本地歌词文件夹」使用
				// （比依赖标签更可靠：ESLyric 的本地来源就是这么找文件的）。
				netease_lyric::ensure_lrc_file(m_id, track.artists, track.title, lyric);
				static std::atomic<int> lyric_logged{ 0 };
				if (lyric_logged.fetch_add(1) < 3) {
					netease_log::write("foo_netease [input] 已把歌词作为 LYRIC 标签提供给 foobar2000（id=" +
						std::to_string(m_id) + "，" + std::to_string(lyric.size()) + " 字节）");
				}
			}
		}

		// 技术信息（采样率 / 声道 / 位深 / 码率）只能问被委托的解码器：
		// 我们自己拿不到这些，而状态栏和「属性」都看它们。
		// 只搬技术字段，不搬 title/artist —— 元数据仍以我们缓存里的为准。
		if (m_decoder.is_valid()) {
			try {
				file_info_impl tech;
				m_decoder->get_info(0, tech, p_abort);
				static const char * const kTechFields[] = {
					"samplerate", "channels", "bitspersample", "bitrate",
					"bitrate_dynamic", "encoding", "codec"
				};
				for (const char * name : kTechFields) {
					const char * value = tech.info_get(name);
					if (value && *value) p_info.info_set(name, value);
				}
				// 记下来：下次"只读元数据"的 get_info 也能带上这些字段。
				store_tech(m_id, tech);
				refresh_info_deferred(std::string(m_path.c_str()));
				if (log_this) {
					netease_log::write(std::string("foo_netease [input]   → 技术信息 ") +
						"samplerate=" + (tech.info_get("samplerate") ? tech.info_get("samplerate") : "?") +
						" channels=" + (tech.info_get("channels") ? tech.info_get("channels") : "?") +
						" bits=" + (tech.info_get("bitspersample") ? tech.info_get("bitspersample") : "?") +
						" bitrate=" + (tech.info_get("bitrate") ? tech.info_get("bitrate") : "?"));
				}
			} catch (const std::exception & ex) {
				if (log_this) {
					netease_log::write(std::string("foo_netease [input]   取技术信息失败：") + ex.what());
				}
			}
		}

		// 没有解码器时（只读元数据）用缓存补上技术字段：
		// 不补的话状态栏左下角的 kbps/Hz 就是问号。
		apply_cached_tech(m_id, p_info);
	}

	t_filestats2 get_stats2(uint32_t f, abort_callback & a) {
		// 播放时向被委托的解码器要真实统计信息（状态栏的码率/采样率就从这来）。
		// 只做元数据读取（m_decoder 还没建立）时返回"未知"。
		if (m_decoder.is_valid()) return m_decoder->get_stats2_(m_path, f, a);
		return t_filestats2{};
	}

	void decode_initialize(unsigned p_flags, abort_callback & p_abort) {
		if (!m_decoder.is_valid()) throw exception_io_data("内部错误：没有可用的解码器");
		m_decoder->initialize(0, p_flags, p_abort);
	}
	bool decode_run(audio_chunk & p_chunk, abort_callback & p_abort) {
		return m_decoder.is_valid() && m_decoder->run(p_chunk, p_abort);
	}
	void decode_seek(double p_seconds, abort_callback & p_abort) {
		if (m_decoder.is_valid()) m_decoder->seek(p_seconds, p_abort);
	}
	bool decode_can_seek() { return m_decoder.is_valid() && m_decoder->can_seek(); }

	// 下面三个 input_stubs 里有默认实现，但对"委托式输入"来说必须显式转发，
	// 否则 foobar2000 拿不到解码器报告的动态信息（码率、采样率、流内切曲等）。
	bool decode_get_dynamic_info(file_info & p_out, double & p_timestamp_delta) {
		return m_decoder.is_valid() && m_decoder->get_dynamic_info(p_out, p_timestamp_delta);
	}
	bool decode_get_dynamic_info_track(file_info & p_out, double & p_timestamp_delta) {
		return m_decoder.is_valid() && m_decoder->get_dynamic_info_track(p_out, p_timestamp_delta);
	}
	void decode_on_idle(abort_callback & p_abort) {
		if (m_decoder.is_valid()) m_decoder->on_idle(p_abort);
	}

	void retag(const file_info &, abort_callback &) { throw exception_tagging_unsupported(); }
	void remove_tags(abort_callback &) { throw exception_tagging_unsupported(); }

	static bool g_is_our_content_type(const char *) { return false; }
	static bool g_is_our_path(const char * p_path, const char *) {
		return p_path && std::strncmp(p_path, "netease://", 10) == 0;
	}
	static const char * g_get_name() { return "Netease Cloud Music"; }
	static GUID g_get_guid() {
		// 本输入组件的 GUID。替换代码复用时必须换成自己的。
		static const GUID guid = { 0xccf6a246, 0x1f26, 0x4b7a, { 0x81, 0xc5, 0xcd, 0x8d, 0xe9, 0xdb, 0xe7, 0xe6 } };
		return guid;
	}
	static GUID g_get_preferences_guid() { return pfc::guid_null; }

private:
	int64_t m_id = 0;
	int64_t m_playlist_no = 0;
	std::string m_level;
	pfc::string8 m_path;
	service_ptr_t<input_decoder> m_decoder;
};

static input_singletrack_factory_t<netease_input> g_netease_input_factory;

// 让它出现在「打开文件」对话框的类型列表里（纯枚举用途，实际入口是浏览窗口）。
DECLARE_FILE_TYPE("Netease Cloud Music tracks", "*.netease");

// 用我们自己的 HTTP 客户端（带 Cookie 与 Referer）拉一小段，看服务端到底给的是什么。
// 目的：把"云盘资源需要鉴权"和"上传的文件格式解不了"这两种完全不同的故障分开。
// 内置 HTTP 输入不会带我们的 Cookie/Referer，所以它拿到的结果可能和这里完全不同。

} // namespace

