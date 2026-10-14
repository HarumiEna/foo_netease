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

	// --- 「封面跟随」模式 ---
	//   0 = 面板自己决定（默认；组件不干预，列表里每行都是自己那首的封面）；
	//   1 = 正在播放（面板问的是选中那首时，换成正在播放那首）；
	//   2 = 选中项（面板问的是正在播放那首时，换成选中那首）。
	// 注意 1/2 是代换：列表里正在播放 / 选中那一行的封面也会跟着变 ——
	// foobar2000 的封面接口分不清请求来自哪个界面（而列表第一列就是封面列）。
	int cover_follow_mode() const;
	void set_cover_follow_mode(int mode);

	// --- 播放列表光标是否跟随正在播放（默认关，见 netease_data.h 的说明） ---
	bool follow_cursor() const;
	void set_follow_cursor(bool on);

	// --- 登录动作 ---
	void start_qr_login();   // 异步：申请 unikey -> 通知 UI -> 轮询
	void cancel_qr_login();  // 异步取消（不阻塞 UI）
	bool logout();           // 清凭据并落盘

	// --- 给 input 用：当前登录 Cookie ---
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
	std::atomic<int> m_cover_follow{ 0 };        // 封面跟随：0=面板自己决定，1=正在播放，2=选中项
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

