#include "stdafx.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "component_log.h"
#include "core/api.h"
#include "core/meta_cache.h"
#include "netease_data.h"
#include "session.h"

// ---------------------------------------------------------------------------
// D3：把网易云歌单链接拖进 foobar2000 就能加载。
//
// 支持的链接形式（都在下面的 parse_playlist_id 里处理）：
//   netease://playlist/<id>
//   https://music.163.com/playlist?id=<id>
//   https://music.163.com/#/playlist?id=<id>          （网页里复制出来的那种）
//   https://y.music.163.com/m/playlist?id=<id>
//
// 实现要点：open() 在工作线程里被调用，拿 trackIds 全集（tracks 会被服务端截断），
// 再分批补元数据塞进缓存，最后逐条回调 on_entry —— 这样拖进来的播放列表
// 立刻就有标题/歌手/专辑，不需要用户再等一轮网络。
// ---------------------------------------------------------------------------

namespace {

// 读一个小文本文件（.netease 里就放一行歌单链接）。
bool read_small_text(const service_ptr_t<file> & hint, const char * path, abort_callback & abort,
	std::string & out) {
	service_ptr_t<file> f = hint;
	try {
		if (!f.is_valid()) filesystem::g_open_read(f, path, abort);
		if (!f.is_valid()) return false;
		f->seek(0, abort);
		const t_filesize size = f->get_size(abort);
		if (size == filesize_invalid || size > 64 * 1024) return false;
		out.assign(static_cast<size_t>(size), '\0');
		if (size > 0) f->read_object(out.data(), static_cast<t_size>(size), abort);
		return true;
	} catch (const std::exception &) {
		return false;
	}
}

std::string song_path(int64_t id, int64_t index) {
	const std::string level = netease::Session::instance().quality();
	return "netease://song/" + std::to_string(id) +
		"?level=" + level + "&no=" + std::to_string(index);
}

class netease_playlist_loader : public playlist_loader {
public:
	void open(const char * p_path, const service_ptr_t<file> & p_file,
		playlist_loader_callback::ptr callback, abort_callback & p_abort) override {
		// 1) 路径本身就是链接；2) 本地 .netease 文件，内容是一行链接。
		int64_t playlist_id = p_path ? netease_data::parse_playlist_link(p_path) : 0;
		if (playlist_id <= 0 && p_path) {
			std::string content;
			if (read_small_text(p_file, p_path, p_abort, content)) {
				playlist_id = netease_data::parse_playlist_link(content);
			}
		}
		if (playlist_id <= 0) throw exception_io_unsupported_format();

		netease_log::write("foo_netease: 拖入 / 打开歌单链接，playlist id=" + std::to_string(playlist_id));
		callback->on_progress(p_path);

		netease::CookieJar jar;
		jar.deserialize(netease::Session::instance().cookie_header());
		if (!jar.has("MUSIC_U")) {
			throw exception_io_data("尚未登录：请先在 参数设置 → 工具 → 网易云音乐 里登录");
		}
		netease::NeteaseApi api(jar, 30000);
		api.set_abort([&p_abort] { return p_abort.is_aborting(); });

		std::vector<int64_t> ids;
		std::string name;
		int64_t track_count = -1;
		netease::ApiCall detail = api.playlist_track_ids(playlist_id, ids, name, track_count);
		if (!detail.ok) throw exception_io_data(("取歌单失败：" + detail.error).c_str());
		if (ids.empty()) throw exception_io_data("这个歌单是空的");

		// 补元数据：只影响速度，不影响条数；失败也照样把曲目加进去。
		std::vector<netease::TrackInfo> tracks;
		std::vector<int64_t> missing;
		netease::ApiCall songs = api.song_details(ids, tracks, &missing);
		if (songs.ok) {
			netease::MetaCache::instance().put_all(tracks);
			netease_log::write("foo_netease: 歌单「" + name + "」元数据 " + std::to_string(tracks.size()) +
				" 首（trackIds " + std::to_string(ids.size()) + "，缺 " + std::to_string(missing.size()) + "）");
		} else {
			netease_log::write("foo_netease: 歌单元数据读取失败，仅按 id 写入：" + songs.error);
		}

		for (size_t i = 0; i < ids.size(); ++i) {
			p_abort.check();
			if ((i & 0x3F) == 0) callback->on_progress(p_path);
			metadb_handle_ptr handle;
			callback->handle_create(handle,
				playable_location_impl(song_path(ids[i], static_cast<int64_t>(i) + 1).c_str(), 0));
			callback->on_entry(handle, playlist_loader_callback::entry_user_requested,
				filestats_invalid, false);
		}
	}

	void write(const char *, const service_ptr_t<file> &, metadb_handle_list_cref, abort_callback &) override {
		throw pfc::exception_not_implemented();
	}

	const char * get_extension() override { return "netease"; }
	bool can_write() override { return false; }
	bool is_our_content_type(const char *) override { return false; }
	bool is_associatable() override { return false; }
};

service_factory_single_t<netease_playlist_loader> g_netease_playlist_loader;
// 让「打开文件」对话框里能看到 .netease 类型
DECLARE_FILE_TYPE("Netease Cloud Music playlist", "*.netease");

} // namespace

