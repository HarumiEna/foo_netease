#include "stdafx.h"

#include <cstdlib>
#include <cstring>

#include "component_log.h"
#include "cover_cache.h"
#include "netease_data.h"

// 漫游电台的自动续播。
//
// 网易云的 /radio/get 一次只给 3 首，而漫游应该是无限的。这里监听播放事件：
//  · 播到"网易云漫游"列表里的**最后一首**时，提前取下一批接上（无缝，不断播）；
//  · 整张列表放完了（EOF）则兜底续一批并继续播。
// 具体写入播放列表的逻辑在 netease_data::fm_*（会保留正在播放的那首、删掉已播过的）。

namespace {

class fm_radio_callback : public play_callback_static {
public:
	unsigned get_flags() override {
		return flag_on_playback_new_track | flag_on_playback_stop;
	}

	void on_playback_new_track(metadb_handle_ptr p_track) override {
		// 在主线程把"正在播放"的路径记下来，给封面回退（它在别的线程）读。
		if (p_track.is_valid()) {
			const char * path = p_track->get_path();
			netease_data::set_now_playing_path(path ? path : "");
			// 换音质重开的：跳回换之前的进度，否则会从 0 重放。
			//
			// 注意：**不能在这里直接 seek**。on_playback_new_track 本身就是播放核心
			// 的回调，此时重入 playback_seek 会崩（实测 failure_00000008：
			// app_mainloop=>main_thread_callback::callback_run=>on_playback_new_track）。
			// fb2k::inMainThread 一定是排队投递（会内联的是 inMainThread2），
			// 等这次回调退出去、播放核心状态稳定了再跳。
			if (path) {
				const double resume = netease_data::take_resume_position(path);
				if (resume > 0.5) {
					fb2k::inMainThread([resume] {
						auto pc = playback_control::get();
						if (!pc.is_valid()) return;
						pc->playback_seek(resume);
						netease_log::write("foo_netease: 换音质后接着播 —— 跳回 " +
							std::to_string(static_cast<int>(resume)) + " 秒");
					});
				}
			}
			// 立刻后台预取封面：切歌会取消未完成的封面请求，不预取的话
			// 封面就会停在旧图上（"切歌时封面不切换"）。
			const char prefix[] = "netease://song/";
			if (path && std::strncmp(path, prefix, sizeof(prefix) - 1) == 0) {
				const int64_t id = std::strtoll(path + sizeof(prefix) - 1, nullptr, 10);
				if (id > 0) {
					netease_cover::prefetch_async(id);
					// 本机播放记录：服务端不认 foobar2000 的播放，自己记一份。
					netease_data::recent_local_add(id);
				}
			}
		} else {
			netease_data::set_now_playing_path(std::string());
		}
		// 切歌时先推一次重绘（让元素至少有机会重新取图）；
		// 封面预取完成之后还会再推一次（那次才是决定性的，见 prefetch_async）。
		netease_cover::nudge_ui_repaint();
		netease_data::follow_playing_deferred();
		netease_data::fm_radio_maybe_extend(p_track);
	}

	void on_playback_stop(play_control::t_stop_reason p_reason) override {
		netease_data::set_now_playing_path(std::string());
		if (p_reason == play_control::stop_reason_eof) {
			netease_data::fm_radio_on_eof();
		}
	}

	// 其余事件不关心（get_flags 里也没要），但仍需实现。
	void on_playback_starting(play_control::t_track_command, bool) override {}
	void on_playback_seek(double) override {}
	void on_playback_pause(bool) override {}
	void on_playback_edited(metadb_handle_ptr) override {}
	void on_playback_dynamic_info(const file_info &) override {}
	void on_playback_dynamic_info_track(const file_info &) override {}
	void on_playback_time(double) override {}
	void on_volume_change(float) override {}
};

play_callback_static_factory_t<fm_radio_callback> g_fm_radio_callback;

} // namespace

