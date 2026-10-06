#include "stdafx.h"

#include "browse_ui.h"

// 主菜单入口：工具 → 网易云音乐 → 浏览歌单…
// 依据：menu.h 的 mainmenu_group_popup_factory + mainmenu_commands_factory_t
// （sdk/foobar2000/SDK/menu.h:154-163），写法照 foo_sample/mainmenu.cpp。

namespace {

const GUID guid_menu_group = { 0x5bd067a3, 0x2f59, 0x4ed1, { 0xa1, 0x78, 0x64, 0x6c, 0xd2, 0xa2, 0x82, 0x3f } };
const GUID guid_cmd_browse = { 0x2f98856d, 0x6005, 0x4ca8, { 0x8a, 0x34, 0x9e, 0x17, 0x12, 0xb9, 0x56, 0x0a } };

// 注意：SDK 的 mainmenu_groups 里并没有 "tools" 这一项（主菜单只有
// 文件/编辑/视图/播放/媒体库/帮助），所以挂到「视图」下面。
// 参数设置里的「工具」分类是另一回事，由 preferences_page 的 parent guid 决定。
static mainmenu_group_popup_factory g_menu_group(guid_menu_group, mainmenu_groups::view,
	mainmenu_commands::sort_priority_dontcare, "网易云音乐");

class netease_menu_commands : public mainmenu_commands {
public:
	t_uint32 get_command_count() override { return 1; }

	GUID get_command(t_uint32 p_index) override {
		(void)p_index;
		return guid_cmd_browse;
	}

	void get_name(t_uint32 p_index, pfc::string_base & p_out) override {
		(void)p_index;
		p_out = "浏览歌单…";
	}

	bool get_description(t_uint32 p_index, pfc::string_base & p_out) override {
		(void)p_index;
		p_out = "浏览网易云音乐账号里的歌单，并把曲目加入播放列表";
		return true;
	}

	bool get_display(t_uint32 p_index, pfc::string_base & p_text, t_uint32 & p_flags) override {
		(void)p_index;
		p_text = "浏览歌单…";
		p_flags = 0;
		return true;
	}

	void execute(t_uint32 p_index, ctx_t p_callback) override {
		(void)p_index;
		(void)p_callback;
		netease_ui::show_browse_window();
	}

	GUID get_parent() override { return guid_menu_group; }
};

static mainmenu_commands_factory_t<netease_menu_commands> g_netease_menu_commands_factory;

} // namespace

