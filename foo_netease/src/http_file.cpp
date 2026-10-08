#include "stdafx.h"
#include "http_file.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "component_log.h"

namespace netease {

namespace {

// 小幅前跳（例如解码器回头重读几个 KB 的帧头）不值得重开连接：
// 在同一连接上把中间字节读掉丢弃即可。跳得更远就重开 + Range，省带宽。
const uint64_t kForwardSkip = 512 * 1024;

// 用我们自己的 HTTP 客户端承载远端音频流，实现 SDK 的 file 接口。
// 为什么要自己来：见 http_file.h 顶部说明 —— 内置 HTTP 输入拿不到云盘/Hi-Res 的数据，
// 而我们自己发请求 100% 拿得到。
class http_file : public file_readonly {
public:
	http_file(std::string url, CookieJar jar) : m_url(std::move(url)), m_jar(std::move(jar)) {}

	t_size read(void * p_buffer, t_size p_bytes, abort_callback & p_abort) override {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (p_bytes == 0) return 0;
		if (m_size >= 0 && static_cast<int64_t>(m_pos) >= m_size) return 0;   // 已到文件末尾

		// 每个文件对象只打前几次：既能证明"解码器真的在读我们的流"，
		// 又不会因为一首歌几万次读而刷屏。
		if (m_read_log < 3) {
			++m_read_log;
			netease_log::write("foo_netease: [自建流] 解码器读取 pos=" + std::to_string(m_pos) +
				" 请求字节=" + std::to_string(p_bytes));
		}

		if (!ensure_stream(p_abort)) {
			p_abort.check();   // 用户取消时抛 exception_aborted，而不是伪装成 I/O 故障
			note_error("打开");
			throw exception_io_data(("网易云音频流打开失败：" + m_last_error).c_str());
		}

		std::string error;
		int64_t got = m_stream->read(p_buffer, p_bytes, &error);
		if (got < 0) {
			// 先区分"用户取消"与"真的失败"：取消必须抛 exception_aborted，
			// 否则 foobar2000 会把取消当成文件损坏。
			p_abort.check();
			// 网络抖动：在当前位置重开一次再试。
			m_stream->close();
			if (ensure_stream(p_abort)) got = m_stream->read(p_buffer, p_bytes, &error);
			if (got < 0) {
				m_last_error = error;
				note_error("读取");
				throw exception_io_data(("网易云音频流读取失败（@" + std::to_string(m_pos) + "）：" + error).c_str());
			}
		}
		m_pos += static_cast<uint64_t>(got);
		return static_cast<t_size>(got);
	}

	t_filesize get_size(abort_callback & p_abort) override {
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_size >= 0) return static_cast<t_filesize>(m_size);
		if (!ensure_stream(p_abort)) return filesize_invalid;
		return m_size >= 0 ? static_cast<t_filesize>(m_size) : filesize_invalid;
	}

	t_filesize get_position(abort_callback &) override { return static_cast<t_filesize>(m_pos); }

	void seek(t_filesize p_position, abort_callback & p_abort) override {
		std::lock_guard<std::mutex> lock(m_mutex);
		(void)p_abort;
		if (m_size >= 0 && static_cast<int64_t>(p_position) > m_size) {
			throw exception_io_seek_out_of_range();
		}
		m_pos = static_cast<uint64_t>(p_position);
		// 不立刻断开：ensure_stream() 会判断是保持连接丢字节，还是重开 + Range。
	}

	bool can_seek() override { return true; }

	// 内容提示一律返回"未知"，让解码器自己嗅探首字节。
	// （早期这里写死过 audio/mpeg，结果 FLAC 流被标成 mp3、格式判断被直接带偏。）
	bool get_content_type(pfc::string_base &) override { return false; }

	void reopen(abort_callback &) override {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_pos = 0;
		if (m_stream) m_stream->close();
	}

	bool is_remote() override { return true; }

private:
	void note_error(const char * what) {
		if (m_error_log < 2) {
			++m_error_log;
			netease_log::write(std::string("foo_netease: [自建流] ") + what + "失败 @" +
				std::to_string(m_pos) + " —— " + m_last_error);
		}
	}

	// 保证有一条连接，且它的交付位置不超过 m_pos。
	//  · 位置相同   -> 直接复用；
	//  · 落后一点   -> 同连接丢弃中间字节；
	//  · 落后很多或位置超前（回退）-> 关闭并用 Range 从 m_pos 重开。
	bool ensure_stream(abort_callback & p_abort) {
		if (!m_stream) m_stream = std::make_unique<HttpStream>();
		if (m_stream->is_open()) {
			const uint64_t stream_pos = m_stream->position();
			if (stream_pos == m_pos) return true;
			if (stream_pos < m_pos && m_pos - stream_pos <= kForwardSkip &&
					discard_forward(p_abort, m_pos - stream_pos)) {
				return true;
			}
			m_stream->close();
		}

		HttpRequestOptions options;
		options.timeout_ms = 30000;
		options.headers.push_back({ "Referer", "https://music.163.com" });
		options.headers.push_back({ "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" });
		options.abort_requested = [&p_abort] { return p_abort.is_aborting(); };

		if (!m_stream->open(m_url, m_jar, options, m_pos, &m_last_error)) return false;

		const int64_t total = m_stream->total_size();
		if (total > 0) m_size = total;

		// 首次打开记一次。之后"从非 0 位置重开"就是解码器在跳转 —— 换音质后回跳、
		// 拖进度、解码器回头读帧头都算。判断"换音质后接着播"有没有真的落点就看这条
		// （每个文件对象最多记 4 条，免得解码器来回蹭位置把日志刷满）。
		if (!m_open_logged || (m_pos > 0 && m_reopen_log < 4)) {
			const bool first = !m_open_logged;
			m_open_logged = true;
			if (!first) ++m_reopen_log;
			netease_log::write("foo_netease: [自建流] " + std::string(first ? "打开" : "跳转重开") +
				" HTTP " + std::to_string(m_stream->status()) +
				(m_stream->range_ignored() ? "（服务端忽略 Range，顺序读取）" : "") +
				" 起点=" + std::to_string(m_pos) +
				" 总长=" + (m_size >= 0 ? std::to_string(m_size) : std::string("未知")));
		}
		return true;
	}

	bool discard_forward(abort_callback & p_abort, uint64_t bytes) {
		uint8_t scratch[16384];
		std::string error;
		while (bytes > 0) {
			if (p_abort.is_aborting()) { m_last_error = "已取消"; return false; }
			const size_t want = static_cast<size_t>((std::min)(bytes, static_cast<uint64_t>(sizeof(scratch))));
			const int64_t got = m_stream->read(scratch, want, &error);
			if (got <= 0) { m_last_error = error; return false; }
			bytes -= static_cast<uint64_t>(got);
		}
		return true;
	}

	std::string m_url;
	CookieJar m_jar;
	mutable std::mutex m_mutex;
	std::unique_ptr<HttpStream> m_stream;
	uint64_t m_pos = 0;         // 逻辑读位置（file 的游标）
	int64_t m_size = -1;        // 总长度，未知为 -1
	std::string m_last_error;
	int m_reopen_log = 0;       // 已打过几次"跳转重开"
	int m_read_log = 0;         // 已打过几次"解码器读取"
	int m_error_log = 0;        // 已打过几次错误
	bool m_open_logged = false; // 本文件对象是否已记录过连接信息
};

} // namespace

service_ptr_t<file> open_http_stream(const std::string & url, const CookieJar & jar) {
	return fb2k::service_new<http_file>(url, jar);
}

} // namespace netease

