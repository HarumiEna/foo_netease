#include "stdafx.h"

#include "browse_ui.h"
#include "component_log.h"

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

// ---------------------------------------------------------------------------
// 播放列表右键菜单：「下一首播放」。
//
// 用 SDK 的 contextmenu_item_simple，注册后 foobar2000 会把它放进播放列表/曲目的
// 右键菜单里（用户可在 参数设置 → 显示 → 快捷方式/右键菜单 里调整）。
// 行为：把选中的曲目插到"正在播放"那一条的后面；没有正在播放就插到最前面。
// ---------------------------------------------------------------------------
class netease_play_next : public contextmenu_item_simple {
public:
	unsigned get_num_items() override { return 1; }

	void get_item_name(unsigned, pfc::string_base & out) override {
		out = "下一首播放";
	}

	bool get_item_description(unsigned, pfc::string_base & out) override {
		out = "把选中的曲目插到正在播放的那一首后面";
		return true;
	}

	GUID get_item_guid(unsigned) override {
		static const GUID guid = { 0x7f1c25a4, 0x9d38, 0x4a61, { 0xb2, 0x0e, 0x51, 0x6d, 0x77, 0x9a, 0x3c, 0x18 } };
		return guid;
	}

	GUID get_parent() override { return contextmenu_groups::root; }   // 必须挂到根分组，否则不显示

	void context_command(unsigned, metadb_handle_list_cref data, const GUID &) override {
		if (data.get_count() == 0) return;
		auto pm = playlist_manager::get();
		if (!pm.is_valid()) return;
		// 走 foobar2000 自带的播放队列 —— 这才是"下一首播放"的标准做法：
		// 之前的做法是把曲目插到"正在播放"后面，但随机/乱序播放下根本轮不到它。
		for (t_size i = 0; i < data.get_count(); ++i) {
			pm->queue_add_item(data[i]);
		}
		netease_log::write("foo_netease: 右键「下一首播放」—— 已加入播放队列 " +
			std::to_string(data.get_count()) + " 首");
	}
};

static contextmenu_item_factory_t<netease_play_next> g_netease_play_next_factory;

} // namespace

