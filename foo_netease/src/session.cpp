#include "stdafx.h"
#include "session.h"

#include <chrono>

#include "component_log.h"
#include "core/crypto.h"
#include "core/dpapi.h"
#include "core/text.h"

namespace netease {

namespace {

// configStore 里用的键名。configStore 是 v2.0 起的 SQLite 配置库。
const char * kCfgCredentials = "foo_netease.credentials"; // DPAPI 密文的 base64
const char * kCfgQuality = "foo_netease.quality";
const char * kCfgAccount = "foo_netease.account";         // "uid|nickname"
const char * kCfgCoverFollow = "foo_netease.cover_follow"; // "1"/"0"
const char * kCfgFollowCursor = "foo_netease.follow_cursor"; // "1"/"0"

const int kPollIntervalMs = 2000;
const int kPollTotalMs = 600000;   // 单次登录会话最长等待 10 分钟（换新码会重置计时）
const int kRequestTimeoutMs = 20000;

std::string cfg_get(const char * key) {
	auto store = fb2k::configStore::get();
	if (!store.is_valid()) return std::string();
	auto value = store->getConfigString(key, "");
	return value.is_valid() ? std::string(value->c_str()) : std::string();
}

void cfg_set(const char * key, const std::string & value) {
	auto store = fb2k::configStore::get();
	if (!store.is_valid()) return;
	store->setConfigString(key, value.c_str());
}

void cfg_delete(const char * key) {
	auto store = fb2k::configStore::get();
	if (!store.is_valid()) return;
	store->deleteConfigString(key);
}

std::string join(const std::string & a, const std::string & b, char sep) { return a + sep + b; }



} // namespace

const QualityOption kQualityOptions[] = {
	{ "standard", "标准 (128k)" },
	{ "higher", "较高 (192k)" },
	{ "exhigh", "极高 (320k)" },
	{ "lossless", "无损 (FLAC)" },
	{ "hires", "Hi-Res" },
};
const size_t kQualityOptionCount = sizeof(kQualityOptions) / sizeof(kQualityOptions[0]);

const char * login_state_text(LoginState state) {
	switch (state) {
	case LoginState::NotLoggedIn: return "未登录";
	case LoginState::RequestingQr: return "正在申请二维码…";
	case LoginState::WaitingScan: return "请用网易云音乐 App 扫码";
	case LoginState::ScannedWaitingConfirm: return "已扫码，请在手机上确认";
	case LoginState::LoggedIn: return "已登录";
	case LoginState::NeedCaptcha: return "需要验证码";
	case LoginState::Failed: return "登录失败";
	}
	return "未知状态";
}

Session & Session::instance() {
	static Session inst;
	return inst;
}

void Session::init() {
	load_credentials();
}

void Session::shutdown() {
	m_shutting_down = true;
	++m_generation; // 让工作线程尽快退出
	clear_notify();
	if (m_worker.joinable()) m_worker.join();
}

void Session::set_notify(Notify fn) {
	std::lock_guard<std::mutex> lock(m_notify_mutex);
	m_notify = std::move(fn);
}

void Session::clear_notify() {
	std::lock_guard<std::mutex> lock(m_notify_mutex);
	m_notify = nullptr;
}

void Session::notify() {
	Notify fn;
	{
		std::lock_guard<std::mutex> lock(m_notify_mutex);
		fn = m_notify;
	}
	if (!fn) return;
	// 回调一律回到主线程；工作线程绝不直接碰 UI。
	fb2k::inMainThread([fn] { fn(); });
}

void Session::set_state(LoginState state, const std::string & detail) {
	m_state = state;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_detail = detail;
	}
	notify();
}

std::string Session::status_detail() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_detail;
}

AccountInfo Session::account() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_account;
}

std::string Session::qr_text() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_qr_text;
}

std::string Session::quality() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_quality.empty() ? "exhigh" : m_quality;
}

void Session::set_quality(const std::string & level) {
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_quality = level;
	}
	cfg_set(kCfgQuality, level);
}

std::string Session::cookie_header() const {
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_jar.header_value();
}

void Session::publish(const CookieJar & worker_jar) {
	std::lock_guard<std::mutex> lock(m_mutex);
	m_jar = worker_jar;
}

bool Session::cover_follow_now_playing() const { return m_cover_follow.load(); }

void Session::set_cover_follow_now_playing(bool on) {
	m_cover_follow = on;
	cfg_set(kCfgCoverFollow, on ? "1" : "0");
}

bool Session::follow_cursor() const { return m_follow_cursor.load(); }

void Session::set_follow_cursor(bool on) {
	m_follow_cursor = on;
	cfg_set(kCfgFollowCursor, on ? "1" : "0");
}

void Session::save_credentials() {
	const std::string cookie = cookie_header();
	if (cookie.empty()) return;

	Bytes cipher;
	std::string error;
	if (!dpapi_protect(cookie, cipher, &error)) {
		netease_log::write("foo_netease: 凭据加密失败，未保存 —— " + error);
		return;
	}
	cfg_set(kCfgCredentials, base64_encode(cipher));
	netease_log::write("foo_netease: 凭据已加密保存");
}

void Session::load_credentials() {
	const std::string stored = cfg_get(kCfgCredentials);
	m_quality = cfg_get(kCfgQuality);
	if (m_quality.empty()) m_quality = "exhigh";

	// 封面跟随"正在播放"：默认开，只有显式存过 "0" 才关。
	m_cover_follow = (cfg_get(kCfgCoverFollow) != "0");
	m_follow_cursor = (cfg_get(kCfgFollowCursor) == "1");   // 默认关

	if (!stored.empty()) {
		Bytes cipher;
		std::string cookie;
		std::string error;
		if (base64_decode(stored, cipher) && dpapi_unprotect(cipher, cookie, &error)) {
			CookieJar jar;
			jar.deserialize(cookie);
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_jar = jar;
			}
			if (jar.has("MUSIC_U")) {
				m_state = LoginState::LoggedIn;
				std::lock_guard<std::mutex> lock(m_mutex);
				m_detail = "已从本地凭据恢复登录（尚未向服务端校验）";
			} else {
				m_state = LoginState::NotLoggedIn;
			}
			netease_log::write("foo_netease: 已从本地恢复凭据（登录态=" +
				std::string(login_state_text(m_state.load())) + "）");
		} else {
			netease_log::write("foo_netease: 本地凭据无法解密，按未登录处理 —— " + error);
			cfg_delete(kCfgCredentials);
		}
	}

	// 恢复上次记录的账号信息，便于登录前就显示昵称。
	const std::string acc = cfg_get(kCfgAccount);
	const size_t sep = acc.find('|');
	if (sep != std::string::npos) {
		AccountInfo info;
		info.valid = true;
		info.uid = std::strtoll(acc.substr(0, sep).c_str(), nullptr, 10);
		info.nickname = acc.substr(sep + 1);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_account = info;
	}
}

bool Session::logout() {
	++m_generation;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_jar.clear();
		m_account = AccountInfo();
		m_qr_text.clear();
	}
	cfg_delete(kCfgCredentials);
	cfg_delete(kCfgAccount);
	set_state(LoginState::NotLoggedIn, "已登出，本地凭据已清除");
	netease_log::write("foo_netease: 已登出并清除本地凭据");
	return true;
}

void Session::cancel_qr_login() {
	++m_generation;
	if (m_state.load() == LoginState::WaitingScan || m_state.load() == LoginState::ScannedWaitingConfirm ||
		m_state.load() == LoginState::RequestingQr) {
		set_state(LoginState::NotLoggedIn, "已取消扫码");
	}
}

void Session::start_qr_login() {
	if (m_shutting_down) return;
	const uint64_t generation = ++m_generation;

	// 上一个工作线程可能还在跑，先放着——它靠 generation 自行退出，不 join，免得卡 UI。
	if (m_worker.joinable()) {
		// 只有在它确实结束时才 join；用一个 detached 的清理线程避免阻塞主线程。
		std::thread old = std::move(m_worker);
		std::thread([old = std::move(old)]() mutable { if (old.joinable()) old.join(); }).detach();
	}

	set_state(LoginState::RequestingQr, "正在向服务端申请二维码…");
	m_worker = std::thread([this, generation] { worker_qr_login(generation); });
}

void Session::worker_qr_login(uint64_t generation) {
	CookieJar worker_jar;
	// 保留本地已有的 Cookie（例如 NMTID），减少服务端风控概率。
	worker_jar.deserialize(cookie_header());
	NeteaseApi api(worker_jar, kRequestTimeoutMs);

	// 计时从「当前这张二维码」开始算；服务端让二维码过期时我们会重新申请并重置，
	// 因此用户可以慢慢扫码，不会因为超时而前功尽弃。
	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
	bool key_ready = false;

	while (!m_shutting_down && generation == m_generation.load()) {
		// 1) 需要（重新）申请二维码
		if (!key_ready) {
			ApiCall call = api.qrcode_unikey();
			publish(worker_jar);
			if (!call.ok) {
				set_state(LoginState::Failed, "申请二维码失败：" + call.error);
				return;
			}
			const json::Value * unikey = call.json.find("unikey");
			if (!unikey || !unikey->is_string()) {
				set_state(LoginState::Failed, "响应里没有 unikey：" + call.raw_body);
				return;
			}
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_qr_text = qrcode_login_url(unikey->as_string());
			}
			key_ready = true;
			started = std::chrono::steady_clock::now(); // 新码，重新计时
			set_state(LoginState::WaitingScan, "二维码已生成，请用网易云音乐 App 扫码");
			continue; // 立刻进入轮询
		}

		// 2) 轮询扫码结果
		const std::string key = [&] {
			const std::string url = qr_text();
			const std::string marker = "codekey=";
			const size_t pos = url.find(marker);
			return pos == std::string::npos ? std::string() : url.substr(pos + marker.size());
		}();
		if (key.empty()) {
			set_state(LoginState::Failed, "二维码内容异常，无法取出 key");
			return;
		}

		ApiCall poll = api.qrcode_check(key);
		publish(worker_jar);
		if (!poll.ok) {
			// 网络抖动不致命，继续轮询；但把原因记下来。
			std::lock_guard<std::mutex> lock(m_mutex);
			m_detail = "轮询失败：" + poll.error;
			notify();
		} else {
			const int64_t code = poll.json.find("code") ? poll.json.find("code")->as_int64() : -1;
			switch (code) {
			case 801:
				set_state(LoginState::WaitingScan, "等待扫码");
				break;
			case 802:
				set_state(LoginState::ScannedWaitingConfirm, "已扫码，请在手机上确认");
				break;
			case 800:
				// 二维码过期：重新申请一张，界面会自动换图。
				netease_log::write("foo_netease: 二维码已过期，自动重新申请");
				key_ready = false;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_qr_text.clear();
				}
				set_state(LoginState::RequestingQr, "二维码已过期，正在重新申请…");
				break;
			case 803: {
				// 登录成功：Cookie 已在 worker_jar 里。
				publish(worker_jar);
				apply_login_response(poll.json);
				save_credentials();
				set_state(LoginState::LoggedIn, "扫码登录成功");
				netease_log::write("foo_netease: 扫码登录成功");
				return;
			}
			default:
				set_state(LoginState::Failed, "未知返回码 " + std::to_string(code) + "：" + poll.raw_body);
				return;
			}
		}

		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count();
		if (elapsed > kPollTotalMs) {
			set_state(LoginState::Failed, "二维码等待超时，请重新登录");
			return;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
	}
}

// 803 的响应里会带账号信息。字段结构以实测为准，因此这里宽松解析，
// 并把原始响应记进日志，方便后续校正。
void Session::apply_login_response(const json::Value & response) {
	netease_log::write("foo_netease: 803 原始响应 = " + response.dump());

	AccountInfo info;
	const json::Value * profile = response.find("profile");
	const json::Value * account = response.find("account");
	const char * paths[] = { "nickname", "userName" };
	for (const char * field : paths) {
		if (info.nickname.empty() && profile) {
			if (const json::Value * v = profile->find(field)) info.nickname = v->as_string();
		}
		if (info.nickname.empty() && account) {
			if (const json::Value * v = account->find(field)) info.nickname = v->as_string();
		}
	}
	const char * id_fields[] = { "userId", "id" };
	for (const char * field : id_fields) {
		if (info.uid == 0 && profile) {
			if (const json::Value * v = profile->find(field)) info.uid = v->as_int64();
		}
		if (info.uid == 0 && account) {
			if (const json::Value * v = account->find(field)) info.uid = v->as_int64();
		}
	}
	info.valid = !info.nickname.empty() || info.uid != 0;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_account = info;
	}
	if (info.valid) {
		cfg_set(kCfgAccount, join(std::to_string(info.uid), info.nickname, '|'));
	} else {
		netease_log::write("foo_netease: 803 响应里没解析出账号信息，字段结构需要按实测校正");
	}
}



} // namespace netease

