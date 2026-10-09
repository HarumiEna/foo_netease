#include "stdafx.h"
#include "browse_ui.h"

#include <commctrl.h>

#include <helpers/DarkMode.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "component_log.h"
#include "core/api.h"
#include "core/meta_cache.h"
#include "meta_store.h"
#include "netease_data.h"
#include "resource.h"
#include "session.h"
#include "theme.h"
#include "ui_scale.h"
#include "win_utf8.h"

#pragma comment(lib, "comctl32.lib")

namespace netease_ui {

namespace {

// 活的窗口标记。工作线程把结果投递回主线程时要先确认窗口还在，
// 否则就是经典的悬垂句柄问题。
struct Liveness {
	std::atomic<bool> alive{ true };
};

std::string format_duration(int64_t ms) {
	if (ms <= 0) return "--:--";
	const int64_t total = ms / 1000;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(total / 60),
		static_cast<long long>(total % 60));
	return buf;
}

std::string track_display(const netease::TrackInfo & t) {
	return t.title + "  -  " + t.artists + "  [" + t.album + "]  " + format_duration(t.duration_ms);
}

class BrowseWindow : public CDialogImpl<BrowseWindow> {
public:
	enum { IDD = IDD_NETEASE_BROWSE };

	// owner 传设置页/主窗口：设置页是模态窗口，没有 owner 的新窗口会被压在后面，
	// 用户看到的现象是「点一次没反应，再点一次才出来」。
	// 打开窗口，**只**显示指定的那一个歌单（面板右键「显示歌单」用）。
	static void open_for(fb2k::hwnd_t owner, int64_t playlist_id) {
		if (s_instance && s_instance->m_hWnd && ::IsWindow(s_instance->m_hWnd)) {
			// 已经开着：直接换成这一个歌单，不用重开窗口。
			s_instance->m_single = true;
			::ShowWindow(s_instance->m_hWnd, SW_SHOW);
			::SetForegroundWindow(s_instance->m_hWnd);
			s_instance->show_single_playlist(playlist_id);
			return;
		}
		s_pending_id = playlist_id;
		s_pending_single = true;
		open(owner);
	}

	// 打开窗口并把搜索结果显示出来（面板左键「搜索」用）。
	static void open_search(fb2k::hwnd_t owner, const std::string & keyword) {
		// 新开的窗口要让 OnInitDialog 知道这是搜索模式，别再拉一遍"我的歌单"。
		const bool fresh = !(s_instance && s_instance->m_hWnd && ::IsWindow(s_instance->m_hWnd));
		if (fresh) s_pending_search = true;
		open(owner);
		s_pending_search = false;
		if (s_instance && s_instance->m_hWnd) s_instance->begin_search(keyword);
	}

	// 面板「更多」调用：让搜索面板接着往下翻一页。窗口不在就返回 false。
	static bool more_search() {
		if (!(s_instance && s_instance->m_hWnd)) return false;
		s_instance->search_more();
		return true;
	}

	// 搜索模式：和右键「显示歌单」同一个面板 —— 只留曲目列表，显示搜索结果。
	void begin_search(const std::string & keyword) {
		m_single = true;
		m_pending_playlist = 0;
		m_singlePlaylist = 0;
		m_searchKeyword = keyword;
		m_searchOffset = 0;
		SetWindowText(netease::to_wide("网易云音乐 · 搜索结果").c_str());
		for (UINT id : { IDC_BROWSE_SEARCH, IDC_BROWSE_SEARCH_BTN, IDC_BROWSE_ACCOUNT,
				IDC_BROWSE_PLAYLISTS }) {
			if (HWND h = ::GetDlgItem(m_hWnd, id)) ::ShowWindow(h, SW_HIDE);
		}
		CRect rc;
		if (GetClientRect(&rc)) layout(rc.Width(), rc.Height());
		set_status(netease::to_wide("正在搜索…").c_str());
		search_async(keyword, 0, false);
	}

	// 单歌单模式：藏掉左边的歌单列表，曲目列表占满窗口。
	void show_single_playlist(int64_t playlist_id) {
		m_pending_playlist = 0;
		m_singlePlaylist = playlist_id;
		m_searchKeyword.clear();   // 已经切到单歌单，「更多」不该再翻搜索的页
		if (HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS)) ::ShowWindow(list, SW_HIDE);
		CRect rc;
		if (GetClientRect(&rc)) layout(rc.Width(), rc.Height());
		set_status(netease::to_wide("正在加载歌单…").c_str());
		load_tracks_async(playlist_id);
	}

	static void open(fb2k::hwnd_t owner) {
		// 旧实例的窗口已经销毁时**不能复用**：WTL 的 Create 要求 m_hWnd 为空，
		// 而且对着死句柄发消息什么都不会发生（"关掉后再也打不开"就是这个）。
		if (s_instance && (!s_instance->m_hWnd || !::IsWindow(s_instance->m_hWnd))) {
			s_instance = nullptr;   // 旧对象不再引用（很小的泄漏，换稳定）
		}
		if (!s_instance) s_instance = new BrowseWindow();
		if (!s_instance->m_hWnd) {
			s_instance->m_alive = std::make_shared<Liveness>();
			s_instance->Create(reinterpret_cast<HWND>(owner));
		}
		s_instance->ShowWindow(SW_SHOW);
		::SetForegroundWindow(s_instance->m_hWnd);
	}

	BEGIN_MSG_MAP_EX(BrowseWindow)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_SIZE(OnSize)
		MESSAGE_HANDLER(netease_ui::kMsgScaleChanged, OnScaleChanged)
		MESSAGE_HANDLER(netease_ui::kMsgThemeChanged, OnThemeChanged)
		MESSAGE_HANDLER(WM_PAINT, OnPaintMsg)
		MSG_WM_LBUTTONDOWN(OnLButtonDownMsg)
		MSG_WM_MOUSEMOVE(OnMouseMoveMsg)
		MSG_WM_LBUTTONUP(OnLButtonUpMsg)
		MSG_WM_DRAWITEM(OnDrawItem)
		MSG_WM_ERASEBKGND(OnEraseBkgnd)
		MSG_WM_CTLCOLORDLG(OnCtlColor)
		MSG_WM_CTLCOLORSTATIC(OnCtlColor)
		MSG_WM_CTLCOLOREDIT(OnCtlColor)
		MSG_WM_CTLCOLORLISTBOX(OnCtlColor)
		MSG_WM_CTLCOLORBTN(OnCtlColor)
		NOTIFY_CODE_HANDLER(NM_CUSTOMDRAW, OnListCustomDraw)
		MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)
		MSG_WM_CLOSE(OnClose)
		MSG_WM_NCDESTROY(OnNcDestroy)
		COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_REFRESH, OnRefresh)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_SEARCH_BTN, OnSearch)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_ADD, OnAdd)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_REPLACE, OnReplace)
		COMMAND_HANDLER_EX(IDC_BROWSE_PLAYLISTS, LBN_SELCHANGE, OnPlaylistChanged)
		NOTIFY_CODE_HANDLER(NM_DBLCLK, OnTrackDoubleClick)
		MSG_WM_CONTEXTMENU(OnContextMenu)
	END_MSG_MAP()

private:
	// 状态行自己刷底色：带 uxtheme 主题的 STATIC 有时不认 WM_CTLCOLORSTATIC，
	// 会出现"文字下面是一条浅色"。子类化后我们自己填主题底色。
	static LRESULT CALLBACK status_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
		UINT_PTR, DWORD_PTR ref) {
		auto * self = reinterpret_cast<BrowseWindow *>(ref);
		if (self && msg == WM_ERASEBKGND) {
			if (!self->m_bgBrush || self->m_bgColor != self->m_theme.background) self->rebuild_brushes();
			RECT rc{};
			::GetClientRect(hwnd, &rc);
			if (self->m_bgBrush) ::FillRect(reinterpret_cast<HDC>(wp), &rc, self->m_bgBrush);
			return 1;
		}
		// 干脆整块自己画：STATIC 有时根本不走 WM_CTLCOLORSTATIC（带主题时），
		// 表现就是文字下面一条白/浅色底。
		if (self && msg == WM_PAINT) {
			if (!self->m_bgBrush || self->m_bgColor != self->m_theme.background) self->rebuild_brushes();
			PAINTSTRUCT ps{};
			HDC dc = ::BeginPaint(hwnd, &ps);
			RECT rc{};
			::GetClientRect(hwnd, &rc);
			if (self->m_bgBrush) ::FillRect(dc, &rc, self->m_bgBrush);
			wchar_t text[512] = {};
			::GetWindowTextW(hwnd, text, 512);
			::SetBkMode(dc, TRANSPARENT);
			::SetTextColor(dc, self->m_theme.text);
			::SelectObject(dc, reinterpret_cast<HGDIOBJ>(::SendMessageW(hwnd, WM_GETFONT, 0, 0)));
			::DrawTextW(dc, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
			::EndPaint(hwnd, &ps);
			return 0;
		}
		return ::DefSubclassProc(hwnd, msg, wp, lp);
	}

	BOOL OnInitDialog(CWindow, LPARAM) {
		m_single = s_pending_single;      // 面板右键「显示歌单」= 只显示这一个歌单
		s_pending_single = false;
		const bool pending_search = s_pending_search;   // 面板左键「搜索」= 搜索模式
		s_pending_search = false;
		SetWindowText(netease::to_wide("网易云音乐 · 歌单").c_str());
		SetDlgItemText(IDC_BROWSE_SEARCH_BTN, netease::to_wide("搜索").c_str());
		SetDlgItemText(IDC_BROWSE_REFRESH, netease::to_wide("刷新").c_str());
		SetDlgItemText(IDC_BROWSE_ADD, netease::to_wide("添加到当前播放列表").c_str());
		SetDlgItemText(IDC_BROWSE_REPLACE, netease::to_wide("替换当前播放列表").c_str());
		// 主题必须在暗色钩子之前刷新：AddDialog() 会拿这里的底色去画
		// STATIC/EDIT/LISTBOX 的背景，用默认白底就会留下一片白。
		refresh_theme();
		apply_dark_hooks();
		apply_fonts();

		if (HWND status = ::GetDlgItem(m_hWnd, IDC_BROWSE_STATUS)) {
			// 隐藏掉：它只用来存文本，状态行由窗口自己画（STATIC 在暗色下不可靠）。
			::ShowWindow(status, SW_HIDE);
		}

		if (HWND tracks = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS)) {
			::SetWindowSubclass(tracks, &BrowseWindow::list_subclass, 1,
				reinterpret_cast<DWORD_PTR>(this));
		}
		// 按钮改成自绘：对话框模板里的按钮在暗色下边框仍是系统亮色
		//（截图里按钮外那圈近白 #FAFAFA 就是它），只有自绘才能彻底去掉。
		for (UINT id : { IDC_BROWSE_ADD, IDC_BROWSE_REPLACE, IDC_BROWSE_REFRESH,
				IDC_BROWSE_SEARCH_BTN }) {
			if (HWND b = ::GetDlgItem(m_hWnd, id)) {
				::SetWindowLongW(b, GWL_STYLE,
					::GetWindowLongW(b, GWL_STYLE) | BS_OWNERDRAW);
			}
		}
		setup_track_columns();
		theme_track_header();

		if (m_single || pending_search) {
			// 只显示曲目列表：单歌单 / 搜索结果都藏掉左侧歌单列表、搜索框和账号标签。
			for (UINT id : { IDC_BROWSE_SEARCH, IDC_BROWSE_SEARCH_BTN, IDC_BROWSE_ACCOUNT,
					IDC_BROWSE_PLAYLISTS }) {
				if (HWND h = ::GetDlgItem(m_hWnd, id)) ::ShowWindow(h, SW_HIDE);
			}
			if (pending_search) {
				m_single = true;
				CRect rc;
				if (GetClientRect(&rc)) layout(rc.Width(), rc.Height());
				return TRUE;   // 真正的搜索由 begin_search() 发起
			}
			show_single_playlist(s_pending_id);
			return TRUE;
		}

		if (!netease::Session::instance().logged_in()) {
			set_status(netease::to_wide(
				"尚未登录。请到 参数设置 → 工具 → 网易云音乐 里扫码登录。").c_str());
			return TRUE;
		}
		set_status(netease::to_wide("正在加载歌单…").c_str());
		load_playlists_async();
		return TRUE;
	}

	// 无模态对话框（Create 而不是 DoModal）必须自己处理关闭：
	// 这条路径上 DefDlgProc 不会销毁窗口，表现就是"打得开、关不掉"。
	void OnClose() {
		DestroyWindow();
	}

	// 窗口销毁后必须把单例清掉：否则下次 open_for() 以为窗口还在，
	// 对着已销毁的 HWND 发消息 —— 表现就是"只能打开一次，关掉后再也打不开"。
	void OnNcDestroy() {
		if (s_instance == this) s_instance = nullptr;
		SetMsgHandled(FALSE);
	}

	void OnCancel(UINT, int, CWindow) {
		DestroyWindow();
	}

	// 歌单列表加载完（或窗口刚开）时，把待定位的歌单选中并加载曲目。
	void apply_pending_playlist() {
		if (m_pending_playlist == 0 || m_playlists.empty()) return;
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS);
		for (size_t i = 0; i < m_playlists.size(); ++i) {
			if (m_playlists[i].id != m_pending_playlist) continue;
			::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(i), 0);
			set_status(netease::to_wide("正在加载曲目…").c_str());
			load_tracks_async(m_playlists[i].id);
			m_pending_playlist = 0;
			return;
		}
	}

	void OnDestroy() {
		if (m_alive) m_alive->alive = false;
		if (m_rowImages) { ImageList_Destroy(m_rowImages); m_rowImages = nullptr; }
		if (m_bgBrush) { ::DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
		if (m_selBrush) { ::DeleteObject(m_selBrush); m_selBrush = nullptr; }
		if (m_hiBrush) { ::DeleteObject(m_hiBrush); m_hiBrush = nullptr; }
	}

	void OnSize(UINT, CSize size) {
		apply_fonts();
		layout(size.cx, size.cy);
	}

	// 设置页改了「界面缩放」：重新设字体 + 重排，不用重启。
	LRESULT OnScaleChanged(UINT, WPARAM, LPARAM, BOOL &) {
		CRect cr;
		if (GetClientRect(&cr)) {
			apply_fonts();
			layout(cr.Width(), cr.Height());
		}
		return 0;
	}

	void OnGetMinMaxInfo(LPMINMAXINFO info) {
		info->ptMinTrackSize.x = netease_ui::scale(m_hWnd, 560);
		info->ptMinTrackSize.y = netease_ui::scale(m_hWnd, 320);
	}

	// ---- 界面搭建 ----

	// ---- 主题色（跟随 foobar2000：背景/文字/选中/正在播放高亮）----

	void refresh_theme() {
		m_theme = netease_ui::theme_colors();
		m_bgColor = CLR_INVALID;
		rebuild_brushes();
	}

	void rebuild_brushes() {
		if (m_bgBrush) { ::DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
		if (m_selBrush) { ::DeleteObject(m_selBrush); m_selBrush = nullptr; }
		if (m_hiBrush) { ::DeleteObject(m_hiBrush); m_hiBrush = nullptr; }
		m_bgColor = m_theme.background;
		m_bgBrush = ::CreateSolidBrush(m_theme.background);
		m_selBrush = ::CreateSolidBrush(m_theme.selection);
		m_hiBrush = ::CreateSolidBrush(m_theme.highlight);
		// ListView 的条目下方空白区由控件自己画，得把它的底色也设成主题色。
		if (HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS)) {
			::SendMessageW(list, LVM_SETBKCOLOR, 0, static_cast<LPARAM>(m_theme.background));
			::SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, static_cast<LPARAM>(m_theme.background));
			::SendMessageW(list, LVM_SETTEXTCOLOR, 0, static_cast<LPARAM>(m_theme.text));
		}
	}

	// ===== 自绘滚动条 =====
	// 原生滚动条是非客户区，暗色主题在组件里改不动它（试过
	// ApplyDarkThemeCtrl2 / SetWindowTheme / SetAppDarkMode / SWP_FRAMECHANGED
	// 全部无效），所以自己画一条放在列表右侧，颜色完全跟主题走。
	bool scroll_bar_needed() const {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		if (!list) return false;
		const int total = static_cast<int>(::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
		const int per_page = static_cast<int>(::SendMessageW(list, LVM_GETCOUNTPERPAGE, 0, 0));
		return per_page > 0 && total > per_page;
	}

	void scroll_bar_metrics(int & thumb_top, int & thumb_h) const {
		thumb_top = 0;
		thumb_h = 0;
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		if (!list) return;
		const int total = static_cast<int>(::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
		const int per_page = static_cast<int>(::SendMessageW(list, LVM_GETCOUNTPERPAGE, 0, 0));
		const int top = static_cast<int>(::SendMessageW(list, LVM_GETTOPINDEX, 0, 0));
		const int height = m_scrollRect.bottom - m_scrollRect.top;
		if (total <= per_page || per_page <= 0 || height <= 0) return;
		thumb_h = height * per_page / total;
		const int min_h = netease_ui::scale(m_hWnd, 28);
		if (thumb_h < min_h) thumb_h = min_h;
		if (thumb_h > height) thumb_h = height;
		const int max_top = total - per_page;
		const int travel = height - thumb_h;
		thumb_top = (max_top > 0 && travel > 0) ? (travel * top / max_top) : 0;
	}

	void draw_scroll_bar(HDC dc) {
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
		scroll_bar_metrics(top, height);
		if (height <= 0) return;
		RECT tr = m_scrollRect;
		tr.top += top;
		tr.bottom = tr.top + height;
		HBRUSH thumb = ::CreateSolidBrush(dark ? RGB(up(r + 58), up(g + 58), up(b + 58))
			: RGB(up(r - 70), up(g - 70), up(b - 70)));
		::FillRect(dc, &tr, thumb);
		::DeleteObject(thumb);
	}

	bool in_scroll_rect(CPoint pt) const {
		return pt.x >= m_scrollRect.left && pt.x < m_scrollRect.right &&
			pt.y >= m_scrollRect.top && pt.y < m_scrollRect.bottom;
	}

	void scroll_list_by(int delta_y) {
		if (HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS)) {
			::SendMessageW(list, LVM_SCROLL, 0, static_cast<LPARAM>(delta_y));
			::InvalidateRect(m_hWnd, &m_scrollRect, FALSE);
		}
	}

	void OnLButtonDownMsg(UINT, CPoint pt) {
		if (!scroll_bar_needed() || !in_scroll_rect(pt)) { SetMsgHandled(FALSE); return; }
		int top = 0, height = 0;
		scroll_bar_metrics(top, height);
		if (height > 0 && pt.y >= m_scrollRect.top + top && pt.y < m_scrollRect.top + top + height) {
			m_dragThumb = true;
			m_dragOffset = pt.y - (m_scrollRect.top + top);
			::SetCapture(m_hWnd);
			return;
		}
		// 点在空白处：按一页翻
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		const int per_page = static_cast<int>(::SendMessageW(list, LVM_GETCOUNTPERPAGE, 0, 0));
		const int row_h = m_rowH > 0 ? m_rowH : netease_ui::scale(m_hWnd, 24);
		const int dir = (pt.y < m_scrollRect.top + top) ? -1 : 1;
		scroll_list_by(dir * per_page * row_h);
	}

	void OnMouseMoveMsg(UINT, CPoint pt) {
		if (!m_dragThumb) { SetMsgHandled(FALSE); return; }
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		const int total = static_cast<int>(::SendMessageW(list, LVM_GETITEMCOUNT, 0, 0));
		const int per_page = static_cast<int>(::SendMessageW(list, LVM_GETCOUNTPERPAGE, 0, 0));
		const int height = m_scrollRect.bottom - m_scrollRect.top;
		int top = 0, thumb_h = 0;
		scroll_bar_metrics(top, thumb_h);
		const int travel = height - thumb_h;
		const int max_top = total - per_page;
		if (travel <= 0 || max_top <= 0) return;
		int want = pt.y - m_scrollRect.top - m_dragOffset;
		if (want < 0) want = 0;
		if (want > travel) want = travel;
		const int want_top = want * max_top / travel;
		const int current = static_cast<int>(::SendMessageW(list, LVM_GETTOPINDEX, 0, 0));
		const int row_h = m_rowH > 0 ? m_rowH : netease_ui::scale(m_hWnd, 24);
		if (want_top != current) scroll_list_by((want_top - current) * row_h);
	}

	void OnLButtonUpMsg(UINT, CPoint) {
		if (!m_dragThumb) { SetMsgHandled(FALSE); return; }
		m_dragThumb = false;
		::ReleaseCapture();
	}

	// 按钮自绘：按钮面取主题背景稍微提亮，边框再亮一点，文字用主题文字色。
	void draw_button(LPDRAWITEMSTRUCT dis) {
		if (!dis) return;
		auto up = [](int v) { return static_cast<BYTE>(v > 255 ? 255 : (v < 0 ? 0 : v)); };
		const bool dark = DarkMode::IsThemeDark(m_theme.text, m_theme.background);
		const int r = GetRValue(m_theme.background);
		const int g = GetGValue(m_theme.background);
		const int b = GetBValue(m_theme.background);
		const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
		// 亮色模式不能靠"提亮"取按钮面/边框 —— 白底提亮还是白，边框就看不见了。
		COLORREF faceColor, borderColor;
		if (dark) {
			const int lift = pressed ? 46 : 20;
			faceColor = RGB(up(r + lift), up(g + lift), up(b + lift));
			borderColor = RGB(up(r + 62), up(g + 62), up(b + 62));
		} else {
			faceColor = pressed ? RGB(up(r - 18), up(g - 18), up(b - 18)) : m_theme.background;
			borderColor = RGB(160, 160, 160);
		}
		HBRUSH face = ::CreateSolidBrush(faceColor);
		::FillRect(dis->hDC, &dis->rcItem, face);
		::DeleteObject(face);
		HPEN pen = ::CreatePen(PS_SOLID, 1, borderColor);
		HGDIOBJ oldPen = ::SelectObject(dis->hDC, pen);
		HGDIOBJ oldBrush = ::SelectObject(dis->hDC, ::GetStockObject(NULL_BRUSH));
		::Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
			dis->rcItem.right, dis->rcItem.bottom);
		::SelectObject(dis->hDC, oldBrush);
		::SelectObject(dis->hDC, oldPen);
		::DeleteObject(pen);
		wchar_t text[128] = {};
		::GetWindowTextW(dis->hwndItem, text, 128);
		::SetBkMode(dis->hDC, TRANSPARENT);
		::SetTextColor(dis->hDC, m_theme.text);
		::SelectObject(dis->hDC,
			reinterpret_cast<HGDIOBJ>(::SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0)));
		RECT rc = dis->rcItem;
		if (pressed) { rc.left += 1; rc.top += 1; }
		::DrawTextW(dis->hDC, text, -1, &rc,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	void OnDrawItem(UINT, LPDRAWITEMSTRUCT dis) {
		if (!dis || dis->CtlType != ODT_BUTTON) { SetMsgHandled(FALSE); return; }
		draw_button(dis);
	}

	// 表头列项是 owner-draw：底色填主题背景，列名用主题文字色画。
	// 注意：owner-draw 的 WM_DRAWITEM 发给表头的**父窗口**（也就是这个 ListView），
	// 不是发给对话框 —— 所以要靠下面的 ListView 子类化把它接过来。
	void draw_header_item(LPDRAWITEMSTRUCT dis) {
		if (!dis || dis->CtlType != ODT_HEADER) return;
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		if (m_bgBrush) ::FillRect(dis->hDC, &dis->rcItem, m_bgBrush);
		wchar_t text[128] = {};
		HDITEMW item{};
		item.mask = HDI_TEXT;
		item.pszText = text;
		item.cchTextMax = 128;
		::SendMessageW(dis->hwndItem, HDM_GETITEMW, dis->itemID, reinterpret_cast<LPARAM>(&item));
		::SetBkMode(dis->hDC, TRANSPARENT);
		::SetTextColor(dis->hDC, m_theme.text);
		::SelectObject(dis->hDC,
			reinterpret_cast<HGDIOBJ>(::SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0)));
		RECT rc = dis->rcItem;
		rc.left += netease_ui::scale(m_hWnd, 6);
		rc.right -= netease_ui::scale(m_hWnd, 4);
		::DrawTextW(dis->hDC, text, -1, &rc,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}

	static LRESULT CALLBACK list_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
		UINT_PTR, DWORD_PTR ref) {
		auto * self = reinterpret_cast<BrowseWindow *>(ref);
		if (self && msg == WM_MOUSEWHEEL) {
			// 摘掉 WS_VSCROLL 后 ListView 自己的滚轮处理就失效了，这里手动滚。
			const int delta = GET_WHEEL_DELTA_WPARAM(wp);
			const int lines = (delta / WHEEL_DELTA) * 3;   // 一格滚 3 行
			const int row_h = self->m_rowH > 0 ? self->m_rowH : netease_ui::scale(self->m_hWnd, 24);
			::SendMessageW(hwnd, LVM_SCROLL, 0, static_cast<LPARAM>(-lines * row_h));
			::InvalidateRect(self->m_hWnd, &self->m_scrollRect, FALSE);
			return 0;
		}
		if (self && (msg == WM_VSCROLL || msg == WM_HSCROLL)) {
			// 同理：键盘/翻页走我们自己的滚动条刷新
			::InvalidateRect(self->m_hWnd, &self->m_scrollRect, FALSE);
		}
		if (self && msg == WM_NCCALCSIZE) {
			// 不要原生滚动条 —— 右侧那条由我们自己画（原生条在暗色下没法着色）。
			// 摘掉样式只影响"显示"，ListView 内部的滚动位置照旧，滚轮/LVM_SCROLL 都能用。
			const LONG style = ::GetWindowLongW(hwnd, GWL_STYLE);
			if (style & (WS_VSCROLL | WS_HSCROLL)) {
				::SetWindowLongW(hwnd, GWL_STYLE, style & ~(WS_VSCROLL | WS_HSCROLL));
			}
		}
		if (self && (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL || msg == WM_KEYDOWN ||
				msg == WM_LBUTTONUP || msg == WM_SIZE)) {
			::InvalidateRect(self->m_hWnd, &self->m_scrollRect, FALSE);
		}
		if (self && msg == WM_DRAWITEM) {
			auto * dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
			if (dis && dis->CtlType == ODT_HEADER) {
				self->draw_header_item(dis);
				return TRUE;
			}
		}
		return ::DefSubclassProc(hwnd, msg, wp, lp);
	}

	// 状态行画在窗口自己身上：STATIC 在带主题/暗色下不可靠（实测底色总是浅的）。
	LRESULT OnPaintMsg(UINT, WPARAM, LPARAM, BOOL & handled) {
		PAINTSTRUCT ps{};
		HDC dc = ::BeginPaint(m_hWnd, &ps);
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		draw_scroll_bar(dc);
		if (m_bgBrush && m_statusRect.right > m_statusRect.left) {
			::FillRect(dc, &m_statusRect, m_bgBrush);
			::SetBkMode(dc, TRANSPARENT);
			::SetTextColor(dc, m_theme.text);
			::SelectObject(dc, reinterpret_cast<HGDIOBJ>(m_font.get(m_hWnd)));
			RECT rc = m_statusRect;
			rc.left += netease_ui::scale(m_hWnd, 2);
			::DrawTextW(dc, m_statusText.c_str(), -1, &rc,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		}
		::EndPaint(m_hWnd, &ps);
		handled = TRUE;
		return 0;
	}

	// 状态文字统一走这里：既更新 STATIC（保持兼容），也刷新窗口上的绘制。
	void set_status(const std::wstring & text) {
		m_statusText = text;
		if (HWND h = ::GetDlgItem(m_hWnd, IDC_BROWSE_STATUS)) ::SetWindowTextW(h, text.c_str());
		if (m_hWnd) ::InvalidateRect(m_hWnd, nullptr, FALSE);
	}

	BOOL OnEraseBkgnd(CDCHandle dc) {
		CRect rc;
		if (!GetClientRect(&rc)) return TRUE;
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		if (m_bgBrush) ::FillRect(dc, &rc, m_bgBrush);
		return TRUE;
	}

	HBRUSH OnCtlColor(HDC dc, HWND) {
		if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
		::SetTextColor(dc, m_theme.text);
		::SetBkColor(dc, m_theme.background);
		return m_bgBrush ? m_bgBrush : static_cast<HBRUSH>(::GetStockObject(NULL_BRUSH));
	}

	// 当前正在播放的是不是这一首（路径形如 netease://song/<id>?level=…）。
	bool is_now_playing(int64_t id) const {
		const std::string path = netease_data::now_playing_path();
		const std::string prefix = "netease://song/";
		if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return false;
		return std::strtoll(path.c_str() + prefix.size(), nullptr, 10) == id;
	}

	// 曲目列表：选中行用宿主的选中色，正在播放那一条用高亮色。
	LRESULT OnListCustomDraw(int, LPNMHDR hdr, BOOL &) {
		if (!hdr) return CDRF_DODEFAULT;
		// 表头的 custom draw：先把整条铺成主题底色（尾部/列间空隙都会被主题
		// 画成浅色），列项再由 HDF_OWNERDRAW 覆盖。
		if (hdr->hwndFrom != nullptr) {
			wchar_t cls[64] = {};
			::GetClassNameW(hdr->hwndFrom, cls, 64);
			if (::wcscmp(cls, L"SysHeader32") == 0) {
				auto * hc = reinterpret_cast<LPNMCUSTOMDRAW>(hdr);
				if (hc->dwDrawStage == CDDS_PREPAINT) {
					if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
					if (m_bgBrush) {
						HDC dc = hc->hdc;
						RECT rc{};
						::GetClientRect(hdr->hwndFrom, &rc);
						::FillRect(dc, &rc, m_bgBrush);
					}
					return CDRF_NOTIFYITEMDRAW;
				}
				return CDRF_DODEFAULT;
			}
		}
		auto * cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(hdr);
		if (!cd) return CDRF_DODEFAULT;
		switch (cd->nmcd.dwDrawStage) {
		case CDDS_PREPAINT:
			return CDRF_NOTIFYITEMDRAW;
		case CDDS_ITEMPREPAINT: {
			const size_t index = static_cast<size_t>(cd->nmcd.dwItemSpec);
			// 用真实查询，而不是 custom draw 里的 uItemState ——
			// 实测它会对所有条目都报 CDIS_SELECTED（整片刷成选中色）。
			HWND lv = reinterpret_cast<HWND>(hdr->hwndFrom);
			const bool selected =
				(::SendMessageW(lv, LVM_GETITEMSTATE, static_cast<WPARAM>(index), LVIS_SELECTED) &
					LVIS_SELECTED) != 0;
			cd->clrTextBk = m_theme.background;
			cd->clrText = m_theme.text;
			if (selected) {
				cd->clrTextBk = m_theme.selection;
				cd->clrText = m_theme.selection_text;
			} else if (index < m_tracks.size() && is_now_playing(m_tracks[index].id)) {
				cd->clrTextBk = m_theme.highlight;
				cd->clrText = m_theme.selection_text;
			}
			return CDRF_DODEFAULT;
		}
		default:
			return CDRF_DODEFAULT;
		}
	}

	LRESULT OnThemeChanged(UINT, WPARAM, LPARAM, BOOL &) {
		refresh_theme();
		apply_dark_hooks();   // 明暗切换后必须重套，否则滚动条还留着上一套
		// 状态栏、搜索框、列表都是子控件，一起重画才会立刻跟随配色。
		::RedrawWindow(m_hWnd, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		return 0;
	}

	// 控件字体按「系统 DPI × 用户缩放」设置：4K 屏下行距才不会被压得又小又挤。
	void apply_fonts() {
		HFONT font = m_font.get(m_hWnd);
		for (HWND h = ::GetWindow(m_hWnd, GW_CHILD); h; h = ::GetWindow(h, GW_HWNDNEXT)) {
			::SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
		}
	}

	void setup_track_columns() {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP,
			LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP);   // INFOTIP：列内容被截断时悬停显示完整文字
		struct Column { const wchar_t * text; int width; };
		const Column columns[] = {
			{ L"#", 40 }, { L"标题", 190 }, { L"歌手", 120 }, { L"专辑", 150 }, { L"时长", 55 },
		};
		for (int i = 0; i < 5; ++i) {
			LVCOLUMNW col{};
			col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
			col.cx = netease_ui::scale(m_hWnd, columns[i].width);
			col.iSubItem = i;
			col.pszText = const_cast<LPWSTR>(columns[i].text);
			::SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
				reinterpret_cast<LPARAM>(&col));
		}
		// 末尾加一个空标题的"填充列"占满剩余宽度：这样表头没有裸露的尾部
		//（那片是表头自己画的，主题会把它画成和前面不一致的浅色）。
		{
			LVCOLUMNW col{};
			col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
			col.cx = netease_ui::scale(m_hWnd, 200);
			col.iSubItem = 5;
			col.pszText = const_cast<LPWSTR>(L"");
			::SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(5),
				reinterpret_cast<LPARAM>(&col));
		}

		theme_track_header();

		// 行距：report 视图的行高只跟字体走，高 DPI 下显得挤。挂一个 1 像素宽、
		// 指定高度的小图标列表，行高就跟着它走（这是 ListView 的常规做法）。
		if (m_rowImages) { ImageList_Destroy(m_rowImages); m_rowImages = nullptr; }
		m_rowH = netease_ui::scale(m_hWnd, 24);
		m_rowImages = ImageList_Create(1, m_rowH, ILC_COLOR32, 0, 1);
		if (m_rowImages) {
			::SendMessageW(list, LVM_SETIMAGELIST, LVSIL_SMALL,
				reinterpret_cast<LPARAM>(m_rowImages));
		}
	}

	// 按文字宽度摆一个按钮，返回下一个按钮的 x。
	int fit_button(HWND btn, int x, int y) {
		if (!btn) return x;
		wchar_t text[128] = {};
		::GetWindowTextW(btn, text, 128);
		HDC dc = ::GetDC(btn);
		HGDIOBJ old = ::SelectObject(dc,
			reinterpret_cast<HGDIOBJ>(::SendMessageW(btn, WM_GETFONT, 0, 0)));
		SIZE size{};
		::GetTextExtentPoint32W(dc, text, static_cast<int>(::wcslen(text)), &size);
		::SelectObject(dc, old);
		::ReleaseDC(btn, dc);
		const int w = size.cx + netease_ui::scale(m_hWnd, 24);
		const int h = netease_ui::scale(m_hWnd, 22);
		::SetWindowPos(btn, nullptr, x, y, w, h, SWP_NOZORDER);
		return x + w + netease_ui::scale(m_hWnd, 8);
	}

	// 曲目列表的表头（SysHeader32）单独套主题 —— 否则它在暗色下还是白的。
	void theme_track_header() {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		if (!list) return;
		HWND header = reinterpret_cast<HWND>(::SendMessageW(list, LVM_GETHEADER, 0, 0));
		if (!header) return;
		// 和 foobar2000 播放列表的表头用同一套：ApplyDarkThemeCtrl2 会把窗口
		// 标记为允许暗色，再用 DarkMode_ItemsView —— 这样底色和文字都是对的。
		// 表头底色交给主题（和播放列表一致），但**文字我们自己画**：
		// 把每个列项设成 owner-draw，WM_DRAWITEM 里用主题文字色画列名 ——
		// 否则 uxtheme 有时会用纯黑文字画在深色表头上，根本看不清。
		const bool dark_bg = DarkMode::IsThemeDark(m_theme.text, m_theme.background);
		DarkMode::ApplyDarkThemeCtrl2(header, dark_bg, L"ItemsView", L"DarkMode_ItemsView");
		// 表头尾部（超出最后一列那片）是表头自己画的，主题会把它画成浅色 ——
		// 整条先铺一遍主题底色，列项再由 owner-draw 覆盖。
		{
			HDC dc = ::GetDC(header);
			RECT rc{};
			::GetClientRect(header, &rc);
			if (!m_bgBrush || m_bgColor != m_theme.background) rebuild_brushes();
			if (m_bgBrush) ::FillRect(dc, &rc, m_bgBrush);
			::ReleaseDC(header, dc);
		}
		const int cols = static_cast<int>(::SendMessageW(header, HDM_GETITEMCOUNT, 0, 0));
		for (int i = 0; i < cols; ++i) {
			HDITEMW item{};
			item.mask = HDI_FORMAT;
			if (!::SendMessageW(header, HDM_GETITEMW, i, reinterpret_cast<LPARAM>(&item))) continue;
			item.fmt |= HDF_OWNERDRAW;
			::SendMessageW(header, HDM_SETITEMW, i, reinterpret_cast<LPARAM>(&item));
		}
		::InvalidateRect(header, nullptr, TRUE);
	}

	static std::wstring window_class_of(HWND h) {
		wchar_t buf[128] = {};
		::GetClassNameW(h, buf, 128);
		return buf;
	}

	// 给控件重新套明暗主题（切明暗/改配色时都要重来）。
	void apply_dark_hooks() {
		m_dark.AddDialog(m_hWnd, m_theme.background);
		const bool dark_bg = DarkMode::IsThemeDark(m_theme.text, m_theme.background);
		// 进程层面先允许暗色：AllowDarkModeForWindow() 需要 SetPreferredAppMode()
		// 生效后才有用，否则滚动条这类非客户区仍然是经典亮色。
		DarkMode::SetAppDarkMode(dark_bg);
		for (HWND child = ::GetWindow(m_hWnd, GW_CHILD); child;
				child = ::GetWindow(child, GW_HWNDNEXT)) {
			const UINT id = ::GetDlgCtrlID(child);
			const std::wstring cls = window_class_of(child);
			if (id == IDC_BROWSE_TRACKS) {
				DarkMode::ApplyDarkThemeCtrl2(child, dark_bg);
				// 先"允许暗色"再明确指定 Explorer 主题 —— 只调一个时滚动条仍是经典亮色。
				// 注意：滚动条属于非客户区，**已经存在**的滚动条不会因 SetWindowTheme
				// 变色，必须用 SWP_FRAMECHANGED 让非客户区重建一次。
				::SetWindowTheme(child, dark_bg ? L"DarkMode_Explorer" : L"Explorer", nullptr);
				::SetWindowPos(child, nullptr, 0, 0, 0, 0,
					SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
			} else if (cls == L"Static") {
				::SetWindowTheme(child, L"", L"");
			} else if (id == IDC_BROWSE_PLAYLISTS) {
				DarkMode::ApplyDarkThemeCtrl2(child, dark_bg);
			} else {
				DarkMode::ApplyDarkThemeCtrl2(child, dark_bg);
				m_dark.AddButton(child);
			}
		}
		theme_track_header();
	}

	void layout(int cx, int cy) {
		// 全部按窗口 DPI 缩放：4K 屏上不缩的话又小又挤（issue #1）。
		const int margin = netease_ui::scale(m_hWnd, 8);
		// 单歌单模式：不占左边那一栏，曲目列表铺满。
		const int left_w = m_single ? 0 : netease_ui::scale(m_hWnd, 208);
		const int top = netease_ui::scale(m_hWnd, 30);
		// 底部区：按钮 26 + 状态行 18 + 间距 8 —— 以前按 34 算，
		// 结果列表底部压在状态行上，两行文字叠在一起看不清。
		const int bottom = netease_ui::scale(m_hWnd, 26 + 18 + 8);

		if (!m_single) {
			::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS), nullptr, margin, top,
				left_w, cy - top - margin * 2, SWP_NOZORDER);
		}
		const int right_x = m_single ? margin : (margin + left_w + netease_ui::scale(m_hWnd, 12));
		const int right_w = cx - right_x - margin;
		if (right_w < netease_ui::scale(m_hWnd, 120)) return;

		// 填充列宽 = 列表宽度 - 前 5 列宽度，让表头铺满。
		if (HWND tracks = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS)) {
			int used = 0;
			for (int i = 0; i < 5; ++i) {
				used += static_cast<int>(::SendMessageW(tracks, LVM_GETCOLUMNWIDTH,
					static_cast<WPARAM>(i), 0));
			}
			// 要减掉**竖向滚动条的宽度**再留点余量：否则填充列把内容顶宽，
			// ListView 就会冒出一条根本不需要的横向滚动条（用户报的现象）。
			// 现在没有原生滚动条了，宽度稳定。填充列必须**正好铺满客户区**，
			// 差一点点就会露出表头自己的尾部（那条颜色不一样的窄栏）。
			// 列表窗口带 WS_BORDER，客户区 = 窗口宽 - 4。
			const int list_w = right_w - netease_ui::scale(m_hWnd, 13);
			const int client_w = list_w - 4;
			int filler = client_w - used;
			if (filler < 0) filler = 0;
			::SendMessageW(tracks, LVM_SETCOLUMNWIDTH, static_cast<WPARAM>(5),
				static_cast<LPARAM>(filler > 0 ? filler : 0));
		}
		if (!m_single) {
			::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_SEARCH), nullptr, right_x, netease_ui::scale(m_hWnd, 10),
				right_w - netease_ui::scale(m_hWnd, 122), netease_ui::scale(m_hWnd, 22), SWP_NOZORDER);
			::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_SEARCH_BTN), nullptr,
				right_x + right_w - netease_ui::scale(m_hWnd, 116), netease_ui::scale(m_hWnd, 9),
				netease_ui::scale(m_hWnd, 54), netease_ui::scale(m_hWnd, 23), SWP_NOZORDER);
		}
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_REFRESH), nullptr,
			right_x + right_w - netease_ui::scale(m_hWnd, 56), netease_ui::scale(m_hWnd, 9),
			netease_ui::scale(m_hWnd, 56), netease_ui::scale(m_hWnd, 23), SWP_NOZORDER);
		// 右侧留出我们自绘滚动条的宽度。
		const int sb_w = netease_ui::scale(m_hWnd, 13);
		const int list_top = netease_ui::scale(m_hWnd, 36);
		const int list_h = cy - list_top - bottom - margin;
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS), nullptr, right_x, list_top,
			right_w - sb_w, list_h, SWP_NOZORDER | SWP_FRAMECHANGED);
		m_scrollRect.left = right_x + right_w - sb_w;
		m_scrollRect.top = list_top;
		m_scrollRect.right = right_x + right_w;
		m_scrollRect.bottom = list_top + list_h;
		m_statusRect.left = right_x;
		m_statusRect.top = cy - netease_ui::scale(m_hWnd, 26 + 18 + 4);
		m_statusRect.right = right_x + right_w;
		m_statusRect.bottom = m_statusRect.top + netease_ui::scale(m_hWnd, 18);
		if (HWND status = ::GetDlgItem(m_hWnd, IDC_BROWSE_STATUS)) {
			::SetWindowPos(status, nullptr, m_statusRect.left, m_statusRect.top,
				m_statusRect.right - m_statusRect.left,
				m_statusRect.bottom - m_statusRect.top, SWP_NOZORDER);
		}
		// 底部两个按钮按**文字实际宽度**排：写死宽度时中文标题会被截断。
		int bx = right_x;
		const int by = cy - netease_ui::scale(m_hWnd, 26);
		bx = fit_button(::GetDlgItem(m_hWnd, IDC_BROWSE_ADD), bx, by);
		bx = fit_button(::GetDlgItem(m_hWnd, IDC_BROWSE_REPLACE), bx, by);
}

	// ---- 数据加载（全部在工作线程，结果投递回主线程） ----

	void load_playlists_async() {
		auto alive = m_alive;
		fb2k::splitTask([alive] {
			netease::CookieJar jar;
			jar.deserialize(netease::Session::instance().cookie_header());
			netease::NeteaseApi api(jar);

			int64_t uid = 0;
			std::string nickname;
			netease::ApiCall account = api.account(uid, nickname);
			if (!account.ok) {
				post_status(alive, "取账号信息失败：" + account.error);
				return;
			}

			std::vector<netease::PlaylistInfo> all;
			std::vector<int64_t> seen;   // 服务端分页会重复返回，按 id 去重
			const int page = 100;
			for (int offset = 0; offset < 2000; offset += page) {
				std::vector<netease::PlaylistInfo> chunk;
				netease::ApiCall call = api.user_playlists(uid, page, offset, chunk);
				if (!call.ok) {
					post_status(alive, "取歌单失败：" + call.error);
					return;
				}
				for (netease::PlaylistInfo & p : chunk) {
					if (std::find(seen.begin(), seen.end(), p.id) != seen.end()) continue;
					seen.push_back(p.id);
					all.push_back(std::move(p));
				}
				if (static_cast<int>(chunk.size()) < page) break;
			}

			if (!alive->alive) return;
			fb2k::inMainThread([alive, all, nickname, uid] {
				if (!alive->alive || !s_instance || !s_instance->m_hWnd) return;
				s_instance->apply_playlists(all, nickname, uid);
			});
		});
	}

	void load_tracks_async(int64_t playlist_id) {
		auto alive = m_alive;
		fb2k::splitTask([alive, playlist_id] {
			netease::CookieJar jar;
			jar.deserialize(netease::Session::instance().cookie_header());
			netease::NeteaseApi api(jar);

			std::vector<int64_t> ids;
			std::string name;
			int64_t track_count = -1;
			std::vector<netease::TrackInfo> seed;
			netease::ApiCall detail = api.playlist_track_ids(playlist_id, ids, name, track_count, &seed);
			// 详情自带的元数据先入缓存：≤1000 首的歌单到这一步已经全有了。
			if (!seed.empty()) netease::MetaCache::instance().put_all(seed);
			if (!detail.ok) {
				post_status(alive, "取歌单详情失败：" + detail.error);
				return;
			}

			// 关键：tracks 会被服务端截断（实测 1368 首只给 1000 条），
			// 所以这里用 trackIds 全集，再按曲目缓存补元数据（命中缓存的**不再请求**）。
			std::vector<netease::TrackInfo> tracks;
			std::vector<int64_t> missing;
			netease_data::TracksFetchStats stats;
			netease::ApiCall songs = netease_data::load_tracks_cached(api, ids, tracks, &missing, &stats);
			if (!songs.ok) {
				post_status(alive, "取曲目详情失败：" + songs.error);
				return;
			}

			std::string summary = name + "：" + std::to_string(ids.size()) + " 首";
			if (track_count >= 0 && static_cast<size_t>(track_count) != ids.size()) {
				summary += "（服务端 trackCount=" + std::to_string(track_count) + "）";
			}
			if (!missing.empty()) summary += "，有 " + std::to_string(missing.size()) + " 首取不到元数据";

			if (!alive->alive) return;
			fb2k::inMainThread([alive, tracks, summary] {
				if (!alive->alive || !s_instance || !s_instance->m_hWnd) return;
				s_instance->apply_tracks(tracks, summary);
			});
		});
	}

	// append=false 覆盖列表（一次新搜索），append=true 追加（「更多」翻页）。
	void search_async(std::string keyword, int offset, bool append) {
		auto alive = m_alive;
		fb2k::splitTask([alive, keyword, offset, append] {
			netease::CookieJar jar;
			jar.deserialize(netease::Session::instance().cookie_header());
			netease::NeteaseApi api(jar);

			std::vector<netease::TrackInfo> tracks;
			netease::ApiCall call = api.search_songs(keyword, 100, offset, tracks);
			if (!call.ok) {
				post_status(alive, "搜索失败：" + call.error);
				return;
			}
			netease::MetaCache::instance().put_all(tracks);
			netease_app::save_meta_cache();

			const bool empty_page = tracks.empty();
			std::string summary;
			if (append) {
				summary = empty_page
					? ("搜索「" + keyword + "」：没有更多了（已 " + std::to_string(offset) + " 首）")
					: ("搜索「" + keyword + "」：又加载 " + std::to_string(tracks.size()) + " 首");
			} else {
				summary = "搜索「" + keyword + "」：" + std::to_string(tracks.size()) + " 首";
			}
			if (!alive->alive) return;
			fb2k::inMainThread([alive, tracks, summary, offset, append, empty_page] {
				if (!alive->alive || !s_instance || !s_instance->m_hWnd) return;
				if (append && empty_page) {
					s_instance->set_status(netease::to_wide(summary).c_str());
					return;
				}
				if (append) {
					s_instance->append_tracks(tracks, summary);
				} else {
					s_instance->apply_tracks(tracks, summary);
				}
				s_instance->m_searchOffset = offset + static_cast<int>(tracks.size());
			});
		});
	}

	// 「更多」：接着当前关键词再取一页，只增不改。
	void search_more() {
		if (m_searchKeyword.empty()) {
			set_status(netease::to_wide("先搜索一次，再来翻页。").c_str());
			return;
		}
		if (!netease::Session::instance().logged_in()) {
			set_status(netease::to_wide("搜索需要登录。").c_str());
			return;
		}
		set_status(netease::to_wide("正在加载更多…").c_str());
		search_async(m_searchKeyword, m_searchOffset, true);
	}

	static void post_status(const std::shared_ptr<Liveness> & alive, const std::string & text) {
		if (!alive->alive) return;
		fb2k::inMainThread([alive, text] {
			if (!alive->alive || !s_instance || !s_instance->m_hWnd) return;
			s_instance->set_status(netease::to_wide(text).c_str());
		});
	}

	// ---- 界面更新（主线程） ----

	void apply_playlists(const std::vector<netease::PlaylistInfo> & playlists,
		const std::string & nickname, int64_t uid) {
		m_playlists = playlists;
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS);
		::SendMessageW(list, LB_RESETCONTENT, 0, 0);
		for (const netease::PlaylistInfo & p : playlists) {
			const std::string text = p.name + "  (" + std::to_string(p.track_count) + ")";
			::SendMessageW(list, LB_ADDSTRING, 0,
				reinterpret_cast<LPARAM>(netease::to_wide(text).c_str()));
		}
		const std::string account = nickname.empty()
			? ("uid " + std::to_string(uid))
			: (nickname + "  (uid " + std::to_string(uid) + ")");
		SetDlgItemText(IDC_BROWSE_ACCOUNT, netease::to_wide(account).c_str());
		SetDlgItemText(IDC_BROWSE_STATUS,
			netease::to_wide("共 " + std::to_string(playlists.size()) + " 个歌单；点一下加载曲目").c_str());
		apply_pending_playlist();   // 面板「显示歌单」要求定位的那个歌单
	}

	// 往曲目列表插行。start 是这批曲目从第几首开始（翻页追加时接着往下排）。
	void insert_track_rows(HWND list, size_t start, const std::vector<netease::TrackInfo> & tracks) {
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		for (size_t i = 0; i < tracks.size(); ++i) {
			const netease::TrackInfo & t = tracks[i];
			const std::wstring index = std::to_wstring(start + i + 1);
			const std::wstring title = netease::to_wide(t.title);
			const std::wstring artists = netease::to_wide(t.artists);
			const std::wstring album = netease::to_wide(t.album);
			const std::wstring duration = netease::to_wide(format_duration(t.duration_ms));

			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(start + i);
			item.iSubItem = 0;
			item.pszText = const_cast<LPWSTR>(index.c_str());
			const int row = static_cast<int>(::SendMessageW(list, LVM_INSERTITEMW, 0,
				reinterpret_cast<LPARAM>(&item)));
			if (row < 0) continue;

			const std::wstring * cells[] = { &title, &artists, &album, &duration };
			for (int sub = 0; sub < 4; ++sub) {
				LVITEMW cell{};
				cell.mask = LVIF_TEXT;   // LVM_SETITEM 需要显式 mask（LVM_SETITEMTEXT 不需要，但那支不可靠）
				cell.iItem = row;
				cell.iSubItem = sub + 1;
				cell.pszText = const_cast<LPWSTR>(cells[sub]->c_str());
				::SendMessageW(list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&cell));
			}
		}
		// 清一次选中状态：ListView 在这条路径上会把新插入的条目都当成"选中"，
		// 自绘就会把整片刷成宿主选中色（截图里"所有歌都是选中色"就是这个）。
		for (size_t i = 0; i < tracks.size(); ++i) {
			LVITEMW clear{};
			clear.mask = LVIF_STATE;
			clear.iItem = static_cast<int>(start + i);
			clear.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
			clear.state = 0;
			::SendMessageW(list, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&clear));
		}
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		// 必须主动失效：WM_SETREDRAW(TRUE) 本身不触发重绘（见 panel_ui.cpp 里的同一处注释）。
		::InvalidateRect(list, nullptr, TRUE);
		::UpdateWindow(list);
	}

	void apply_tracks(const std::vector<netease::TrackInfo> & tracks, const std::string & summary) {
		m_tracks = tracks;
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
		insert_track_rows(list, 0, tracks);
		set_status(netease::to_wide(summary).c_str());

		// 诊断：区分"数据没拿到"和"界面没显示"这两种完全不同的故障。
		if (!tracks.empty()) {
			netease_log::write("foo_netease: 曲目列表已填充 " + std::to_string(tracks.size()) +
				" 行，首曲标题=「" + tracks[0].title + "」歌手=「" + tracks[0].artists + "」");
		} else {
			netease_log::write("foo_netease: 曲目列表为空（数据侧就没有曲目）");
		}
	}

	// 搜索面板「更多」：把下一页接到列表后面，不清掉已经显示的。
	void append_tracks(const std::vector<netease::TrackInfo> & tracks, const std::string & summary) {
		const size_t start = m_tracks.size();
		m_tracks.insert(m_tracks.end(), tracks.begin(), tracks.end());
		insert_track_rows(::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS), start, tracks);
		set_status(netease::to_wide(summary).c_str());
		netease_log::write("foo_netease: 曲目列表追加 " + std::to_string(tracks.size()) +
			" 行，现在共 " + std::to_string(m_tracks.size()) + " 行");
	}

	// ---- 事件 ----

	void OnRefresh(UINT, int, CWindow) {
		if (!netease::Session::instance().logged_in()) {
			set_status(netease::to_wide("尚未登录。").c_str());
			return;
		}
		// 单歌单 / 搜索面板：左侧歌单列表是藏着的，"刷新"要针对当前显示的内容。
		if (m_single) {
			if (!m_searchKeyword.empty()) {
				m_searchOffset = 0;
				set_status(netease::to_wide("正在重新搜索…").c_str());
				search_async(m_searchKeyword, 0, false);
			} else if (m_singlePlaylist > 0) {
				set_status(netease::to_wide("正在刷新歌单…").c_str());
				load_tracks_async(m_singlePlaylist);
			}
			return;
		}
		set_status(netease::to_wide("正在刷新歌单…").c_str());
		load_playlists_async();
	}

	void OnSearch(UINT, int, CWindow) {
		const int len = ::GetWindowTextLengthW(::GetDlgItem(m_hWnd, IDC_BROWSE_SEARCH));
		std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
		const int got = ::GetDlgItemTextW(m_hWnd, IDC_BROWSE_SEARCH, buffer.data(),
			static_cast<int>(buffer.size()));
		buffer.resize(got > 0 ? static_cast<size_t>(got) : 0);
		const std::string keyword = netease::to_utf8(buffer);
		if (keyword.empty()) {
			set_status(netease::to_wide("请输入搜索关键词。").c_str());
			return;
		}
		if (!netease::Session::instance().logged_in()) {
			set_status(netease::to_wide("搜索需要登录。").c_str());
			return;
		}
		m_searchKeyword = keyword;
		m_searchOffset = 0;
		set_status(netease::to_wide("正在搜索…").c_str());
		search_async(keyword, 0, false);
	}

	void OnPlaylistChanged(UINT, int, CWindow) {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS);
		const int sel = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
		if (sel < 0 || static_cast<size_t>(sel) >= m_playlists.size()) return;
		set_status(netease::to_wide("正在加载曲目…").c_str());
		load_tracks_async(m_playlists[sel].id);
	}

	// 当前选中的曲目；没有选中就返回全部（列表类界面的惯例）。
	std::vector<netease::TrackInfo> selected_tracks() const {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		std::vector<netease::TrackInfo> chosen;
		int sel = -1;
		for (;;) {
			sel = static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM,
				static_cast<WPARAM>(sel), LVNI_SELECTED));
			if (sel < 0) break;
			if (static_cast<size_t>(sel) < m_tracks.size()) chosen.push_back(m_tracks[sel]);
		}
		if (chosen.empty()) chosen = m_tracks;
		return chosen;
	}

	// 右键曲目列表：下一首播放（进播放队列）/ 添加 / 替换。
	void OnContextMenu(HWND wnd, CPoint pt) {
		if (wnd != ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS)) return;
		if (pt.x == -1 && pt.y == -1) {          // 键盘唤出（菜单键）
			CRect rc;
			if (::GetWindowRect(wnd, &rc)) { pt.x = rc.left + 60; pt.y = rc.top + 60; }
		}
		HMENU menu = ::CreatePopupMenu();
		::AppendMenuW(menu, MF_STRING, 1, netease::to_wide("下一首播放").c_str());
		::AppendMenuW(menu, MF_STRING, 2, netease::to_wide("添加到当前播放列表").c_str());
		::AppendMenuW(menu, MF_STRING, 3, netease::to_wide("替换当前播放列表").c_str());
		const int cmd = ::TrackPopupMenu(menu,
			TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, m_hWnd, nullptr);
		::DestroyMenu(menu);
		if (cmd < 1 || cmd > 3) return;

		const std::vector<netease::TrackInfo> chosen = selected_tracks();
		if (chosen.empty()) {
			set_status(netease::to_wide("没有选中曲目。").c_str());
			return;
		}
		if (cmd == 1) {
			const size_t n = netease_data::queue_tracks_next(chosen);
			SetDlgItemText(IDC_BROWSE_STATUS,
				netease::to_wide("已加入播放队列（下一首播放）：" + std::to_string(n) + " 首。").c_str());
		} else {
			do_insert(chosen, cmd == 3);
		}
	}

	void OnAdd(UINT, int, CWindow) { do_insert(false); }
	void OnReplace(UINT, int, CWindow) { do_insert(true); }

	// 曲目列表是 SysListView32：双击通知走 NM_DBLCLK（WM_NOTIFY），不是 LBN_DBLCLK。
	// 之前挂在 LBN_DBLCLK 上，所以双击一直没有反应。
	LRESULT OnTrackDoubleClick(int idCtrl, LPNMHDR hdr, BOOL & handled) {
		handled = TRUE;
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		if (idCtrl != IDC_BROWSE_TRACKS || !hdr || hdr->hwndFrom != list) return 0;

		int sel = -1;
		auto * ia = reinterpret_cast<LPNMITEMACTIVATE>(hdr);
		if (ia) sel = ia->iItem;
		if (sel < 0) {
			sel = static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM,
				static_cast<WPARAM>(-1), LVNI_SELECTED));
		}
		if (sel < 0 || static_cast<size_t>(sel) >= m_tracks.size()) return 0;

		// 双击 = 播放这一首：**追加**到当前播放列表末尾，再播放刚加进去的那条。
		// 双击只是"放一下"，不能替换 —— 把人家正在听的播放列表整片清掉是破坏性的。
		std::vector<netease::TrackInfo> one{ m_tracks[sel] };
		size_t added = 0;
		try {
			added = netease_data::insert_tracks(one, false);
		} catch (const std::exception & ex) {
			set_status(netease::to_wide(std::string("播放失败：") + ex.what()).c_str());
			return 0;
		}
		auto pm = playlist_manager::get();
		if (!pm.is_valid()) {
			set_status(netease::to_wide("拿不到播放列表管理器。").c_str());
			return 0;
		}
		const size_t total = pm->activeplaylist_get_item_count();
		if (added > 0 && total > 0) {
			const size_t target = total - 1;   // 刚追加进去的就是最后一条
			pm->activeplaylist_set_focus_item(target);
			pm->activeplaylist_execute_default_action(target);
			set_status(netease::to_wide("已追加到播放列表并播放：" + m_tracks[sel].title).c_str());
			netease_log::write("foo_netease: 双击播放（追加）「" + m_tracks[sel].title + "」");
		} else {
			set_status(netease::to_wide("没能加入播放列表。").c_str());
		}
		return 0;
	}

	void do_insert(bool replace) {
		if (m_tracks.empty()) {
			set_status(netease::to_wide("右侧没有曲目可添加。").c_str());
			return;
		}
		do_insert(selected_tracks(), replace);
	}

	void do_insert(const std::vector<netease::TrackInfo> & input, bool replace) {
		std::vector<netease::TrackInfo> chosen = input;
		if (chosen.empty()) chosen = m_tracks;

		size_t added = 0;
		std::string text;
		try {
			// 替换时把"这堆曲目来自哪个歌单"一起传下去：这个播放列表就等于那个歌单，
			// 在播放列表管理器里点中它会自动刷新。单歌单模式才有这个 id，列表浏览模式传 0。
			added = netease_data::insert_tracks(chosen, replace,
				replace ? m_singlePlaylist : 0);
			text = std::string(replace ? "已替换为 " : "已添加 ") +
				std::to_string(added) + " 首到当前播放列表";
		} catch (const std::exception & ex) {
			text = std::string("添加失败：") + ex.what();
		}
		set_status(netease::to_wide(text).c_str());
		netease_log::write("foo_netease: " + text);
	}

	static BrowseWindow * s_instance;
	static int64_t s_pending_id;        // open_for 传进来的歌单
	static bool s_pending_single;       // 是否"只显示这一个歌单"
	static bool s_pending_search;       // 是否"打开就搜索"

	std::shared_ptr<Liveness> m_alive;
	std::vector<netease::PlaylistInfo> m_playlists;
	std::vector<netease::TrackInfo> m_tracks;
	std::string m_searchKeyword;      // 搜索面板当前关键词（「更多」翻页用）
	int m_searchOffset = 0;           // 已经加载到第几首
	int64_t m_singlePlaylist = 0;     // 单歌单模式当前显示的歌单（「刷新」用）
	fb2k::CDarkModeHooks m_dark;
	HIMAGELIST m_rowImages = nullptr;
	netease_ui::FontCache m_font;
	netease_ui::ThemeColors m_theme;
	HBRUSH m_bgBrush = nullptr;
	HBRUSH m_selBrush = nullptr;
	HBRUSH m_hiBrush = nullptr;
	COLORREF m_bgColor = CLR_INVALID;
	int64_t m_pending_playlist = 0;   // 面板「显示歌单」要求定位的歌单
	bool m_single = false;            // 只显示一个歌单（无左侧列表）
	std::wstring m_statusText;        // 状态文字（自己画在窗口上）
	RECT m_statusRect{};              // 状态行位置，layout() 里算
	RECT m_scrollRect{};              // 自绘滚动条的位置
	bool m_dragThumb = false;         // 是否正在拖滚动条
	int m_dragOffset = 0;
	int m_rowH = 0;                   // 行高（滚动条换算用）
};

BrowseWindow * BrowseWindow::s_instance = nullptr;
int64_t BrowseWindow::s_pending_id = 0;
bool BrowseWindow::s_pending_single = false;
bool BrowseWindow::s_pending_search = false;

} // namespace

void show_browse_window(fb2k::hwnd_t parent) {
	BrowseWindow::open(parent);
}

void show_browse_window_for(int64_t playlist_id, fb2k::hwnd_t parent) {
	BrowseWindow::open_for(parent, playlist_id);
}

void show_browse_window_search(const std::string & keyword, fb2k::hwnd_t parent) {
	BrowseWindow::open_search(parent, keyword);
}

bool show_browse_window_search_more() {
	return BrowseWindow::more_search();
}

} // namespace netease_ui

