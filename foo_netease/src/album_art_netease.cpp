#include "stdafx.h"

#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "helpers/album_art_helpers.h"

#include "component_log.h"
#include "core/api.h"
#include "core/http.h"
#include "core/meta_cache.h"
#include "cover_cache.h"
#include "netease_data.h"
#include "session.h"

// 封面（album art）。
//
// 为什么做成 album_art_fallback 而不是普通的 album_art_extractor：
// 网易云的封面不在文件里，而是一个 CDN 图片地址（元数据里的 al.picUrl）。
// foobar2000 的 fallback 接口正是在"内置/外挂/其它来源都拿不到图"时被调用的，
// 拿到 metadb 句柄后我们可以自己决定怎么去取图 —— 正好用来做在线查封面。
//
// 另外有一层内存缓存 + 播放开始时预取（见 cover_cache.h 的注释）：
// 切歌会取消未完成的封面请求，不预取的话封面就会停在旧图上。

namespace {

bool is_cover_type(const GUID & what) {
	return what == album_art_ids::cover_front || what == album_art_ids::cover_back;
}

// 从 netease://song/<id>... 里取歌曲 id；不是我们的路径返回 0。
int64_t parse_song_id(const char * path) {
	const char prefix[] = "netease://song/";
	if (!path) return 0;
	if (std::strncmp(path, prefix, sizeof(prefix) - 1) != 0) return 0;
	const int64_t id = std::strtoll(path + sizeof(prefix) - 1, nullptr, 10);
	return id > 0 ? id : 0;
}

// 取封面图字节。走我们自己的 HTTP 层（带登录 Cookie / Referer），
// abort 贯穿，用户取消时不会白等满超时。
bool fetch_cover_bytes(const std::string & url, std::string & bytes, abort_callback & abort) {
	netease::CookieJar jar;
	jar.deserialize(netease::Session::instance().cookie_header());
	netease::HttpRequestOptions options;
	options.timeout_ms = 20000;
	options.headers.push_back({ "Referer", "https://music.163.com" });
	options.headers.push_back({ "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" });
	options.abort_requested = [&abort] { return abort.is_aborting(); };

	netease::HttpResult result = netease::http_get(url, jar, options);
	if (!result.ok) {
		netease_log::write("foo_netease: 封面下载失败 —— " + result.error);
		return false;
	}
	bytes = std::move(result.response.body);
	return !bytes.empty();
}

// 查封面地址：先看元数据缓存，未命中再补一次接口（和 input 的 get_info 同一条路径）。
std::string cover_url_for(int64_t id, abort_callback & abort) {
	netease::TrackInfo track;
	if (netease::MetaCache::instance().get(id, track) && !track.cover_url.empty()) return track.cover_url;

	netease::CookieJar jar;
	jar.deserialize(netease::Session::instance().cookie_header());
	auto api = std::make_unique<netease::NeteaseApi>(jar, 20000);
	api->set_abort([&abort] { return abort.is_aborting(); });
	std::vector<int64_t> ids{ id };
	std::vector<netease::TrackInfo> out;
	netease::ApiCall call = api->song_details(ids, out);
	if (call.ok && !out.empty()) {
		netease::MetaCache::instance().put(out[0]);
		return out[0].cover_url;
	}
	return std::string();
}

// 一次封面查询的实例：真正下载发生在 query() 里，foobar2000 需要图时才会调用。
class netease_cover_instance : public album_art_extractor_instance_v2 {
public:
	// follow_now = true 时**不把封面定死在 open() 那一刻**：
	// 每次 query() 都重新看"现在在放什么"。因为 foobar2000 会按条目缓存
	// 提取器实例，而"封面跟随正在播放"返回的图与条目无关 ——
	// 定死的话切歌后同一个实例会一直吐旧图（用户实测：切歌不切换，
	// 要再点一下界面触发重新请求才更新）。
	netease_cover_instance(int64_t id, std::string url, bool follow_now)
		: m_id(id), m_url(std::move(url)), m_follow_now(follow_now) {}

	// 按 id 出图：先缓存、再下载，被取消就等一下预取。
	album_art_data_ptr query_for(int64_t id, abort_callback & p_abort) {
		if (id <= 0) throw exception_album_art_not_found();
		{
			std::string cached;
			if (netease_cover::get(id, cached)) {
				return album_art_data_impl::g_create(cached.data(), cached.size());
			}
		}
		if (id != m_id) {
			m_id = id;
			m_url = cover_url_for(id, p_abort);
			m_bytes.clear();
			m_cached = false;
		}
		if (m_url.empty()) throw exception_album_art_not_found();
		p_abort.check();
		if (!m_cached) {
			std::string bytes;
			if (fetch_cover_bytes(m_url, bytes, p_abort)) {
				netease_cover::put(m_id, bytes);
				m_bytes = std::move(bytes);
				m_cached = true;
			} else if (netease_cover::wait_for(m_id, bytes, 4000)) {
				m_bytes = std::move(bytes);
				m_cached = true;
			} else {
				throw exception_album_art_not_found();
			}
		}
		return album_art_data_impl::g_create(m_bytes.data(), m_bytes.size());
	}

	// 当前该显示哪首歌的封面：跟随正在播放时每次重新取。
	int64_t target_id() const {
		if (!m_follow_now) return m_id;
		const std::string now = netease_data::now_playing_path();
		const int64_t now_id = parse_song_id(now.c_str());
		return now_id > 0 ? now_id : m_id;
	}

	album_art_data_ptr query(const GUID & what, abort_callback & p_abort) override {
		if (!is_cover_type(what)) throw exception_album_art_not_found();
		return query_for(target_id(), p_abort);
	}

	album_art_path_list::ptr query_paths(const GUID & what, abort_callback & p_abort) override {
		if (!is_cover_type(what)) throw exception_album_art_not_found();
		const int64_t id = target_id();
		if (id <= 0) throw exception_album_art_not_found();
		std::string url = m_url;
		if (id != m_id || url.empty()) url = cover_url_for(id, p_abort);
		if (url.empty()) throw exception_album_art_not_found();
		return fb2k::service_new<album_art_path_list_impl>(url.c_str());
	}

private:
	int64_t m_id = 0;
	std::string m_url;
	std::string m_bytes;      // 下载到的图片字节
	bool m_cached = false;
	bool m_follow_now = false;
};

class netease_album_art_fallback : public album_art_fallback {
public:
	album_art_extractor_instance_v2::ptr open(metadb_handle_list_cref items,
		pfc::list_base_const_t<GUID> const & ids, abort_callback & p_abort) override {
		// 兜底：这个函数跑在封面加载线程，抛任何非 foobar2000 的异常都可能直接把进程带走，
		// 所以统一收敛成"没有封面"。
		try {
		// 取哪首歌的封面有两个来源：
		//  1) 设置里打开"封面跟随正在播放"时，用正在播放的那首
		//     —— 否则封面会跟着列表里鼠标选中的条目跑；
		//  2) 否则用调用方给的曲目（foobar2000 自己的规则）。
		// 注意：这里**不能**调用 playback_control::get_now_playing()。
		// open() 跑在"专辑封面加载线程"上，那样会让进程崩掉
		//（实测崩溃：专辑封面加载线程=>album_art_manager_v2::open，函数栈检查失败）。
		// 正在播放的路径由 play_callback 在主线程写好，这里只读缓存。
		const bool follow_now = netease::Session::instance().cover_follow_now_playing();
		int64_t id = 0;
		if (follow_now) {
			const std::string now = netease_data::now_playing_path();
			id = parse_song_id(now.c_str());
		}
		if (id <= 0) {
			for (t_size i = 0; i < items.get_count(); ++i) {
				id = parse_song_id(items[i]->get_path());
				if (id > 0) break;
			}
		}
		if (id <= 0) throw exception_album_art_not_found();

		bool want_cover = false;
		for (t_size i = 0; i < ids.get_count(); ++i) {
			if (is_cover_type(ids[i])) { want_cover = true; break; }
		}
		if (!want_cover) throw exception_album_art_not_found();

		const std::string url = cover_url_for(id, p_abort);
		if (url.empty()) throw exception_album_art_not_found();

		// 取证用：把"请求方给的条目"也打出来 —— 自动切歌时到底有没有人来要图，
		// 一眼就能看出来（用户反馈：自动切歌要交互一下封面才换）。
		{
			std::string asked = "无";
			if (items.get_count() > 0) {
				const char * first = items[0]->get_path();
				asked = std::string("共 ") + std::to_string(items.get_count()) + " 条，首条=" +
					(first ? first : "(null)");
			}
			netease_log::write("foo_netease: 封面请求 —— 请求方给的是[" + asked + "]，跟随正在播放=" +
				(follow_now ? "是" : "否") + "，最终用 id=" + std::to_string(id) + " -> " + url +
				(follow_now ? "（实例动态，query 时重取）" : ""));
		}
		return fb2k::service_new<netease_cover_instance>(id, url, follow_now);
		} catch (const exception_album_art_not_found &) {
			throw;   // foobar2000 自己的"没有封面"，照常传递
		} catch (const std::exception & ex) {
			netease_log::write(std::string("foo_netease: 封面回退异常 —— ") + ex.what());
			throw exception_album_art_not_found();
		} catch (...) {
			netease_log::write("foo_netease: 封面回退遇到未知异常");
			throw exception_album_art_not_found();
		}
	}
};

FB2K_SERVICE_FACTORY(netease_album_art_fallback);

} // namespace

// 封面字节缓存 + 预取（声明见 cover_cache.h）

namespace netease_cover {

namespace {

const size_t kMaxBytes = 32u * 1024u * 1024u;   // 最多缓存 32 MB 封面

std::mutex g_mutex;
std::unordered_map<int64_t, std::shared_ptr<const std::string>> g_bytes;
std::list<int64_t> g_order;          // 先进先出，用来淘汰
size_t g_total = 0;
std::set<int64_t> g_inflight;        // 正在后台下载的

void evict_locked() {
	while (g_total > kMaxBytes && !g_order.empty()) {
		const int64_t victim = g_order.front();
		g_order.pop_front();
		auto it = g_bytes.find(victim);
		if (it == g_bytes.end()) continue;
		g_total -= it->second->size();
		g_bytes.erase(it);
	}
}

} // namespace

bool get(int64_t id, std::string & bytes) {
	if (id <= 0) return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_bytes.find(id);
	if (it == g_bytes.end()) return false;
	bytes = *it->second;
	return true;
}

void put(int64_t id, const std::string & bytes) {
	if (id <= 0 || bytes.empty()) return;
	std::lock_guard<std::mutex> lock(g_mutex);
	auto old = g_bytes.find(id);
	if (old != g_bytes.end()) {
		g_total -= old->second->size();
		g_order.remove(id);
	}
	auto copy = std::make_shared<const std::string>(bytes);
	g_total += copy->size();
	g_bytes[id] = copy;
	g_order.push_back(id);
	evict_locked();
}

size_t count() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_bytes.size();
}

void nudge_ui_repaint() {
	fb2k::splitTask([] {
		fb2k::inMainThread([] {
			if (HWND main = core_api::get_main_window()) {
				// UPDATENOW：立刻重画，而不是等下一次 WM_PAINT ——
				// 目的就是逼封面元素重新取一次图。
				::RedrawWindow(main, nullptr, nullptr,
					RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
			}
		});
	});
}

bool wait_for(int64_t id, std::string & bytes, int timeout_ms) {
	if (id <= 0) return false;
	const int64_t deadline = static_cast<int64_t>(::GetTickCount64()) + timeout_ms;
	for (;;) {
		if (get(id, bytes)) return true;
		bool busy = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			busy = g_inflight.count(id) > 0;
		}
		if (!busy) return false;   // 没人正在下载，别白等
		if (static_cast<int64_t>(::GetTickCount64()) >= deadline) return false;
		::Sleep(30);
	}
}

void prefetch_async(int64_t id) {
	if (id <= 0) return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_bytes.count(id)) return;
		if (!g_inflight.insert(id).second) return;
	}
	fb2k::splitTask([id] {
		std::string bytes;
		abort_callback_dummy no_abort;
		if (!netease_cover::get(id, bytes)) {
			const std::string url = cover_url_for(id, no_abort);
			if (!url.empty() && fetch_cover_bytes(url, bytes, no_abort)) {
				netease_cover::put(id, bytes);
				netease_log::write("foo_netease: 封面已预取 id=" + std::to_string(id) +
					"（" + std::to_string(bytes.size()) + " 字节，缓存 " +
					std::to_string(netease_cover::count()) + " 张）");
				// 封面真的备好了，这时候推重绘才有意义：
				// 太早推的话元素重画出来的还是上一张。
				nudge_ui_repaint();
			}
		}
		std::lock_guard<std::mutex> lock(g_mutex);
		g_inflight.erase(id);
	});
}

} // namespace netease_cover

