#include "stdafx.h"
#include "panel_ui.h"

#include <helpers/atl-misc.h>      // ui_element_impl<>
#include <helpers/BumpableElem.h>  // ui_element_impl_withpopup<>
#include <helpers/DarkMode.h>      // 跟随 foobar2000 的明/暗主题
#include <libPPUI/win32_op.h>

#include <commctrl.h>

#include <memory>
#include <string>
#include <vector>

#include "component_log.h"
#include "core/meta_cache.h"
#include "lyric_ui.h"
#include "netease_data.h"
#include "session.h"
#include "ui_scale.h"
#include "win_utf8.h"

#pragma comment(lib, "comctl32.lib")

// 可停靠面板：只列"来源"（每日推荐 / 漫游 / 私人雷达 / 我的歌单 / 搜索），
// **不显示曲目列表** —— 点一下来源，它的曲目就直接发到播放列表。
//
// 这样做的好处是面板可以很窄，也可以一直挂在布局里当"入口"。
// 曲目本身在 foobar2000 自己的播放列表里看/操作。

namespace {

const GUID guid_panel = { 0x9c4f1a72, 0x3d6b, 0x4f21, { 0x8e, 0x2a, 0x77, 0x1c, 0x5b, 0x90, 0x44, 0xd3 } };

enum {
	kIdSource = 2001,
	kIdSearch,
	kIdSearchBtn,
	kIdRefresh,
	kIdMore,
	kIdLyric,
	kIdAutoFm,
	kIdStatus,
};

const UINT_PTR kTimerLogin = 1;   // 等待会话就绪的定时器

std::wstring w(const std::string & utf8) { return netease::to_wide(utf8); }

struct Entry {
	enum Kind { Daily, Fm, FmCn, Recent, Radar, Playlist, Search };
	Kind kind = Playlist;
	int64_t id = 0;
	std::string name;
};

class PanelWindow : public ui_element_instance, public CWindowImpl<PanelWindow> {
public:
	DECLARE_WND_CLASS_EX(TEXT("{9C4F1A72-3D6B-4F21-8E2A-771C5B9044D3}"), CS_VREDRAW | CS_HREDRAW, -1);

	PanelWindow(ui_element_config::ptr config, ui_element_instance_callback_ptr callback)
		: m_config(config), m_callback(callback) {}

	void initialize_window(HWND parent) { WIN32_OP(Create(parent) != NULL); }

	void set_configuration(ui_element_config::ptr config) { m_config = config; }
	ui_element_config::ptr get_configuration() { return m_config; }

	static GUID g_get_guid() { return guid_panel; }
	static GUID g_get_subclass() { return ui_element_subclass_utility; }
	static void g_get_name(pfc::string_base & out) { out = "网易云音乐"; }
	static ui_element_config::ptr g_get_default_configuration() {
		return ui_element_config::g_create_empty(g_get_guid());
	}
	static const char * g_get_description() {
		return "网易云音乐：点歌单/每日推荐/漫游/私人雷达，直接把曲目发到播放列表。";
	}

	void notify(const GUID & what, t_size, const void *, t_size) {
		if (what == ui_element_notify_colors_changed || what == ui_element_notify_font_changed) {
			Invalidate();
		}
	}

	BEGIN_MSG_MAP_EX(PanelWindow)
		MSG_WM_CREATE(OnCreate)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_SIZE(OnSize)
		MESSAGE_HANDLER(netease_ui::kMsgScaleChanged, OnScaleChanged)
		MSG_WM_ERASEBKGND(OnEraseBkgnd)
		MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)
		MSG_WM_TIMER(OnTimer)
		MSG_WM_CONTEXTMENU(OnContextMenu)
		COMMAND_ID_HANDLER_EX(kIdRefresh, OnRefresh)
		COMMAND_ID_HANDLER_EX(kIdSearchBtn, OnSearch)
		COMMAND_ID_HANDLER_EX(kIdMore, OnMore)
		COMMAND_ID_HANDLER_EX(kIdLyric, OnLyric)
		COMMAND_ID_HANDLER_EX(kIdAutoFm, OnAutoFm)
		COMMAND_HANDLER_EX(kIdSource, LBN_SELCHANGE, OnSourceChanged)
		COMMAND_HANDLER_EX(kIdSource, LBN_DBLCLK, OnSourceActivated)
	END_MSG_MAP()

protected:
	const ui_element_instance_callback_ptr m_callback;

private:
	HWND make(const wchar_t * cls, const wchar_t * text, DWORD style, DWORD ex, int id) {
		HWND h = ::CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
			0, 0, 10, 10, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
		// 字体按窗口 DPI 建，DEFAULT_GUI_FONT 在高 DPI 下不会放大。
		if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get(h)), TRUE);
		return h;
	}

	BOOL OnEraseBkgnd(CDCHandle dc) {
		CRect rc;
		if (!GetClientRect(&rc)) return TRUE;
		const COLORREF bg = m_callback.is_valid()
			? m_callback->query_std_color(ui_color_background) : ::GetSysColor(COLOR_BTNFACE);
		CBrush brush;
		if (brush.CreateSolidBrush(bg)) dc.FillRect(&rc, brush);
		return TRUE;
	}

	int OnCreate(LPCREATESTRUCT) {
		m_alive = std::make_shared<netease_data::Liveness>();

		m_search = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0, kIdSearch);
		m_searchBtn = make(L"BUTTON", w("搜索").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdSearchBtn);
		m_refresh = make(L"BUTTON", w("刷新").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdRefresh);
		m_more = make(L"BUTTON", w("更多").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdMore);
		m_lyric = make(L"BUTTON", w("歌词").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdLyric);
		m_autoFm = make(L"BUTTON", w("漫游自动更新播放列表").c_str(),
			BS_AUTOCHECKBOX | WS_TABSTOP, 0, kIdAutoFm);
		if (m_autoFm) ::SendMessageW(m_autoFm, BM_SETCHECK, BST_CHECKED, 0);
		netease_data::set_fm_radio_enabled(true);
		m_source = make(L"LISTBOX", L"",
			WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_TABSTOP, 0, kIdSource);
		m_status = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, kIdStatus);

		// 暗色钩子可能替换控件（libPPUI 会把 SysListView32 换成 CListControl），
		// 所以挂钩之后必须重新取一遍句柄。
		m_dark.AddDialogWithControls(m_hWnd);
		resolve_controls();

		fill_sources();

		CRect cr;
		if (GetClientRect(&cr)) layout(cr.Width(), cr.Height());

		// 停靠面板会在 foobar2000 启动时就被创建，可能早于组件的 on_init()，
		// 那时凭据还没从配置里读出来 —— 一上来就显示"未登录"，得手动刷新一次才行。
		// 这里挂个定时器等会话就绪，自己把歌单加载出来。
		if (netease::Session::instance().logged_in()) {
			set_status("正在加载歌单…（双击即可发到播放列表）");
			load_playlists();
		} else {
			set_status("正在等待登录状态…");
			m_login_retry = 0;
			SetTimer(kTimerLogin, 500);
		}
		return 0;
	}

	void OnTimer(UINT_PTR id) {
		if (id != kTimerLogin) return;
		if (netease::Session::instance().logged_in()) {
			KillTimer(kTimerLogin);
			set_status("正在加载歌单…（双击即可发到播放列表）");
			load_playlists();
			return;
		}
		if (++m_login_retry > 40) {   // 约 20 秒还没就绪就停手
			KillTimer(kTimerLogin);
			set_status("尚未登录。请到 参数设置 → 工具 → 网易云音乐 里扫码登录。");
		}
	}

	void OnDestroy() {
		KillTimer(kTimerLogin);
		if (m_alive) m_alive->alive = false;
	}

	void OnSize(UINT, CSize size) {
		// 界面缩放改过之后字体要重建，所以重排前先重新取句柄 + 设字体。
		resolve_controls();
		layout(size.cx, size.cy);
	}

	// 设置页改了「界面缩放」：字体和几何都要按新比例重来。
	LRESULT OnScaleChanged(UINT, WPARAM, LPARAM, BOOL &) {
		CRect cr;
		if (GetClientRect(&cr)) {
			resolve_controls();
			layout(cr.Width(), cr.Height());
		}
		return 0;
	}

	// 面板里已经没有曲目列表了，所以不需要为它留宽度 —— 可以拖得很小。
	// （宿主还会问 get_min_max_info()，那个也一起调小。）
	void OnGetMinMaxInfo(LPMINMAXINFO info) {
		info->ptMinTrackSize.x = 180;
		info->ptMinTrackSize.y = 80;
	}

	void resolve_controls() {
		m_search = ::GetDlgItem(m_hWnd, kIdSearch);
		m_searchBtn = ::GetDlgItem(m_hWnd, kIdSearchBtn);
		m_refresh = ::GetDlgItem(m_hWnd, kIdRefresh);
		m_more = ::GetDlgItem(m_hWnd, kIdMore);
		m_lyric = ::GetDlgItem(m_hWnd, kIdLyric);
		m_autoFm = ::GetDlgItem(m_hWnd, kIdAutoFm);
		m_source = ::GetDlgItem(m_hWnd, kIdSource);
		m_status = ::GetDlgItem(m_hWnd, kIdStatus);
		const HWND all[] = { m_search, m_searchBtn, m_refresh, m_more, m_lyric, m_autoFm,
			m_source, m_status };
		for (HWND h : all) {
			if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get(h)), TRUE);
		}
	}

	void layout(int cx, int cy) {
		if (cx <= 0 || cy <= 0) return;
		// 所有尺寸都按窗口 DPI 缩放：4K 屏上不缩的话又小又挤。
		const int m = netease_ui::scale(m_hWnd, 6);
		const int rowH = netease_ui::scale(m_hWnd, 23);
		const int btnW = netease_ui::scale(m_hWnd, 52);
		const int cbW = netease_ui::scale(m_hWnd, 148);
		const int gap = netease_ui::scale(m_hWnd, 4);

		// 自适应：宽度够就一行，不够就把按钮换到第二行 —— 面板可以拖得很窄。
		int y = m;
		int searchW = cx - 2 * m - btnW - netease_ui::scale(m_hWnd, 8);
		if (searchW < netease_ui::scale(m_hWnd, 70)) searchW = netease_ui::scale(m_hWnd, 70);
		move(m_search, m, y, searchW, rowH);
		move(m_searchBtn, cx - m - btnW, y, btnW, rowH);

		const int restW = btnW * 3 + netease_ui::scale(m_hWnd, 12) + cbW;
		if (m + searchW + netease_ui::scale(m_hWnd, 8) + restW <= cx - m) {
			int bx = m + searchW + netease_ui::scale(m_hWnd, 8);
			move(m_refresh, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_more, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_lyric, bx, y, btnW, rowH); bx += btnW + netease_ui::scale(m_hWnd, 6);
			move(m_autoFm, bx, y, cbW, rowH);
		} else {
			y += rowH + gap;
			int bx = m;
			move(m_refresh, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_more, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_lyric, bx, y, btnW, rowH); bx += btnW + netease_ui::scale(m_hWnd, 6);
			int w2 = cx - m - bx;
			if (w2 > cbW) w2 = cbW;
			if (w2 < netease_ui::scale(m_hWnd, 60)) w2 = netease_ui::scale(m_hWnd, 60);
			move(m_autoFm, bx, y, w2, rowH);
		}

		const int top = y + rowH + netease_ui::scale(m_hWnd, 6);
		const int bottomH = netease_ui::scale(m_hWnd, 18 + 6) + m;
		int listH = cy - top - bottomH;
		if (listH < netease_ui::scale(m_hWnd, 40)) listH = netease_ui::scale(m_hWnd, 40);
		move(m_source, m, top, cx - 2 * m, listH);
		move(m_status, m, top + listH + gap, cx - 2 * m, netease_ui::scale(m_hWnd, 18));


		// 行距：LISTBOX 的行高默认只跟字体等高，高 DPI 下会显得挤，
		// 这里显式给出行高（字号 + 一点呼吸空间）。
		if (m_source) {
			::SendMessageW(m_source, LB_SETITEMHEIGHT, 0, netease_ui::scale(m_hWnd, 22));
		}
	}

	void move(HWND h, int x, int y, int cx, int cy) {
		if (h) ::SetWindowPos(h, nullptr, x, y, cx, cy, SWP_NOZORDER);
	}

	void set_status(const std::string & text) {
		if (m_status) ::SetWindowTextW(m_status, w(text).c_str());
	}

	void add_entry(const Entry & e, const std::string & text) {
		m_entries.push_back(e);
		::SendMessageW(m_source, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w(text).c_str()));
	}

	void fill_sources() {
		m_entries.clear();
		::SendMessageW(m_source, LB_RESETCONTENT, 0, 0);
		add_entry({ Entry::Daily, 0, "每日推荐" }, "每日推荐");
		add_entry({ Entry::Fm, 0, "漫游（私人 FM）" }, "漫游（私人 FM）");
		add_entry({ Entry::FmCn, 0, "华语私人雷达" }, "华语私人雷达");
		add_entry({ Entry::Recent, 0, "最近播放" }, "最近播放");
		add_entry({ Entry::Radar, 0, "私人雷达" }, "私人雷达");
		add_entry({ Entry::Playlist, 0, "" }, "──── 我的歌单 ────");
		for (const netease::PlaylistInfo & p : m_playlists) {
			add_entry({ Entry::Playlist, p.id, p.name },
				p.name + "  (" + std::to_string(p.track_count) + ")");
		}
	}

	void load_playlists() {
		auto alive = m_alive;
		netease_data::load_playlists_async(alive, [this](netease_data::PlaylistsResult r) {
			if (!m_alive || !m_alive->alive) return;
			if (!r.ok) { set_status(r.error); return; }
			m_playlists = std::move(r.items);
			const std::string account = r.nickname.empty()
				? ("uid " + std::to_string(r.uid))
				: (r.nickname + "  (uid " + std::to_string(r.uid) + ")");
			set_status("共 " + std::to_string(m_playlists.size()) + " 个歌单 · " + account +
				"；双击发到播放列表（右键可追加）");
			fill_sources();
		});
	}

	bool fm_radio_on() const {
		return m_autoFm && ::SendMessageW(m_autoFm, BM_GETCHECK, 0, 0) == BST_CHECKED;
	}

	// 收到曲目 = 直接发到播放列表（漫游电台则进它自己的列表）。
	void on_feed(netease_data::FeedResult r) {
		if (!m_alive || !m_alive->alive) return;
		if (!r.ok) {
			netease_log::write("foo_netease [panel] 加载失败：" + r.error);
			set_status(r.error);
			return;
		}
		netease_log::write("foo_netease [panel] 加载成功：" + r.title);
		try {
			// 只有真正的"漫游（私人 FM）"走电台那条路（它为了无限续播有 24 首硬上限）；
			// 「华语私人雷达」是**普通歌单**（35 首），必须走下面的常规写入，
			// 否则会被电台的裁剪逻辑删到只剩 24 首 —— 之前"歌单不全"就是这个原因。
			if (m_current_kind == Entry::Fm && fm_radio_on()) {
				netease_data::sync_fm_playlist(r.tracks);
				set_status(r.title + " —— 已发到「网易云漫游」播放列表");
				return;
			}
			const size_t added = netease_data::insert_tracks(r.tracks, !m_append_mode);
			set_status(r.title + " —— 已" + (m_append_mode ? "添加到" : "替换") +
				"当前播放列表（" + std::to_string(added) + " 首）");
		} catch (const std::exception & ex) {
			netease_log::write(std::string("foo_netease [panel] 发送失败：") + ex.what());
			set_status(std::string("发送失败：") + ex.what());
		}
	}

	void load_entry(const Entry & e) {
		switch (e.kind) {
		case Entry::Daily:
			set_status("正在加载每日推荐…");
			netease_data::load_daily_async(m_alive, [this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			break;
		case Entry::Fm:
			netease_data::set_fm_mode("");
			set_status("正在加载漫游…");
			netease_data::load_fm_async(m_alive, [this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			break;
		case Entry::FmCn:
			set_status("正在加载华语私人雷达…");
			netease_data::load_cn_roam_async(m_alive, [this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			break;
		case Entry::Recent:
			set_status("正在加载最近播放…");
			netease_data::load_recent_async(m_alive, [this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			break;
		case Entry::Radar:
			set_status("正在加载私人雷达…");
			netease_data::load_radar_async(m_alive, [this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			break;
		case Entry::Playlist:
			if (e.id == 0) return;   // 分隔行
			set_status("正在加载曲目…");
			netease_data::load_playlist_tracks_async(m_alive, e.id,
				[this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			break;
		case Entry::Search:
			load_search(0, false);
			break;
		}
	}

	// 单击：只记录选中的来源，**不做任何动作**（用户要求改成双击才发送）。
	void OnSourceChanged(UINT, int, CWindow) {
		const int sel = static_cast<int>(::SendMessageW(m_source, LB_GETCURSEL, 0, 0));
		if (sel < 0 || static_cast<size_t>(sel) >= m_entries.size()) return;
		const Entry e = m_entries[sel];
		m_current_kind = e.kind;
		m_current_id = e.id;
		netease_log::write("foo_netease [panel] 选中来源 #" + std::to_string(sel) +
			"（kind=" + std::to_string(static_cast<int>(e.kind)) + " id=" + std::to_string(e.id) +
			" name=" + e.name + "）—— 双击才会发到播放列表");
	}

	// 双击：把这一项发到播放列表。
	void OnSourceActivated(UINT, int, CWindow) {
		const int sel = static_cast<int>(::SendMessageW(m_source, LB_GETCURSEL, 0, 0));
		if (sel < 0 || static_cast<size_t>(sel) >= m_entries.size()) return;
		const Entry e = m_entries[sel];
		netease_log::write("foo_netease [panel] 双击来源 #" + std::to_string(sel) + " -> " + e.name);
		m_current_kind = e.kind;
		m_current_id = e.id;
		m_append_mode = false;   // 双击一律替换（右键菜单才会设成追加）
		if (!netease::Session::instance().logged_in()) {
			set_status("尚未登录。请到 参数设置 → 工具 → 网易云音乐 里扫码登录。");
			return;
		}
		load_entry(e);
	}

	// 右键：把选中的来源"添加"到当前播放列表（左键是替换）。
	void OnContextMenu(HWND, CPoint pt) {
		if (!m_source) return;
		if (pt.x == -1 && pt.y == -1) {          // 键盘唤出（菜单键）
			RECT rc{};
			::GetWindowRect(m_source, &rc);
			pt.x = rc.left + 20;
			pt.y = rc.top + 20;
		}
		POINT client{ pt.x, pt.y };
		::ScreenToClient(m_source, &client);
		const LRESULT hit = ::SendMessageW(m_source, LB_ITEMFROMPOINT, 0,
			MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y)));
		if (HIWORD(hit) != 0) return;            // 没点在条目上
		const int index = static_cast<int>(LOWORD(hit));
		if (index < 0 || static_cast<size_t>(index) >= m_entries.size()) return;
		::SendMessageW(m_source, LB_SETCURSEL, static_cast<WPARAM>(index), 0);

		HMENU menu = ::CreatePopupMenu();
		::AppendMenuW(menu, MF_STRING, 1, w("添加到当前播放列表").c_str());
		::AppendMenuW(menu, MF_STRING, 2, w("用这个来源替换当前播放列表").c_str());
		::SetForegroundWindow(m_hWnd);
		const int cmd = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_hWnd, nullptr);
		::DestroyMenu(menu);
		if (cmd != 1 && cmd != 2) return;

		const Entry e = m_entries[index];
		m_append_mode = (cmd == 1);
		m_current_kind = e.kind;
		m_current_id = e.id;
		netease_log::write(std::string("foo_netease [panel] 右键：") +
			(cmd == 1 ? "添加" : "替换") + "当前播放列表 -> " + e.name);
		if (!netease::Session::instance().logged_in()) { set_status("尚未登录。"); return; }
		load_entry(e);
	}

	void OnRefresh(UINT, int, CWindow) {
		if (!netease::Session::instance().logged_in()) { set_status("尚未登录。"); return; }
		set_status("正在刷新歌单…");
		load_playlists();
	}

	void OnSearch(UINT, int, CWindow) {
		const int len = ::GetWindowTextLengthW(m_search);
		std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
		const int got = ::GetWindowTextW(m_search, buffer.data(), static_cast<int>(buffer.size()));
		buffer.resize(got > 0 ? static_cast<size_t>(got) : 0);
		const std::string keyword = netease::to_utf8(buffer);
		if (keyword.empty()) { set_status("请输入搜索关键词或歌单链接。"); return; }
		if (!netease::Session::instance().logged_in()) { set_status("搜索需要登录。"); return; }

		if (netease_data::parse_playlist_link(keyword)) {
			set_status("正在加载歌单链接…");
			netease_data::load_playlist_link_async(m_alive, keyword,
				[this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			return;
		}
		// 记下来，「更多」按钮就靠它继续翻页。
		m_search_keyword = keyword;
		m_search_offset = 0;
		load_search(0, false);
	}

	// 搜索：offset 是起始位置（翻页），append=true 时把这一页接到播放列表后面。
	void load_search(int offset, bool append) {
		if (m_search_keyword.empty()) { set_status("请先搜索一次。"); return; }
		if (!netease::Session::instance().logged_in()) { set_status("搜索需要登录。"); return; }
		m_append_mode = append;
		m_current_kind = Entry::Search;
		m_current_id = 0;
		set_status(append ? "正在加载更多搜索结果…" : "正在搜索…");
		const std::string keyword = m_search_keyword;
		netease_data::search_async(m_alive, keyword, offset,
			[this](netease_data::FeedResult r) {
				if (r.ok) m_search_offset += static_cast<int>(r.tracks.size());
				on_feed(std::move(r));
			});
	}

	void OnMore(UINT, int, CWindow) {
		if (!netease::Session::instance().logged_in()) { set_status("尚未登录。"); return; }
		if (m_current_kind == Entry::Fm) {
			// 漫游：再取一批追加进"网易云漫游"。
			set_status("正在取下一批漫游曲目…");
			netease_data::fm_radio_extend_now();
			return;
		}
		if (m_current_kind == Entry::FmCn) {
			// 华语私人雷达是每天更新的歌单，"更多"= 重新拉一次（拿到当天的新歌）。
			set_status("正在重新加载华语私人雷达…");
			netease_data::load_cn_roam_async(m_alive, [this](netease_data::FeedResult r) { on_feed(std::move(r)); });
			return;
		}
		if (m_current_kind == Entry::Search) {
			// 搜索：加载下一页并追加（不覆盖已有的结果）。
			load_search(m_search_offset, true);
			return;
		}
		if (m_current_kind == Entry::Playlist && m_current_id == 0) {
			set_status("请先在上面的列表里选一个来源。");
			return;
		}
		Entry e;
		e.kind = m_current_kind;
		e.id = m_current_id;
		load_entry(e);
	}

	// 歌词：面板不再有曲目列表，所以看"正在播放"的那首。
	void OnLyric(UINT, int, CWindow) {
		metadb_handle_ptr now;
		if (!playback_control::get()->get_now_playing(now) || !now.is_valid()) {
			set_status("当前没有在播放的曲目。");
			return;
		}
		const char * path = now->get_path();
		const char prefix[] = "netease://song/";
		if (!path || std::strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
			set_status("当前播放的不是网易云曲目。");
			return;
		}
		const int64_t id = std::strtoll(path + sizeof(prefix) - 1, nullptr, 10);
		if (id <= 0) { set_status("拿不到当前曲目的 id。"); return; }
		netease::TrackInfo track;
		const std::string title = netease::MetaCache::instance().get(id, track) ? track.title : std::string();
		netease_lyric_ui::show_for(id, title.empty() ? std::to_string(id) : title);
	}

	void OnAutoFm(UINT, int, CWindow) {
		const bool on = fm_radio_on();
		netease_data::set_fm_radio_enabled(on);
		netease_log::write(std::string("foo_netease [panel] 漫游自动更新播放列表 = ") + (on ? "开" : "关"));
	}

	ui_element_config::ptr m_config;
	std::shared_ptr<netease_data::Liveness> m_alive;
	std::vector<Entry> m_entries;
	std::vector<netease::PlaylistInfo> m_playlists;
	Entry::Kind m_current_kind = Entry::Daily;
	int64_t m_current_id = 0;
	bool m_append_mode = false;   // 右键"添加"为 true；左键/默认是替换
	int m_login_retry = 0;
	std::string m_search_keyword;   // 上次搜索词（供「更多」翻页）
	int m_search_offset = 0;        // 已载入多少首搜索结果

	HWND m_search = nullptr;
	HWND m_searchBtn = nullptr;
	HWND m_refresh = nullptr;
	HWND m_more = nullptr;
	HWND m_lyric = nullptr;
	HWND m_autoFm = nullptr;
	HWND m_source = nullptr;
	netease_ui::FontCache m_font;
	HWND m_status = nullptr;

	fb2k::CDarkModeHooks m_dark;
};

class ui_element_panel : public ui_element_impl_withpopup<PanelWindow> {};

service_factory_single_t<ui_element_panel> g_ui_element_panel_factory;

} // namespace

namespace netease_ui {

void open_panel_window() {
	service_ptr_t<ui_element> elem;
	if (!ui_element::g_find(elem, guid_panel) || !elem.is_valid()) return;
	auto methods = ui_element_common_methods_v2::get();
	if (methods.is_valid()) {
		methods->spawn_host_simple(core_api::get_main_window(), elem, false);
	}
}

} // namespace netease_ui

