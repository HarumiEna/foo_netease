#pragma once
// foo_netease —— 界面层共用的数据操作。
//
// 浏览窗口（模态对话框）和可停靠面板都用这里的东西，避免两份实现各自漂移：
//  · insert_tracks：把曲目变成 netease:// 句柄写进当前播放列表；
//  · 各来源的异步加载：每日推荐 / 漫游 / 私人雷达 / 歌单曲目 / 搜索。
//
// 线程约定：所有 load_* 立刻返回，网络在 fb2k::splitTask 的工作线程里做，
// 结果通过 done 回调投递回**主线程**。窗口只需在回调里更新控件。

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/api.h"

namespace netease_data {

// 窗口存活标记：工作线程把结果投回主线程前先看它，避免窗口已销毁还去碰句柄。
struct Liveness { std::atomic<bool> alive{ true }; };
using LivenessPtr = std::shared_ptr<Liveness>;

// 把一组曲目写成 netease:// 句柄放进当前播放列表；replace=true 时先清空。
size_t insert_tracks(const std::vector<netease::TrackInfo> & tracks, bool replace);

struct FeedResult {
	bool ok = false;
	std::string error;
	std::string title;                      // 一句话摘要，直接显示在状态栏
	std::vector<netease::TrackInfo> tracks;
};

struct PlaylistsResult {
	bool ok = false;
	std::string error;
	std::string nickname;
	int64_t uid = 0;
	std::vector<netease::PlaylistInfo> items;
};

// ---- 漫游电台 ----
//
// 网易云的 /radio/get 一次只给 3 首，而漫游应该是无限的。做法：
//  · "网易云漫游"播放列表始终只放"当前这一批 + 正在播放的那首"；
//  · 播到本批最后一首时，后台再取一批接上，并把已经播过的删掉。
// 这样列表一直很短，却可以一直放下去。

// 面板上的复选框（默认开）。
void set_fm_radio_enabled(bool on);
bool fm_radio_enabled();

// 把一批漫游曲目写进播放列表：追加新歌，再删掉"正在播放那首之前"的所有旧条目
// （正在播放的永不删除）。没有正在播放的条目时就只保留这一批。
size_t sync_fm_playlist(const std::vector<netease::TrackInfo> & batch);

// 播放回调调用：判断这一批是否快放完（随机播放下也成立），需要就自动续一批。
void fm_radio_maybe_extend(metadb_handle_ptr track);
// 面板「更多」：立刻再取一批并追加进漫游列表。
void fm_radio_extend_now();

// ---- 让播放列表光标跟随正在播放（可选，默认关）----
//
// 起因（实测）：用户界面上那个封面元素
// **只在"列表选中项变化"或交互时才重新取图** —— 自动切歌时光标不动，
// 封面就一直停在上一首，非要动一下鼠标才更新。
// 打开这个开关后，每切一首就把选中项/光标移到正在播放那一条，
// 元素自然就跟着更新了（等价于 foobar2000 自带的「光标跟随播放」）。
void set_follow_cursor_enabled(bool on);
bool follow_cursor_enabled();
void follow_playing_deferred();

// ---- "正在播放"的路径缓存 ----
//
// 封面回退（album_art_fallback::open）跑在 foobar2000 的**封面加载线程**上。
// 那里绝不能调用 playback_control::get_now_playing() —— 实测直接崩
//（crash report: 专辑封面加载线程=>album_art_manager_v2::open，栈检查失败）。
// 所以由 play_callback 在主线程把正在播放的路径写进来，封面线程只读。
void set_now_playing_path(const std::string & path);
std::string now_playing_path();
// 播放回调调用：整个列表放完了的兜底（续一批并继续播）。
void fm_radio_on_eof();

// 从各种"歌单链接"里取歌单 id：
//   netease://playlist/<id>
//   https://music.163.com/playlist?id=<id>（含 #/playlist?id= 这种网页形式）
//   https://y.music.163.com/m/playlist?id=<id>
// 取不到返回 0。
int64_t parse_playlist_link(const std::string & text);

// 直接把一个歌单链接加载成曲目列表（面板搜索框里粘链接就是走这条）。
void load_playlist_link_async(LivenessPtr alive, const std::string & link,
	std::function<void(FeedResult)> done);

// 按 ids 取曲目元数据：命中曲目缓存的**不再发请求**，只对缺的走批量接口。
// 这是"重复打开同一个歌单不用等"的关键——第一次拉全量，之后就只补新歌。
// out 与 ids 顺序一致，取不到的 id 收进 missing。stats 可传 nullptr。
struct TracksFetchStats {
	size_t ids = 0;        // 请求的曲目数
	size_t cached = 0;     // 命中缓存、没走网络的
	size_t requested = 0;  // 实际发给服务端的
	size_t fetched = 0;    // 服务端返回的
};
netease::ApiCall load_tracks_cached(netease::NeteaseApi & api, const std::vector<int64_t> & ids,
	std::vector<netease::TrackInfo> & out, std::vector<int64_t> * missing = nullptr,
	TracksFetchStats * stats = nullptr);

void load_playlists_async(LivenessPtr alive, std::function<void(PlaylistsResult)> done);
void load_daily_async(LivenessPtr alive, std::function<void(FeedResult)> done);
void load_fm_async(LivenessPtr alive, std::function<void(FeedResult)> done);
// 漫游「更多」：跳过 exclude 里的 id，再取一批新歌（漫游本就取之不尽）。
void load_fm_more_async(LivenessPtr alive, const std::vector<int64_t> & exclude,
	std::function<void(FeedResult)> done);
void load_radar_async(LivenessPtr alive, std::function<void(FeedResult)> done);
void load_playlist_tracks_async(LivenessPtr alive, int64_t playlist_id,
	std::function<void(FeedResult)> done);
// offset = 从第几首开始（「更多」翻页用），首次搜索传 0。
// ---- 本机播放记录 ----
// 服务端的"最近播放"只认客户端自己的播放，foobar2000 里播的不算，
// 所以组件自己记一份（落盘到 profile 目录），加载"最近播放"时并到最前面。
void recent_local_add(int64_t song_id);
void recent_local_load();
std::vector<int64_t> recent_local_list();

void search_async(LivenessPtr alive, const std::string & keyword, int offset,
	std::function<void(FeedResult)> done);

// 最近播放：服务端的播放记录（POST /weapi/v1/play/record，需要 uid）。
void load_recent_async(LivenessPtr alive, std::function<void(FeedResult)> done);

// 华语漫游：它是客户端「精选 → 华语」页里的**官方资源**，不是 FM 参数。
// 实测：/v1/discovery/recommend/resource 里有一项
// 「华语私人雷达 | 最懂你的华语推荐 每日更新35首」（id 2829883282）。
// 跟「私人雷达」同源，所以按名字动态定位、找不到再退回已知 id。
void load_cn_roam_async(LivenessPtr alive, std::function<void(FeedResult)> done);

// 当前漫游模式（""=默认，"LANGUAGE_CN"=华语）；续批时沿用同一个模式。
void set_fm_mode(const std::string & mode);
std::string fm_mode();

} // namespace netease_data

