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
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <unordered_map>
#include <utility>

#include "component_log.h"
#include "cover_cache.h"
#include "win_utf8.h"
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

// 把一批漫游曲目写进「网易云漫游」播放列表。调用者必须保证在主线程。
// new_session = true（用户重新选了漫游，即双击面板上的漫游）：**整片替换** ——
//   列表里只留这一批（正在播放的那首除外），旧会话剩下的歌全部清掉。
// new_session = false（「更多」按钮 / 自动续批）：**追加**，并删掉确实播过的旧条目。
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
	(void)now_pos;
	if (new_session) {
		// 重新选漫游 = 新会话：只留下刚取回来的这一批，旧会话的歌整片清掉。
		// （正在播放的那首保留 —— 删了它会立刻打断播放。）
		// 之前这里只重置 g_fm_played、不删旧条目，于是双击漫游看起来变成了"追加"。
		for (t_size i = 0; i < all.get_count(); ++i) {
			if (now.is_valid() && all[i] == now) continue;
			bool in_fresh = false;
			for (t_size j = 0; j < fresh.get_count(); ++j) {
				if (all[i] == fresh[j]) { in_fresh = true; break; }
			}
			if (!in_fresh) { mask.set(i, true); ++dropped; }
		}
	} else {
		// 「更多」/自动续批：只删确实播过的（g_fm_played 里记着），不按位置删。
		// 以前的做法是"把正在播放之前的都删掉"，假定那些都播过了 ——
		// 但用户手动往后跳时，前面往往还有没听过的歌，会被一起删掉（已修）。
		for (t_size i = 0; i < all.get_count(); ++i) {
			if (now.is_valid() && all[i] == now) continue;
			const int64_t hid = fm_id_from_path(all[i]->get_path());
			if (hid != 0 && g_fm_played.count(hid)) { mask.set(i, true); ++dropped; }
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
			// 先删播过的；还不够再从头删（跳过正在播放那首）。这样优先扔掉听过的。
			for (t_size i = 0; i < all2.get_count() && to_drop > 0; ++i) {
				if (now.is_valid() && all2[i] == now) continue;
				const int64_t hid = fm_id_from_path(all2[i]->get_path());
				if (hid != 0 && g_fm_played.count(hid)) { mask2.set(i, true); --to_drop; }
			}
			for (t_size i = 0; i < all2.get_count() && to_drop > 0; ++i) {
				if (now.is_valid() && all2[i] == now) continue;
				if (mask2.get(i)) continue;
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

namespace {

// 每首歌"能切换哪些档位"的缓存。右键菜单同步构建，只能靠缓存避免每次都联网。
std::mutex g_level_mutex;
struct LevelCacheEntry {
	std::string top;
	bool hidden = false;      // 判为"档位对这首不起作用"（云盘上传的曲子）
	int64_t at_ms = 0;
};
std::unordered_map<int64_t, LevelCacheEntry> g_level_cache;
const int64_t kLevelTtlOkMs = 30 * 60 * 1000;   // 查到了：半小时（会员状态可能变）
const int64_t kLevelTtlFailMs = 30 * 1000;      // 没查到：半分钟就允许重试

int64_t tick_ms() { return static_cast<int64_t>(::GetTickCount64()); }

// top 档位名 → "从最低档到 top"的档位列表。表外的名字一律返回空。
std::vector<std::string> levels_up_to(const std::string & top) {
	std::vector<std::string> out;
	if (top.empty()) return out;
	for (size_t i = 0; i < netease::kQualityOptionCount; ++i) {
		out.push_back(netease::kQualityOptions[i].level);
		if (top == netease::kQualityOptions[i].level) return out;
	}
	out.clear();   // 服务器给了我们表里没有的档位名：宁可什么都不列，也不列错的
	return out;
}

} // namespace

std::vector<std::string> available_levels(int64_t song_id) {
	if (song_id <= 0) return {};
	{
		std::lock_guard<std::mutex> lock(g_level_mutex);
		auto it = g_level_cache.find(song_id);
		if (it != g_level_cache.end()) {
			const bool known = it->second.hidden || !it->second.top.empty();
			const int64_t ttl = known ? kLevelTtlOkMs : kLevelTtlFailMs;
			if (tick_ms() - it->second.at_ms < ttl) {
				return it->second.hidden ? std::vector<std::string>() : levels_up_to(it->second.top);
			}
		}
	}

	std::string top;
	bool hidden = false;
	try {
		netease::CookieJar jar;
		jar.deserialize(netease::Session::instance().cookie_header());
		netease::NeteaseApi api(jar, 5000);   // 菜单里等太久不行，超时给短一点
		std::vector<std::pair<int64_t, std::string>> got;
		const netease::ApiCall call = api.song_privileges({ song_id }, got);
		if (call.ok) {
			for (const auto & kv : got) {
				if (kv.first == song_id) { top = kv.second; break; }
			}
			netease_log::write("foo_netease: 查音质档位 id=" + std::to_string(song_id) +
				" → 最高档=" + (top.empty() ? std::string("(无)") : top));

			// 云盘上传的曲子：服务端**忽略请求的档位**，任何档位都回同一份云盘原文件。
			// 实测 id=28812027（用户确认是云盘歌）：请求 standard 仍然回
			// 档位=lossless / 942 kbps FLAC / 同一份 37 MB 的流；而普通歌 190072
			// 请求 standard 老老实实回 128 kbps mp3。
			// 判据：拿最低档试一次，回来的不是 standard（或码率远超标称）就当云盘，
			// 整项隐藏 —— 这种歌"切换档位"没有任何效果，列出来只会误导。
			if (!top.empty()) {
				const netease::SongUrlInfo probe = api.song_url(song_id, "standard");
				if (!probe.url.empty() &&
					(probe.level != "standard" || probe.br > 400000)) {
					hidden = true;
					netease_log::write("foo_netease: 查音质档位 id=" + std::to_string(song_id) +
						" → 试 standard 拿到 档位=" + probe.level +
						" 码率=" + std::to_string(probe.br) +
						"，判定为云盘曲目（档位对它无效）→ 隐藏「切换音质」");
				}
			}
		} else {
			netease_log::write("foo_netease: 查音质档位失败 id=" + std::to_string(song_id) +
				" —— " + call.error);
		}
	} catch (const std::exception & ex) {
		netease_log::write(std::string("foo_netease: 查音质档位异常 id=") +
			std::to_string(song_id) + " —— " + ex.what());
		top.clear();
	}

	{
		std::lock_guard<std::mutex> lock(g_level_mutex);
		LevelCacheEntry & e = g_level_cache[song_id];
		e.top = top;
		e.hidden = hidden;
		e.at_ms = tick_ms();
	}
	if (hidden) return {};
	return levels_up_to(top);
}

namespace {
std::mutex g_resume_mutex;
std::unordered_map<std::string, double> g_resume_pos;
} // namespace

void set_resume_position(const std::string & path, double seconds) {
	if (path.empty()) return;
	std::lock_guard<std::mutex> lock(g_resume_mutex);
	g_resume_pos[path] = seconds;
}

double take_resume_position(const std::string & path) {
	std::lock_guard<std::mutex> lock(g_resume_mutex);
	auto it = g_resume_pos.find(path);
	if (it == g_resume_pos.end()) return 0.0;
	const double seconds = it->second;
	g_resume_pos.erase(it);
	return seconds;
}

size_t sync_fm_playlist(const std::vector<netease::TrackInfo> & batch, bool new_session) {
	if (batch.empty()) return 0;
	fm_apply_batch(batch, new_session);
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
		// 冷却时间（原 3 秒 / 10 秒太长了：快速切歌时列表会被削到只剩一首，
		// 冷却期间只能反复播同一首）。现在分三档：
		//   列表已空 → 0.3 秒（只做防抖，立刻补歌）
		//   还剩一首在缓冲 → 1 秒
		//   池子暂时榨干（上批全是老歌） → 5 秒退避，别猛打 /radio/get
		const int64_t interval = (unplayed_others == 0)
			? 300
			: ((g_fm_last_batch_fresh.load() == 0) ? 5000 : 1000);
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

size_t queue_tracks_next(const std::vector<netease::TrackInfo> & tracks) {
	if (tracks.empty()) return 0;
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return 0;
	const std::string level = netease::Session::instance().quality();
	size_t n = 0;
	for (size_t i = 0; i < tracks.size(); ++i) {
		const std::string path = "netease://song/" + std::to_string(tracks[i].id) +
			"?level=" + level + "&no=" + std::to_string(i + 1);
		metadb_handle_ptr h = metadb::get()->handle_create(path.c_str(), 0);
		if (!h.is_valid()) continue;
		pm->queue_add_item(h);
		++n;
	}
	netease_log::write("foo_netease: 右键「下一首播放」—— 已加入播放队列 " + std::to_string(n) + " 首");
	return n;
}

size_t insert_tracks_next(const std::vector<netease::TrackInfo> & tracks) {
	if (tracks.empty()) return 0;
	metadb_handle_list handles;
	const std::string level = netease::Session::instance().quality();
	for (size_t i = 0; i < tracks.size(); ++i) {
		const std::string path = "netease://song/" + std::to_string(tracks[i].id) +
			"?level=" + level + "&no=" + std::to_string(i + 1);
		handles.add_item(metadb::get()->handle_create(path.c_str(), 0));
	}
	auto pm = playlist_manager::get();
	if (!pm.is_valid() || handles.get_count() == 0) return 0;

	// 插到正在播放那一条的后面；没有正在播放就插到最前面。
	t_size at = 0;
	metadb_handle_ptr now;
	playback_control::get()->get_now_playing(now);
	const t_size active = pm->get_active_playlist();
	if (now.is_valid() && active != pfc::infinite_size) {
		metadb_handle_list all;
		pm->playlist_get_all_items(active, all);
		for (t_size i = 0; i < all.get_count(); ++i) {
			if (all[i] == now) { at = i + 1; break; }
		}
	}
	pm->activeplaylist_insert_items(at, handles, bit_array_false());
	netease_log::write("foo_netease: 已插入「下一首播放」" + std::to_string(handles.get_count()) +
		" 首（插在第 " + std::to_string(at + 1) + " 条）");
	auto io = metadb_io_v2::get();
	if (io.is_valid()) {
		io->load_info_async(handles, metadb_io::load_info_force, nullptr,
			metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
	}
	return handles.get_count();
}

namespace {

// 一批曲目 → netease:// 句柄。
// no= 是源歌单内序号（从 1 开始）。写进路径，条目身份就带着它，
// 本地排序不会改动它，用户可按 %netease_no% 还原歌单原顺序。
std::string song_path(int64_t id, size_t index, const std::string & level) {
	return "netease://song/" + std::to_string(id) + "?level=" + level +
		"&no=" + std::to_string(index + 1);
}

void make_song_handles(const std::vector<netease::TrackInfo> & tracks,
	const std::string & level, metadb_handle_list & out) {
	out.remove_all();
	for (size_t i = 0; i < tracks.size(); ++i) {
		const std::string path = song_path(tracks[i].id, i, level);
		out.add_item(metadb::get()->handle_create(path.c_str(), 0));
	}
}

// 关键一步：让 foobar2000 去读元数据。
// 只插入句柄的话，metadb 可能一直沿用「空」的缓存，播放列表里就只剩一串数字。
void load_info_background(const metadb_handle_list & handles) {
	auto io = metadb_io_v2::get();
	if (io.is_valid()) {
		io->load_info_async(handles, metadb_io::load_info_force, nullptr,
			metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
	}
}

// 防御：正常歌单最多几千首。真出现异常大的数量，宁可报错也不要让上层无限增长
//（曾经因为选中项遍历写错，chosen 无限膨胀到 bad_alloc 崩掉宿主）。
void guard_track_count(size_t count) {
	if (count > 20000) throw exception_io_data("曲目数量异常（超过 20000），已中止");
}

// 「添加到新的播放列表」用的名字：重名就加 (2)(3)…，
// 这样"列表名 → 歌单 id"的对应关系才是唯一的（刷新靠名字找列表）。
std::string unique_playlist_name(const std::string & wanted) {
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return wanted;
	if (pm->find_playlist(wanted.c_str()) == pfc::infinite_size) return wanted;
	for (int n = 2; n < 1000; ++n) {
		const std::string candidate = wanted + " (" + std::to_string(n) + ")";
		if (pm->find_playlist(candidate.c_str()) == pfc::infinite_size) return candidate;
	}
	return wanted;
}

} // namespace

// ---- fb2k 播放列表 ←→ 网易云歌单 的对应关系 ----
//
// 为什么需要它：把歌单整片写进一个 fb2k 播放列表之后，列表名和歌单之间就没有
// 任何联系了 —— 歌单在服务端变了（加歌/删歌/改名），本地那份永远是旧的。
// 记下对应关系，用户在 foobar2000 的播放列表管理器里点中这个列表时，就自动重拉一次。
namespace {

// 只在主线程碰：playlist_manager 的方法都要求主线程，异步回调用 fb2k::inMainThread
// 投回来，所以回调里改它同样安全。
std::map<std::string, int64_t> g_pl_source;      // 播放列表名 → 网易云歌单 id（负数 = 无歌单来源）
std::map<std::string, int64_t> g_pl_source_stamp; // 上次"内容等于歌单"的时刻（冷却用）
bool g_pl_source_dirty = false;
// 刷新用的存活标记：退出时置 false，避免回调在组件卸载后去碰 playlist_manager。
LivenessPtr g_pl_alive = std::make_shared<Liveness>();

const int64_t kPlSourceCooldownMs = 15 * 1000;   // 同一个列表 15 秒内最多刷一次（防来回点连发请求）

// 每日推荐 / 最近播放在网易云侧没有对应歌单，可点中列表照样该能刷新 ——
// 映射里给这类来源存**负数哨兵 id**（>0 的一律是真歌单 id），刷新时按哨兵分派。
const int64_t kPlSourceDaily = -1;
const int64_t kPlSourceRecent = -2;

// 映射来源的可读描述（写日志用）。
std::string playlist_source_desc(int64_t id) {
	if (id == kPlSourceDaily) return "每日推荐";
	if (id == kPlSourceRecent) return "最近播放";
	return "歌单 id=" + std::to_string(id);
}

std::string playlist_map_path() {
	const std::string dir = netease_log::profile_dir();
	return dir.empty() ? std::string() : dir + "\\foo_netease_playlists.txt";
}

} // namespace

void playlist_source_save() {
	if (!g_pl_source_dirty) return;
	const std::string path = playlist_map_path();
	if (path.empty()) return;
	std::ofstream out(netease::to_wide(path).c_str(), std::ios::trunc);
	if (!out) return;
	out << "# foo_netease —— fb2k 播放列表名 <TAB> 网易云歌单 id（负数 = 无歌单来源）\n";
	for (const auto & kv : g_pl_source) out << kv.first << "\t" << kv.second << "\n";
	g_pl_source_dirty = false;
}

void playlist_source_set(const std::string & playlist_name, int64_t playlist_id) {
	if (playlist_name.empty() || playlist_id == 0) return;   // 0 = 没来源；负数是哨兵来源
	g_pl_source[playlist_name] = playlist_id;
	// 这里**故意不设冷却时间戳**：内容比对比快，让"新建列表后自动切过去"也走一遍
	// 刷新流程（结果会是"已是最新、未改动"），链路一眼能在日志里看出来。
	g_pl_source_dirty = true;
	playlist_source_save();
}

int64_t playlist_source_get(const std::string & playlist_name) {
	auto it = g_pl_source.find(playlist_name);
	return it == g_pl_source.end() ? 0 : it->second;
}

void playlist_source_forget(const std::string & playlist_name) {
	auto it = g_pl_source.find(playlist_name);
	if (it == g_pl_source.end()) return;
	g_pl_source.erase(it);
	g_pl_source_stamp.erase(playlist_name);
	g_pl_source_dirty = true;
	playlist_source_save();
	netease_log::write("foo_netease: 播放列表「" + playlist_name + "」已删除，对应关系一并清掉");
}

void playlist_source_load() {
	const std::string path = playlist_map_path();
	if (path.empty()) return;
	std::ifstream in(netease::to_wide(path).c_str());
	if (!in) return;
	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#') continue;
		const size_t tab = line.find('\t');
		if (tab == std::string::npos || tab == 0) continue;
		const int64_t id = std::strtoll(line.c_str() + tab + 1, nullptr, 10);
		if (id != 0) g_pl_source[line.substr(0, tab)] = id;
	}
	netease_log::write("foo_netease: 播放列表对应关系已载入 —— " +
		std::to_string(g_pl_source.size()) + " 个（点中这些列表会自动刷新）");
}

void playlist_source_shutdown() {
	if (g_pl_alive) g_pl_alive->alive = false;
	playlist_source_save();
}

// 把重新拉到的内容写回映射的那个列表：内容一模一样就什么都不动（免得打断选中项 /
// 滚动位置），不一样才整片替换，并留一个撤销点。what 只用于日志。
void rewrite_mapped_playlist(const std::string & key, FeedResult r, const std::string & what) {
	if (!r.ok) {
		netease_log::write("foo_netease: 刷新「" + key + "」失败：" + r.error);
		return;
	}
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	const t_size index = pm->find_playlist(key.c_str());
	if (index == pfc::infinite_size) {
		netease_log::write("foo_netease: 刷新「" + key + "」时列表已不在，跳过");
		return;
	}
	const std::string level = netease::Session::instance().quality();

	std::vector<std::string> want;
	want.reserve(r.tracks.size());
	for (size_t i = 0; i < r.tracks.size(); ++i) {
		want.push_back(song_path(r.tracks[i].id, i, level));
	}
	std::vector<std::string> have;
	pm->playlist_enum_items(index, [&have](size_t, const metadb_handle_ptr & h, bool) -> bool {
		const char * path = h.is_valid() ? h->get_path() : nullptr;
		have.push_back(path ? std::string(path) : std::string());
		return true;
	}, bit_array_true());

	if (want == have) {
		netease_log::write("foo_netease: 「" + key + "」已是最新（" +
			std::to_string(want.size()) + " 首），未改动");
		return;
	}

	metadb_handle_list handles;
	make_song_handles(r.tracks, level, handles);
	if (handles.get_count() == 0) return;
	// 留一个撤销点：用户 Ctrl+Z 能退回刷新前的内容。
	pm->playlist_undo_backup(index);
	if (!pm->playlist_remove_items(index, bit_array_true())) {
		netease_log::write("foo_netease: 「" + key + "」被锁定，刷新未写入");
		return;
	}
	pm->playlist_insert_items(index, 0, handles, bit_array_false());
	load_info_background(handles);
	netease_log::write("foo_netease: 已刷新「" + key + "」—— " +
		std::to_string(have.size()) + " → " + std::to_string(handles.get_count()) +
		" 首（" + what + "）");
}

void playlist_source_refresh_if_mapped(t_size playlist_index) {
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	if (playlist_index >= pm->get_playlist_count()) return;
	pfc::string8 buf;
	if (!pm->playlist_get_name(playlist_index, buf)) return;
	const std::string key(buf.get_ptr());
	auto found = g_pl_source.find(key);
	if (found == g_pl_source.end() || found->second == 0) return;   // 负数哨兵来源也要刷
	if (!netease::Session::instance().logged_in()) return;

	const int64_t now = now_ms();
	auto stamp = g_pl_source_stamp.find(key);
	if (stamp != g_pl_source_stamp.end() && now - stamp->second < kPlSourceCooldownMs) {
		netease_log::write("foo_netease: 点中「" + key + "」—— 刚刷过，还有 " +
			std::to_string((kPlSourceCooldownMs - (now - stamp->second)) / 1000) +
			" 秒冷却，跳过");
		return;
	}
	g_pl_source_stamp[key] = now;
	const int64_t id = found->second;

	netease_log::write("foo_netease: 点中播放列表「" + key + "」—— 自动刷新（" +
		playlist_source_desc(id) + "）");
	// 真歌单走歌单详情；每日推荐 / 最近播放按哨兵分派到各自的加载器，写回逻辑共用。
	if (id == kPlSourceDaily) {
		load_daily_async(g_pl_alive, [key](FeedResult r) {
			// 注意别捕获 id：这里直接按哨兵常量出描述。
			rewrite_mapped_playlist(key, std::move(r), playlist_source_desc(kPlSourceDaily));
		});
	} else if (id == kPlSourceRecent) {
		load_recent_async(g_pl_alive, [key](FeedResult r) {
			rewrite_mapped_playlist(key, std::move(r), playlist_source_desc(kPlSourceRecent));
		});
	} else {
		load_playlist_tracks_async(g_pl_alive, id, [key, id](FeedResult r) {
			rewrite_mapped_playlist(key, std::move(r), playlist_source_desc(id));
		});
	}
}

size_t insert_tracks_into_new_playlist(const std::vector<netease::TrackInfo> & tracks,
	const std::string & name, int64_t playlist_id) {
	guard_track_count(tracks.size());
	if (tracks.empty()) return 0;

	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return 0;

	const std::string playlist_name = unique_playlist_name(
		name.empty() ? std::string("网易云歌单") : name);
	const t_size index = pm->create_playlist(playlist_name.c_str(),
		pfc::infinite_size, pfc::infinite_size);
	if (index == pfc::infinite_size) return 0;

	metadb_handle_list handles;
	make_song_handles(tracks, netease::Session::instance().quality(), handles);
	if (handles.get_count() == 0) return 0;
	pm->playlist_insert_items(index, 0, handles, bit_array_false());
	// 顺序要紧：**先**记对应关系再切过去。切过去会立刻触发播放列表回调
	//（就是"点中列表 → 自动刷新"那条），反过来的话回调那一刻还查不到对应哪个歌单。
	if (playlist_id != 0) playlist_source_set(playlist_name, playlist_id);   // 含负数哨兵来源
	// 跟 foobar2000 自带"发送到新建播放列表"一致：切过去，让用户直接看到结果。
	pm->set_active_playlist(index);
	netease_log::write("foo_netease: 新建播放列表「" + playlist_name + "」写入 " +
		std::to_string(handles.get_count()) + " 项" +
		(playlist_id != 0
			? ("（对应" + playlist_source_desc(playlist_id) + "；点中该列表会自动刷新）")
			: std::string()));
	load_info_background(handles);
	return handles.get_count();
}

size_t insert_tracks(const std::vector<netease::TrackInfo> & tracks, bool replace,
	int64_t source_playlist_id) {
	guard_track_count(tracks.size());

	metadb_handle_list handles;
	make_song_handles(tracks, netease::Session::instance().quality(), handles);
	if (handles.get_count() == 0) return 0;

	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return 0;
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

	// 整片替换 = 这个列表的内容就是那个来源（歌单 / 每日推荐 / 最近播放）：
	// 记下对应关系，点中它就能自动刷新。
	// 追加不记 —— 列表里混了用户自己加的东西，刷新会把它们冲掉。
	if (replace && source_playlist_id != 0) {
		pfc::string8 name;
		const t_size active = pm->get_active_playlist();
		if (active != pfc::infinite_size && pm->playlist_get_name(active, name)) {
			playlist_source_set(std::string(name.get_ptr()), source_playlist_id);
		}
	}

	load_info_background(handles);
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


// 歌单 id -> 曲目。用 trackIds 全集（tracks 会被服务端截断），再按缓存补元数据。
FeedResult load_playlist_tracks(netease::NeteaseApi & api, int64_t playlist_id) {
	FeedResult r;
	std::vector<int64_t> ids;
	std::string name;
	int64_t track_count = -1;
	std::vector<netease::TrackInfo> seed;
	netease::ApiCall detail = api.playlist_track_ids(playlist_id, ids, name, track_count, &seed);
	if (!detail.ok) { r.error = "取歌单详情失败：" + detail.error; return r; }
	// 详情自带的元数据先入缓存：≤1000 首的歌单到这一步已经全有了。
	if (!seed.empty()) netease::MetaCache::instance().put_all(seed);

	std::vector<int64_t> missing;
	TracksFetchStats stats;
	netease::ApiCall songs = load_tracks_cached(api, ids, r.tracks, &missing, &stats);
	if (!songs.ok) { r.error = "取曲目详情失败：" + songs.error; return r; }
	netease_log::write("foo_netease: 歌单曲目 " + std::to_string(stats.ids) + " 首 —— 缓存命中 " +
		std::to_string(stats.cached) + "，请求 " + std::to_string(stats.requested) +
		"，返回 " + std::to_string(stats.fetched));

	r.name = name.empty() ? ("歌单 " + std::to_string(playlist_id)) : name;
	r.source_playlist_id = playlist_id;
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

// 按 ids 取曲目元数据：命中缓存的直接取，只把缺的发给服务端。
netease::ApiCall load_tracks_cached(netease::NeteaseApi & api, const std::vector<int64_t> & ids,
	std::vector<netease::TrackInfo> & out, std::vector<int64_t> * missing,
	TracksFetchStats * stats) {
	out.clear();
	if (missing) missing->clear();
	netease::ApiCall ok;
	ok.ok = true;
	if (ids.empty()) return ok;

	auto & cache = netease::MetaCache::instance();
	std::unordered_map<int64_t, netease::TrackInfo> have;
	std::vector<int64_t> need;
	have.reserve(ids.size());
	need.reserve(64);
	for (int64_t id : ids) {
		netease::TrackInfo t;
		if (cache.get(id, t)) have.emplace(id, std::move(t));
		else need.push_back(id);
	}
	if (stats) { stats->ids = ids.size(); stats->cached = have.size(); stats->requested = need.size(); }

	if (!need.empty()) {
		std::vector<netease::TrackInfo> fetched;
		netease::ApiCall call = api.song_details(need, fetched, nullptr);
		if (!call.ok) return call;
		cache.put_all(fetched);
		if (stats) stats->fetched = fetched.size();
		for (auto & t : fetched) have.emplace(t.id, std::move(t));
	}

	out.reserve(ids.size());
	for (int64_t id : ids) {
		auto it = have.find(id);
		if (it != have.end()) out.push_back(it->second);
		else if (missing) missing->push_back(id);
	}
	netease_app::save_meta_cache();
	return ok;
}

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
		r.name = "每日推荐";
		r.source_playlist_id = kPlSourceDaily;
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
		r.name = "漫游（私人 FM）";
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
		r.name = "最近播放";
		r.source_playlist_id = kPlSourceRecent;
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
			r.name = "华语私人雷达";
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
		if (r.ok) {
			r.title = name + "：" + std::to_string(r.tracks.size()) + " 首";
			r.name = name;
			r.source_playlist_id = id;
		}
		return r;
	});
}

void load_playlist_tracks_async(LivenessPtr alive, int64_t playlist_id,
	std::function<void(FeedResult)> done) {
	run_async(alive, done, [playlist_id](netease::NeteaseApi & api) {
		return load_playlist_tracks(api, playlist_id);
	});
}

void search_playlists_async(LivenessPtr alive, const std::string & keyword, int offset,
	std::function<void(PlaylistsResult)> done) {
	// 注意：run_async 只支持 FeedResult，这条走和 load_playlists_async 一样的写法。
	fb2k::splitTask([alive, keyword, offset, done] {
		PlaylistsResult r;
		netease::CookieJar jar;
		std::unique_ptr<netease::NeteaseApi> holder;
		if (!make_api(jar, holder)) {
			r.error = "尚未登录（或本地凭据里没有 MUSIC_U）";
			post_result(alive, done, std::move(r));
			return;
		}
		int total = 0;
		netease::ApiCall call = holder->search_playlists(keyword, 30, offset, r.items, &total);
		if (!call.ok) {
			r.error = call.error.empty() ? "歌单搜索失败" : call.error;
			post_result(alive, done, std::move(r));
			return;
		}
		r.ok = true;
		r.total = total;
		post_result(alive, done, std::move(r));
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

