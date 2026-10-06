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

	static void open() {
		if (!s_instance) s_instance = new BrowseWindow();
		if (!s_instance->m_hWnd) {
			s_instance->m_alive = std::make_shared<Liveness>();
			s_instance->Create(nullptr);
		} else {
			s_instance->ShowWindow(SW_SHOW);
			::SetForegroundWindow(s_instance->m_hWnd);
		}
	}

	BEGIN_MSG_MAP_EX(BrowseWindow)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_SIZE(OnSize)
		MSG_WM_GETMINMAXINFO(OnGetMinMaxInfo)
		MSG_WM_CLOSE(OnClose)
		COMMAND_ID_HANDLER_EX(IDCANCEL, OnCancel)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_REFRESH, OnRefresh)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_SEARCH_BTN, OnSearch)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_ADD, OnAdd)
		COMMAND_ID_HANDLER_EX(IDC_BROWSE_REPLACE, OnReplace)
		COMMAND_HANDLER_EX(IDC_BROWSE_PLAYLISTS, LBN_SELCHANGE, OnPlaylistChanged)
		COMMAND_HANDLER_EX(IDC_BROWSE_TRACKS, LBN_DBLCLK, OnTrackDoubleClick)
	END_MSG_MAP()

private:
	BOOL OnInitDialog(CWindow, LPARAM) {
		SetWindowText(netease::to_wide("网易云音乐 · 歌单").c_str());
		SetDlgItemText(IDC_BROWSE_SEARCH_BTN, netease::to_wide("搜索").c_str());
		SetDlgItemText(IDC_BROWSE_REFRESH, netease::to_wide("刷新").c_str());
		SetDlgItemText(IDC_BROWSE_ADD, netease::to_wide("添加到当前播放列表").c_str());
		SetDlgItemText(IDC_BROWSE_REPLACE, netease::to_wide("替换当前播放列表").c_str());
		m_dark.AddDialogWithControls(*this);

		setup_track_columns();

		if (!netease::Session::instance().logged_in()) {
			SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide(
				"尚未登录。请到 参数设置 → 工具 → 网易云音乐 里扫码登录。").c_str());
			return TRUE;
		}
		SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("正在加载歌单…").c_str());
		load_playlists_async();
		return TRUE;
	}

	// 无模态对话框（Create 而不是 DoModal）必须自己处理关闭：
	// 这条路径上 DefDlgProc 不会销毁窗口，表现就是"打得开、关不掉"。
	void OnClose() {
		DestroyWindow();
	}

	void OnCancel(UINT, int, CWindow) {
		DestroyWindow();
	}

	void OnDestroy() {
		if (m_alive) m_alive->alive = false;
	}

	void OnSize(UINT, CSize size) {
		layout(size.cx, size.cy);
	}

	void OnGetMinMaxInfo(LPMINMAXINFO info) {
		info->ptMinTrackSize.x = 560;
		info->ptMinTrackSize.y = 320;
	}

	// ---- 界面搭建 ----

	void setup_track_columns() {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		::SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT,
			LVS_EX_FULLROWSELECT);
		struct Column { const wchar_t * text; int width; };
		const Column columns[] = {
			{ L"#", 40 }, { L"标题", 190 }, { L"歌手", 120 }, { L"专辑", 150 }, { L"时长", 55 },
		};
		for (int i = 0; i < 5; ++i) {
			LVCOLUMNW col{};
			col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
			col.cx = columns[i].width;
			col.iSubItem = i;
			col.pszText = const_cast<LPWSTR>(columns[i].text);
			::SendMessageW(list, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i),
				reinterpret_cast<LPARAM>(&col));
		}
	}

	void layout(int cx, int cy) {
		const int margin = 8;
		const int left_w = 208;
		const int bottom = 34;

		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS), nullptr, margin, 30,
			left_w, cy - 30 - margin * 2, SWP_NOZORDER);
		const int right_x = margin + left_w + 12;
		const int right_w = cx - right_x - margin;
		if (right_w < 120) return;

		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_SEARCH), nullptr, right_x, 10,
			right_w - 122, 22, SWP_NOZORDER);
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_SEARCH_BTN), nullptr, right_x + right_w - 116, 9, 54, 23, SWP_NOZORDER);
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_REFRESH), nullptr, right_x + right_w - 56, 9, 56, 23, SWP_NOZORDER);
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS), nullptr, right_x, 36,
			right_w, cy - 36 - bottom - margin, SWP_NOZORDER);
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_STATUS), nullptr, right_x, cy - bottom - 20,
			right_w, 16, SWP_NOZORDER);
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_ADD), nullptr, right_x, cy - 26, 104, 22, SWP_NOZORDER);
		::SetWindowPos(::GetDlgItem(m_hWnd, IDC_BROWSE_REPLACE), nullptr, right_x + 112, cy - 26, 116, 22, SWP_NOZORDER);
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
			netease::ApiCall detail = api.playlist_track_ids(playlist_id, ids, name, track_count);
			if (!detail.ok) {
				post_status(alive, "取歌单详情失败：" + detail.error);
				return;
			}

			// 关键：tracks 会被服务端截断（实测 1368 首只给 1000 条），
			// 所以这里用 trackIds 全集，再分批补元数据。
			std::vector<netease::TrackInfo> tracks;
			std::vector<int64_t> missing;
			netease::ApiCall songs = api.song_details(ids, tracks, &missing);
			if (!songs.ok) {
				post_status(alive, "取曲目详情失败：" + songs.error);
				return;
			}
			netease::MetaCache::instance().put_all(tracks);
			// 拿到就存：不依赖"干净退出"，强杀进程也不会丢缓存。
			netease_app::save_meta_cache();

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

	void search_async(std::string keyword) {
		auto alive = m_alive;
		fb2k::splitTask([alive, keyword] {
			netease::CookieJar jar;
			jar.deserialize(netease::Session::instance().cookie_header());
			netease::NeteaseApi api(jar);

			std::vector<netease::TrackInfo> tracks;
			netease::ApiCall call = api.search_songs(keyword, 100, 0, tracks);
			if (!call.ok) {
				post_status(alive, "搜索失败：" + call.error);
				return;
			}
			netease::MetaCache::instance().put_all(tracks);
			netease_app::save_meta_cache();

			const std::string summary = "搜索「" + keyword + "」：" + std::to_string(tracks.size()) + " 首";
			if (!alive->alive) return;
			fb2k::inMainThread([alive, tracks, summary] {
				if (!alive->alive || !s_instance || !s_instance->m_hWnd) return;
				s_instance->apply_tracks(tracks, summary);
			});
		});
	}

	static void post_status(const std::shared_ptr<Liveness> & alive, const std::string & text) {
		if (!alive->alive) return;
		fb2k::inMainThread([alive, text] {
			if (!alive->alive || !s_instance || !s_instance->m_hWnd) return;
			s_instance->SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide(text).c_str());
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
	}

	void apply_tracks(const std::vector<netease::TrackInfo> & tracks, const std::string & summary) {
		m_tracks = tracks;
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		::SendMessageW(list, LVM_DELETEALLITEMS, 0, 0);
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		for (size_t i = 0; i < tracks.size(); ++i) {
			const netease::TrackInfo & t = tracks[i];
			const std::wstring index = std::to_wstring(i + 1);
			const std::wstring title = netease::to_wide(t.title);
			const std::wstring artists = netease::to_wide(t.artists);
			const std::wstring album = netease::to_wide(t.album);
			const std::wstring duration = netease::to_wide(format_duration(t.duration_ms));

			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
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
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		// 必须主动失效：WM_SETREDRAW(TRUE) 本身不触发重绘（见 panel_ui.cpp 里的同一处注释）。
		::InvalidateRect(list, nullptr, TRUE);
		::UpdateWindow(list);
		SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide(summary).c_str());

		// 诊断：区分"数据没拿到"和"界面没显示"这两种完全不同的故障。
		if (!tracks.empty()) {
			netease_log::write("foo_netease: 曲目列表已填充 " + std::to_string(tracks.size()) +
				" 行，首曲标题=「" + tracks[0].title + "」歌手=「" + tracks[0].artists + "」");
		} else {
			netease_log::write("foo_netease: 曲目列表为空（数据侧就没有曲目）");
		}
	}

	// ---- 事件 ----

	void OnRefresh(UINT, int, CWindow) {
		if (!netease::Session::instance().logged_in()) {
			SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("尚未登录。").c_str());
			return;
		}
		SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("正在刷新歌单…").c_str());
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
			SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("请输入搜索关键词。").c_str());
			return;
		}
		if (!netease::Session::instance().logged_in()) {
			SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("搜索需要登录。").c_str());
			return;
		}
		SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("正在搜索…").c_str());
		search_async(keyword);
	}

	void OnPlaylistChanged(UINT, int, CWindow) {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_PLAYLISTS);
		const int sel = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
		if (sel < 0 || static_cast<size_t>(sel) >= m_playlists.size()) return;
		SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("正在加载曲目…").c_str());
		load_tracks_async(m_playlists[sel].id);
	}

	void OnAdd(UINT, int, CWindow) { do_insert(false); }
	void OnReplace(UINT, int, CWindow) { do_insert(true); }

	void OnTrackDoubleClick(UINT, int, CWindow) {
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		const int sel = static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1),
			LVNI_SELECTED));
		if (sel < 0 || static_cast<size_t>(sel) >= m_tracks.size()) return;

		std::vector<netease::TrackInfo> one{ m_tracks[sel] };
		netease_data::insert_tracks(one, true);
		auto pm = playlist_manager::get();
		if (pm->activeplaylist_get_item_count() > 0) {
			pm->activeplaylist_set_focus_item(0);
			pm->activeplaylist_execute_default_action(0);
		}
	}

	void do_insert(bool replace) {
		if (m_tracks.empty()) {
			SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide("右侧没有曲目可添加。").c_str());
			return;
		}
		// 有选中项就只加选中项，否则加全部——这是列表类界面的惯例。
		HWND list = ::GetDlgItem(m_hWnd, IDC_BROWSE_TRACKS);
		// 同上：LVM_GETNEXTITEM 的 wParam 要原样传上一次找到的下标。
		std::vector<netease::TrackInfo> chosen;
		int sel = -1;
		for (;;) {
			sel = static_cast<int>(::SendMessageW(list, LVM_GETNEXTITEM,
				static_cast<WPARAM>(sel), LVNI_SELECTED));
			if (sel < 0) break;
			if (static_cast<size_t>(sel) < m_tracks.size()) chosen.push_back(m_tracks[sel]);
		}
		if (chosen.empty()) chosen = m_tracks;

		size_t added = 0;
		std::string text;
		try {
			added = netease_data::insert_tracks(chosen, replace);
			text = std::string(replace ? "已替换为 " : "已添加 ") +
				std::to_string(added) + " 首到当前播放列表";
		} catch (const std::exception & ex) {
			text = std::string("添加失败：") + ex.what();
		}
		SetDlgItemText(IDC_BROWSE_STATUS, netease::to_wide(text).c_str());
		netease_log::write("foo_netease: " + text);
	}

	static BrowseWindow * s_instance;

	std::shared_ptr<Liveness> m_alive;
	std::vector<netease::PlaylistInfo> m_playlists;
	std::vector<netease::TrackInfo> m_tracks;
	fb2k::CDarkModeHooks m_dark;
};

BrowseWindow * BrowseWindow::s_instance = nullptr;

} // namespace

void show_browse_window() {
	BrowseWindow::open();
}

} // namespace netease_ui

