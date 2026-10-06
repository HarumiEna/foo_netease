#pragma once
// foo_netease —— 登录会话。
//
// 职责：
//  · 持有登录 Cookie（工作线程用一份，UI/播放用一份只读快照）；
//  · 扫码登录的状态机与轮询；
//  · 凭据的加密持久化（DPAPI + configStore）；
//  · 状态变化时把通知送回主线程。
//
// 线程约定（重要）：
//  · 所有网络 I/O 都在工作线程；主线程只通过本类的 const 接口读状态；
//  · 状态变化用 fb2k::inMainThread 投递到主线程，UI 不需要自己同步。

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/api.h"
#include "core/http.h"

namespace netease {

enum class LoginState {
	NotLoggedIn,
	RequestingQr,
	WaitingScan,
	ScannedWaitingConfirm,
	LoggedIn,
	NeedCaptcha,   // 服务端要求验证码（实测 code=503）
	Failed,
};

const char * login_state_text(LoginState state);

struct AccountInfo {
	bool valid = false;
	int64_t uid = 0;
	std::string nickname;
};

struct QualityOption {
	const char * level;
	const char * label;
};

extern const QualityOption kQualityOptions[];
extern const size_t kQualityOptionCount;

class Session {
public:
	static Session & instance();

	void init();     // on_init
	void shutdown(); // on_quit

	// --- 状态（任意线程可读） ---
	LoginState state() const { return m_state.load(); }
	std::string status_detail() const;
	AccountInfo account() const;
	std::string qr_text() const;

	// --- 音质（默认 exhigh=320k） ---
	std::string quality() const;
	void set_quality(const std::string & level);

	// --- 封面是否跟随"正在播放"（默认开） ---
	// 关掉时按调用方给的曲目显示（通常是列表里选中的那首）。
	bool cover_follow_now_playing() const;
	void set_cover_follow_now_playing(bool on);

	// --- 播放列表光标是否跟随正在播放（默认关，见 netease_data.h 的说明） ---
	bool follow_cursor() const;
	void set_follow_cursor(bool on);

	// --- 登录动作 ---
	void start_qr_login();   // 异步：申请 unikey -> 通知 UI -> 轮询
	void cancel_qr_login();  // 异步取消（不阻塞 UI）
	bool logout();           // 清凭据并落盘

	// --- 给 input（M5）用：当前登录 Cookie ---
	std::string cookie_header() const;
	bool logged_in() const { return m_state.load() == LoginState::LoggedIn; }

	// --- UI 通知（回调在主线程执行） ---
	using Notify = std::function<void()>;
	void set_notify(Notify fn);
	void clear_notify();

private:
	Session() = default;
	~Session() = default;
	Session(const Session &) = delete;
	Session & operator=(const Session &) = delete;

	void worker_qr_login(uint64_t generation);
	void publish(const CookieJar & worker_jar);
	void set_state(LoginState state, const std::string & detail = std::string());
	void notify();
	void save_credentials();
	void load_credentials();
	void apply_login_response(const json::Value & response);

	std::atomic<LoginState> m_state{ LoginState::NotLoggedIn };
	std::atomic<bool> m_cover_follow{ true };   // 封面跟随正在播放
	std::atomic<bool> m_follow_cursor{ false };  // 列表光标跟随正在播放
	std::atomic<uint64_t> m_generation{ 0 };
	std::atomic<bool> m_shutting_down{ false };

	mutable std::mutex m_mutex;          // 保护下面这些可变状态
	std::string m_detail;
	std::string m_qr_text;
	CookieJar m_jar;                     // 已发布的快照（UI / 播放可读）
	AccountInfo m_account;
	std::string m_quality;

	std::thread m_worker;                // 只保留最后一个工作线程，join 在 shutdown

	mutable std::mutex m_notify_mutex;
	Notify m_notify;
};

} // namespace netease

