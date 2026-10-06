#include "stdafx.h"
#include "resource.h"

#ifdef _WIN32
#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>

#include "component_log.h"
#include "netease_data.h"
#include "core/api.h"
#include "login_ui.h"
#include "browse_ui.h"
#include "panel_ui.h"
#include "session.h"
#include "win_utf8.h"

namespace {

constexpr GUID guid_prefs_main = { 0x2ff5dddc, 0x593e, 0x4e97, { 0x8f, 0xb5, 0x97, 0x1e, 0xe7, 0x58, 0x09, 0x30 } };

void log_line(const std::string & text) {
	netease_log::write(text);
}

// 用当前登录态查一次歌曲直链，确认 VIP 音质确实生效（未登录时只会给 45 秒试听）。

// ---------------------------------------------------------------------------
// 设置页。
//  · 构造函数接收 preferences_page_callback；
//  · 不自己 Create 窗口，preferences_page_impl<> 负责；
//  · get_wnd() 由 preferences_page_impl<> 提供。
// ---------------------------------------------------------------------------
class netease_prefs_dialog : public CDialogImpl<netease_prefs_dialog>, public preferences_page_instance {
public:
	explicit netease_prefs_dialog(preferences_page_callback::ptr callback) : m_callback(callback) {}

	enum { IDD = IDD_NETEASE_PREFS };

	t_uint32 get_state() override {
		// 必须声明 dark_mode_supported，否则暗色模式下本页会被强制转成经典配色。
		return preferences_state::dark_mode_supported;
	}
	void apply() override {}
	void reset() override {}

	BEGIN_MSG_MAP_EX(netease_prefs_dialog)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_DESTROY(OnDestroy)
		COMMAND_HANDLER_EX(IDC_NETEASE_LOGIN_BTN, BN_CLICKED, OnLogin)
		COMMAND_HANDLER_EX(IDC_NETEASE_BROWSE_BTN, BN_CLICKED, OnBrowse)
		COMMAND_HANDLER_EX(IDC_NETEASE_LOGOUT_BTN, BN_CLICKED, OnLogout)
		COMMAND_HANDLER_EX(IDC_NETEASE_CLEAR, BN_CLICKED, OnClear)
		COMMAND_HANDLER_EX(IDC_NETEASE_PANEL_BTN, BN_CLICKED, OnPanel)
		COMMAND_HANDLER_EX(IDC_NETEASE_COVER_NOW, BN_CLICKED, OnCoverNow)
		COMMAND_HANDLER_EX(IDC_NETEASE_FOLLOW_CURSOR, BN_CLICKED, OnFollowCursor)
		COMMAND_HANDLER_EX(IDC_NETEASE_QUALITY, CBN_SELCHANGE, OnQualityChanged)
	END_MSG_MAP()

private:
	BOOL OnInitDialog(CWindow, LPARAM) {
		m_dark.AddDialogWithControls(*this);

		SetDlgItemText(IDC_NETEASE_HINT, netease::to_wide(
			"登录后即可浏览歌单 / 每日推荐 / 漫游 / 私人雷达并直接播放；"
			"面板可停靠在布局里（视图 → 布局 → 添加 → 网易云音乐）。"
			"日志写在 foobar2000 配置目录的 foo_netease.log。").c_str());
		SetDlgItemText(IDC_NETEASE_LOGIN_BTN, netease::to_wide("扫码登录").c_str());
		SetDlgItemText(IDC_NETEASE_BROWSE_BTN, netease::to_wide("浏览歌单").c_str());
		SetDlgItemText(IDC_NETEASE_LOGOUT_BTN, netease::to_wide("登出").c_str());
		SetDlgItemText(IDC_NETEASE_CLEAR, netease::to_wide("清除全部数据").c_str());
		SetDlgItemText(IDC_NETEASE_PANEL_BTN, netease::to_wide("打开面板").c_str());
		SetDlgItemText(IDC_NETEASE_COVER_NOW, netease::to_wide("封面跟随正在播放").c_str());
		::CheckDlgButton(m_hWnd, IDC_NETEASE_COVER_NOW,
			netease::Session::instance().cover_follow_now_playing() ? BST_CHECKED : BST_UNCHECKED);
		::SetDlgItemTextW(m_hWnd, IDC_NETEASE_FOLLOW_CURSOR,
			netease::to_wide("列表光标跟随正在播放").c_str());
		::CheckDlgButton(m_hWnd, IDC_NETEASE_FOLLOW_CURSOR,
			netease_data::follow_cursor_enabled() ? BST_CHECKED : BST_UNCHECKED);
		SetDlgItemText(IDC_NETEASE_STATUS_LBL, netease::to_wide("状态：").c_str());
		SetDlgItemText(IDC_NETEASE_ACCOUNT_LBL, netease::to_wide("账号：").c_str());
		SetDlgItemText(IDC_NETEASE_QUALITY_LBL, netease::to_wide("音质：").c_str());

		// 音质下拉框：直接用 Win32 消息，省去 WTL 控件类的转换麻烦。
		const HWND quality = ::GetDlgItem(m_hWnd, IDC_NETEASE_QUALITY);
		const std::string current = netease::Session::instance().quality();
		int selected = 0;
		for (size_t i = 0; i < netease::kQualityOptionCount; ++i) {
			const std::wstring label = netease::to_wide(netease::kQualityOptions[i].label);
			::SendMessageW(quality, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
			if (current == netease::kQualityOptions[i].level) selected = static_cast<int>(i);
		}
		::SendMessageW(quality, CB_SETCURSEL, selected, 0);

		refresh();
		return FALSE; // 焦点由我们自己控制
	}

	void OnDestroy() {
		netease::Session::instance().clear_notify();
	}

	LRESULT OnLoginState(UINT, WPARAM, LPARAM, BOOL &) {
		refresh();
		return 0;
	}

	void OnLogin(UINT, int, CWindow) {
		netease_ui::show_login_dialog(m_hWnd);
		refresh();
	}


	void OnBrowse(UINT, int, CWindow) {
		netease_ui::show_browse_window();
	}


	void OnLogout(UINT, int, CWindow) {
		netease::Session::instance().logout();
		refresh();
	}

	void OnClear(UINT, int, CWindow) {
		const std::wstring question = netease::to_wide("确定要清除本地保存的登录凭据吗？");
		if (::MessageBoxW(m_hWnd, question.c_str(), L"foo_netease", MB_YESNO | MB_ICONQUESTION) == IDYES) {
			netease::Session::instance().logout();
			refresh();
		}
	}



	void OnFollowCursor(UINT, int, CWindow) {
		const bool on = ::IsDlgButtonChecked(m_hWnd, IDC_NETEASE_FOLLOW_CURSOR) == BST_CHECKED;
		netease_data::set_follow_cursor_enabled(on);
		log_line(std::string("foo_netease: 光标跟随正在播放 = ") + (on ? "开" : "关"));
	}

	void OnCoverNow(UINT, int, CWindow) {
		const bool on = ::IsDlgButtonChecked(m_hWnd, IDC_NETEASE_COVER_NOW) == BST_CHECKED;
		netease::Session::instance().set_cover_follow_now_playing(on);
		log_line(std::string("foo_netease: 封面跟随正在播放 = ") + (on ? "开" : "关"));
	}

	void OnPanel(UINT, int, CWindow) {
		netease_ui::open_panel_window();
	}

	void OnQualityChanged(UINT, int, CWindow) {
		const HWND quality = ::GetDlgItem(m_hWnd, IDC_NETEASE_QUALITY);
		const int index = static_cast<int>(::SendMessageW(quality, CB_GETCURSEL, 0, 0));
		if (index >= 0 && static_cast<size_t>(index) < netease::kQualityOptionCount) {
			netease::Session::instance().set_quality(netease::kQualityOptions[index].level);
			log_line(std::string("foo_netease: 音质设置为 ") + netease::kQualityOptions[index].level);
		}
	}

	void refresh() {
		auto & session = netease::Session::instance();
		std::string status = netease::login_state_text(session.state());
		const std::string detail = session.status_detail();
		if (!detail.empty()) status += " —— " + detail;
		SetDlgItemText(IDC_NETEASE_STATUS, netease::to_wide(status).c_str());

		const netease::AccountInfo account = session.account();
		std::string account_text = "（未知）";
		if (account.valid) {
			account_text = account.nickname;
			if (account.uid != 0) account_text += "  (uid " + std::to_string(account.uid) + ")";
		}
		SetDlgItemText(IDC_NETEASE_ACCOUNT, netease::to_wide(account_text).c_str());

		// 登录中/已登录时不允许再次发起登录
		const netease::LoginState state = session.state();
		const bool busy = state == netease::LoginState::RequestingQr ||
			state == netease::LoginState::WaitingScan ||
			state == netease::LoginState::ScannedWaitingConfirm;
		// 已登录时也允许重新登录 / 换账号（例如换一个 VIP 账号，或 Cookie 过期后重新导入），
		// 因此这里只在「正在登录中」时禁用。
		const bool can_login = !busy;
		GetDlgItem(IDC_NETEASE_LOGIN_BTN).EnableWindow(can_login);
		GetDlgItem(IDC_NETEASE_LOGOUT_BTN).EnableWindow(state == netease::LoginState::LoggedIn);
	}

	const preferences_page_callback::ptr m_callback;
	fb2k::CDarkModeHooks m_dark;
};

class netease_prefs_page : public preferences_page_impl<netease_prefs_dialog> {
public:
	const char * get_name() override { return "网易云音乐 (Netease Cloud Music)"; }
	GUID get_guid() override { return guid_prefs_main; }
	GUID get_parent_guid() override { return preferences_page::guid_tools; }
};

static preferences_page_factory_t<netease_prefs_page> g_netease_prefs_page_factory;

} // namespace

#endif // _WIN32

