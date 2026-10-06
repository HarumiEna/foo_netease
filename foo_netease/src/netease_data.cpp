#include "stdafx.h"
#include "netease_data.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <utility>

#include "component_log.h"
#include "cover_cache.h"
#include "core/meta_cache.h"
#include "meta_store.h"
#include "session.h"

namespace netease_data {

namespace {

// 定义在文件后面的匿名命名空间里（同一个 TU，前向声明即可）。
bool make_api(netease::CookieJar & jar, std::unique_ptr<netease::NeteaseApi> & holder);
// 定义在后面（同一个匿名命名空间，前向声明即可）：华语漫游的筛选。
bool track_looks_chinese(const netease::TrackInfo & t);

const char * kFmPlaylist = "网易云漫游";
const size_t kFmBatch = 3;                 // /radio/get 一次就给 3 首
std::atomic<bool> g_fm_radio{ true };      // 电台开关（面板复选框）
std::atomic<bool> g_fm_extending{ false }; // 防止并发重复续批
// 这一轮电台里已经播过的歌曲 id。随机播放模式下列表位置没有参考价值，
// 判断"该不该续批/是不是重复"必须靠这个集合。
std::set<int64_t> g_fm_played;

// 播放控制绝对不能从 play_callback 里直接调用：在 on_playback_new_track 里切歌会
// 重入播放引擎 —— 新歌又触发一次 on_playback_new_track，于是递归下去直到栈溢出
//（崩溃报告里就是 /GS 栈检查失败，调用路径 app_mainloop=>...=>on_playback_new_track）。
// 统一走"先跳工作线程、再回主线程"的延后路径，保证在本次回调返回之后才执行。
std::atomic<bool> g_playback_action{ false };

// 冷却：避免"续批/跳过"互相触发形成风暴（实测过：服务器把放过的歌再发回来时，
// 每 3 秒就续一次批，既刷屏又可能被限流）。
std::atomic<int64_t> g_fm_last_extend_ms{ 0 };
std::atomic<int64_t> g_fm_last_skip_ms{ 0 };
// 上一批带来几首"没播过的"。服务器把老歌重发时这个值是 0 ——
// 说明池子暂时榨干了，这时候续批要退避得久一点，别一个劲打接口。
std::atomic<size_t> g_fm_last_batch_fresh{ 0 };

int64_t now_ms() { return static_cast<int64_t>(::GetTickCount64()); }

bool cooldown_ok(std::atomic<int64_t> & stamp, int64_t interval_ms) {
	const int64_t now = now_ms();
	const int64_t last = stamp.load();
	if (last != 0 && now - last < interval_ms) return false;
	stamp.store(now);
	return true;
}

void defer_playback(std::function<void()> action) {
	if (g_playback_action.exchange(true)) return;   // 已经有一个在排队，不叠加
	fb2k::splitTask([action = std::move(action)] {
		fb2k::inMainThread([action] {
			g_playback_action.store(false);
			try {
				action();
			} catch (const std::exception & ex) {
				netease_log::write(std::string("foo_netease: 延后的播放操作失败 —— ") + ex.what());
			} catch (...) {
				netease_log::write("foo_netease: 延后的播放操作失败（未知异常）");
			}
		});
	});
}

int64_t fm_id_from_path(const char * path) {
	const char prefix[] = "netease://song/";
	if (!path || std::strncmp(path, prefix, sizeof(prefix) - 1) != 0) return 0;
	const int64_t id = std::strtoll(path + sizeof(prefix) - 1, nullptr, 10);
	return id > 0 ? id : 0;
}

// no= 是 %netease_no%（源列表内序号）的来源 —— 缺了它列表里就是问号。
// tracknumber 走的是专辑音轨号，两者是不同字段，这里补的是前者。
std::string fm_path(int64_t id, size_t index) {
	return "netease://song/" + std::to_string(id) +
		"?level=" + netease::Session::instance().quality() +
		"&fm=1&no=" + std::to_string(index + 1);
}

bool is_fm_path(const char * path) {
	return path && std::strstr(path, "netease://song/") && std::strstr(path, "&fm=1");
}

t_size fm_playlist_index(bool create) {
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return pfc::infinite_size;
	t_size index = pm->find_playlist(kFmPlaylist);
	if (index == pfc::infinite_size && create) {
		index = pm->create_playlist(kFmPlaylist, pfc::infinite_size, pfc::infinite_size);
	}
	return index;
}

// 追加新歌 + 删掉"正在播放那首之前"的旧条目。调用者必须保证在主线程。
// new_session = true 表示用户重新选了漫游，此时重置"已播过"的记录。
void fm_apply_batch(const std::vector<netease::TrackInfo> & batch, bool new_session) {
	if (batch.empty()) return;
	if (new_session) g_fm_played.clear();
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	const t_size index = fm_playlist_index(true);
	if (index == pfc::infinite_size) return;

	// 只把"没播过的"排进列表 —— 播过的直接不收。
	// 配合下面"切歌时把播过的从列表删掉"，随机播放根本抽不到重复的歌，
	// 也就不需要"发现重复就 next() 跳过"那种会让人以为在跳歌的动作了。
	std::vector<netease::TrackInfo> use;
	use.reserve(batch.size());
	for (const netease::TrackInfo & t : batch) {
		if (!g_fm_played.count(t.id)) use.push_back(t);
	}
	const bool pool_dry = use.empty();
	if (pool_dry) {
		// 服务器只肯重发老歌：只能接受，否则没得播。记一笔，续批也会退避。
		netease_log::write("foo_netease: 漫游这一批全是已播过的歌（池子暂时榨干），本轮只能重播");
		use = batch;
	}
	g_fm_last_batch_fresh = pool_dry ? 0 : use.size();

	metadb_handle_list fresh;
	for (size_t i = 0; i < use.size(); ++i) {
		fresh.add_item(metadb::get()->handle_create(fm_path(use[i].id, i).c_str(), 0));
		// 整批的封面先预取好：这样下面几首自动播到时，封面是内存里的、立刻能显示。
		netease_cover::prefetch_async(use[i].id);
	}
	pm->playlist_add_items(index, fresh, bit_array_true());

	metadb_handle_ptr now;
	playback_control::get()->get_now_playing(now);

	metadb_handle_list all;
	pm->playlist_get_all_items(index, all);
	bit_array_bittable mask(all.get_count());
	t_size now_pos = pfc::infinite_size;
	for (t_size i = 0; i < all.get_count(); ++i) {
		if (now.is_valid() && all[i] == now) { now_pos = i; break; }
	}
	size_t dropped = 0;
	if (now_pos != pfc::infinite_size) {
		// 正在播：只把它之前的（已经播过的）删掉
		for (t_size i = 0; i < now_pos; ++i) { mask.set(i, true); ++dropped; }
	} else {
		// 没有在播的：只留下刚加进来的这一批
		const t_size keep = fresh.get_count();
		if (all.get_count() > keep) {
			for (t_size i = 0; i + keep < all.get_count(); ++i) { mask.set(i, true); ++dropped; }
		}
	}
	if (dropped) pm->playlist_remove_items(index, mask);

	// 兜底：随机播放下"正在播放"可能一直在列表最前面，前面没有可删的旧条目，
	// 列表就会慢慢变长。这里给一个硬上限，超了就从头删（仍然跳过正在播放的那首）。
	{
		const size_t kFmMax = 24;
		const t_size count = pm->playlist_get_item_count(index);
		if (count > kFmMax) {
			metadb_handle_list all2;
			pm->playlist_get_all_items(index, all2);
			bit_array_bittable mask2(all2.get_count());
			size_t to_drop = static_cast<size_t>(count) - kFmMax;
			for (t_size i = 0; i < all2.get_count() && to_drop > 0; ++i) {
				if (now.is_valid() && all2[i] == now) continue;
				mask2.set(i, true);
				--to_drop;
			}
			pm->playlist_remove_items(index, mask2);
		}
	}

	if (pm->get_active_playlist() != index) pm->set_active_playlist(index);

	auto io = metadb_io_v2::get();
	if (io.is_valid()) {
		io->load_info_async(fresh, metadb_io::load_info_force, nullptr,
			metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
	}
	netease_log::write("foo_netease: 漫游电台已更新 —— 新增 " + std::to_string(fresh.get_count()) +
		" 首，删旧 " + std::to_string(dropped) + " 首，当前 " +
		std::to_string(pm->playlist_get_item_count(index)) + " 首");
	// 打出第一条路径：%netease_no% 靠路径里的 no=，这里能一眼看出带没带。
	if (fresh.get_count() > 0) {
		const char * first = fresh[0]->get_path();
		netease_log::write(std::string("foo_netease: 漫游条目示例路径 = ") + (first ? first : "?"));
	}
}

// 后台取一小批，回到主线程写进播放列表。
void fm_extend_async() {
	if (!g_fm_radio.load()) return;
	if (g_fm_extending.exchange(true)) return;
	fb2k::splitTask([] {
		std::vector<netease::TrackInfo> batch;
		std::string error;
		netease::CookieJar jar;
		std::unique_ptr<netease::NeteaseApi> holder;
		if (!make_api(jar, holder)) {
			error = "尚未登录";
		} else {
			const std::string mode = fm_mode();
			netease::ApiCall call = holder->personal_fm(batch, kFmBatch, nullptr,
				mode.empty() ? nullptr : mode.c_str());
			// 华语模式再加一层筛选，和首次加载保持一致。
			if (call.ok && mode == "LANGUAGE_CN") {
				std::vector<netease::TrackInfo> only;
				for (const netease::TrackInfo & t : batch) {
					if (track_looks_chinese(t)) only.push_back(t);
				}
				if (!only.empty()) batch = std::move(only);
			}
			if (!call.ok) error = call.error;
		}
		fb2k::inMainThread([batch, error]() mutable {
			g_fm_extending.store(false);
			if (batch.empty()) {
				netease_log::write("foo_netease: 漫游续批失败 —— " + error);
				return;
			}
			fm_apply_batch(batch, false);
		});
	});
}

} // namespace

void set_fm_radio_enabled(bool on) { g_fm_radio.store(on); }
bool fm_radio_enabled() { return g_fm_radio.load(); }

// 开关存在 Session 里（会落盘），这里只是转发。
void set_follow_cursor_enabled(bool on) { netease::Session::instance().set_follow_cursor(on); }
bool follow_cursor_enabled() { return netease::Session::instance().follow_cursor(); }

namespace {

void follow_playing_in_playlist() {
	if (!netease::Session::instance().follow_cursor()) return;
	auto pc = playback_control::get();
	if (!pc.is_valid()) return;
	metadb_handle_ptr now;
	if (!pc->get_now_playing(now) || !now.is_valid()) return;
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	const t_size act = pm->get_active_playlist();
	if (act == pfc::infinite_size) return;
	const t_size count = pm->playlist_get_item_count(act);
	for (t_size i = 0; i < count; ++i) {
		metadb_handle_ptr h = pm->playlist_get_item_handle(act, i);
		if (!h.is_valid() || h != now) continue;
		if (pm->playlist_get_focus_item(act) != i) {
			// **先清空整个选中，再只选中这一条。**
			// 不能用 playlist_set_selection_single(i, true) —— 它只"追加"，
			// 上一首会一直保持选中，界面上就出现多首歌同时高亮（用户实测反馈）。
			// playlist_set_selection(affected, status) 的语义是
			// "把每一条的选中状态都设成 status[该条]"；affected 传"全部"即可清掉旧的。
			{
				bit_array_bittable status(count);
				status.set(i, true);
				pm->playlist_set_selection(act, bit_array_true(), status);
			}
			pm->playlist_set_focus_item(act, i);
			netease_log::write("foo_netease: 光标/选中项已跟随正在播放（第 " + std::to_string(i + 1) + " 项）");
		}
		break;
	}
}

} // namespace

void follow_playing_deferred() {
	if (!netease::Session::instance().follow_cursor()) return;
	// 播放回调里不能直接动播放列表：延后到回调之外再执行。
	fb2k::splitTask([] {
		fb2k::inMainThread([] {
			try {
				follow_playing_in_playlist();
			} catch (...) {
			}
		});
	});
}

namespace {
std::mutex g_now_mutex;
std::shared_ptr<const std::string> g_now_path;   // 只在主线程写，任意线程读
} // namespace

void set_now_playing_path(const std::string & path) {
	std::lock_guard<std::mutex> lock(g_now_mutex);
	g_now_path = path.empty() ? nullptr : std::make_shared<const std::string>(path);
}

std::string now_playing_path() {
	std::lock_guard<std::mutex> lock(g_now_mutex);
	return g_now_path ? *g_now_path : std::string();
}

size_t sync_fm_playlist(const std::vector<netease::TrackInfo> & batch) {
	if (batch.empty()) return 0;
	fm_apply_batch(batch, true);
	return batch.size();
}

void fm_radio_extend_now() { fm_extend_async(); }

void fm_radio_maybe_extend(metadb_handle_ptr track) {
	if (!g_fm_radio.load() || !track.is_valid()) return;
	const char * path = track->get_path();
	if (!is_fm_path(path)) return;
	const int64_t id = fm_id_from_path(path);
	if (id <= 0) return;

	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	const t_size index = fm_playlist_index(false);
	if (index == pfc::infinite_size) return;
	if (pm->get_active_playlist() != index) return;   // 用户没在听漫游，别插手

	// 把刚离开的那首（以及任何已经播过的）**从列表里删掉** ——
	// 列表里只剩没播过的歌，随机播放自然抽不到重复。
	// 这样就不需要"发现重复就 next() 跳过"（那会让人以为在跳歌，用户明确不要）。
	g_fm_played.insert(id);
	if (g_fm_played.size() > 4000) g_fm_played.clear();   // 兜底，别无限涨

	metadb_handle_list all;
	pm->playlist_get_all_items(index, all);
	bit_array_bittable drop(all.get_count());
	size_t unplayed_others = 0;
	size_t removed = 0;
	bool found_current = false;
	for (t_size i = 0; i < all.get_count(); ++i) {
		const int64_t hid = fm_id_from_path(all[i]->get_path());
		if (hid == 0) continue;
		const bool is_current = (all[i] == track);   // 正在播放的那首永不删
		if (is_current) found_current = true;
		if (!is_current && g_fm_played.count(hid)) {
			drop.set(i, true);
			++removed;
		} else if (!is_current && !g_fm_played.count(hid)) {
			++unplayed_others;
		}
	}
	// 安全检查：列表里找不到"正在播放"那一条时，千万不要删 ——
	// 那样可能把它当成"播过的"删掉，fb2k 只好立刻跳下一首
	//（表现就是"每 1 秒自动切歌"，实测踩过）。
	if (!found_current) {
		netease_log::write("foo_netease: 漫游列表里没找到正在播放的条目（路径可能不一致），本次不删");
	} else if (removed) {
		pm->playlist_remove_items(index, drop);
		netease_log::write("foo_netease: 漫游已从列表删除播过的 " + std::to_string(removed) +
			" 首（剩 " + std::to_string(unplayed_others) + " 首没播）");
	}

	if (unplayed_others <= 1) {
		// 冷却：拿到新歌时 3 秒（用户要求，手动连切时也能很快补上新歌）；
		// 如果上一批全是"重发的老歌"，说明池子暂时榨干了，退避到 10 秒，
		// 免得每 3 秒打一次 /radio/get。
		const int64_t interval = (g_fm_last_batch_fresh.load() == 0) ? 10000 : 3000;
		if (!cooldown_ok(g_fm_last_extend_ms, interval)) return;
		netease_log::write("foo_netease: 漫游本批快放完了（还有 " + std::to_string(unplayed_others) +
			" 首没播），自动续下一批");
		fm_extend_async();
	}
}

void fm_radio_on_eof() {
	if (!g_fm_radio.load()) return;
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	const t_size index = fm_playlist_index(false);
	if (index == pfc::infinite_size) return;
	if (pm->get_active_playlist() != index) return;
	netease_log::write("foo_netease: 漫游列表播放结束，续一批并继续");
	fm_extend_async();
	// 同样必须延后：这会在 on_playback_stop 里被调用。
	defer_playback([] { playback_control::get()->start(playback_control::track_command_play, false); });
}

int64_t parse_playlist_link(const std::string & text) {
	// netease://playlist/<id>
	const std::string prefix = "netease://playlist/";
	if (text.compare(0, prefix.size(), prefix) == 0) {
		const int64_t id = std::strtoll(text.c_str() + prefix.size(), nullptr, 10);
		return id > 0 ? id : 0;
	}
	// 网页链接：必须同时出现 music.163.com 与 playlist，再取 id=<数字>
	if (text.find("music.163.com") == std::string::npos) return 0;
	if (text.find("playlist") == std::string::npos) return 0;
	const size_t at = text.find("id=");
	if (at == std::string::npos) return 0;
	const int64_t id = std::strtoll(text.c_str() + at + 3, nullptr, 10);
	return id > 0 ? id : 0;
}

size_t insert_tracks(const std::vector<netease::TrackInfo> & tracks, bool replace) {
	// 防御：正常歌单最多几千首。真出现异常大的数量，宁可报错也不要让
	// 上层无限增长（曾经因为选中项遍历写错，chosen 无限膨胀到 bad_alloc 崩掉宿主）。
	if (tracks.size() > 20000) throw exception_io_data("曲目数量异常（超过 20000），已中止");

	metadb_handle_list handles;
	const std::string level = netease::Session::instance().quality();
	for (size_t i = 0; i < tracks.size(); ++i) {
		// no= 是源歌单内序号（从 1 开始）。写进路径，条目身份就带着它，
		// 本地排序不会改动它，用户可按 %netease_no% 还原歌单原顺序。
		const std::string path = "netease://song/" + std::to_string(tracks[i].id) +
			"?level=" + level + "&no=" + std::to_string(i + 1);
		handles.add_item(metadb::get()->handle_create(path.c_str(), 0));
	}
	if (handles.get_count() == 0) return 0;

	auto pm = playlist_manager::get();
	// 别把曲目塞进"网易云漫游"—— 那是漫游电台专用的列表，
	// 而且电台会把它设为当前列表，于是"发送到当前播放列表"会误伤它。
	// 这种情况下改用第一个（默认）播放列表。
	{
		const t_size fm = pm->find_playlist("网易云漫游");
		if (fm != pfc::infinite_size && pm->get_active_playlist() == fm) {
			if (pm->get_playlist_count() > 0 && pm->get_active_playlist() != 0) {
				pm->set_active_playlist(0);
			}
		}
	}
	if (replace) pm->activeplaylist_clear();
	pm->activeplaylist_add_items(handles, bit_array_false());
	netease_log::write("foo_netease: 已写入播放列表 " + std::to_string(handles.get_count()) +
		" 项（" + std::string(replace ? "替换" : "追加") + "）");

	// 关键一步：让 foobar2000 去读元数据。
	// 只插入句柄的话，metadb 可能一直沿用「空」的缓存，播放列表里就只剩一串数字。
	auto io = metadb_io_v2::get();
	if (io.is_valid()) {
		io->load_info_async(handles, metadb_io::load_info_force, nullptr,
			metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
	}
	return handles.get_count();
}

namespace {

bool make_api(netease::CookieJar & jar, std::unique_ptr<netease::NeteaseApi> & holder) {
	jar.deserialize(netease::Session::instance().cookie_header());
	if (!jar.has("MUSIC_U")) return false;
	holder = std::make_unique<netease::NeteaseApi>(jar);
	return true;
}

template <typename T>
void post_result(LivenessPtr alive, std::function<void(T)> done, T result) {
	if (!alive->alive) return;
	fb2k::inMainThread([alive, done, result]() mutable {
		if (!alive->alive) return;
		done(std::move(result));
	});
}

// 歌单 id -> 曲目。用 trackIds 全集（tracks 会被服务端截断），再分批补元数据。
FeedResult load_playlist_tracks(netease::NeteaseApi & api, int64_t playlist_id) {
	FeedResult r;
	std::vector<int64_t> ids;
	std::string name;
	int64_t track_count = -1;
	netease::ApiCall detail = api.playlist_track_ids(playlist_id, ids, name, track_count);
	if (!detail.ok) { r.error = "取歌单详情失败：" + detail.error; return r; }

	std::vector<int64_t> missing;
	netease::ApiCall songs = api.song_details(ids, r.tracks, &missing);
	if (!songs.ok) { r.error = "取曲目详情失败：" + songs.error; return r; }

	netease::MetaCache::instance().put_all(r.tracks);
	netease_app::save_meta_cache();

	r.title = (name.empty() ? ("歌单 " + std::to_string(playlist_id)) : name) +
		"：" + std::to_string(ids.size()) + " 首";
	if (track_count >= 0 && static_cast<size_t>(track_count) != ids.size()) {
		r.title += "（服务端 trackCount=" + std::to_string(track_count) + "）";
	}
	if (!missing.empty()) r.title += "，有 " + std::to_string(missing.size()) + " 首取不到元数据";
	r.ok = true;
	return r;
}

// 公共外壳：建 api -> 跑 body -> 投回主线程。body 返回 FeedResult。
void run_async(LivenessPtr alive, std::function<void(FeedResult)> done,
	std::function<FeedResult(netease::NeteaseApi &)> body) {
	fb2k::splitTask([alive, done, body] {
		FeedResult r;
		netease::CookieJar jar;
		std::unique_ptr<netease::NeteaseApi> holder;
		if (!make_api(jar, holder)) {
			r.error = "尚未登录（或本地凭据里没有 MUSIC_U）";
			post_result(alive, done, std::move(r));
			return;
		}
		try {
			r = body(*holder);
		} catch (const std::exception & ex) {
			r.ok = false;
			r.error = std::string("内部错误：") + ex.what();
		}
		post_result(alive, done, std::move(r));
	});
}

} // namespace

void load_playlist_link_async(LivenessPtr alive, const std::string & link,
	std::function<void(FeedResult)> done) {
	const int64_t id = parse_playlist_link(link);
	if (id <= 0) {
		FeedResult r;
		r.error = "这不是能识别的网易云歌单链接";
		if (alive->alive) fb2k::inMainThread([alive, done, r]() mutable { if (alive->alive) done(std::move(r)); });
		return;
	}
	load_playlist_tracks_async(alive, id, done);
}

void load_playlists_async(LivenessPtr alive, std::function<void(PlaylistsResult)> done) {
	fb2k::splitTask([alive, done] {
		PlaylistsResult r;
		netease::CookieJar jar;
		std::unique_ptr<netease::NeteaseApi> holder;
		if (!make_api(jar, holder)) {
			r.error = "尚未登录（或本地凭据里没有 MUSIC_U）";
			post_result(alive, done, std::move(r));
			return;
		}
		netease::ApiCall account = holder->account(r.uid, r.nickname);
		if (!account.ok) {
			r.error = "取账号信息失败：" + account.error;
			post_result(alive, done, std::move(r));
			return;
		}
		const int page = 100;
		std::vector<int64_t> seen;   // 服务端分页可能重复返回，按 id 去过重
		for (int offset = 0; offset < 2000; offset += page) {
			std::vector<netease::PlaylistInfo> chunk;
			netease::ApiCall call = holder->user_playlists(r.uid, page, offset, chunk);
			if (!call.ok) {
				r.error = "取歌单失败：" + call.error;
				post_result(alive, done, std::move(r));
				return;
			}
			for (netease::PlaylistInfo & p : chunk) {
				if (std::find(seen.begin(), seen.end(), p.id) != seen.end()) continue;
				seen.push_back(p.id);
				r.items.push_back(std::move(p));
			}
			if (static_cast<int>(chunk.size()) < page) break;
		}
		netease_log::write("foo_netease: 歌单列表已加载 " + std::to_string(r.items.size()) +
			" 个（账号 " + r.nickname + "）");
		r.ok = true;
		post_result(alive, done, std::move(r));
	});
}

void load_daily_async(LivenessPtr alive, std::function<void(FeedResult)> done) {
	run_async(alive, done, [](netease::NeteaseApi & api) {
		FeedResult r;
		netease::ApiCall call = api.daily_recommend(r.tracks);
		if (!call.ok) { r.error = "日推失败：" + call.error; return r; }
		netease::MetaCache::instance().put_all(r.tracks);
		netease_app::save_meta_cache();
		r.title = "每日推荐：" + std::to_string(r.tracks.size()) + " 首";
		r.ok = true;
		return r;
	});
}

void load_fm_async(LivenessPtr alive, std::function<void(FeedResult)> done) {
	// 一次拿 30 首（内部会多次请求 /radio/get 累积）。
	run_async(alive, done, [](netease::NeteaseApi & api) {
		FeedResult r;
		// 一次就要一批（3 首）—— 这是"漫游只保留他有的几首"的前提。
		netease::ApiCall call = api.personal_fm(r.tracks, 3);
		if (!call.ok) { r.error = "漫游失败：" + call.error; return r; }
		netease::MetaCache::instance().put_all(r.tracks);
		netease_app::save_meta_cache();
		r.title = "漫游（私人 FM）：" + std::to_string(r.tracks.size()) +
			" 首（播完自动换下一批）";
		r.ok = true;
		return r;
	});
}

namespace {

std::mutex g_fm_mode_mutex;
std::string g_fm_mode;   // "" 或 "LANGUAGE_CN"

// 粗略判断"华语"：含汉字，且不含假名/谚文。
// 为什么要排假名：日语歌名大量使用汉字（实测「霙」「光の道標」），
// 只看"有没有汉字"会把日语歌全收进来。
bool utf8_has_han_without_kana(const std::string & s) {
	bool has_han = false;
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		uint32_t cp = 0;
		int len = 1;
		if (c < 0x80) { cp = c; len = 1; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
		else { ++i; continue; }
		for (int k = 1; k < len && i + k < s.size(); ++k) {
			cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
		}
		i += len;
		if (cp >= 0x3040 && cp <= 0x30FF) return false;    // 平假名 / 片假名
		if (cp >= 0xAC00 && cp <= 0xD7AF) return false;    // 谚文
		if (cp >= 0x4E00 && cp <= 0x9FFF) has_han = true;  // 汉字
	}
	return has_han;
}

bool track_looks_chinese(const netease::TrackInfo & t) {
	return utf8_has_han_without_kana(t.title) || utf8_has_han_without_kana(t.artists);
}

} // namespace

void set_fm_mode(const std::string & mode) {
	std::lock_guard<std::mutex> lock(g_fm_mode_mutex);
	g_fm_mode = mode;
}

std::string fm_mode() {
	std::lock_guard<std::mutex> lock(g_fm_mode_mutex);
	return g_fm_mode;
}

namespace {
std::mutex g_recent_mutex;
std::deque<int64_t> g_recent_local;   // 最近播的在最前面

std::string recent_file_path() {
	const std::string dir = netease_log::profile_dir();
	return dir.empty() ? std::string() : dir + "\\foo_netease_recent.txt";
}
} // namespace

void recent_local_load() {
	const std::string path = recent_file_path();
	if (path.empty()) return;
	std::ifstream in(path.c_str());
	if (!in) return;
	std::string line;
	std::lock_guard<std::mutex> lock(g_recent_mutex);
	while (std::getline(in, line) && g_recent_local.size() < 500) {
		const int64_t id = std::strtoll(line.c_str(), nullptr, 10);
		if (id > 0) g_recent_local.push_back(id);
	}
	netease_log::write("foo_netease: 本机播放记录已载入 —— " + std::to_string(g_recent_local.size()) + " 首");
}

void recent_local_save() {
	const std::string path = recent_file_path();
	if (path.empty()) return;
	std::vector<int64_t> ids;
	{
		std::lock_guard<std::mutex> lock(g_recent_mutex);
		ids.assign(g_recent_local.begin(), g_recent_local.end());
	}
	std::ofstream out(path.c_str(), std::ios::trunc);
	for (int64_t id : ids) out << id << "\n";
}

void recent_local_add(int64_t song_id) {
	if (song_id <= 0) return;
	{
		std::lock_guard<std::mutex> lock(g_recent_mutex);
		g_recent_local.erase(std::remove(g_recent_local.begin(), g_recent_local.end(), song_id),
			g_recent_local.end());
		g_recent_local.push_front(song_id);
		while (g_recent_local.size() > 500) g_recent_local.pop_back();
	}
	recent_local_save();   // 每切一首写一次；文件就几百个数字，开销可以忽略
}

std::vector<int64_t> recent_local_list() {
	std::lock_guard<std::mutex> lock(g_recent_mutex);
	return std::vector<int64_t>(g_recent_local.begin(), g_recent_local.end());
}

void load_recent_async(LivenessPtr alive, std::function<void(FeedResult)> done) {
	run_async(alive, done, [](netease::NeteaseApi & api) {
		FeedResult r;
		int64_t uid = netease::Session::instance().account().uid;
		if (uid == 0) {
			// 启动时是"从本地恢复凭据"，Session 里可能只有登录态、没有账号信息 ——
			// 这里自己问一次账号接口，别让用户先去刷新歌单。
			std::string nickname;
			netease::ApiCall acc = api.account(uid, nickname);
			if (!acc.ok || uid == 0) {
				r.error = "拿不到账号 uid：" + (acc.error.empty() ? "账号接口没返回 userId" : acc.error);
				return r;
			}
		}
		// 先用客户端自己的接口（一次 300 首，实测裸参数可用）；
		// 失败再退回旧的 weapi /v1/play/record（100 首）。
		netease::ApiCall call = api.recent_played_v2(300, r.tracks);
		if (!call.ok || r.tracks.empty()) {
			const std::string first_error = call.error;
			call = api.recent_played(uid, 100, 0, r.tracks);
			if (!call.ok) {
				r.error = "最近播放失败：" + (first_error.empty() ? call.error : first_error + "；退回旧接口也失败：" + call.error);
				return r;
			}
		}
		if (r.tracks.empty()) { r.error = "最近播放是空的（服务端还没记录）"; return r; }
		netease::MetaCache::instance().put_all(r.tracks);
		netease_app::save_meta_cache();
		// 把本机播过的并到最前面（服务端那份不认 foobar2000 的播放）。
		size_t local_added = 0;
		{
			const std::vector<int64_t> local = recent_local_list();
			std::vector<netease::TrackInfo> merged;
			std::set<int64_t> taken;
			for (int64_t id : local) {
				if (!taken.insert(id).second) continue;
				netease::TrackInfo t;
				if (!netease::MetaCache::instance().get(id, t)) continue;
				merged.push_back(t);
				++local_added;
			}
			for (netease::TrackInfo & t : r.tracks) {
				if (taken.insert(t.id).second) merged.push_back(t);
			}
			r.tracks = std::move(merged);
		}
		r.title = "最近播放：" + std::to_string(r.tracks.size()) + " 首" +
			(local_added ? "（其中本机播放 " + std::to_string(local_added) + " 首）" : "");
		r.ok = true;
		return r;
	});
}

void load_cn_roam_async(LivenessPtr alive, std::function<void(FeedResult)> done) {
	run_async(alive, done, [](netease::NeteaseApi & api) {
		// 「华语漫游」= 客户端「华语旗舰」分栏里的 24h 频道卡。
		// 实测确认它是个内部 tag，公开接口拿不到。
		// 用户拍板：改用官方资源「华语私人雷达」，id 2829883282
		//（https://music.163.com/playlist?id=2829883282，每日更新 35 首）。
		const int64_t kCnRadarId = 2829883282;
		FeedResult r = load_playlist_tracks(api, kCnRadarId);
		if (!r.ok || r.tracks.empty()) {
			// 兜底：按名字在精选资源里再找一次。
			std::string name;
			int64_t id = 0;
			netease::ApiCall call = api.featured_playlist("华语", "华语私人雷达", kCnRadarId, name, id);
			if (call.ok && id != 0) r = load_playlist_tracks(api, id);
		}
		if (r.ok) {
			r.title = "华语私人雷达：" + std::to_string(r.tracks.size()) + " 首";
			netease_log::write("foo_netease: 华语私人雷达 —— " + std::to_string(r.tracks.size()) + " 首");
		} else if (r.error.empty()) {
			r.error = "华语私人雷达没取到曲目";
		}
		return r;
	});
}
void load_fm_more_async(LivenessPtr alive, const std::vector<int64_t> & exclude,
	std::function<void(FeedResult)> done) {
	run_async(alive, done, [exclude](netease::NeteaseApi & api) {
		FeedResult r;
		netease::ApiCall call = api.personal_fm(r.tracks, 3, &exclude);
		if (!call.ok) { r.error = "漫游失败：" + call.error; return r; }
		if (r.tracks.empty()) { r.error = "漫游暂时没有新歌了"; return r; }
		netease::MetaCache::instance().put_all(r.tracks);
		netease_app::save_meta_cache();
		r.title = "漫游：又追加 " + std::to_string(r.tracks.size()) + " 首";
		r.ok = true;
		return r;
	});
}

void load_radar_async(LivenessPtr alive, std::function<void(FeedResult)> done) {
	run_async(alive, done, [](netease::NeteaseApi & api) {
		std::string name;
		int64_t id = 0;
		netease::ApiCall call = api.private_radar_playlist(name, id);
		if (!call.ok || id == 0) {
			FeedResult r;
			r.error = "私人雷达定位失败：" + (call.error.empty() ? "没有拿到歌单 id" : call.error);
			return r;
		}
		FeedResult r = load_playlist_tracks(api, id);
		if (r.ok) r.title = name + "：" + std::to_string(r.tracks.size()) + " 首";
		return r;
	});
}

void load_playlist_tracks_async(LivenessPtr alive, int64_t playlist_id,
	std::function<void(FeedResult)> done) {
	run_async(alive, done, [playlist_id](netease::NeteaseApi & api) {
		return load_playlist_tracks(api, playlist_id);
	});
}

void search_async(LivenessPtr alive, const std::string & keyword, int offset,
	std::function<void(FeedResult)> done) {
	run_async(alive, done, [keyword, offset](netease::NeteaseApi & api) {
		FeedResult r;
		// 一次搜索最多先拿 300 首（3 页）；想要更多就用面板的「更多」继续，
		// 免得一次点下去打十几个请求。
		const size_t kWant = 300;
		// 已经出现过的 id 去重（接口翻页偶尔会重复）。
		std::set<int64_t> seen;
		int total = 0;
		int cursor = offset;
		bool maybe_more = false;
		netease::ApiCall call;
		while (r.tracks.size() < kWant) {
			std::vector<netease::TrackInfo> page;
			call = api.search_songs(keyword, 100, cursor, page, &total);
			if (!call.ok) break;
			if (page.empty()) break;
			for (netease::TrackInfo & t : page) {
				if (t.id == 0 || !seen.insert(t.id).second) continue;
				r.tracks.push_back(t);
			}
			cursor += static_cast<int>(page.size());
			// 只有"这一页不满"才算到底。**不能用 result.songCount 判断** ——
			// 实测它不可信（报"共 330 首"，实际 400、500 首还有）。
			maybe_more = page.size() >= 100;
			if (!maybe_more) break;
		}
		if (!call.ok && r.tracks.empty()) {
			r.error = "搜索失败：" + call.error;
			return r;
		}
		if (r.tracks.empty()) {
			if (offset > 0) {
				// 翻到底了（ok 保持 false，面板只显示提示、不动播放列表）。
				r.error = "没有更多搜索结果了（已载入 " + std::to_string(offset) + " 首）";
				return r;
			}
			r.title = "搜索「" + keyword + "」：没有找到结果";
			r.ok = true;
			return r;
		}
		netease::MetaCache::instance().put_all(r.tracks);
		netease_app::save_meta_cache();
		std::string title = "搜索「" + keyword + "」：";
		title += (offset > 0) ? ("又加 " + std::to_string(r.tracks.size()) + " 首")
			: ("已载入 " + std::to_string(r.tracks.size()) + " 首");
		title += "（已到第 " + std::to_string(cursor) + " 首";
		title += maybe_more ? "，点「更多」继续）" : "，已经是全部）";
		if (total > 0 && total != cursor) {
			// 接口自报的总数经常不准，只在日志里留个痕迹，不显示给用户。
			netease_log::write("foo_netease: 搜索接口自报 songCount=" + std::to_string(total) +
				"，实际已取 " + std::to_string(cursor) + " 首（该字段不可信，不参与判断）");
		}
		r.title = title;
		r.ok = true;
		return r;
	});
}

} // namespace netease_data

