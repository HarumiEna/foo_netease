#pragma once
// foo_netease —— 网易云接口封装。
//
// 只把「已经用真实请求验证过」的接口放进这里；未验证的一律先写探针。
// 每次调用都会把原始响应体交给调用方，便于出错时原样记录（脱敏后）。

#include <functional>
#include <string>
#include <vector>

#include "core/http.h"
#include "core/json.h"

namespace netease {

struct ApiCall {
	bool ok = false;
	std::string error;
	int http_status = 0;
	std::string raw_body;   // 服务端原样返回；记日志前必须脱敏
	json::Value json;       // 解析成功时有效
};

// 歌曲直链查询结果。
// 实测要点：
//  · 未登录时即使请求 exhigh，也只会返回 standard 档的 45 秒试听片段（free_trial 非空）；
//  · 直链带时效签名，响应里的 expi 是有效秒数（实测 1200），因此缓存必须远短于它。
struct SongUrlInfo {
	bool ok = false;
	std::string error;
	int64_t id = 0;
	std::string url;          // 播放直链；为空表示拿不到（无版权 / 需会员 / 需登录）
	int64_t br = 0;           // 实际码率
	int64_t size = 0;
	std::string level;        // 服务端实际给出的档位（可能低于请求档位）
	std::string type;         // mp3 / flac / ...
	int64_t fee = 0;
	int64_t expi = 0;         // 直链有效秒数
	bool has_free_trial = false;
	int64_t trial_end = 0;    // 试听片段结束秒数（has_free_trial 时有效）
	json::Value raw;          // 服务端原样返回
};

// 曲目元数据。字段名全部来自实测响应，不是猜的。
struct TrackInfo {
	int64_t id = 0;
	std::string title;      // name
	std::string artists;    // ar[].name，多个用 / 连接
	std::string album;      // al.name
	int64_t duration_ms = 0;// dt
	int64_t fee = 0;        // 0 免费 / 1 需付费或会员 / 4 等
	int64_t track_number = 0; // no：专辑内音轨号（播放列表按序号排序要用它）
	int64_t album_id = 0;     // al.id
	std::string cover_url;    // al.picUrl：封面图地址
};

struct PlaylistInfo {
	int64_t id = 0;
	std::string name;
	int64_t track_count = 0;
	std::string creator;
};

// 从 JSON 里解析一首歌（v3/song/detail、v6/playlist/detail 的 tracks、
// cloudsearch 的 songs 都是同一套字段，所以共用一个解析函数）。
TrackInfo parse_track(const json::Value & item);

class NeteaseApi {
public:
	// jar 的生命周期由调用方管理；构造时会写入 os=pc / appver 这类必需 Cookie。
	explicit NeteaseApi(CookieJar & jar, int timeout_ms = 30000);

	ApiCall weapi_post(const std::string & path, const std::string & json_body);
	// 指定宿主的 weapi（日志上报主机是 clientlogusf.music.163.com）。
	ApiCall weapi_post_on(const char * host, const std::string & path, const std::string & json_body);

	// 诊断用的 GET（带 cookie）。
	ApiCall get_common(const std::string & path, const char * host = nullptr);

	ApiCall eapi_post(const std::string & path, const std::string & json_body);

	// 网页版的播放上报：**裸 JSON body** 打到 /api/feedback/weblog（实测抓包确认）。
	// body = {"logs":"[{action,json}]","csrf_token":"<cookies 里的 __csrf>"}
	// host 为空用 music.163.com；也可试 clientlogusf.music.163.com。
	// 读某个 cookie 的值（诊断用；如 __csrf）。
	std::string cookie(const char * name) const;

	ApiCall report_play_web(int64_t song_id, int played_seconds, const char * end,
		const char * source, const char * source_id, const char * host = nullptr);

	// 带"客户端/网页"全套头的播放上报（诊断用）。
	ApiCall report_play_web_styled(int64_t song_id, int played_seconds, const char * end,
		const char * source, const char * source_id, const char * host);

	// 用**纯网页 Cookie 身份**发播放日志（临时清掉 os/appver/clientSign 等客户端指纹）。
	ApiCall report_play_web_minimal(int64_t song_id, int played_seconds, const char * end);

	// 明文 POST 到 /api/<path>（网页版发播放日志就是这么发的：不加密、表单体）。
	ApiCall plain_api_post(const std::string & path, const std::string & form_body);

	// 已实测：POST /weapi/login/qrcode/unikey  -> {"code":200,"unikey":"..."}
	ApiCall qrcode_unikey();
	// 已实测：POST /weapi/login/qrcode/client/login -> {"code":801,"message":"等待扫码"}
	ApiCall qrcode_check(const std::string & key);

	// 已实测：POST /weapi/song/enhance/player/url/v1（weapi 至今仍然可用，不需要 xeapi）
	SongUrlInfo song_url(int64_t id, const std::string & level = "exhigh",
		const std::string & encode_type = "flac");

	// ===== M4：账号 / 歌单 / 搜索（全部已用真实请求验证过）=====

	// POST /weapi/w/nuser/account/get -> account.id / profile.nickname
	ApiCall account(int64_t & uid, std::string & nickname);

	// POST /weapi/user/playlist -> playlist[]
	ApiCall user_playlists(int64_t uid, int limit, int offset, std::vector<PlaylistInfo> & out);

	// 歌单广场：POST /weapi/playlist/list，cat 是分类名（"华语" / "欧美" / "摇滚" …）。
	// 实测 cat=华语 返回的是真正的大陆华语歌单（孙燕姿、邓紫棋…）——
	// 这是目前唯一能稳定拿到"华语曲库"的公开接口（客户端那张"华语漫游"卡是内部 tag，接不到）。
	ApiCall playlist_square_list(const std::string & cat, int limit, int offset,
		std::vector<PlaylistInfo> & out);

	// POST /weapi/v6/playlist/detail
	// 注意：响应里的 tracks 数组会被服务端截断在 1000 条，而 trackIds 是全集，
	// 所以这里优先返回 trackIds；需要元数据时再走 song_details 分批补。
	ApiCall playlist_track_ids(int64_t playlist_id, std::vector<int64_t> & ids,
		std::string & name, int64_t & track_count);

	// POST /weapi/v3/song/detail，内部按 200 个一批自动分批。
	ApiCall song_details(const std::vector<int64_t> & ids, std::vector<TrackInfo> & out,
		std::vector<int64_t> * missing = nullptr);

	// POST /weapi/cloudsearch/get/web（实测：未登录会返回 code=50000005，必须登录）
	// p_total 回传 result.songCount —— 有了它才知道"总共多少首、还差多少"。
	ApiCall search_songs(const std::string & keyword, int limit, int offset,
		std::vector<TrackInfo> & out, int * p_total = nullptr);

	// 日推：POST /weapi/v1/discovery/recommend/songs
	// 响应里歌曲数组可能是 data.dailySongs / data.recommend / 顶层 recommend，三种都兼容。
	ApiCall daily_recommend(std::vector<TrackInfo> & out);

	// 漫游（私人 FM）：POST /weapi/radio/get
	// 注意两点：
	//  1) 它返回的是**旧字段结构**（artists / album / duration），parse_track 已兼容；
	//  2) 一次只返回几首（实测 3 首），但**每次调用给的都是新歌**，所以这里循环累积
	//     到 want 首为止 —— 漫游本来就该是取之不尽的。
	// exclude 里的 id 会跳过，用于面板的「更多」。
	// mode 用来选漫游模式（"华语漫游"之类）。传空串就是默认漫游。
	// 注意：模式值必须先实测确认，猜的值服务端会忽略。
	ApiCall personal_fm(std::vector<TrackInfo> & out, int want = 30,
		const std::vector<int64_t> * exclude = nullptr, const char * mode = nullptr);

	// 播放上报（打卡）：POST /weapi/feedback/weblog
	// 不上报的话，foobar2000 里播的歌不会进服务端的"最近播放"。
	// end 传 "playend" 表示播完了；played_seconds 是实际听了多少秒。
	ApiCall report_play(int64_t song_id, int played_seconds, const char * end);

	// 最近播放（客户端自己的接口）：POST /eapi/play-record/song/list
	// 实测裸参数 {"limit":"300"} 可用，一次最多 300 首；
	// 另外两条也叫 play-record/*/list（playlist / album）。
	// 注意：照抄客户端的 header/e_r 字段会被网关静默拒绝（HTTP 200 空响应体）。
	ApiCall recent_played_v2(int limit, std::vector<TrackInfo> & out);

	// 最近播放（旧 weapi 接口，保留作为兜底）：POST /weapi/v1/play/record
	// 响应是 allData / weekData 两个数组，元素形如 {playCount, score, song:{...}}。
	ApiCall recent_played(int64_t uid, int limit, int offset, std::vector<TrackInfo> & out);

	// 私人雷达：它**不是独立接口**，而是官方以"歌单"形式给出的一个推荐歌单。
	// 实测（2026-10-05）：/weapi/v1/discovery/recommend/resource 的 recommend 列表里
	// 第一条就是「私人雷达」。这里按名字查它的歌单 id，查不到再退回已知 id。
	ApiCall private_radar_playlist(std::string & name, int64_t & playlist_id);

	// 在 /v1/discovery/recommend/resource（客户端"精选"页的数据源）里，
	// 按名字**包含 keyword** 找一个官方歌单。
	// "华语漫游"就是这么一项：「华语私人雷达 | 最懂你的华语推荐 每日更新35首」，
	// 与"私人雷达"同源，都不是 FM 参数。找不到时退回 fallback_name/fallback_id。
	ApiCall featured_playlist(const std::string & keyword, const std::string & fallback_name,
		int64_t fallback_id, std::string & name, int64_t & playlist_id);

	// 歌词：POST /weapi/song/lyric
	// 返回 lrc.lyric（原文）与 tlyric.lyric（翻译，可能没有）。
	// 实测：未登录也能拿到歌词，登录后更稳。
	ApiCall lyrics(int64_t id, std::string & lrc, std::string & translated);

	// 音质降级链：从请求档位起依次往下试，返回第一个能拿到直链的结果。
	// 顺序：hires -> lossless -> exhigh -> higher -> standard
	SongUrlInfo song_url_with_fallback(int64_t id, const std::string & preferred_level,
		std::string * used_level = nullptr);


	CookieJar & jar() { return m_jar; }
	void set_user_agent(const std::string & ua) { m_user_agent = ua; }
	// 设置中断回调；返回 true 表示调用方已取消。会应用到之后所有请求。
	void set_abort(std::function<bool()> fn) { m_abort = std::move(fn); }
	void set_referer(const std::string & r) { m_referer = r; }

	static const char * base_url();
	// 诊断用：实际发出去的 appver（客户端版本），排查登录被拒时非常有用。
	static std::string outgoing_appver();

private:
	// host 为空时用 base_url()（music.163.com）；eapi 要走 interfacepc.music.163.com ——
	// 实测打到 music.163.com 会返回 {"code":404,"message":"接口未找到！"}。
	ApiCall post_common(const std::string & path, const std::string & body,
		const char * host = nullptr, const char * content_type = nullptr);
	CookieJar & m_jar;
	int m_timeout_ms;
	std::string m_user_agent;
	std::function<bool()> m_abort;
	std::string m_referer;
};

// 二维码文本内容：https://music.163.com/login?codekey=<unikey>
std::string qrcode_login_url(const std::string & unikey);

} // namespace netease

