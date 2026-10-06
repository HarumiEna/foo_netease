#include "stdafx.h"
#include "lyric_ui.h"

#include <atomic>
#include <string>

#include <helpers/DarkMode.h>

#include "component_log.h"
#include "lyric_store.h"
#include "win_utf8.h"

// ---------------------------------------------------------------------------
// 歌词窗口 + %netease_lyric% 字段。
//
// 窗口用一个普通 CWindowImpl（不是对话框），所以 WM_CLOSE 走 DefWindowProc 正常关闭，
// 不会遇到无模态对话框"关不掉"的坑。
// ---------------------------------------------------------------------------

namespace {

std::wstring to_w(const std::string & utf8) { return netease::to_wide(utf8); }

std::atomic<int64_t> g_showing_id{ 0 };

class LyricWindow : public CWindowImpl<LyricWindow> {
public:
	DECLARE_WND_CLASS_EX(TEXT("{3E7A1C55-9B42-4F0D-8A61-2E5D7C904431}"), CS_VREDRAW | CS_HREDRAW, -1);

	BEGIN_MSG_MAP_EX(LyricWindow)
		MSG_WM_CREATE(OnCreate)
		MSG_WM_SIZE(OnSize)
		MSG_WM_CLOSE(OnClose)
		MSG_WM_DESTROY(OnDestroy)
	END_MSG_MAP()

	static LyricWindow * instance() {
		static LyricWindow w;
		return &w;
	}

	void show(int64_t id, const std::string & title) {
		const std::wstring caption = to_w("歌词 · " + title);
		if (!m_hWnd) {
			CRect rc(220, 150, 740, 770);
			Create(nullptr, rc, caption.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		} else {
			::SetWindowTextW(m_hWnd, caption.c_str());
			ShowWindow(SW_SHOW);
			::SetForegroundWindow(m_hWnd);
		}
		m_id = id;
		g_showing_id.store(id);

		std::string text;
		if (netease_lyric::get_cached(id, text)) {
			set_text(text);
			return;
		}
		set_text("正在获取歌词…");
		const HWND hwnd = m_hWnd;
		fb2k::splitTask([id, hwnd] {
			std::string body;
			if (!netease_lyric::fetch_now(id, body)) body = "（这首歌没有歌词）";
			fb2k::inMainThread([id, hwnd, body] {
				if (!::IsWindow(hwnd)) return;
				if (g_showing_id.load() != id) return;   // 用户已经切到别的歌
				::SetDlgItemTextW(hwnd, 1, to_w(body).c_str());
			});
		});
	}

private:
	int OnCreate(LPCREATESTRUCT) {
		m_edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
			0, 0, 100, 100, m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), nullptr, nullptr);
		if (m_edit) ::SendMessageW(m_edit, WM_SETFONT,
			reinterpret_cast<WPARAM>(::GetStockObject(DEFAULT_GUI_FONT)), TRUE);
		// 跟随 foobar2000 的明/暗主题（和面板用同一套助手）。
		m_dark.AddDialogWithControls(m_hWnd);
		return 0;
	}

	void OnSize(UINT, CSize size) {
		if (HWND edit = edit_handle()) ::SetWindowPos(edit, nullptr, 0, 0, size.cx, size.cy, SWP_NOZORDER);
	}

	void OnClose() { DestroyWindow(); }

	void OnDestroy() {
		m_edit = nullptr;
		g_showing_id.store(0);
	}

	// 每次都按 ID 取：暗色钩子可能替换控件，缓存 HWND 会写进已销毁的窗口。
	HWND edit_handle() const { return m_hWnd ? ::GetDlgItem(m_hWnd, 1) : nullptr; }

	void set_text(const std::string & text) {
		if (m_hWnd) ::SetDlgItemTextW(m_hWnd, 1, to_w(text).c_str());
	}

	HWND m_edit = nullptr;
	int64_t m_id = 0;
	fb2k::CDarkModeHooks m_dark;
};

// netease://song/<id>... 里取 id。
int64_t parse_song_id(const char * path) {
	const char prefix[] = "netease://song/";
	if (!path) return 0;
	if (std::strncmp(path, prefix, sizeof(prefix) - 1) != 0) return 0;
	const int64_t id = std::strtoll(path + sizeof(prefix) - 1, nullptr, 10);
	return id > 0 ? id : 0;
}

// 暴露给 titleformat 的 %netease_lyric%。
// 这里只读我们自己的缓存（几微秒），不做任何网络/foobar2000 API 调用。
class netease_lyric_field : public metadb_display_field_provider {
public:
	// 两个名字：netease_lyric 是本组件专有字段；lyric 是很多歌词面板认的通用约定。
	t_uint32 get_field_count() override { return 2; }
	void get_field_name(t_uint32 index, pfc::string_base & out) override {
		if (index == 0) out = "netease_lyric";
		else if (index == 1) out = "lyric";
	}
	bool process_field(t_uint32 index, metadb_handle * handle, titleformat_text_out * out) override {
		if (index > 1 || !handle || !out) return false;
		const int64_t id = parse_song_id(handle->get_path());
		if (id <= 0) return false;
		std::string text;
		if (!netease_lyric::get_cached(id, text) || text.empty()) return false;
		out->write(titleformat_inputtypes::meta, text.c_str());
		return true;
	}
};

service_factory_single_t<netease_lyric_field> g_netease_lyric_field;

} // namespace

namespace netease_lyric_ui {

void show_for(int64_t id, const std::string & title) {
	if (id <= 0) {
		netease_log::write("foo_netease: 没有选中曲目，无法显示歌词");
		return;
	}
	LyricWindow::instance()->show(id, title);
}

} // namespace netease_lyric_ui

