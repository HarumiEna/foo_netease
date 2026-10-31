#include "stdafx.h"

// Columns UI 面板支持（https://github.com/reupen/columns_ui-sdk）。
//
// 组件原本只有 Default UI 面板（ui_element）。这里再包一层 uie::window，
// 让 Columns UI 里也能把「网易云音乐」插进布局。
// 没装 Columns UI 时这段只是多注册了一个服务，不会被调用，不影响使用。
#include "../../sdk/columns_ui/ui_extension.h"
#include "../../sdk/columns_ui/window.h"

#include "panel_ui.h"

// CUI 的接口 GUID 必须和 Columns UI 自己用的一致，否则枚举不到这个面板。
// 值取自 columns_ui-sdk 的 ui_extension.cpp（32/64 位不同，两边都要对）。
#ifdef _WIN64
const GUID uie::window::class_guid =
	{ 0x7e537dfd, 0x436a, 0x452e, { 0x9f, 0x5d, 0x8b, 0x5f, 0x60, 0xbb, 0x53, 0x47 } };
#else
const GUID uie::window::class_guid =
	{ 0xf84c1030, 0x4407, 0x496c, { 0xb0, 0x9a, 0x14, 0xe5, 0xec, 0x5c, 0xa1, 0xc3 } };
#endif

namespace {

class cui_panel_window : public uie::window {
public:
	const GUID & get_extension_guid() const override {
		static const GUID g = { 0x8f21c4d0, 0x5b3e, 0x4a77, { 0x9c, 0x02, 0x6d, 0x18, 0xa7, 0x35, 0xbe, 0x41 } };
		return g;
	}

	void get_name(pfc::string_base & out) const override {
		out = "网易云音乐";
	}

	void get_category(pfc::string_base & out) const override {
		out = "Panels";
	}

	bool get_description(pfc::string_base & out) const override {
		out = "网易云音乐：歌单 / 每日推荐 / 漫游 / 最近播放 / 搜索，直接发到播放列表。";
		return true;
	}

	unsigned get_type() const override { return uie::type_panel; }

	bool is_available(const uie::window_host_ptr &) const override { return true; }

	HWND create_or_transfer_window(HWND parent, const uie::window_host_ptr & host,
		const ui_helpers::window_position_t & position) override {
		if (m_wnd) return m_wnd;
		m_host = host;
		m_wnd = netease_ui::create_panel_child(parent);
		if (m_wnd && position.cx > 0 && position.cy > 0) {
			::SetWindowPos(m_wnd, nullptr, position.x, position.y,
				static_cast<int>(position.cx), static_cast<int>(position.cy),
				SWP_NOZORDER | SWP_NOACTIVATE);
		}
		return m_wnd;
	}

	void destroy_window() override {
		if (m_wnd) {
			netease_ui::destroy_panel_child(m_wnd);
			m_wnd = nullptr;
		}
		m_host.release();
	}

	HWND get_wnd() const override { return m_wnd; }

private:
	HWND m_wnd = nullptr;
	uie::window_host_ptr m_host;
};

uie::window_factory<cui_panel_window> g_cui_panel_factory;

} // namespace

