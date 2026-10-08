#include "stdafx.h"

#include <cstdlib>
#include <cstring>

#include "browse_ui.h"
#include "component_log.h"
#include "netease_data.h"
#include "session.h"

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

// ---------------------------------------------------------------------------
// 播放列表右键菜单：「切换音质 ▸」。
//
// 只列**当前账号真的能播**的档位：菜单展开时按选中曲目查一次
// /weapi/song/enhance/privilege（netease_data::available_levels，按 id 缓存），
// 拿 playMaxBrLevel，再沿档位链从低到高铺开。一个都没有就整项不显示。
// 多选取交集 —— 只有所有选中曲目都支持的档位才会出现。
//
// 这里刻意用基类 contextmenu_item_v2 而不是 contextmenu_item_simple：
// 只有基类的 instantiate_item(index, data, caller) 能按选中内容**动态生成子项**；
// _simple 的 get_num_items() 拿不到选中内容，没法把不支持的档位藏起来。
// ---------------------------------------------------------------------------

const char * kSongPathPrefix = "netease://song/";

// netease://song/<id>?level=<档位>&fm=1&no=<序号> —— 只要 id 和查询串。
bool split_song_path(const char * path, int64_t & id, std::string & query) {
	id = 0;
	query.clear();
	if (!path) return false;
	const size_t plen = std::strlen(kSongPathPrefix);
	if (std::strncmp(path, kSongPathPrefix, plen) != 0) return false;
	const char * rest = path + plen;
	const char * q = std::strchr(rest, '?');
	const size_t id_len = q ? static_cast<size_t>(q - rest) : std::strlen(rest);
	if (id_len == 0) return false;
	id = std::strtoll(std::string(rest, id_len).c_str(), nullptr, 10);
	if (id <= 0) return false;
	if (q) query.assign(q + 1);
	return true;
}

// 换掉 level，其余键值（fm / no …）原样保留 —— no 丢了列表里就变成问号。
std::string path_with_level(int64_t id, const std::string & query, const std::string & level) {
	std::string out = std::string(kSongPathPrefix) + std::to_string(id) + "?level=" + level;
	size_t start = 0;
	while (start < query.size()) {
		const size_t amp = query.find('&', start);
		const std::string kv = query.substr(start,
			amp == std::string::npos ? std::string::npos : amp - start);
		if (!kv.empty() && kv.compare(0, 6, "level=") != 0) out += "&" + kv;
		if (amp == std::string::npos) break;
		start = amp + 1;
	}
	return out;
}

// 取查询串里的 level= 值。
std::string query_level(const std::string & query) {
	const size_t at = query.find("level=");
	if (at == std::string::npos) return std::string();
	const size_t begin = at + 6;
	const size_t end = query.find('&', begin);
	return query.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

int level_index(const std::string & level) {
	for (size_t i = 0; i < netease::kQualityOptionCount; ++i) {
		if (level == netease::kQualityOptions[i].level) return static_cast<int>(i);
	}
	return -1;
}

// 选中曲目**共同**支持的档位（低 → 高）；空表示这一项不该出现在菜单里。
std::vector<std::string> shared_levels(metadb_handle_list_cref data) {
	int top = -1;
	bool any = false;
	for (t_size i = 0; i < data.get_count(); ++i) {
		int64_t id = 0;
		std::string query;
		if (!split_song_path(data[i]->get_path(), id, query)) continue;   // 不是网易云的曲目
		const std::vector<std::string> avail = netease_data::available_levels(id);
		if (avail.empty()) return {};   // 有一首查不到就整项不显示 —— 宁可不给，也不给错的
		const int one = level_index(avail.back());
		if (one < 0) return {};
		if (top < 0 || one < top) top = one;
		any = true;
	}
	if (!any || top < 0) return {};
	std::vector<std::string> out;
	for (int i = 0; i <= top; ++i) out.push_back(netease::kQualityOptions[i].level);
	return out;
}

// 选中曲目当前一致的档位；不一致返回空（那种情况不打勾）。
std::string common_level(metadb_handle_list_cref data) {
	std::string cur;
	bool first = true;
	for (t_size i = 0; i < data.get_count(); ++i) {
		int64_t id = 0;
		std::string query;
		if (!split_song_path(data[i]->get_path(), id, query)) continue;
		const std::string one = query_level(query);
		if (first) { cur = one; first = false; }
		else if (cur != one) return std::string();
	}
	return cur;
}

// 把选中条目换成 level：**原地替换**（playlist_replace_item），位置和顺序都不变。
void apply_level(metadb_handle_list_cref data, const std::string & level) {
	auto pm = playlist_manager::get();
	if (!pm.is_valid()) return;
	const t_size active = pm->get_active_playlist();
	if (active == pfc::infinite_size) return;

	// 换档位 = 换直链，正在播的那条必须重新打开。先把"播到哪儿了"记下来。
	auto pc = playback_control::get();
	metadb_handle_ptr now;
	if (pc.is_valid()) pc->get_now_playing(now);
	const bool was_playing = pc.is_valid() && pc->is_playing();
	const double resume_at = was_playing ? pc->playback_get_position() : 0.0;

	const t_size count = pm->playlist_get_item_count(active);
	bit_array_bittable changed_mask(count);
	metadb_handle_list reload;
	t_size replay_at = pfc::infinite_size;
	std::string replay_path;
	size_t changed = 0;
	for (t_size i = 0; i < count; ++i) {
		metadb_handle_ptr h = pm->playlist_get_item_handle(active, i);
		bool selected = false;
		for (t_size j = 0; j < data.get_count(); ++j) {
			if (data[j] == h) { selected = true; break; }
		}
		if (!selected) continue;
		int64_t id = 0;
		std::string query;
		if (!split_song_path(h->get_path(), id, query)) continue;
		if (query_level(query) == level) continue;   // 已经是这一档，不用动
		const std::string path = path_with_level(id, query, level);
		// 用 replace_path 系列建新句柄：它沿用原句柄的 subsong，路径之外的
		// 身份信息不会丢。别用 handle_create(path,0) 从零造一个。
		metadb_handle_ptr nh;
		metadb::get()->handle_create_replace_path_canonical(nh, h, path.c_str());
		if (!nh.is_valid()) continue;
		if (pm->playlist_replace_item(active, i, nh)) {
			changed_mask.set(i, true);
			reload.add_item(nh);
			++changed;
			if (now.is_valid() && h == now) { replay_at = i; replay_path = path; }
		}
	}
	if (changed) {
		// 换掉句柄后光标/选中会留在旧句柄上，按新句柄重新选一遍（只动我们改过的条目）。
		pm->playlist_set_selection(active, changed_mask, changed_mask);
		// 新句柄的元数据**框架不会自己去读**（插入曲目时也是我们显式读的）。
		// 不读的话列表里标题会退化成路径里的那串数字、其余列全是问号。
		auto io = metadb_io_v2::get();
		if (io.is_valid()) {
			io->load_info_async(reload, metadb_io::load_info_force, nullptr,
				metadb_io_v2::op_flag_silent | metadb_io_v2::op_flag_background, nullptr);
		}
	}
	// 正在播的那条换完要接着播；进度交给播放回调，等这条真的起来了再跳回去。
	if (replay_at != pfc::infinite_size) {
		if (was_playing && resume_at > 0.5) {
			netease_data::set_resume_position(replay_path, resume_at);
		}
		pm->playlist_set_focus_item(active, replay_at);
		pm->activeplaylist_execute_default_action(replay_at);
	}
	netease_log::write("foo_netease: 右键「切换音质」→ " + level +
		"，改写 " + std::to_string(changed) + " 条" +
		(replay_at != pfc::infinite_size ? "（正在播放的那条已重开）" : ""));
}

// 单个档位的叶子项。
class quality_leaf_node : public contextmenu_item_node_leaf {
public:
	quality_leaf_node(std::string level, std::string label, bool checked)
		: m_level(std::move(level)), m_label(std::move(label)), m_checked(checked) {}

	bool get_display_data(pfc::string_base & out, unsigned & flags,
		metadb_handle_list_cref, const GUID &) override {
		out = m_label.c_str();
		flags = m_checked
			? (contextmenu_item_simple::FLAG_RADIOCHECKED | contextmenu_item_simple::FLAG_CHECKED)
			: 0u;
		return true;
	}

	void execute(metadb_handle_list_cref data, const GUID &) override {
		apply_level(data, m_level);
	}

	bool get_description(pfc::string_base & out) override {
		out = ("换成「" + m_label + "」播放选中的曲目").c_str();
		return true;
	}

	GUID get_guid() override { return pfc::guid_null; }
	bool is_mappable_shortcut() override { return false; }

private:
	std::string m_level;
	std::string m_label;
	bool m_checked = false;
};

// 「切换音质」这个弹出项本身；子项按选中曲目现算。
class quality_popup_node : public contextmenu_item_node_root_popup {
public:
	explicit quality_popup_node(metadb_handle_list_cref data) {
		m_levels = shared_levels(data);
		if (m_levels.empty()) return;
		const std::string cur = common_level(data);
		m_leaves.reserve(m_levels.size());
		for (const std::string & lv : m_levels) {
			std::string label;
			for (size_t i = 0; i < netease::kQualityOptionCount; ++i) {
				if (lv == netease::kQualityOptions[i].level) { label = netease::kQualityOptions[i].label; break; }
			}
			m_leaves.emplace_back(lv, label, !cur.empty() && cur == lv);
		}
	}

	t_size get_children_count() override { return m_leaves.size(); }

	contextmenu_item_node * get_child(t_size index) override {
		return index < m_leaves.size() ? &m_leaves[index] : nullptr;
	}

	bool get_display_data(pfc::string_base & out, unsigned & flags,
		metadb_handle_list_cref, const GUID &) override {
		if (m_levels.empty()) return false;   // 没有可选项：整项不显示
		out = "切换音质";
		flags = 0;
		return true;
	}

	bool get_description(pfc::string_base & out) override {
		out = "换成这首歌当前能播的其它音质（只列出真的能选的档位）";
		return true;
	}

	GUID get_guid() override { return pfc::guid_null; }
	bool is_mappable_shortcut() override { return false; }

private:
	std::vector<std::string> m_levels;
	std::vector<quality_leaf_node> m_leaves;
};

class netease_quality_switch : public contextmenu_item_v2 {
public:
	unsigned get_num_items() override { return 1; }

	GUID get_item_guid(unsigned) override {
		static const GUID guid = { 0x3d6a5f21, 0x8c74, 0x4b0e, { 0x9e, 0x2a, 0x66, 0x1d, 0x4f, 0x83, 0xb7, 0x52 } };
		return guid;
	}

	void get_item_name(unsigned, pfc::string_base & out) override {
		out = "切换音质";
	}

	bool get_item_description(unsigned, pfc::string_base & out) override {
		out = "把选中的曲目换成指定的音质（只列出当前账号能播的档位）";
		return true;
	}

	t_enabled_state get_enabled_state(unsigned) override { return contextmenu_item::DEFAULT_ON; }

	GUID get_parent() override { return contextmenu_groups::root; }

	contextmenu_item_node_root * instantiate_item(unsigned, metadb_handle_list_cref data,
		const GUID &) override {
		return new quality_popup_node(data);
	}

	// 子项是动态生成的、没有稳定 GUID，所以不支持从快捷键列表直接执行。
	void item_execute_simple(unsigned, const GUID &, metadb_handle_list_cref, const GUID &) override {}
};

static contextmenu_item_factory_t<netease_quality_switch> g_netease_quality_switch_factory;

} // namespace

