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
// replace 且 source_playlist_id > 0 时，记下"当前播放列表名 → 这个歌单 id"的对应关系
//（之后在 foobar2000 的播放列表管理器里点中它，就会自动重拉一次并替换内容）。
size_t insert_tracks(const std::vector<netease::TrackInfo> & tracks, bool replace,
	int64_t source_playlist_id = 0);

// 「添加到新的播放列表」：新建一个以 name 命名的 fb2k 播放列表，整片写入这批曲目，
// 并把它设为当前列表（跟 foobar2000 自带"发送到新建播放列表"的行为一致）。
// playlist_id > 0 时顺带记下对应关系。
size_t insert_tracks_into_new_playlist(const std::vector<netease::TrackInfo> & tracks,
	const std::string & name, int64_t playlist_id);

// ---- fb2k 播放列表 ←→ 网易云歌单 的对应关系 ----
//
// 只有"整片替换"和"新建列表"两种情况才记：那两种情况下列表内容 == 那个歌单，
// 自动刷新是安全的。追加不记 —— 列表里混了用户自己加的东西，刷新会把它们冲掉。
void playlist_source_set(const std::string & playlist_name, int64_t playlist_id);
int64_t playlist_source_get(const std::string & playlist_name);
void playlist_source_forget(const std::string & playlist_name);  // 播放列表被删掉时清掉对应关系
void playlist_source_load();      // on_init：读 profile 目录里的 foo_netease_playlists.txt
void playlist_source_save();      // 有改动就写一次
void playlist_source_shutdown();  // on_quit：停掉刷新用的存活标记 + 兜底保存
// 播放列表回调调用（主线程）：点中的列表若对应某个歌单，就异步重拉一次并替换内容。
// 有冷却时间，来回点不会反复发请求；拉完发现内容没变就什么都不做。
void playlist_source_refresh_if_mapped(t_size playlist_index);

// 插到"正在播放"的下一条（右键「下一首播放」）。找不到正在播放就插到最前面。
size_t insert_tracks_next(const std::vector<netease::TrackInfo> & tracks);

// 加进 foobar2000 的播放队列 —— 这才是真正的"下一首播放"（随机播放下也生效）。
size_t queue_tracks_next(const std::vector<netease::TrackInfo> & tracks);

// 该曲目「当前账号可播」的音质档位（低 → 高，取值就是 netease::kQualityOptions 的 level）。
//
// **同步**返回，这是有意的：上下文菜单是同步构建的，要"不显示不支持的档位"就必须
// 在构建那一刻就知道答案，没法等异步回调。首次会联网查一次 /song/enhance/privilege，
// 之后按 id 缓存（成功 30 分钟，失败 30 秒就允许重试）。返回空 = 查不到。
// 这首歌当前能切换的档位（从低到高）。空 = 整项不显示，包括两种情况：
//  · 服务器说这首没有可播的正规档位（playMaxBrLevel=none）；
//  · 云盘上传的曲子 —— 服务端忽略请求的档位，试最低档也会回云端原文件，
//    切档位没有任何效果，所以也隐藏。
std::vector<std::string> available_levels(int64_t song_id);

// 换音质要重新打开这一条（直链是按档位签的，换档就得换链），起点得保住：
// 记下「路径 → 秒数」，等这条真的开始播时再跳回去（见 radio_callback）。
void set_resume_position(const std::string & path, double seconds);
double take_resume_position(const std::string & path);   // 取走即清，别影响下一次正常播放

struct FeedResult {
	bool ok = false;
	std::string error;
	std::string title;                      // 一句话摘要，直接显示在状态栏
	std::string name;                       // 来源名字（歌单名等，不带"：N 首"）；新建播放列表时用它命名
	int64_t source_playlist_id = 0;         // 来源本身就是网易云歌单时的歌单 id（每日推荐等为 0）
	std::vector<netease::TrackInfo> tracks;
};

struct PlaylistsResult {
	bool ok = false;
	std::string error;
	std::string nickname;
	int64_t uid = 0;
	std::vector<netease::PlaylistInfo> items;
	int total = 0;   // 服务端报告的总数（playlistCount），供「更多」判断还有没有
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
// new_session=true：用户重新选了漫游（双击）→ 整片替换成这一批；
// new_session=false：右键「添加」→ 只是追加。
size_t sync_fm_playlist(const std::vector<netease::TrackInfo> & batch, bool new_session);

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

// 搜歌单（只用来在面板上列出来，不写播放列表）。
// offset = 从第几个开始（「更多」翻页用），首次搜索传 0。
void search_playlists_async(LivenessPtr alive, const std::string & keyword, int offset,
	std::function<void(PlaylistsResult)> done);

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

