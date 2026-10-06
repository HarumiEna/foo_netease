#include "core/api.h"

#include <algorithm>

#include "core/crypto.h"
#include "core/text.h"

#include <chrono>
#include <cstdio>
#include <random>

namespace netease {

namespace {

const char * kDefaultUserAgent =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
	"Chrome/122.0.0.0 Safari/537.36";

// 依据参考实现（ncm-api util/request.js 的 processCookieObject）补齐客户端"指纹"。
// 密码登录比扫码受更严的风控，Cookie 太单薄时服务端会回一个不说明原因的通用失败码。
std::string random_hex(size_t bytes) {
	static std::random_device rd;
	std::uniform_int_distribution<int> dist(0, 15);
	static const char * kHex = "0123456789abcdef";
	std::string out;
	out.reserve(bytes * 2);
	for (size_t i = 0; i < bytes; ++i) {
		const int v = dist(rd);
		out.push_back(kHex[v]);
	}
	return out;
}

std::string random_letters(size_t count) {
	static std::random_device rd;
	std::uniform_int_distribution<int> dist(0, 25);
	std::string out;
	for (size_t i = 0; i < count; ++i) out.push_back(static_cast<char>('a' + dist(rd)));
	return out;
}

// 用 chrono 取毫秒时间戳，避免让 core/ 依赖 windows.h。
std::string now_millis() {
	using namespace std::chrono;
	return std::to_string(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// 当前 PC 客户端版本。值本身是有意义的：服务端会按 appver 判断"这个客户端还能不能登录"，
// 太旧会直接回 {"code":502,"message":"请切换登录方式或升级版本"}（实测 appver=2.9.7 就被拒）。
const char * kPcAppver = "3.1.17.204416";
const char * kPcOsver = "Microsoft-Windows-10-Professional-build-19045-64bit";

void enrich_client_fingerprint(CookieJar & jar) {
	// ⚠ 这里是踩过的坑：客户端身份字段**必须每次以当前版本为准**。
	// 曾经写成"仅在缺失时设置"，结果登录凭据里存下的旧 appver 被一路沿用，
	// 服务端一直回"请切换登录方式或升级版本"，而排查时完全看不出是版本问题。
	jar.set("os", "pc");
	jar.set("appver", kPcAppver);
	jar.set("osver", kPcOsver);
	jar.set("channel", "netease");
	jar.set("__remember_me", "true");
	jar.set("ntes_kaola_ad", "1");
	jar.set("WEVNSM", "1.0.0");

	// 设备的随机标识则相反：一旦生成就保持稳定，否则每次请求都换设备，风控会觉得可疑。
	const std::string nuid = jar.has("_ntes_nuid") ? jar.get("_ntes_nuid") : random_hex(16);
	if (!jar.has("_ntes_nuid")) jar.set("_ntes_nuid", nuid);
	if (!jar.has("_ntes_nnid")) jar.set("_ntes_nnid", nuid + "," + now_millis());
	if (!jar.has("WNMCID")) jar.set("WNMCID", random_letters(6) + "." + now_millis() + ".01.0");
	if (!jar.has("deviceId")) {
		const std::string raw = random_hex(16);
		std::string mac;
		for (size_t i = 0; i < 16; i += 2) {
			if (!mac.empty()) mac.push_back(':');
			mac += raw.substr(i, 2);
		}
		jar.set("deviceId", mac);
	}
}

// 供诊断日志使用：把真正要发出去的客户端版本读出来。
std::string client_appver() { return kPcAppver; }

std::string status_hint(int status) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "HTTP 状态码 %d", status);
	return buf;
}

} // namespace

const char * NeteaseApi::base_url() { return "https://music.163.com"; }

std::string NeteaseApi::outgoing_appver() { return client_appver(); }

std::string qrcode_login_url(const std::string & unikey) {
	return "https://music.163.com/login?codekey=" + unikey;
}

NeteaseApi::NeteaseApi(CookieJar & jar, int timeout_ms)
	: m_jar(jar), m_timeout_ms(timeout_ms), m_user_agent(kDefaultUserAgent),
	  m_referer("https://music.163.com") {
	// 客户端身份 Cookie 每次构造都刷新（见 enrich_client_fingerprint 里的说明）；
	// MUSIC_U 等真实登录票据由登录流程写入，不会被这里覆盖。
	enrich_client_fingerprint(m_jar);
}

ApiCall NeteaseApi::post_common(const std::string & path, const std::string & body,
	const char * host, const char * content_type) {
	ApiCall call;

	HttpRequestOptions options;
	options.timeout_ms = m_timeout_ms;
	options.headers.push_back({ "Content-Type", content_type ? content_type : "application/x-www-form-urlencoded" });
	options.headers.push_back({ "Referer", m_referer });
	options.headers.push_back({ "User-Agent", m_user_agent });
	options.headers.push_back({ "Accept", "*/*" });
	options.abort_requested = m_abort;

	const std::string url = std::string(host ? host : base_url()) + path;
	HttpResult http = http_post_form(url, body, m_jar, options);
	if (!http.ok) {
		call.error = http.error;
		return call;
	}
	if (m_abort && m_abort()) {   // 请求过程中被取消：不要拿半截数据当结果
		call.error = "已取消";
		return call;
	}
	call.http_status = http.response.status;
	call.raw_body = http.response.body;

	if (http.response.status < 200 || http.response.status >= 300) {
		call.error = status_hint(http.response.status);
		return call;
	}

	std::string parse_error;
	if (!json::Value::parse(call.raw_body, call.json, &parse_error)) {
		call.error = "响应不是合法 JSON：" + parse_error;
		return call;
	}
	call.ok = true;
	return call;
}

ApiCall NeteaseApi::weapi_post_on(const char * host, const std::string & path,
	const std::string & json_body) {
	ApiCall call;
	WeapiResult enc;
	std::string error;
	if (!weapi_encrypt(json_body, enc, &error)) {
		call.error = "weapi 加密失败：" + error;
		return call;
	}
	const std::string body = "params=" + url_encode(enc.params) + "&encSecKey=" + enc.enc_sec_key;
	return post_common("/weapi" + path, body, host);
}

ApiCall NeteaseApi::weapi_post(const std::string & path, const std::string & json_body) {
	ApiCall call;
	WeapiResult enc;
	std::string error;
	if (!weapi_encrypt(json_body, enc, &error)) {
		call.error = "weapi 加密失败：" + error;
		return call;
	}
	const std::string body = "params=" + url_encode(enc.params) + "&encSecKey=" + enc.enc_sec_key;
	return post_common("/weapi" + path, body);
}

ApiCall NeteaseApi::eapi_post(const std::string & path, const std::string & json_body) {
	ApiCall call;
	std::string params;
	std::string error;
	// 摘要与密文里的路径要用 /api<path>，而请求 URL 是 /eapi<path> ——
	// 两处不一样（社区实现如此）。写错了服务端一律回 {"code":404,"message":"接口未找到！"}。
	if (!eapi_encrypt("/api" + path, json_body, params, &error)) {
		call.error = "eapi 加密失败：" + error;
		return call;
	}
	const std::string body = "params=" + params;
	// eapi 的宿主是 interfacepc.music.163.com（客户端实测），
	// 打到 music.163.com 会被网关当成"接口不存在"。
	return post_common("/eapi" + path, body, "https://interfacepc.music.163.com");
}

ApiCall NeteaseApi::qrcode_unikey() {
	return weapi_post("/login/qrcode/unikey", "{\"type\":3}");
}

ApiCall NeteaseApi::qrcode_check(const std::string & key) {
	return weapi_post("/login/qrcode/client/login", "{\"key\":\"" + json::escape(key) + "\",\"type\":3}");
}

SongUrlInfo NeteaseApi::song_url(int64_t id, const std::string & level, const std::string & encode_type) {
	SongUrlInfo info;
	const std::string body = "{\"ids\":\"[" + std::to_string(id) + "]\",\"level\":\"" +
		json::escape(level) + "\",\"encodeType\":\"" + json::escape(encode_type) + "\"}";

	ApiCall call = weapi_post("/song/enhance/player/url/v1", body);
	info.raw = call.json;
	if (!call.ok) {
		info.error = call.error.empty() ? ("服务端返回 code=" + call.json.find("code")->as_string("?")) : call.error;
		if (!call.raw_body.empty() && info.error.empty()) info.error = call.raw_body;
		return info;
	}

	const json::Value * data = call.json.find("data");
	const json::Value * first = (data && data->is_array()) ? data->at(0) : nullptr;
	if (!first) {
		info.error = "响应里没有 data[0]：" + call.raw_body;
		return info;
	}

	info.id = first->find("id") ? first->find("id")->as_int64() : id;
	if (const json::Value * v = first->find("url")) info.url = v->as_string();
	if (const json::Value * v = first->find("br")) info.br = v->as_int64();
	if (const json::Value * v = first->find("size")) info.size = v->as_int64();
	if (const json::Value * v = first->find("level")) info.level = v->as_string();
	if (const json::Value * v = first->find("type")) info.type = v->as_string();
	if (const json::Value * v = first->find("fee")) info.fee = v->as_int64();
	if (const json::Value * v = first->find("expi")) info.expi = v->as_int64();
	if (const json::Value * v = first->find("freeTrialInfo")) {
		info.has_free_trial = !v->is_null();
		if (info.has_free_trial) {
			if (const json::Value * end = v->find("end")) info.trial_end = end->as_int64();
		}
	}

	info.ok = true;
	if (info.url.empty()) {
		// 把服务端给的线索带上：fee 表示付费属性，code 是曲目状态码。
		// 这样"为什么放不了"（无版权 / 需会员 / 未登录 / id 不存在）能有个方向。
		const int64_t song_code = first->find("code") ? first->find("code")->as_int64() : -1;
		std::string reason = "服务端未返回直链（曲目状态码 " + std::to_string(song_code) +
			"，fee=" + std::to_string(info.fee) + "）";
		if (info.fee != 0) reason += "：该曲目需要付费或会员";
		else reason += "：可能无版权、已下架，或需要登录";
		info.error = reason;
	}
	return info;
}

SongUrlInfo NeteaseApi::song_url_with_fallback(int64_t id, const std::string & preferred_level,
	std::string * used_level) {
	// 顺序固定：越往右越容易拿到。exhigh 未登录时服务端会自动降到 standard，
	// 但显式降级能让「用户选的档位拿不到」这件事被记录清楚。
	static const char * kChain[] = { "hires", "lossless", "exhigh", "higher", "standard" };
	bool started = false;
	SongUrlInfo last;
	for (const char * level : kChain) {
		if (!started) {
			if (preferred_level != level) continue;
			started = true;
		}
		SongUrlInfo info = song_url(id, level);
		if (!info.url.empty()) {
			if (used_level) *used_level = info.level.empty() ? level : info.level;
			return info;
		}
		// 网络层错误直接返回，不要拿降级链去掩盖网络问题。
		if (!info.ok) return info;
		last = info;   // 记住最后一次的业务层理由，别把有用信息丢掉换成一句套话
	}
	if (last.error.empty()) last.error = "拿不到播放直链";
	last.error += "（已从最高音质档位逐级尝试到最小）";
	last.ok = false;
	return last;
}


// 从 [{name:...}, ...] 里拼出 "a/b/c"；新旧两套 schema 的歌手数组都是这个形状。
static std::string join_names(const json::Value * array) {
	std::string out;
	if (!array || !array->is_array()) return out;
	for (size_t i = 0; i < array->size(); ++i) {
		const json::Value * one = array->at(i);
		if (!one) continue;
		if (const json::Value * n = one->find("name")) {
			if (!out.empty()) out += "/";
			out += n->as_string();
		}
	}
	return out;
}

// 解析一首歌。
//
// 这里刻意**同时兼容两套 schema**：v3/song/detail、v6/playlist/detail、cloudsearch 用
// 新字段（ar/al/dt），而漫游（radio/get）等仍返回旧字段（artists/album/duration）。
// 两套混用同一个解析函数，界面层就不必关心数据来自哪个接口。
TrackInfo parse_track(const json::Value & item) {
	TrackInfo track;
	if (const json::Value * v = item.find("id")) track.id = v->as_int64();
	if (const json::Value * v = item.find("name")) track.title = v->as_string();
	// 新：dt；旧：duration。
	if (const json::Value * v = item.find("dt")) track.duration_ms = v->as_int64();
	else if (const json::Value * v = item.find("duration")) track.duration_ms = v->as_int64();
	if (const json::Value * v = item.find("fee")) track.fee = v->as_int64();

	// 新：ar[]；旧：artists[]。
	track.artists = join_names(item.find("ar"));
	if (track.artists.empty()) track.artists = join_names(item.find("artists"));

	// 新：al；旧：album。
	const json::Value * al = item.find("al");
	if (!al) al = item.find("album");
	if (al) {
		if (const json::Value * n = al->find("name")) track.album = n->as_string();
		if (const json::Value * v = al->find("id")) track.album_id = v->as_int64();
		if (const json::Value * v = al->find("picUrl")) track.cover_url = v->as_string();
	}
	if (track.cover_url.empty()) {
		if (const json::Value * v = item.find("picUrl")) track.cover_url = v->as_string();
	}
	// no 是专辑内音轨号。没有它，播放列表里只能显示问号，也没法按序号排序。
	if (const json::Value * v = item.find("no")) track.track_number = v->as_int64();
	return track;
}

ApiCall NeteaseApi::daily_recommend(std::vector<TrackInfo> & out) {
	out.clear();
	ApiCall call = weapi_post("/v1/discovery/recommend/songs", "{\"csrf_token\":\"\"}");
	if (!call.ok) return call;

	// 歌曲数组在社区实现与实测里出现过三种位置，全部兼容。
	const json::Value * songs = nullptr;
	if (const json::Value * data = call.json.find("data")) {
		if (const json::Value * v = data->find("dailySongs"); v && v->is_array()) songs = v;
		else if (const json::Value * v = data->find("recommend"); v && v->is_array()) songs = v;
	}
	if (!songs) {
		if (const json::Value * v = call.json.find("recommend"); v && v->is_array()) songs = v;
	}
	if (!songs) {
		call.ok = false;
		call.error = "日推响应里没有歌曲数组：" + call.raw_body.substr(0, 300);
		return call;
	}
	for (size_t i = 0; i < songs->size(); ++i) {
		if (const json::Value * one = songs->at(i)) out.push_back(parse_track(*one));
	}
	return call;
}

ApiCall NeteaseApi::lyrics(int64_t id, std::string & lrc, std::string & translated) {
	lrc.clear();
	translated.clear();
	const std::string body = "{\"id\":" + std::to_string(id) +
		",\"lv\":-1,\"kv\":-1,\"tv\":-1,\"csrf_token\":\"\"}";
	ApiCall call = weapi_post("/song/lyric", body);
	if (!call.ok) return call;

	if (const json::Value * block = call.json.find("lrc")) {
		if (const json::Value * v = block->find("lyric")) lrc = v->as_string();
	}
	if (const json::Value * block = call.json.find("tlyric")) {
		if (const json::Value * v = block->find("lyric")) translated = v->as_string();
	}
	if (lrc.empty()) {
		call.ok = false;
		call.error = "这首歌没有歌词";
	}
	return call;
}

ApiCall NeteaseApi::private_radar_playlist(std::string & name, int64_t & playlist_id) {
	name.clear();
	playlist_id = 0;
	ApiCall call = weapi_post("/v1/discovery/recommend/resource", "{\"csrf_token\":\"\"}");
	if (!call.ok) return call;

	const json::Value * list = call.json.find("recommend");
	if (!list || !list->is_array()) {
		call.ok = false;
		call.error = "推荐歌单响应里没有 recommend 数组：" + call.raw_body.substr(0, 300);
		return call;
	}
	for (size_t i = 0; i < list->size(); ++i) {
		const json::Value * item = list->at(i);
		if (!item) continue;
		const json::Value * n = item->find("name");
		if (!n || n->as_string() != "私人雷达") continue;
		const json::Value * id = item->find("id");
		if (!id) continue;
		name = n->as_string();
		playlist_id = id->as_int64();
		return call;
	}
	// 请求成功但这批推荐里没有：退回已知 id（实测 3136952023 就是私人雷达）。
	name = "私人雷达";
	playlist_id = 3136952023;
	return call;
}

ApiCall NeteaseApi::featured_playlist(const std::string & keyword,
	const std::string & fallback_name, int64_t fallback_id, std::string & name, int64_t & playlist_id) {
	name.clear();
	playlist_id = 0;
	ApiCall call = weapi_post("/v1/discovery/recommend/resource", "{\"csrf_token\":\"\"}");
	if (!call.ok) return call;

	const json::Value * list = call.json.find("recommend");
	if (!list || !list->is_array()) {
		call.ok = false;
		call.error = "精选资源响应里没有 recommend 数组：" + call.raw_body.substr(0, 300);
		return call;
	}
	for (size_t i = 0; i < list->size(); ++i) {
		const json::Value * item = list->at(i);
		if (!item) continue;
		const json::Value * n = item->find("name");
		if (!n) continue;
		const std::string entry = n->as_string();
		if (entry.find(keyword) == std::string::npos) continue;
		// "华语" 也要能命中有"私人雷达"字样的项 —— 但要避免误抓普通私雷：
		// 这里只要求包含 keyword，具体用哪个由调用方给的关键词决定。
		const json::Value * id = item->find("id");
		if (!id) continue;
		name = entry;
		playlist_id = id->as_int64();
		return call;
	}
	name = fallback_name;
	playlist_id = fallback_id;
	return call;
}

ApiCall NeteaseApi::personal_fm(std::vector<TrackInfo> & out, int want,
	const std::vector<int64_t> * exclude, const char * mode) {
	out.clear();
	if (want <= 0) want = 1;

	std::vector<int64_t> taken;
	if (exclude) taken = *exclude;
	if (taken.empty()) taken.push_back(0);   // 占位，避免空表时反复线性查找

	ApiCall last;                 // 保存最后一次失败的原因，便于报错
	int rounds = 0;
	const int kMaxRounds = 40;    // 硬上限，防止服务端异常时死循环
	while (static_cast<int>(out.size()) < want && rounds < kMaxRounds) {
		++rounds;
		// 漫游模式（华语/欧美/日语…）就是 body 里多一个 mode 字段。
		std::string body = "{\"csrf_token\":\"\"";
		if (mode && *mode) {
			body += ",\"mode\":\"" + json::escape(mode) + "\"";
		}
		body += "}";
		ApiCall call = weapi_post("/radio/get", body);
		if (!call.ok) {
			if (out.empty()) return call;   // 一首都没拿到才算失败
			break;
		}
		const json::Value * data = call.json.find("data");
		if (!data || !data->is_array()) {
			if (out.empty()) {
				call.ok = false;
				call.error = "漫游响应里没有 data 数组：" + call.raw_body.substr(0, 300);
				return call;
			}
			break;
		}
		size_t added = 0;
		for (size_t i = 0; i < data->size(); ++i) {
			const json::Value * one = data->at(i);
			if (!one) continue;
			TrackInfo t = parse_track(*one);
			if (t.id == 0) continue;
			if (std::find(taken.begin(), taken.end(), t.id) != taken.end()) continue;
			taken.push_back(t.id);
			out.push_back(std::move(t));
			++added;
			if (static_cast<int>(out.size()) >= want) break;
		}
		// 服务端不再给新歌就停，别空转。
		if (added == 0) break;
	}
	last.ok = true;
	return last;
}

ApiCall NeteaseApi::account(int64_t & uid, std::string & nickname) {
	ApiCall call = weapi_post("/w/nuser/account/get", "{\"csrf_token\":\"\"}");
	uid = 0;
	nickname.clear();
	if (!call.ok) return call;

	if (const json::Value * account = call.json.find("account")) {
		if (const json::Value * v = account->find("id")) uid = v->as_int64();
	}
	if (const json::Value * profile = call.json.find("profile")) {
		if (uid == 0) {
			if (const json::Value * v = profile->find("userId")) uid = v->as_int64();
		}
		if (const json::Value * v = profile->find("nickname")) nickname = v->as_string();
	}
	if (uid == 0 && nickname.empty()) {
		call.ok = false;
		call.error = "账号接口没有返回 uid 或昵称：" + call.raw_body;
	}
	return call;
}

ApiCall NeteaseApi::user_playlists(int64_t uid, int limit, int offset,
	std::vector<PlaylistInfo> & out) {
	out.clear();
	const std::string body = "{\"uid\":" + std::to_string(uid) +
		",\"limit\":" + std::to_string(limit) +
		",\"offset\":" + std::to_string(offset) +
		",\"includeVideo\":true,\"csrf_token\":\"\"}";
	ApiCall call = weapi_post("/user/playlist", body);
	if (!call.ok) return call;

	const json::Value * list = call.json.find("playlist");
	if (!list || !list->is_array()) {
		call.ok = false;
		call.error = "歌单列表响应里没有 playlist 数组：" + call.raw_body;
		return call;
	}
	for (size_t i = 0; i < list->size(); ++i) {
		const json::Value * item = list->at(i);
		if (!item) continue;
		PlaylistInfo info;
		if (const json::Value * v = item->find("id")) info.id = v->as_int64();
		if (const json::Value * v = item->find("name")) info.name = v->as_string();
		if (const json::Value * v = item->find("trackCount")) info.track_count = v->as_int64();
		if (const json::Value * c = item->find("creator")) {
			if (const json::Value * n = c->find("nickname")) info.creator = n->as_string();
		}
		if (info.id != 0) out.push_back(std::move(info));
	}
	return call;
}

ApiCall NeteaseApi::recent_played(int64_t uid, int limit, int offset,
	std::vector<TrackInfo> & out) {
	out.clear();
	if (uid <= 0) {
		ApiCall bad;
		bad.ok = false;
		bad.error = "还没拿到 uid（先刷新歌单列表）";
		return bad;
	}
	const std::string body = "{\"uid\":" + std::to_string(uid) +
		",\"type\":0,\"limit\":" + std::to_string(limit) +
		",\"offset\":" + std::to_string(offset) +
		",\"total\":true,\"csrf_token\":\"\"}";
	ApiCall call = weapi_post("/v1/play/record", body);
	if (!call.ok) return call;

	// allData = 所有时间，weekData = 最近一周；优先 allData（"最近播放"通常指这个）。
	const json::Value * arr = call.json.find("allData");
	if (!arr || !arr->is_array()) arr = call.json.find("weekData");
	if (!arr || !arr->is_array()) {
		call.ok = false;
		call.error = "最近播放响应里没有 allData/weekData：" + call.raw_body.substr(0, 300);
		return call;
	}
	for (size_t i = 0; i < arr->size(); ++i) {
		const json::Value * one = arr->at(i);
		if (!one) continue;
		const json::Value * song = one->find("song");
		if (!song) continue;
		TrackInfo t = parse_track(*song);
		if (t.id != 0) out.push_back(std::move(t));
	}
	return call;
}

std::string NeteaseApi::cookie(const char * name) const {
	return name ? m_jar.get(name) : std::string();
}

ApiCall NeteaseApi::get_common(const std::string & path, const char * host) {
	ApiCall call;
	HttpRequestOptions options;
	options.timeout_ms = m_timeout_ms;
	options.headers.push_back({ "Referer", m_referer });
	options.headers.push_back({ "User-Agent", m_user_agent });
	options.headers.push_back({ "Accept", "*/*" });
	options.abort_requested = m_abort;
	const std::string url = std::string(host ? host : base_url()) + path;
	HttpResult http = http_get(url, m_jar, options);
	if (!http.ok) { call.error = http.error; return call; }
	call.http_status = http.response.status;
	call.raw_body = http.response.body;
	if (http.response.status < 200 || http.response.status >= 300) {
		call.error = "HTTP 状态码 " + std::to_string(http.response.status);
		return call;
	}
	std::string pe;
	if (!json::Value::parse(call.raw_body, call.json, &pe)) { call.error = "响应不是合法 JSON：" + pe; return call; }
	call.ok = true;
	return call;
}

ApiCall NeteaseApi::report_play_web_minimal(int64_t song_id, int played_seconds, const char * end) {
	// 浏览器实测：网页版发这条日志时，Cookie 里只有 _ntes_nnid / WNMCID / MUSIC_U / __csrf，
	// 没有 os=pc、appver、clientSign、deviceId 这些"客户端指纹"。
	// 服务端很可能按指纹决定"这条日志算不算数"——这里就临时换成纯网页身份试。
	const std::string saved = m_jar.serialize();
	const std::string music_u = m_jar.get("MUSIC_U");
	const std::string csrf = m_jar.get("__csrf");
	const std::string wnmcid = m_jar.get("WNMCID");
	const std::string nnid = m_jar.get("_ntes_nnid");
	m_jar.clear();
	if (!music_u.empty()) m_jar.set("MUSIC_U", music_u);
	if (!csrf.empty()) m_jar.set("__csrf", csrf);
	if (!wnmcid.empty()) m_jar.set("WNMCID", wnmcid);
	if (!nnid.empty()) m_jar.set("_ntes_nnid", nnid);

	const std::string inner = std::string("{\"type\":\"song\",\"wifi\":0,\"download\":0,\"id\":") +
		std::to_string(song_id) + ",\"time\":" + std::to_string(played_seconds) +
		",\"end\":\"" + (end ? end : "ui") + "\",\"source\":\"list\",\"mainsite\":\"1\","
		"\"mainsiteWeb\":\"1\",\"content\":\"\"}";
	const std::string logs = "[{\"action\":\"play\",\"json\":" + inner + "}]";
	const std::string body = "{\"logs\":\"" + json::escape(logs) + "\",\"csrf_token\":\"" +
		json::escape(csrf) + "\"}";
	const std::string path = std::string("/feedback/weblog?csrf_token=") + csrf;
	ApiCall call = weapi_post_on("https://clientlogusf.music.163.com", path, body);
	m_jar.deserialize(saved);
	return call;
}

ApiCall NeteaseApi::report_play_web_styled(int64_t song_id, int played_seconds, const char * end,
	const char * source, const char * source_id, const char * host) {
	std::string inner = "{\"type\":\"song\",\"wifi\":0,\"download\":0,\"id\":" +
		std::to_string(song_id) + ",\"time\":" + std::to_string(played_seconds) +
		",\"end\":\"" + (end ? end : "ui") + "\",\"source\":\"" + (source ? source : "list") + "\"";
	if (source_id && *source_id) inner += ",\"sourceId\":\"" + std::string(source_id) + "\"";
	inner += ",\"mainsite\":\"1\",\"mainsiteWeb\":\"1\",\"content\":\"id=" + std::string(source_id) + "\"}";
	const std::string logs = "[{\"action\":\"play\",\"json\":" + inner + "}]";
	const std::string body = "{\"logs\":\"" + json::escape(logs) + "\",\"csrf_token\":\"" +
		json::escape(m_jar.get("__csrf")) + "\"}";

	// 临时换成网页 UA，并补上客户端那套头，然后原样 POST 裸 JSON。
	const std::string saved_ua = m_user_agent;
	m_user_agent = "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
		"Safari/537.36 Chrome/91.0.4472.164 NeteaseMusicWeb/1.0";
	ApiCall call;
	HttpRequestOptions options;
	options.timeout_ms = m_timeout_ms;
	options.headers.push_back({ "Content-Type", "application/json" });
	options.headers.push_back({ "Referer", "https://music.163.com/" });
	options.headers.push_back({ "User-Agent", m_user_agent });
	options.headers.push_back({ "Accept", "*/*" });
	options.headers.push_back({ "X-Client-Enc-State", "ENCRYPTED" });
	options.headers.push_back({ "MConfig-Info", "{\"appver\":\"3.1.40.205461\"}" });
	options.abort_requested = m_abort;
	const std::string url = std::string(host ? host : base_url()) + "/api/feedback/weblog";
	HttpResult http = http_post_form(url, body, m_jar, options);
	m_user_agent = saved_ua;
	if (!http.ok) { call.error = http.error; return call; }
	call.http_status = http.response.status;
	call.raw_body = http.response.body;
	if (http.response.status < 200 || http.response.status >= 300) { call.error = "HTTP 状态码 " + std::to_string(http.response.status); return call; }
	std::string pe;
	if (!json::Value::parse(call.raw_body, call.json, &pe)) { call.error = "响应不是合法 JSON：" + pe; return call; }
	call.ok = true;
	return call;
}

ApiCall NeteaseApi::report_play_web(int64_t song_id, int played_seconds, const char * end,
	const char * source, const char * source_id, const char * host) {
	// 完全照抄网页版抓到的格式（明文 JSON，非表单、非 weapi）。
	std::string inner = "{\"type\":\"song\",\"wifi\":0,\"download\":0,\"id\":" +
		std::to_string(song_id) + ",\"time\":" + std::to_string(played_seconds) +
		",\"end\":\"" + (end ? end : "playend") + "\",\"source\":\"" + (source ? source : "list") + "\"";
	if (source_id && *source_id) inner += ",\"sourceId\":\"" + std::string(source_id) + "\"";
	inner += ",\"mainsite\":\"1\",\"mainsiteWeb\":\"1\",\"content\":\"\"}";
	const std::string logs = "[{\"action\":\"play\",\"json\":" + inner + "}]";
	const std::string csrf = m_jar.get("__csrf");
	const std::string body = "{\"logs\":\"" + json::escape(logs) + "\",\"csrf_token\":\"" +
		json::escape(csrf) + "\"}";
	return post_common("/api/feedback/weblog", body, host, "application/json");
}

ApiCall NeteaseApi::plain_api_post(const std::string & path, const std::string & form_body) {
	// web 端注册表里 "bi-log" 的 url 就是 /api/feedback/weblog，且体子是明文表单。
	return post_common(path, form_body);
}

ApiCall NeteaseApi::report_play(int64_t song_id, int played_seconds, const char * end) {
	// 客户端上报播放用的就是这个结构（logs 是"JSON 字符串里再嵌一个 JSON 数组"）。
	const std::string logs = std::string("[{\"action\":\"play\",\"json\":{\"download\":0,\"end\":\"") +
		(end ? end : "playend") + "\",\"id\":" + std::to_string(song_id) +
		",\"sourceId\":0,\"time\":" + std::to_string(played_seconds) +
		",\"type\":\"song\",\"wifi\":0,\"source\":\"list\"}}]";
	const std::string body = "{\"logs\":\"" + json::escape(logs) + "\"}";
	return weapi_post("/feedback/weblog", body);
}

ApiCall NeteaseApi::recent_played_v2(int limit, std::vector<TrackInfo> & out) {
	out.clear();
	const std::string body = "{\"limit\":\"" + std::to_string(limit) + "\"}";
	ApiCall call = eapi_post("/play-record/song/list", body);
	if (!call.ok) return call;

	// 响应形状：{"code":200,"data":{"total":300,"list":[{"resourceId":"...",
	//   "playTime":...,"resourceType":"SONG","data":{<歌曲对象>}}]}}
	const json::Value * data = call.json.find("data");
	const json::Value * list = data ? data->find("list") : nullptr;
	if (!list || !list->is_array()) {
		call.ok = false;
		call.error = "play-record 响应里没有 data.list：" + call.raw_body.substr(0, 200);
		return call;
	}
	for (size_t i = 0; i < list->size(); ++i) {
		const json::Value * item = list->at(i);
		if (!item) continue;
		const json::Value * song = item->find("data");
		if (!song) continue;
		TrackInfo t = parse_track(*song);
		if (t.id != 0) out.push_back(std::move(t));
	}
	return call;
}

ApiCall NeteaseApi::playlist_square_list(const std::string & cat, int limit, int offset,
	std::vector<PlaylistInfo> & out) {
	out.clear();
	const std::string body = "{\"cat\":\"" + json::escape(cat) +
		"\",\"order\":\"hot\",\"limit\":" + std::to_string(limit) +
		",\"offset\":" + std::to_string(offset) +
		",\"total\":true,\"csrf_token\":\"\"}";
	ApiCall call = weapi_post("/playlist/list", body);
	if (!call.ok) return call;

	const json::Value * list = call.json.find("playlists");
	if (!list || !list->is_array()) {
		call.ok = false;
		call.error = "歌单广场响应里没有 playlists：" + call.raw_body.substr(0, 200);
		return call;
	}
	for (size_t i = 0; i < list->size(); ++i) {
		const json::Value * item = list->at(i);
		if (!item) continue;
		PlaylistInfo info;
		if (const json::Value * v = item->find("id")) info.id = v->as_int64();
		if (const json::Value * v = item->find("name")) info.name = v->as_string();
		if (const json::Value * v = item->find("trackCount")) info.track_count = v->as_int64();
		if (const json::Value * c = item->find("creator")) {
			if (const json::Value * n = c->find("nickname")) info.creator = n->as_string();
		}
		if (info.id != 0) out.push_back(std::move(info));
	}
	return call;
}

ApiCall NeteaseApi::playlist_track_ids(int64_t playlist_id, std::vector<int64_t> & ids,
	std::string & name, int64_t & track_count) {
	ids.clear();
	name.clear();
	track_count = -1;
	const std::string body = "{\"id\":" + std::to_string(playlist_id) +
		",\"n\":100000,\"s\":8,\"csrf_token\":\"\"}";
	ApiCall call = weapi_post("/v6/playlist/detail", body);
	if (!call.ok) return call;

	const json::Value * playlist = call.json.find("playlist");
	if (!playlist) {
		call.ok = false;
		call.error = "歌单详情里没有 playlist：" + call.raw_body;
		return call;
	}
	if (const json::Value * v = playlist->find("name")) name = v->as_string();
	if (const json::Value * v = playlist->find("trackCount")) track_count = v->as_int64();

	// trackIds 是全集；tracks 会被截断（实测 1368 首的歌单只返回 1000 条）。
	if (const json::Value * arr = playlist->find("trackIds")) {
		if (arr->is_array()) {
			for (size_t i = 0; i < arr->size(); ++i) {
				const json::Value * item = arr->at(i);
				if (!item) continue;
				const int64_t id = item->is_object()
					? (item->find("id") ? item->find("id")->as_int64() : 0)
					: item->as_int64();
				if (id != 0) ids.push_back(id);
			}
		}
	}
	return call;
}

ApiCall NeteaseApi::song_details(const std::vector<int64_t> & ids, std::vector<TrackInfo> & out,
	std::vector<int64_t> * missing) {
	out.clear();
	if (missing) missing->clear();
	if (ids.empty()) {
		ApiCall empty;
		empty.ok = true;
		return empty;
	}

	const size_t kBatch = 200;
	ApiCall last;
	last.ok = true;
	for (size_t start = 0; start < ids.size(); start += kBatch) {
		const size_t end = (std::min)(start + kBatch, ids.size());
		std::string c = "[";
		for (size_t i = start; i < end; ++i) {
			if (i != start) c += ",";
			c += "{\"id\":" + std::to_string(ids[i]) + "}";
		}
		c += "]";
		ApiCall call = weapi_post("/v3/song/detail",
			"{\"c\":\"" + json::escape(c) + "\",\"csrf_token\":\"\"}");
		if (!call.ok) return call;
		last = call;

		const json::Value * songs = call.json.find("songs");
		if (!songs || !songs->is_array()) {
			call.ok = false;
			call.error = "歌曲详情里没有 songs 数组：" + call.raw_body;
			return call;
		}
		for (size_t i = 0; i < songs->size(); ++i) {
			if (const json::Value * item = songs->at(i)) out.push_back(parse_track(*item));
		}
	}
	if (missing) {
		// 服务端可能对下架的曲目直接不给数据，这里算出缺哪些 id，便于上层提示。
		for (int64_t id : ids) {
			bool found = false;
			for (const TrackInfo & t : out) {
				if (t.id == id) { found = true; break; }
			}
			if (!found) missing->push_back(id);
		}
	}
	return last;
}

ApiCall NeteaseApi::search_songs(const std::string & keyword, int limit, int offset,
	std::vector<TrackInfo> & out, int * p_total) {
	out.clear();
	const std::string body = "{\"s\":\"" + json::escape(keyword) +
		"\",\"type\":1,\"limit\":" + std::to_string(limit) +
		",\"offset\":" + std::to_string(offset) + ",\"csrf_token\":\"\"}";
	ApiCall call = weapi_post("/cloudsearch/get/web", body);
	if (!call.ok) return call;

	const json::Value * result = call.json.find("result");
	if (p_total) {
		*p_total = 0;
		if (const json::Value * count = result ? result->find("songCount") : nullptr) {
			*p_total = static_cast<int>(count->as_int64());
		}
	}
	if (!result) {
		call.ok = false;
		call.error = "搜索响应里没有 result 字段：" + call.raw_body;
		return call;
	}
	// 注意：**翻页到底时接口返回的是 {"result":{"songCount":0},"code":200}** ——
	// 没有 songs 字段表示"没有更多了"，不能当失败（实测踩过）。
	const json::Value * songs = result->find("songs");
	if (!songs || !songs->is_array()) return call;   // ok，out 为空
	for (size_t i = 0; i < songs->size(); ++i) {
		if (const json::Value * item = songs->at(i)) out.push_back(parse_track(*item));
	}
	return call;
}

} // namespace netease

