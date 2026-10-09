#include "stdafx.h"
#include "component_log.h"
#include "lyric_store.h"
#include "meta_store.h"
#include "netease_data.h"
#include "session.h"
#include "theme.h"

#include <filesystem>
#include <windows.h>

// 组件标识。
DECLARE_COMPONENT_VERSION(
	"网易云音乐 (Netease Cloud Music)",
	"0.22.0",
	"在 foobar2000 内登录网易云音乐账号、浏览歌单并直接播放。\n"
	"\n"
	"功能：\n"
	" · 扫码登录；\n"
	" · 凭据用 Windows DPAPI 加密后存入 foobar2000 配置；\n"
	" · 浏览歌单（含收藏/自建，支持搜索），曲目元数据完整；\n"
	" · 把曲目加入播放列表并直接播放；\n"
	" · 音质可选，默认极高 320k；播放时解析 CDN 直链，并由组件自己拉流解码。\n"
	" · 播放列表右键「切换音质」可原地改档位，菜单只列出这首歌当前真的能播的音质。\n"
	" · 自动获取专辑封面（在线查图，不依赖文件内嵌封面）；\n"
	" · 可停靠面板（跟随 foobar2000 明/暗主题）：只列来源（每日推荐 / 漫游 / 私人雷达 / 我的歌单），\n"
	"   左键点一下=替换当前播放列表，右键=添加到当前播放列表 / 添加到新的播放列表；面板可自由缩放；\n"
	"   搜索框里粘歌单链接同样可用。\n"
	" · 歌单（每日推荐 / 最近播放这类来源也一样）会记住自己的播放列表：用「添加到新的\n"
	"   播放列表」写成 fb2k 播放列表后，在播放列表管理器里点中它就会自动重拉一次并更新\n"
	"  （追加式写入不记，不会冲掉自己加的曲子）。\n"
	" · 歌词：播放时写成 <profile>\\lyrics 下的 .lrc —— 文件名两个方向都写一份\n"
	"   （「标题 - 歌手」/「歌手 - 标题」），ESLyric 的本地歌词模板不管哪个方向都能命中；\n"
	"   这个目录就是 ESLyric 默认找的，无需任何配置；同时照旧以**运行时标签**提供\n"
	"  （只存在于内存、绝不写进文件），按标签读歌词的显示端（如 WebView2 前端）也能用。\n"
	"   有逐字版权时优先写增强型（逐字）歌词；超过一周没再写过的会自动清理掉。\n"
	"   另有 %lyric% / %netease_lyric% / %netease_lyric_enhanced% / %netease_yrc% 字段\n"
	"   与独立歌词窗口（支持逐字高亮）。\n"
	" · 漫游电台：列表里只放当前一批（3 首）+ 正在播放的那首，播到最后一首自动取下一批，\n"
	"   已播过的自动删除，可以一直放下去。\n"
	"\n"
	"入口：视图 → 网易云音乐 → 浏览歌单…；设置：参数设置 → 工具 → 网易云音乐。\n"
);

VALIDATE_COMPONENT_FILENAME("foo_netease.dll");

// 元数据缓存落盘。
//
// 意义：重启 foobar2000 后，之前看过的歌单不用再等一轮网络请求才能显示标题/歌手，
// 播放列表加载也更快。文件是「每行一个 JSON 对象」，出问题可以直接看。
namespace {


// 生命周期。
// on_init() 时服务系统已可用，可以安全调用 console::print 与 configStore；
// 但静态对象构造期间不可以。
class netease_lifecycle : public initquit {
public:
	void on_init() override {
		netease_log::init();
		netease_log::write("foo_netease: on_init() —— 组件已加载");
		netease_log::write("foo_netease: 日志文件 " + netease_log::file_path());
		netease::Session::instance().init();
		netease_app::load_meta_cache();
		netease_data::playlist_source_load();
		// 本地歌词（<profile>\lyrics 里我们写的那几份）超过一周没再写过就清掉。
		netease_lyric::cleanup_lrc_files();
		netease_data::recent_local_load();
		netease_ui::theme_watch_start();   // 跟随 foobar2000 的主题配色




	}
	void on_quit() override {
		netease_ui::theme_watch_stop();
		netease_data::playlist_source_shutdown();
		netease_app::save_meta_cache();
		netease::Session::instance().shutdown();
		netease_log::write("foo_netease: on_quit()");
		netease_log::shutdown();
	}
};

// 播放列表回调：在 foobar2000 的播放列表管理器里点中一个"对应着某个网易云歌单"的
// 播放列表时，自动重拉一次那个歌单并替换内容（对应关系见 netease_data::playlist_source_*）。
// 通知都在主线程来。
class netease_playlist_watch : public playlist_callback_static {
public:
	unsigned get_flags() override {
		return flag_on_playlist_activate | flag_on_playlists_removing;
	}

	void on_playlist_activate(t_size, t_size p_new) override {
		if (p_new == pfc::infinite_size) return;
		try {
			netease_data::playlist_source_refresh_if_mapped(p_new);
		} catch (...) {
			// 回调里出任何问题都不该影响宿主
		}
	}

	// 列表被删掉时顺手清掉对应关系，免得配置文件里越积越多。
	void on_playlists_removing(const bit_array & p_mask, t_size p_old_count, t_size) override {
		auto pm = playlist_manager::get();
		if (!pm.is_valid()) return;
		for (t_size i = 0; i < p_old_count; ++i) {
			if (!p_mask.get(i)) continue;
			pfc::string8 name;
			if (pm->playlist_get_name(i, name)) {
				netease_data::playlist_source_forget(std::string(name.get_ptr()));
			}
		}
	}

	// 其余通知用不到。
	void on_items_added(t_size, t_size, const pfc::list_base_const_t<metadb_handle_ptr> &, const bit_array &) override {}
	void on_items_reordered(t_size, const t_size *, t_size) override {}
	void on_items_removing(t_size, const bit_array &, t_size, t_size) override {}
	void on_items_removed(t_size, const bit_array &, t_size, t_size) override {}
	void on_items_selection_change(t_size, const bit_array &, const bit_array &) override {}
	void on_item_focus_change(t_size, t_size, t_size) override {}
	void on_items_modified(t_size, const bit_array &) override {}
	void on_items_modified_fromplayback(t_size, const bit_array &, play_control::t_display_level) override {}
	void on_items_replaced(t_size, const bit_array &, const pfc::list_base_const_t<t_on_items_replaced_entry> &) override {}
	void on_item_ensure_visible(t_size, t_size) override {}
	void on_playlist_created(t_size, const char *, t_size) override {}
	void on_playlists_reorder(const t_size *, t_size) override {}
	void on_playlists_removed(const bit_array &, t_size, t_size) override {}
	void on_playlist_renamed(t_size, const char *, t_size) override {}
	void on_default_format_changed() override {}
	void on_playback_order_changed(t_size) override {}
	void on_playlist_locked(t_size, bool) override {}
};

} // namespace

FB2K_SERVICE_FACTORY(netease_lifecycle);
FB2K_SERVICE_FACTORY(netease_playlist_watch);

