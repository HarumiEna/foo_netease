#include "stdafx.h"
#include "lyric_ui.h"

#include <atomic>
#include <string>

#include <helpers/DarkMode.h>

#include "component_log.h"
#include "theme.h"
#include "ui_scale.h"
#include "lyric_store.h"
#include "win_utf8.h"

// 歌词窗口 + %netease_lyric% 字段。
//
// 窗口用一个普通 CWindowImpl（不是对话框），所以 WM_CLOSE 走 DefWindowProc 正常关闭，
// 不会遇到无模态对话框"关不掉"的坑。

namespace {

std::wstring to_w(const std::string & utf8) { return netease::to_wide(utf8); }

std::atomic<int64_t> g_showing_id{ 0 };

// 一行歌词里的一个"词"（逐字高亮用）。end_ms 是这个词唱完的时间。
struct LyricWord {
	int64_t end_ms = 0;
	std::wstring text;
};

// 一行歌词。
struct LyricLine {
	int64_t start_ms = 0;
	std::wstring text;
	std::vector<LyricWord> words;   // 空 = 没有逐字，整行高亮
};

class LyricWindow : public CWindowImpl<LyricWindow> {
public:
	DECLARE_WND_CLASS_EX(TEXT("{3E7A1C55-9B42-4F0D-8A61-2E5D7C904431}"), CS_VREDRAW | CS_HREDRAW, -1);

	BEGIN_MSG_MAP_EX(LyricWindow)
		MSG_WM_CREATE(OnCreate)
		MSG_WM_SIZE(OnSize)
		MSG_WM_CLOSE(OnClose)
		MSG_WM_DESTROY(OnDestroy)
		MSG_WM_ERASEBKGND(OnEraseBkgnd)
		MESSAGE_HANDLER(WM_PAINT, OnPaintMsg)
		MSG_WM_TIMER(OnTimer)
		MSG_WM_MOUSEWHEEL(OnMouseWheel)
		MSG_WM_LBUTTONDOWN(OnLButtonDown)
		MESSAGE_HANDLER(netease_ui::kMsgThemeChanged, OnThemeChanged)
	END_MSG_MAP()

	static LyricWindow * instance() {
		static LyricWindow w;
		return &w;
	}

	void show(int64_t id, const std::string & title) {
		const std::wstring caption = to_w("歌词 · " + title);
		if (!m_hWnd) {
			CRect rc(220, 150, 760, 780);
			Create(nullptr, rc, caption.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		} else {
			::SetWindowTextW(m_hWnd, caption.c_str());
			ShowWindow(SW_SHOW);
			::SetForegroundWindow(m_hWnd);
		}
		m_id = id;
		m_title = to_w(title);
		g_showing_id.store(id);

		std::string plain, enhanced;
		const bool have_plain = netease_lyric::get_cached(id, plain);
		const bool have_enh = netease_lyric::get_cached_enhanced(id, enhanced);
		if (have_plain || have_enh) {
			set_lyrics(have_enh ? enhanced : plain, have_enh);
			return;
		}
		set_status(L"正在获取歌词…");
		const HWND hwnd = m_hWnd;
		fb2k::splitTask([id, hwnd] {
			std::string body;
			const bool ok = netease_lyric::fetch_now(id, body);
			std::string enh;
			const bool has_enh = netease_lyric::get_cached_enhanced(id, enh);
			fb2k::inMainThread([id, hwnd, body, ok, enh, has_enh] {
				if (!::IsWindow(hwnd)) return;
				if (g_showing_id.load() != id) return;   // 用户已经切到别的歌
				if (has_enh) LyricWindow::instance()->set_lyrics(enh, true);
				else if (ok) LyricWindow::instance()->set_lyrics(body, false);
				else LyricWindow::instance()->set_status(L"（这首歌没有歌词）");
			});
		});
	}

private:
	static const UINT_PTR kTimerTick = 1;

	// ---- 主题 ----
	void refresh_theme() {
		m_theme = netease_ui::theme_colors();
		rebuild_brush();
	}

	void rebuild_brush() {
		if (m_bgBrush) { ::DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
		m_bgBrush = ::CreateSolidBrush(m_theme.background);
	}

	BOOL OnEraseBkgnd(CDCHandle dc) {
		CRect rc;
		if (!GetClientRect(&rc)) return TRUE;
		if (m_bgBrush) ::FillRect(dc, &rc, m_bgBrush);
		return TRUE;
	}

	LRESULT OnThemeChanged(UINT, WPARAM, LPARAM, BOOL &) {
		refresh_theme();
		::RedrawWindow(m_hWnd, nullptr, nullptr,
			RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		return 0;
	}

	int OnCreate(LPCREATESTRUCT) {
		refresh_theme();
		m_dark.AddDialog(m_hWnd, m_theme.background);
		m_line_h = netease_ui::scale(m_hWnd, 34);
		SetTimer(kTimerTick, 100, nullptr);   // 逐字进度刷新
		return 0;
	}

	void OnSize(UINT, CSize) {
		m_line_h = netease_ui::scale(m_hWnd, 34);
		Invalidate(FALSE);
	}

	void OnClose() { DestroyWindow(); }

	void OnDestroy() {
		KillTimer(kTimerTick);
		if (m_bgBrush) { ::DeleteObject(m_bgBrush); m_bgBrush = nullptr; }
		g_showing_id.store(0);
	}

	// ---- 歌词解析 ----
	// [mm:ss.xx] / [mm:ss] / [m:ss.xx] -> 毫秒；失败返回 -1。
	static int64_t parse_time(const std::wstring & s, size_t begin, size_t * out_end) {
		size_t i = begin;
		while (i < s.size() && (s[i] < L'0' || s[i] > L'9')) ++i;   // 跳过 [
		const size_t ns = i;
		while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') ++i;
		if (i == ns) return -1;
		int64_t minutes = 0;
		for (size_t k = ns; k < i; ++k) minutes = minutes * 10 + (s[k] - L'0');
		if (i >= s.size() || s[i] != L':') return -1;
		++i;
		const size_t ss = i;
		while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') ++i;
		if (i == ss) return -1;
		int64_t seconds = 0;
		for (size_t k = ss; k < i; ++k) seconds = seconds * 10 + (s[k] - L'0');
		int64_t hundredths = 0;
		if (i < s.size() && (s[i] == L'.' || s[i] == L':')) {
			++i;
			int digits = 0;
			while (i < s.size() && s[i] >= L'0' && s[i] <= L'9' && digits < 2) {
				hundredths = hundredths * 10 + (s[i] - L'0');
				++i;
				++digits;
			}
			if (digits == 1) hundredths *= 10;
		}
		while (i < s.size() && s[i] != L']' && s[i] != L'>') ++i;
		if (out_end) *out_end = i;
		return (minutes * 60 + seconds) * 1000 + hundredths * 10;
	}

	// 解析增强型（A2）LRC： [mm:ss.xx]词<mm:ss.xx>词<...>
	static void parse_enhanced(const std::wstring & text, std::vector<LyricLine> & out) {
		size_t pos = 0;
		while (pos <= text.size()) {
			size_t eol = text.find(L'\n', pos);
			if (eol == std::wstring::npos) eol = text.size();
			std::wstring line = text.substr(pos, eol - pos);
			if (!line.empty() && line.back() == L'\r') line.pop_back();
			pos = eol + 1;
			size_t p = 0;
			int64_t line_time = -1;
			// 行首可能有多个 [..]，取第一个能解析成时间的
			while (p < line.size() && line[p] == L'[') {
				size_t close = 0;
				const int64_t ms = parse_time(line, p, &close);
				if (ms >= 0 && line_time < 0) line_time = ms;
				if (close >= line.size() || line[close] != L']') break;
				p = close + 1;
			}
			if (line_time < 0) continue;
			std::wstring rest = line.substr(p);
			LyricLine out_line;
			out_line.start_ms = line_time;
			// 增强型（A2）： <词开始>词<词开始>词…
			std::wstring plain_text;
			int64_t pending_start = -1;
			size_t q = 0;
			bool any = false;
			while (q < rest.size()) {
				if (rest[q] == L'<') {
					size_t close = 0;
					const int64_t ms = parse_time(rest, q, &close);
					pending_start = ms >= 0 ? ms : pending_start;
					if (close >= rest.size()) break;
					q = close + 1;
					continue;
				}
				const size_t lt = rest.find(L'<', q);
				const size_t stop = (lt == std::wstring::npos) ? rest.size() : lt;
				LyricWord w;
				w.text = rest.substr(q, stop - q);
				w.end_ms = pending_start;      // 先存"开始"，下面再换算成结束
				if (!w.text.empty()) {
					out_line.words.push_back(w);
					plain_text += w.text;
					any = true;
				}
				pending_start = -1;
				q = stop;
			}
			if (!any) continue;                                   // 这行没内容
			// 每个词的结束 = 下一个词的开始；最后一个词用下一行开始（稍后统一补）。
			for (size_t k = 0; k + 1 < out_line.words.size(); ++k) {
				out_line.words[k].end_ms = out_line.words[k + 1].end_ms;
			}
			if (!out_line.words.empty() && out_line.words.back().end_ms < 0) {
				out_line.words.back().end_ms = 0;
			}
			out_line.text = plain_text;
			out.push_back(std::move(out_line));
			if (eol >= text.size()) break;
		}
	}

	// 解析普通 LRC（没有逐字）
	static void parse_plain(const std::wstring & text, std::vector<LyricLine> & out) {
		size_t pos = 0;
		while (pos <= text.size()) {
			size_t eol = text.find(L'\n', pos);
			if (eol == std::wstring::npos) eol = text.size();
			std::wstring line = text.substr(pos, eol - pos);
			if (!line.empty() && line.back() == L'\r') line.pop_back();
			pos = eol + 1;
			size_t p = 0;
			int64_t line_time = -1;
			while (p < line.size() && line[p] == L'[') {
				size_t close = 0;
				const int64_t ms = parse_time(line, p, &close);
				if (ms >= 0 && line_time < 0) line_time = ms;
				if (close >= line.size() || line[close] != L']') break;
				p = close + 1;
			}
			if (line_time < 0) continue;
			std::wstring rest = line.substr(p);
			while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'\t')) rest.pop_back();
			if (rest.empty()) continue;
			LyricLine out_line;
			out_line.start_ms = line_time;
			out_line.text = rest;
			out.push_back(std::move(out_line));
			if (eol >= text.size()) break;
		}
	}

	void set_lyrics(const std::string & text, bool enhanced) {
		const std::wstring wide = to_w(text);
		std::vector<LyricLine> lines;
		if (enhanced) parse_enhanced(wide, lines);
		if (lines.empty()) parse_plain(wide, lines);
		std::stable_sort(lines.begin(), lines.end(),
			[](const LyricLine & a, const LyricLine & b) { return a.start_ms < b.start_ms; });
		// 每行最后一个词没有"下一个词"可参照：用下一行的开始时间当结束。
		for (size_t i = 0; i < lines.size(); ++i) {
			if (lines[i].words.empty()) continue;
			LyricWord & last = lines[i].words.back();
			if (last.end_ms > 0) continue;
			last.end_ms = (i + 1 < lines.size()) && lines[i + 1].start_ms > lines[i].start_ms
				? lines[i + 1].start_ms
				: lines[i].start_ms + 800;
		}
		m_lines = std::move(lines);
		m_current = -1;
		m_scroll = 0;
		m_status.clear();
		update_from_playback();
		Invalidate(FALSE);
	}

	void set_status(const std::wstring & text) {
		m_lines.clear();
		m_status = text;
		m_current = -1;
		Invalidate(FALSE);
	}

	// ---- 播放进度 ----
	void update_from_playback() {
		if (m_lines.empty()) return;
		auto pc = playback_control::get();
		if (!pc.is_valid()) return;
		const double position = pc->playback_get_position();
		const int64_t ms = static_cast<int64_t>(position * 1000.0);
		int current = -1;
		for (size_t i = 0; i < m_lines.size(); ++i) {
			if (m_lines[i].start_ms <= ms) current = static_cast<int>(i);
			else break;
		}
		if (current != m_current) {
			m_current = current;
			CRect rc;
			if (GetClientRect(&rc) && m_line_h > 0) {
				const int want = (m_current - (rc.Height() / m_line_h) / 2) * m_line_h;
				m_scroll = want < 0 ? 0 : want;
			}
		}
	}

	void OnTimer(UINT_PTR id) {
		if (id != kTimerTick) return;
		update_from_playback();
		Invalidate(FALSE);   // 逐字进度要持续重画
	}

	BOOL OnMouseWheel(UINT, short delta, CPoint) {
		const int step = m_line_h > 0 ? m_line_h : 20;
		int scroll = m_scroll - (delta / WHEEL_DELTA) * step * 3;
		if (scroll < 0) scroll = 0;
		m_manual_scroll = true;
		m_scroll = scroll;
		Invalidate(FALSE);
		return TRUE;
	}

	void OnLButtonDown(UINT, CPoint pt) {
		if (m_lines.empty() || m_line_h <= 0) return;
		const int index = m_scroll / m_line_h + pt.y / m_line_h;
		if (index < 0 || index >= static_cast<int>(m_lines.size())) return;
		auto pc = playback_control::get();
		if (pc.is_valid()) {
			pc->playback_seek(static_cast<double>(m_lines[index].start_ms) / 1000.0);
			m_manual_scroll = false;
			m_current = index;
			Invalidate(FALSE);
		}
	}

	LRESULT OnPaintMsg(UINT, WPARAM, LPARAM, BOOL & handled) {
		PAINTSTRUCT ps{};
		HDC dc = ::BeginPaint(m_hWnd, &ps);
		CRect rc;
		GetClientRect(&rc);
		if (m_bgBrush) ::FillRect(dc, &rc, m_bgBrush);
		::SetBkMode(dc, TRANSPARENT);
		::SelectObject(dc, m_font.get(m_hWnd));

		if (m_lines.empty()) {
			::SetTextColor(dc, m_theme.text);
			RECT r = rc;
			::DrawTextW(dc, m_status.c_str(), -1, &r,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			::EndPaint(m_hWnd, &ps);
			handled = TRUE;
			return 0;
		}

		auto pc = playback_control::get();
		const int64_t ms = pc.is_valid()
			? static_cast<int64_t>(pc->playback_get_position() * 1000.0) : 0;
		const int left = netease_ui::scale(m_hWnd, 18);
		const int width = rc.Width() - left * 2;
		int y = -m_scroll;
		for (size_t i = 0; i < m_lines.size(); ++i) {
			const int top = y;
			y += m_line_h;
			if (y < 0 || top > rc.Height()) continue;
			const bool active = (static_cast<int>(i) == m_current);
			draw_line(dc, m_lines[i], left, top, width, active, ms);
		}
		::EndPaint(m_hWnd, &ps);
		handled = TRUE;
		return 0;
	}

	// 当前字体的行高（FontCache 只给 HFONT，度量自己取）。
	int text_height(HDC dc) {
		if (HFONT font = m_font.get(m_hWnd)) ::SelectObject(dc, font);
		TEXTMETRICW tm{};
		::GetTextMetricsW(dc, &tm);
		return tm.tmHeight;
	}

	// 逐字着色：唱过的字用高亮色，没唱的用普通文字色。
	void draw_line(HDC dc, const LyricLine & line, int x, int top, int width,
		bool active, int64_t ms) {
		const int text_h = text_height(dc);
		const int y = top + (m_line_h - text_h) / 2;
		if (!active) {
			::SetTextColor(dc, m_theme.text);
			RECT r{ x, y, x + width, y + text_h };
			::DrawTextW(dc, line.text.c_str(), -1, &r,
				DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			return;
		}
		if (line.words.empty()) {
			::SetTextColor(dc, m_theme.highlight);
			RECT r{ x, y, x + width, y + text_h };
			::DrawTextW(dc, line.text.c_str(), -1, &r,
				DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
			return;
		}
		int cx = x;
		for (const LyricWord & w : line.words) {
			SIZE size{};
			::GetTextExtentPoint32W(dc, w.text.c_str(), static_cast<int>(w.text.size()), &size);
			if (cx + size.cx > x + width) break;
			::SetTextColor(dc, (w.end_ms > 0 && ms >= w.end_ms) ? m_theme.highlight : m_theme.text);
			::TextOutW(dc, cx, y, w.text.c_str(), static_cast<int>(w.text.size()));
			cx += size.cx;
		}
	}

	std::vector<LyricLine> m_lines;
	std::wstring m_status;
	std::wstring m_title;
	int m_current = -1;
	int m_scroll = 0;
	int m_line_h = 0;
	bool m_manual_scroll = false;
	int64_t m_id = 0;
	fb2k::CDarkModeHooks m_dark;
	netease_ui::FontCache m_font;
	netease_ui::ThemeColors m_theme;
	HBRUSH m_bgBrush = nullptr;
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
	// 四个字段：
	//   netease_lyric / lyric        —— 普通 LRC（兼容所有歌词面板）
	//   netease_lyric_enhanced       —— **增强型 LRC**（逐字，词尾用 <mm:ss.xx> 标注）
	//   netease_yrc                  —— 网易原始逐字文本（原样，给需要的人）
	t_uint32 get_field_count() override { return 4; }
	void get_field_name(t_uint32 index, pfc::string_base & out) override {
		switch (index) {
		case 0: out = "netease_lyric"; break;
		case 1: out = "lyric"; break;
		case 2: out = "netease_lyric_enhanced"; break;
		default: out = "netease_yrc"; break;
		}
	}
	bool process_field(t_uint32 index, metadb_handle * handle, titleformat_text_out * out) override {
		if (index > 3 || !handle || !out) return false;
		const int64_t id = parse_song_id(handle->get_path());
		if (id <= 0) return false;
		std::string text;
		if (index == 2) {
			if (!netease_lyric::get_cached_enhanced(id, text) || text.empty()) return false;
		} else if (index == 3) {
			if (!netease_lyric::get_cached_raw_yrc(id, text) || text.empty()) return false;
		} else {
			if (!netease_lyric::get_cached(id, text) || text.empty()) return false;
		}
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

