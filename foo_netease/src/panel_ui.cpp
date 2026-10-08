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
#include "browse_ui.h"
#include "session.h"
#include "theme.h"
#include "ui_scale.h"
#include "win_utf8.h"

#pragma comment(lib, "comctl32.lib")

// 可停靠面板：只列"来源"（每日推荐 / 漫游 / 私人雷达 / 我的歌单 / 搜索），
// **不显示曲目列表** —— 点一下来源，它的曲目就直接发到播放列表。
//
// 这样做的好处是面板可以很窄，也可以一直挂在布局里当"入口"。
// 曲目本身在 foobar2000 自己的播放列表里看/操作。

namespace netease_ui {
} // namespace netease_ui

namespace {

const GUID guid_panel = { 0x9c4f1a72, 0x3d6b, 0x4f21, { 0x8e, 0x2a, 0x77, 0x1c, 0x5b, 0x90, 0x44, 0xd3 } };

enum {
	kIdSource = 2001,
	kIdSearch,
	kIdSearchBtn,
	kIdRefresh,
	kIdMore,
	kIdSearchList,   // 「搜索歌单」
	kIdClearFound,   // 「清除歌单」（搜到歌单后才显示）
	kIdLyric,
	kIdAutoFm,
	kIdStatus,
};

const UINT_PTR kTimerLogin = 1;   // 等待会话就绪的定时器
const UINT_PTR kListSubclassId = 1;     // 来源列表子类化 ID
const UINT_PTR kStatusSubclassId = 2;   // 状态行子类化 ID
const UINT_PTR kSearchBtnSubclassId = 3;   // 「搜索」按钮子类化 ID（右键=发到播放列表）
const UINT_PTR kTimerTip = 3;           // 悬停提示延时定时器
	const UINT_PTR kTimerScroll = 5;        // 自绘滚动条自动隐藏定时器

std::wstring w(const std::string & utf8) { return netease::to_wide(utf8); }

struct Entry {
	enum Kind { Daily, Fm, FmCn, Recent, Radar, Playlist, Search, PlSearch };
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
		MESSAGE_HANDLER(netease_ui::kMsgThemeChanged, OnThemeChanged)
		MSG_WM_CTLCOLORSTATIC(OnCtlColor)
		MSG_WM_CTLCOLOREDIT(OnCtlColor)
		MSG_WM_CTLCOLORLISTBOX(OnCtlColor)
		MSG_WM_CTLCOLORBTN(OnCtlColor)
		MSG_WM_MEASUREITEM(OnMeasureItem)
		MESSAGE_HANDLER(WM_PAINT, OnPaintMsg)
		MSG_WM_LBUTTONDOWN(OnLButtonDownMsg)
		MSG_WM_MOUSEMOVE(OnMouseMoveMsg)
		MSG_WM_MOUSELEAVE(OnMouseLeaveMsg)
		MSG_WM_LBUTTONUP(OnLButtonUpMsg)
		NOTIFY_CODE_HANDLER(TTN_NEEDTEXTW, OnTooltipNeedText)
		MSG_WM_DRAWITEM(OnDrawItem)
		MSG_WM_ERASEBKGND(OnEraseBkgnd)
		MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)
		MSG_WM_TIMER(OnTimer)
		MSG_WM_CONTEXTMENU(OnContextMenu)
		COMMAND_ID_HANDLER_EX(kIdRefresh, OnRefresh)
		COMMAND_ID_HANDLER_EX(kIdSearchBtn, OnSearch)
		COMMAND_ID_HANDLER_EX(kIdMore, OnMore)
		COMMAND_ID_HANDLER_EX(kIdSearchList, OnSearchPlaylists)
		COMMAND_ID_HANDLER_EX(kIdClearFound, OnClearFound)
		COMMAND_ID_HANDLER_EX(kIdLyric, OnLyric)
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
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		if (m_bgBrush) ::FillRect(dc, &rc, m_bgBrush);
		return TRUE;
	}

	// 主题色：背景 / 文字 / 选中 / 高亮，都问宿主（见 theme.h）。
	void refresh_theme() {
		m_theme = netease_ui::theme_colors();
		m_bgColor = CLR_INVALID;
		rebuild_brushes();
	}

	void rebuild_brushes() {
		if (m_bgBrush) { ::DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
		if (m_selBrush) { ::DeleteObject(m_selBrush); m_selBrush = nullptr; }
		m_bgColor = m_theme.background;
		m_bgBrush = ::CreateSolidBrush(m_theme.background);
		m_selBrush = ::CreateSolidBrush(m_theme.selection);
	}

	// 悬停提示：面板里的文字常被省略号截断，鼠标停上去显示完整内容。
	// 用 TTF_SUBCLASS + TTF_IDISHWND 让提示框自己跟踪子控件，文本在
	// TTN_NEEDTEXT 里按"光标当前压在哪个控件/哪一条"动态给。
	void init_tooltip() {
		m_tip = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
			WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
			CW_USEDEFAULT, CW_USEDEFAULT, m_hWnd, nullptr, nullptr, nullptr);
		if (!m_tip) return;
		::SetWindowPos(m_tip, HWND_TOPMOST, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		// 手动跟踪模式：位置/显示时机由我们在子类化里控制（TTF_SUBCLASS 对
		// 自绘 LISTBOX 不生效，实测提示根本不出现）。
		TOOLINFOW ti{};
		ti.cbSize = sizeof(ti);
		ti.uFlags = TTF_ABSOLUTE | TTF_TRACK;
		ti.hwnd = m_hWnd;
		ti.uId = 1;
		ti.lpszText = const_cast<LPWSTR>(L"");
		::SendMessageW(m_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
	}

	// 鼠标停到列表条目/状态行上时，把完整文字用提示框显示出来。
	void on_hover(HWND source, POINT client) {
		if (!m_tip) return;
		std::wstring text;
		int key = -1;
		if (source == m_source) {
			const LRESULT hit = ::SendMessageW(m_source, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y)));
			if (HIWORD(hit) == 0) {
				wchar_t buf[512] = {};
				if (::SendMessageW(m_source, LB_GETTEXT, LOWORD(hit),
						reinterpret_cast<LPARAM>(buf)) > 0) {
					text = buf;
					key = static_cast<int>(LOWORD(hit));
				}
			}
		} else if (source == m_status) {
			wchar_t buf[1024] = {};
			::GetWindowTextW(m_status, buf, 1024);
			text = buf;
			key = -2;
		}
		if (text.empty()) {
			hide_tip();
			return;
		}
		if (key == m_tipPending) return;   // 还停在同一处：不要重置计时
		m_tipPending = key;
		m_tipText = text;
		show_tip(false);                    // 换内容先收起来
		// 悬停 0.6 秒之后才显示 —— 鼠标扫过去不该立刻弹提示。
		KillTimer(kTimerTip);
		SetTimer(kTimerTip, 600, nullptr);
	}

	void hide_tip() {
		KillTimer(kTimerTip);
		m_tipPending = -1;
		show_tip(false);
	}

	void show_tip(bool on) {
		if (!m_tip) return;
		if (!on) {
			::SendMessageW(m_tip, TTM_TRACKACTIVATE, FALSE, 0);
			return;
		}
		TOOLINFOW ti{};
		ti.cbSize = sizeof(ti);
		ti.hwnd = m_hWnd;
		ti.uId = 1;
		ti.lpszText = m_tipText.data();
		::SendMessageW(m_tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
		POINT pt{};
		::GetCursorPos(&pt);
		pt.y += netease_ui::scale(m_hWnd, 20);   // 放光标下方一点，别挡住
		::SendMessageW(m_tip, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x, pt.y));
		::SendMessageW(m_tip, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
	}

	LRESULT OnTooltipNeedText(int, LPNMHDR hdr, BOOL &) {
		auto * info = reinterpret_cast<LPNMTTDISPINFOW>(hdr);
		m_tipText.clear();
		POINT pt{};
		::GetCursorPos(&pt);
		const HWND under = ::WindowFromPoint(pt);
		if (under == m_source) {
			// 列表里光标停在哪一条就把哪一条的完整文字显示出来。
			POINT client = pt;
			::ScreenToClient(m_source, &client);
			const LRESULT hit = ::SendMessageW(m_source, LB_ITEMFROMPOINT, 0,
				MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y)));
			if (HIWORD(hit) == 0) {
				const int index = static_cast<int>(LOWORD(hit));
				wchar_t text[512] = {};
				if (::SendMessageW(m_source, LB_GETTEXT, index, reinterpret_cast<LPARAM>(text)) > 0) {
					m_tipText = text;
				}
			}
		} else if (under == m_status) {
			wchar_t text[1024] = {};
			::GetWindowTextW(m_status, text, 1024);
			m_tipText = text;
		}
		if (!m_tipText.empty()) {
			info->lpszText = m_tipText.data();
		}
		return 0;
	}

	// 列表框子类化：自己擦背景，保证条目下方/背后也是主题底色。
	static LRESULT CALLBACK ctrl_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
		UINT_PTR id, DWORD_PTR ref) {
		auto * self = reinterpret_cast<PanelWindow *>(ref);
		if (self && id == kSearchBtnSubclassId) {
			// 「搜索」按钮：右键 = 直接把结果发到当前播放列表（左键是开浏览窗口看结果）。
			if (msg == WM_RBUTTONUP) { self->do_search(true); return 0; }
			if (msg == WM_CONTEXTMENU) return 0;   // 右键已自己处理，别再弹系统菜单
			if (msg == WM_NCDESTROY) {
				::RemoveWindowSubclass(hwnd, &PanelWindow::ctrl_subclass, id);
			}
			return ::DefSubclassProc(hwnd, msg, wp, lp);
		}
		if (self) {
			if (msg == WM_MOUSEMOVE) {
				// 要收 WM_MOUSELEAVE 才能把提示关掉。
				TRACKMOUSEEVENT tme{};
				tme.cbSize = sizeof(tme);
				tme.dwFlags = TME_LEAVE;
				tme.hwndTrack = hwnd;
				::TrackMouseEvent(&tme);
				POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
				if (id == kListSubclassId) self->show_scroll_bar();   // 鼠标进来才显示自绘滚动条
				self->on_hover(hwnd, pt);
			} else if (msg == WM_MOUSEWHEEL && id == kListSubclassId) {
				// 摘掉 WS_VSCROLL 后列表框自己的滚轮也失效了，手动滚。
				const int delta = GET_WHEEL_DELTA_WPARAM(wp);
				const int lines = (delta / WHEEL_DELTA) * 3;
				self->scroll_list_to(self->list_top_index() - lines);
				self->show_scroll_bar();
				return 0;
			} else if ((msg == WM_VSCROLL || msg == WM_KEYDOWN) && id == kListSubclassId) {
				::InvalidateRect(self->m_hWnd, &self->m_scrollRect, FALSE);
				self->show_scroll_bar();
			} else if (msg == WM_NCCALCSIZE && id == kListSubclassId) {
				// 不要原生滚动条（右侧那条由我们自己画，还能自动隐藏）。
				const LONG style = ::GetWindowLongW(hwnd, GWL_STYLE);
				if (style & WS_VSCROLL) {
					::SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_VSCROLL);
				}
			} else if (msg == WM_MOUSELEAVE) {
				self->hide_tip();
				// 用户要求"离开就马上消失"：只有鼠标正好落在右侧那条滚动条上
				// （准备拖动）时才留着，否则立刻隐藏。
				if (id == kListSubclassId) self->hide_scroll_if_outside();
			} else if (msg == WM_NCDESTROY) {
				::RemoveWindowSubclass(hwnd, &PanelWindow::ctrl_subclass, id);
			}
		}
		if (id == kListSubclassId) {
			return list_erase(hwnd, msg, wp, lp, self);
		}
		return ::DefSubclassProc(hwnd, msg, wp, lp);
	}

	static LRESULT list_erase(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, PanelWindow * self) {
		if (self && msg == WM_ERASEBKGND) {
			RECT rc{};
			if (::GetClientRect(hwnd, &rc)) {
				if (!self->m_bgBrush || self->m_bgColor != self->m_theme.background) self->rebuild_brushes();
				if (self->m_bgBrush) ::FillRect(reinterpret_cast<HDC>(wp), &rc, self->m_bgBrush);
			}
			return 1;
		}
		return ::DefSubclassProc(hwnd, msg, wp, lp);
	}

	// 普通控件的文字/底色（自绘的列表另有 OnDrawItem）。
	HBRUSH OnCtlColor(HDC dc, HWND) {
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		::SetTextColor(dc, m_theme.text);
		::SetBkColor(dc, m_theme.background);
		return m_bgBrush ? m_bgBrush : static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH));
	}

	// 自绘列表的行高（自绘模式下由 WM_MEASUREITEM 决定）。
	void OnMeasureItem(UINT, LPMEASUREITEMSTRUCT mis) {
		if (!mis) return;
		mis->itemHeight = static_cast<UINT>(netease_ui::scale(m_hWnd, 22));
	}

	// 自绘列表：选中行用宿主的选中色，其余用背景/文字色。
	BOOL OnDrawItem(UINT, LPDRAWITEMSTRUCT dis) {
		if (!dis || dis->CtlID != kIdSource) return FALSE;
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		if (dis->itemID == static_cast<UINT>(-1)) return TRUE;
		const bool selected = (dis->itemState & ODS_SELECTED) != 0;
		const COLORREF bg = selected ? m_theme.selection : m_theme.background;
		const COLORREF fg = selected ? m_theme.selection_text : m_theme.text;
		HBRUSH brush = selected ? m_selBrush : m_bgBrush;
		if (brush) ::FillRect(dis->hDC, &dis->rcItem, brush);
		wchar_t text[512] = {};
		::SendMessageW(m_source, LB_GETTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
		RECT rc = dis->rcItem;
		rc.left += netease_ui::scale(m_hWnd, 4);
		::SetBkMode(dis->hDC, TRANSPARENT);
		::SetTextColor(dis->hDC, fg);
		::DrawTextW(dis->hDC, text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
		return TRUE;
	}

	// 宿主换了配色：重取颜色并重画。
	LRESULT OnThemeChanged(UINT, WPARAM, LPARAM, BOOL &) {
		refresh_theme();
		apply_dark_hooks();   // 明暗切换后必须重套，否则滚动条还留着上一套
		// 关键：搜索框、状态行、复选框都是**子控件**。实测 RDW_ALLCHILDREN
		// 对带主题的 EDIT/STATIC 不生效（它们要等重启才变），所以逐个显式重画。
		const HWND kids[] = { m_search, m_searchBtn, m_refresh, m_more, m_searchList, m_lyric, m_clearFound, m_source, m_status };
		for (HWND h : kids) {
			if (h) {
				::InvalidateRect(h, nullptr, TRUE);
				::RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
			}
		}
		::InvalidateRect(m_hWnd, nullptr, TRUE);
		::UpdateWindow(m_hWnd);
		return 0;
	}

	int OnCreate(LPCREATESTRUCT) {
		m_alive = std::make_shared<netease_data::Liveness>();
		refresh_theme();

		m_search = make(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0, kIdSearch);
		m_searchBtn = make(L"BUTTON", w("搜索").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdSearchBtn);
		m_refresh = make(L"BUTTON", w("刷新").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdRefresh);
		m_more = make(L"BUTTON", w("更多").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdMore);
		m_searchList = make(L"BUTTON", w("搜索歌单").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdSearchList);
		m_clearFound = make(L"BUTTON", w("清除歌单").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdClearFound);
		::ShowWindow(m_clearFound, SW_HIDE);   // 搜到歌单后才显示
		m_lyric = make(L"BUTTON", w("歌词").c_str(), BS_PUSHBUTTON | WS_TABSTOP, 0, kIdLyric);
		// 「漫游自动更新播放列表」不再做成开关：这个行为本来就是对的，默认常开。
		netease_data::set_fm_radio_enabled(true);
		// 自绘：选中行要用宿主主题的「选中色」，标准 LISTBOX 用不了。
		m_source = make(L"LISTBOX", L"",
			WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_TABSTOP |
			LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 0, kIdSource);
		// SS_NOTIFY：STATIC 默认不接收鼠标消息，加了它悬停提示才会触发。
		m_status = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOTIFY, 0, kIdStatus);

		// 逐个控件显式挂暗色处理：只调 AddDialogWithControls 时，自绘列表框的
		// 滚动条和按钮不一定会被换皮（面板是自定义窗口，不是对话框）。
		apply_dark_hooks();

		// 暗色钩子可能替换控件（libPPUI 会把 SysListView32 换成 CListControl），
		// 所以挂钩之后必须重新取一遍句柄。
		resolve_controls();
		// 列表框的空区（条目下方）不会走 WM_DRAWITEM，需要自己擦背景，
		// 否则深色主题下会露出一条/一片系统亮色。
		if (m_source) ::SetWindowSubclass(m_source, &PanelWindow::ctrl_subclass,
			kListSubclassId, reinterpret_cast<DWORD_PTR>(this));
		if (m_status) ::SetWindowSubclass(m_status, &PanelWindow::ctrl_subclass,
			kStatusSubclassId, reinterpret_cast<DWORD_PTR>(this));
		if (m_searchBtn) ::SetWindowSubclass(m_searchBtn, &PanelWindow::ctrl_subclass,
			kSearchBtnSubclassId, reinterpret_cast<DWORD_PTR>(this));

		fill_sources();

		CRect cr;
		if (GetClientRect(&cr)) layout(cr.Width(), cr.Height());

		init_tooltip();

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
		if (id == kTimerScroll) {       // 鼠标停下 1.2 秒：把自绘滚动条收起来
			KillTimer(kTimerScroll);
			hide_scroll_bar_now();
			return;
		}
		if (id == kTimerTip) {          // 悬停够久，显示提示
			KillTimer(kTimerTip);
			if (!m_tipText.empty()) show_tip(true);
			return;
		}
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
		if (m_tip) { ::DestroyWindow(m_tip); m_tip = nullptr; }
		if (m_source) ::RemoveWindowSubclass(m_source, &PanelWindow::ctrl_subclass, kListSubclassId);
		if (m_status) ::RemoveWindowSubclass(m_status, &PanelWindow::ctrl_subclass, kStatusSubclassId);
		if (m_searchBtn) ::RemoveWindowSubclass(m_searchBtn, &PanelWindow::ctrl_subclass, kSearchBtnSubclassId);
		KillTimer(kTimerLogin);
		if (m_alive) m_alive->alive = false;
		if (m_bgBrush) { ::DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
		if (m_selBrush) { ::DeleteObject(m_selBrush); m_selBrush = nullptr; }
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
		m_searchList = ::GetDlgItem(m_hWnd, kIdSearchList);
		m_clearFound = ::GetDlgItem(m_hWnd, kIdClearFound);
		m_lyric = ::GetDlgItem(m_hWnd, kIdLyric);
		m_source = ::GetDlgItem(m_hWnd, kIdSource);
		m_status = ::GetDlgItem(m_hWnd, kIdStatus);
		const HWND all[] = { m_search, m_searchBtn, m_refresh, m_more, m_searchList, m_lyric, m_clearFound, m_source, m_status };
		for (HWND h : all) {
			if (h) ::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(m_font.get(h)), TRUE);
		}
	}

	// ===== 自绘滚动条（平时隐藏，鼠标移过来才显示）=====
	int list_count() const {
		return m_source ? static_cast<int>(::SendMessageW(m_source, LB_GETCOUNT, 0, 0)) : 0;
	}
	int list_row_h() const {
		if (!m_source) return 0;
		const LRESULT h = ::SendMessageW(m_source, LB_GETITEMHEIGHT, 0, 0);
		return h > 0 ? static_cast<int>(h) : netease_ui::scale(m_hWnd, 22);
	}
	int list_visible_rows() const {
		if (!m_source) return 0;
		CRect rc;
		if (!::GetClientRect(m_source, &rc)) return 0;
		const int row = list_row_h();
		return row > 0 ? (rc.Height() / row) : 0;
	}
	int list_top_index() const {
		return m_source ? static_cast<int>(::SendMessageW(m_source, LB_GETTOPINDEX, 0, 0)) : 0;
	}
	bool scroll_needed() const {
		const int per_page = list_visible_rows();
		return per_page > 0 && list_count() > per_page;
	}
	void scroll_metrics(int & thumb_top, int & thumb_h) const {
		thumb_top = 0;
		thumb_h = 0;
		const int total = list_count();
		const int per_page = list_visible_rows();
		const int height = m_scrollRect.bottom - m_scrollRect.top;
		if (total <= per_page || per_page <= 0 || height <= 0) return;
		thumb_h = height * per_page / total;
		const int min_h = netease_ui::scale(m_hWnd, 28);
		if (thumb_h < min_h) thumb_h = min_h;
		if (thumb_h > height) thumb_h = height;
		const int max_top = total - per_page;
		const int travel = height - thumb_h;
		thumb_top = (max_top > 0 && travel > 0) ? (travel * list_top_index() / max_top) : 0;
	}
	void draw_scroll_bar(HDC dc) {
		if (!m_scrollVisible || !scroll_needed()) return;
		if (m_scrollRect.right <= m_scrollRect.left) return;
		auto up = [](int v) { return static_cast<BYTE>(v > 255 ? 255 : (v < 0 ? 0 : v)); };
		const bool dark = DarkMode::IsThemeDark(m_theme.text, m_theme.background);
		const int r = GetRValue(m_theme.background);
		const int g = GetGValue(m_theme.background);
		const int b = GetBValue(m_theme.background);
		HBRUSH track = ::CreateSolidBrush(dark ? RGB(up(r + 6), up(g + 6), up(b + 6))
			: RGB(up(r - 14), up(g - 14), up(b - 14)));
		::FillRect(dc, &m_scrollRect, track);
		::DeleteObject(track);
		int top = 0, height = 0;
		scroll_metrics(top, height);
		if (height <= 0) return;
		RECT tr = m_scrollRect;
		tr.top += top;
		tr.bottom = tr.top + height;
		HBRUSH thumb = ::CreateSolidBrush(dark ? RGB(up(r + 58), up(g + 58), up(b + 58))
			: RGB(up(r - 70), up(g - 70), up(b - 70)));
		::FillRect(dc, &tr, thumb);
		::DeleteObject(thumb);
	}
	void show_scroll_bar() {
		if (!m_scrollVisible) {
			m_scrollVisible = true;
			// 必须带 erase —— 只重画不擦的话，旧的滚动条像素会留在那儿。
			::InvalidateRect(m_hWnd, &m_scrollRect, TRUE);
		}
		// 鼠标停着不动 1.2 秒后收起来
		KillTimer(kTimerScroll);
		SetTimer(kTimerScroll, 800, nullptr);
	}
	void scroll_list_to(int top) {
		if (!m_source) return;
		const int total = list_count();
		const int per_page = list_visible_rows();
		int max_top = total - per_page;
		if (max_top < 0) max_top = 0;
		if (top < 0) top = 0;
		if (top > max_top) top = max_top;
		::SendMessageW(m_source, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
		::InvalidateRect(m_hWnd, &m_scrollRect, FALSE);
	}
	bool in_scroll_rect(POINT pt) const {
		return pt.x >= m_scrollRect.left && pt.x < m_scrollRect.right &&
			pt.y >= m_scrollRect.top && pt.y < m_scrollRect.bottom;
	}
	LRESULT OnPaintMsg(UINT, WPARAM, LPARAM, BOOL & handled) {
		PAINTSTRUCT ps{};
		HDC dc = ::BeginPaint(m_hWnd, &ps);
		draw_scroll_bar(dc);
		::EndPaint(m_hWnd, &ps);
		handled = TRUE;
		return 0;
	}
	void OnLButtonDownMsg(UINT, CPoint pt) {
		if (!m_scrollVisible || !scroll_needed() || !in_scroll_rect(pt)) { SetMsgHandled(FALSE); return; }
		int top = 0, height = 0;
		scroll_metrics(top, height);
		if (height > 0 && pt.y >= m_scrollRect.top + top && pt.y < m_scrollRect.top + top + height) {
			m_dragThumb = true;
			m_dragOffset = pt.y - (m_scrollRect.top + top);
			::SetCapture(m_hWnd);
			return;
		}
		const int per_page = list_visible_rows();
		const int dir = (pt.y < m_scrollRect.top + top) ? -1 : 1;
		scroll_list_to(list_top_index() + dir * per_page);
	}
	void OnMouseLeaveMsg() {
		hide_scroll_bar_now();
	}

	void OnMouseMoveMsg(UINT, CPoint pt) {
		// 要收 WM_MOUSELEAVE，鼠标从滚动条直接移出面板时才能立刻隐藏。
		TRACKMOUSEEVENT tme{};
		tme.cbSize = sizeof(tme);
		tme.dwFlags = TME_LEAVE;
		tme.hwndTrack = m_hWnd;
		::TrackMouseEvent(&tme);
		if (m_dragThumb) {
			const int total = list_count();
			const int per_page = list_visible_rows();
			const int height = m_scrollRect.bottom - m_scrollRect.top;
			int top = 0, thumb_h = 0;
			scroll_metrics(top, thumb_h);
			const int travel = height - thumb_h;
			const int max_top = total - per_page;
			if (travel > 0 && max_top > 0) {
				int want = pt.y - m_scrollRect.top - m_dragOffset;
				if (want < 0) want = 0;
				if (want > travel) want = travel;
				scroll_list_to(want * max_top / travel);
			}
			show_scroll_bar();
			return;
		}
		if (in_scroll_rect(pt)) {
			show_scroll_bar();
			return;
		}
		SetMsgHandled(FALSE);
	}
	// 鼠标离开列表时调用：不在滚动条上就立刻隐藏。
	void hide_scroll_if_outside() {
		if (m_dragThumb) return;
		POINT pt{};
		::GetCursorPos(&pt);
		::ScreenToClient(m_hWnd, &pt);
		if (in_scroll_rect(pt)) return;
		hide_scroll_bar_now();
	}

	void hide_scroll_bar_now() {
		if (m_dragThumb) return;   // 正在拖动，不能藏
		if (m_scrollVisible) {
			m_scrollVisible = false;
			::InvalidateRect(m_hWnd, &m_scrollRect, TRUE);
		}
	}
	void OnLButtonUpMsg(UINT, CPoint) {
		if (!m_dragThumb) { SetMsgHandled(FALSE); return; }
		m_dragThumb = false;
		::ReleaseCapture();
	}

	// 给控件重新套明暗主题。**切明暗（或改配色）时必须重来一遍** ——
	// 否则控件还留着上一套 uxtheme，表现就是"滚动条颜色和当前模式对不上"。
	void apply_dark_hooks() {
		m_dark.AddDialog(m_hWnd, m_theme.background);
		if (m_searchBtn) m_dark.AddButton(m_searchBtn);
		if (m_refresh) m_dark.AddButton(m_refresh);
		if (m_more) m_dark.AddButton(m_more);
		if (m_lyric) m_dark.AddButton(m_lyric);
		if (m_searchList) m_dark.AddButton(m_searchList);
		if (m_clearFound) m_dark.AddButton(m_clearFound);
		// 列表框**不套 uxtheme**：它现在不用原生滚动条了，主题唯一的副作用
		// 就是鼠标悬停时画一圈蓝色"热边框"。底色/条目都是我们自己画的。
		if (m_source) ::SetWindowTheme(m_source, L"", L"");
		if (m_search) ::SetWindowTheme(m_search, L"", L"");
		if (m_status) ::SetWindowTheme(m_status, L"", L"");
		if (m_source) {
			::SetWindowSubclass(m_source, &PanelWindow::ctrl_subclass,
				kListSubclassId, reinterpret_cast<DWORD_PTR>(this));
		}
		if (m_status) {
			::SetWindowSubclass(m_status, &PanelWindow::ctrl_subclass,
				kStatusSubclassId, reinterpret_cast<DWORD_PTR>(this));
		}
		if (m_searchBtn) {
			::SetWindowSubclass(m_searchBtn, &PanelWindow::ctrl_subclass,
				kSearchBtnSubclassId, reinterpret_cast<DWORD_PTR>(this));
		}
	}

	void layout(int cx, int cy) {
		if (cx <= 0 || cy <= 0) return;
		// 所有尺寸都按窗口 DPI 缩放：4K 屏上不缩的话又小又挤。
		const int m = netease_ui::scale(m_hWnd, 6);
		const int rowH = netease_ui::scale(m_hWnd, 23);
		const int btnW = netease_ui::scale(m_hWnd, 52);
		const int gap = netease_ui::scale(m_hWnd, 4);

		// 自适应：宽度够就一行，不够就把按钮换到第二行 —— 面板可以拖得很窄。
		int y = m;
		int searchW = cx - 2 * m - btnW - netease_ui::scale(m_hWnd, 8);
		if (searchW < netease_ui::scale(m_hWnd, 70)) searchW = netease_ui::scale(m_hWnd, 70);
		move(m_search, m, y, searchW, rowH);
		move(m_searchBtn, cx - m - btnW, y, btnW, rowH);

		// 按钮行：宽度够就和搜索框同一行，不够就整体换到第二行。
		// 「清除歌单」只在搜到歌单后显示，按实际可见数量算宽度。
		const bool showClear = (m_clearFound && ::IsWindowVisible(m_clearFound));
		const int restCount = showClear ? 5 : 4;
		const int restW = btnW * restCount + gap * (restCount - 1);
		if (m + searchW + netease_ui::scale(m_hWnd, 8) + restW <= cx - m) {
			int bx = m + searchW + netease_ui::scale(m_hWnd, 8);
			move(m_refresh, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_more, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_lyric, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_searchList, bx, y, btnW, rowH); bx += btnW + gap;
			if (showClear) move(m_clearFound, bx, y, btnW, rowH);
		} else {
			y += rowH + gap;
			int bx = m;
			move(m_refresh, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_more, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_lyric, bx, y, btnW, rowH); bx += btnW + gap;
			move(m_searchList, bx, y, btnW, rowH); bx += btnW + gap;
			if (showClear) move(m_clearFound, bx, y, btnW, rowH);
		}

		const int top = y + rowH + netease_ui::scale(m_hWnd, 6);
		const int bottomH = netease_ui::scale(m_hWnd, 18 + 6) + m;
		int listH = cy - top - bottomH;
		if (listH < netease_ui::scale(m_hWnd, 40)) listH = netease_ui::scale(m_hWnd, 40);
		// 右侧留出自绘滚动条的位置（平时隐藏，鼠标进来才画）。
		const int sb_w = netease_ui::scale(m_hWnd, 13);
		move(m_source, m, top, cx - 2 * m - sb_w, listH);
		m_scrollRect.left = m + cx - 2 * m - sb_w;
		m_scrollRect.top = top;
		m_scrollRect.right = m + cx - 2 * m;
		m_scrollRect.bottom = top + listH;
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
		if (!m_status) return;
		::SetWindowTextW(m_status, w(text).c_str());
		// STATIC 改文本后不一定重画背景，显式刷一下。
		::InvalidateRect(m_status, nullptr, TRUE);
	}

	void add_entry(const Entry & e, const std::string & text) {
		m_entries.push_back(e);
		::SendMessageW(m_source, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(w(text).c_str()));
	}

	void fill_sources() {
		m_entries.clear();
		::SendMessageW(m_source, LB_RESETCONTENT, 0, 0);
		// 搜索/链接来的歌单放最上面一栏；没搜到就不显示这一栏。
		if (!m_foundPlaylists.empty()) {
			add_entry({ Entry::PlSearch, 0, "" }, "──── 搜索到的歌单 ────");
			for (const netease::PlaylistInfo & p : m_foundPlaylists) {
				add_entry({ Entry::Playlist, p.id, p.name },
					p.name + "  (" + std::to_string(p.track_count) + ")");
			}
		}
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
		update_clear_button();
	}

	// 「清除歌单」按钮：搜到歌单后显示，清掉后隐藏（按钮行要重排）。
	void update_clear_button() {
		if (!m_clearFound) return;
		const bool want = !m_foundPlaylists.empty();
		if ((::IsWindowVisible(m_clearFound) != FALSE) == want) return;
		::ShowWindow(m_clearFound, want ? SW_SHOW : SW_HIDE);
		CRect cr;
		if (GetClientRect(&cr)) layout(cr.Width(), cr.Height());
	}

	// 一键清掉来源列表最上面那一栏「搜索到的歌单」（搜索/链接来的都在这里）。
	void clear_found_playlists() {
		m_foundPlaylists.clear();
		m_pl_search_keyword.clear();
		m_pl_search_offset = 0;
		if (m_current_kind == Entry::PlSearch) { m_current_kind = Entry::Daily; m_current_id = 0; }
		fill_sources();
		set_status("已清除「搜索到的歌单」结果。");
	}

	void OnClearFound(UINT, int, CWindow) { clear_found_playlists(); }

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

	bool fm_radio_on() const { return true; }   // 常开

	// 收到曲目 = 直接发到播放列表（漫游电台则进它自己的列表）。
	// 打开歌单链接：**不**动当前播放列表，只在来源列表最上面那一栏里加一条。
	void on_link_preview(netease_data::FeedResult r, int64_t playlist_id) {
		if (!m_alive || !m_alive->alive) return;
		if (!r.ok) { set_status(r.error); return; }
		netease::PlaylistInfo info;
		info.id = playlist_id;
		info.name = r.title;
		info.track_count = static_cast<int64_t>(r.tracks.size());
		m_foundPlaylists.clear();
		m_foundPlaylists.push_back(info);
		fill_sources();
		set_status("歌单《" + r.title + "》已加到列表最上面；双击即可加载");
	}

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
				// 双击漫游（m_append_mode=false）= 新会话，整片替换；
				// 右键「添加到当前播放列表」= 只追加。
				netease_data::sync_fm_playlist(r.tracks, !m_append_mode);
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
		case Entry::PlSearch:
			// 「搜索到的歌单」分隔栏本身不可加载；「更多」会按这个来源翻页。
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

		const Entry entry = m_entries[index];
		const bool is_playlist = (entry.kind == Entry::Playlist && entry.id > 0);
		HMENU menu = ::CreatePopupMenu();
		::AppendMenuW(menu, MF_STRING, 1, w("添加到当前播放列表").c_str());
		::AppendMenuW(menu, MF_STRING, 2, w("用这个来源替换当前播放列表").c_str());
		// 只有歌单才能在新窗口里显示（其它来源没有歌单 id）。
		::AppendMenuW(menu, MF_STRING | (is_playlist ? 0 : MF_GRAYED), 3, w("显示歌单（新窗口）").c_str());
		::SetForegroundWindow(m_hWnd);
		const int cmd = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_hWnd, nullptr);
		::DestroyMenu(menu);
		if (cmd == 3) {
			if (!is_playlist) { set_status("这个来源不是歌单，无法在新窗口显示。"); return; }
			netease_log::write("foo_netease [panel] 右键：在新窗口显示歌单 id=" + std::to_string(entry.id));
			netease_ui::show_browse_window_for(entry.id, m_hWnd);
			return;
		}
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

	// 读搜索框内容。
	std::string search_text() const {
		const int len = ::GetWindowTextLengthW(m_search);
		std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
		const int got = ::GetWindowTextW(m_search, buffer.data(), static_cast<int>(buffer.size()));
		buffer.resize(got > 0 ? static_cast<size_t>(got) : 0);
		return netease::to_utf8(buffer);
	}

	// 「搜索歌单」：只搜歌单，结果放来源列表最上面那一栏（不动播放列表）。
	void OnSearchPlaylists(UINT, int, CWindow) {
		const std::string keyword = search_text();
		if (keyword.empty()) { set_status("请输入要搜索的歌单关键词。"); return; }
		if (!netease::Session::instance().logged_in()) { set_status("搜索需要登录。"); return; }
		m_pl_search_keyword = keyword;
		m_pl_search_offset = 0;
		load_playlists_search(0, false);
	}

	// 歌单搜索翻页：offset 是起始位置（「更多」用），append=true 把这一页接到已有结果后面。
	// 用独立的 Entry::PlSearch 作为当前来源，和歌曲搜索的「更多」互不影响。
	void load_playlists_search(int offset, bool append) {
		if (m_pl_search_keyword.empty()) { set_status("请先搜索一次歌单。"); return; }
		if (!netease::Session::instance().logged_in()) { set_status("搜索需要登录。"); return; }
		m_current_kind = Entry::PlSearch;
		m_current_id = 0;
		set_status(append ? "正在加载更多歌单…" : "正在搜索歌单…");
		netease_data::search_playlists_async(m_alive, m_pl_search_keyword, offset,
			[this, append](netease_data::PlaylistsResult r) {
				if (!m_alive || !m_alive->alive) return;
				if (!r.ok) { set_status(r.error); return; }
				if (append && r.items.empty()) {
					set_status("没有更多歌单了（已显示 " + std::to_string(m_foundPlaylists.size()) + " 个）。");
					return;
				}
				if (append) {
					for (const netease::PlaylistInfo & p : r.items) m_foundPlaylists.push_back(p);
				} else {
					m_foundPlaylists = std::move(r.items);
				}
				m_pl_search_offset = static_cast<int>(m_foundPlaylists.size());
				m_current_kind = Entry::PlSearch;   // fill_sources 会清空列表选中，保持翻页来源
				fill_sources();
				if (m_foundPlaylists.empty()) { set_status("没有搜到歌单：" + m_pl_search_keyword); return; }
				const std::string shown = std::to_string(m_foundPlaylists.size());
				if (r.total > static_cast<int>(m_foundPlaylists.size())) {
					set_status("已显示 " + shown + " / 共 " + std::to_string(r.total) +
						" 个歌单（点「更多」继续）");
				} else {
					set_status("搜到 " + shown + " 个歌单（已是全部）");
				}
			});
	}

	void OnSearch(UINT, int, CWindow) { do_search(false); }

	// 搜索歌曲：默认（左键点「搜索」）在歌单浏览窗口里显示结果，**不动播放列表**；
	// 右键点「搜索」才把结果直接发到当前播放列表（to_playlist=true）。
	void do_search(bool to_playlist) {
		const int len = ::GetWindowTextLengthW(m_search);
		std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
		const int got = ::GetWindowTextW(m_search, buffer.data(), static_cast<int>(buffer.size()));
		buffer.resize(got > 0 ? static_cast<size_t>(got) : 0);
		const std::string keyword = netease::to_utf8(buffer);
		if (keyword.empty()) { set_status("请输入搜索关键词或歌单链接。"); return; }
		if (!netease::Session::instance().logged_in()) { set_status("搜索需要登录。"); return; }

		if (netease_data::parse_playlist_link(keyword)) {
			const int64_t link_id = netease_data::parse_playlist_link(keyword);
			set_status("正在加载歌单链接…");
			// 只把歌单加到来源列表最上面那一栏，不直接替换当前播放列表。
			netease_data::load_playlist_link_async(m_alive, keyword,
				[this, link_id](netease_data::FeedResult r) { on_link_preview(std::move(r), link_id); });
			return;
		}
		if (!to_playlist) {
			// 默认：开「只显示曲目」的搜索面板显示结果，当前播放列表保持原样。
			// 关键词记下来，「更多」按钮跟着翻这个面板的页（m_search_pages_window）。
			m_search_keyword = keyword;
			m_search_offset = 0;
			m_search_pages_window = true;
			m_current_kind = Entry::Search;
			m_current_id = 0;
			set_status("已在搜索面板显示结果（右键「搜索」才发到播放列表）");
			netease_log::write("foo_netease [panel] 左键搜索：搜索面板显示「" + keyword + "」");
			netease_ui::show_browse_window_search(keyword, m_hWnd);
			return;
		}
		// 右键：记下关键词（「更多」按钮靠它继续翻页），并直接发到当前播放列表。
		m_search_pages_window = false;
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
		if (m_current_kind == Entry::PlSearch) {
			// 歌单搜索：加载下一页，接到「搜索到的歌单」那一栏后面（不碰歌曲搜索）。
			load_playlists_search(m_pl_search_offset, true);
			return;
		}
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
			if (m_search_pages_window) {
				// 左键搜索的结果在「只显示曲目」面板里，「更多」让那个面板往下翻。
				if (!netease_ui::show_browse_window_search_more()) {
					set_status("搜索面板已关闭，请重新搜索。");
				}
				return;
			}
			// 右键搜索：加载下一页并追加到当前播放列表（不覆盖已有的结果）。
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

	// 漫游始终自动更新播放列表（原来那个复选框已去掉）。

	ui_element_config::ptr m_config;
	std::shared_ptr<netease_data::Liveness> m_alive;
	std::vector<Entry> m_entries;
	std::vector<netease::PlaylistInfo> m_playlists;
	Entry::Kind m_current_kind = Entry::Daily;
	int64_t m_current_id = 0;
	bool m_append_mode = false;   // 右键"添加"为 true；左键/默认是替换
	int m_login_retry = 0;
	std::string m_search_keyword;   // 上次搜索词（供歌曲「更多」翻页）
	int m_search_offset = 0;        // 已载入多少首搜索结果
	bool m_search_pages_window = false;  // 「更多」翻的是「搜索结果」面板还是播放列表
	std::string m_pl_search_keyword;   // 上次歌单搜索词（供歌单「更多」翻页）
	int m_pl_search_offset = 0;        // 已载入多少个歌单搜索结果

	HWND m_search = nullptr;
	HWND m_searchBtn = nullptr;
	HWND m_refresh = nullptr;
	HWND m_more = nullptr;
	HWND m_searchList = nullptr;
	HWND m_clearFound = nullptr;
	HWND m_lyric = nullptr;
	HWND m_source = nullptr;
	// 搜到的歌单：只列在来源列表**最上面**那一栏，不写进播放列表。
	std::vector<netease::PlaylistInfo> m_foundPlaylists;
	netease_ui::FontCache m_font;
	netease_ui::ThemeColors m_theme;
	HWND m_tip = nullptr;
	// 自绘滚动条：平时隐藏，鼠标移到列表/滚动条上才出现。
	RECT m_scrollRect{};
	bool m_scrollVisible = false;
	bool m_dragThumb = false;
	int m_dragOffset = 0;
	int m_tipPending = -1;   // 当前悬停的条目（-1 无，-2 状态行）
	std::wstring m_tipText;
	HBRUSH m_bgBrush = nullptr;
	HBRUSH m_selBrush = nullptr;
	COLORREF m_bgColor = CLR_INVALID;
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

