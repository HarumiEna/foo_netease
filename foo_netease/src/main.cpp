#include "stdafx.h"
#include "component_log.h"
#include "meta_store.h"
#include "netease_data.h"
#include "session.h"
#include "theme.h"

#include <filesystem>
#include <windows.h>

// 组件标识。
DECLARE_COMPONENT_VERSION(
	"网易云音乐 (Netease Cloud Music)",
	"0.21.0",
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
	"   左键点一下=替换当前播放列表，右键=添加到当前播放列表；面板可自由缩放；\n"
	"   搜索框里粘歌单链接同样可用。\n"
	" · 歌词输出：同时写入本地歌词文件（<profile>\\lyrics，文件名「标题 - 歌手」）并作为\n"
	"   LYRIC / LYRICS / UNSYNCEDLYRICS 标签提供，foobar2000 的歌词显示器（如 ESLyric）直接可用；\n"
	"   有逐字版权时优先输出增强型（逐字）歌词，另有 %lyric% / %netease_lyric% /\n"
	"   %netease_lyric_enhanced% / %netease_yrc% 字段与独立歌词窗口（支持逐字高亮）。\n"
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
		netease_data::recent_local_load();
		netease_ui::theme_watch_start();   // 跟随 foobar2000 的主题配色


	}
	void on_quit() override {
		netease_ui::theme_watch_stop();
		netease_app::save_meta_cache();
		netease::Session::instance().shutdown();
		netease_log::write("foo_netease: on_quit()");
		netease_log::shutdown();
	}
};

} // namespace

FB2K_SERVICE_FACTORY(netease_lifecycle);

