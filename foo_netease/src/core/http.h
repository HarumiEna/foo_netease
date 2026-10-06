#pragma once
// foo_netease —— HTTP 传输层（WinHTTP）+ Cookie 罐。
//
// 为什么不用 SDK 的 http_client：
// 简述：SDK 的 http_client 没有 Cookie 罐、超时策略不可控，而且它只能在
// foobar2000 进程内运行，无法在独立控制台探针里验证接口契约。
// WinHTTP 让我们在组件与探针里跑同一份代码，并且完全掌控超时/重定向/Cookie。
//
// 本层不依赖 foobar2000 SDK。

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace netease {

struct HttpHeader {
	std::string name;
	std::string value;
};

struct HttpResponse {
	int status = 0;
	std::string body;
	std::vector<HttpHeader> headers;      // 全部响应头（Set-Cookie 会重复出现）
	std::vector<std::string> set_cookies; // 仅 Set-Cookie 行
};

// 极简 Cookie 罐。我们只与网易云的少数几个域打交道，因此按名字保存即可，
// 不做域名/路径匹配——前提是所有请求都发往同一批域。
class CookieJar {
public:
	void set(const std::string & name, const std::string & value);
	// 解析一行 Set-Cookie；只取第一个 name=value，忽略属性段。
	void set_from_set_cookie(const std::string & line);

	bool has(const std::string & name) const;
	std::string get(const std::string & name) const;
	std::string header_value() const; // "a=1; b=2"
	void clear();
	void remove(const std::string & name);
	size_t count() const { return m_items.size(); }

	// 用于持久化（调用方负责加密，见 session 层）。
	std::string serialize() const;
	void deserialize(const std::string & text);

private:
	std::vector<std::pair<std::string, std::string>> m_items;
};

struct HttpRequestOptions {
	int timeout_ms = 30000;
	std::vector<HttpHeader> headers;
	// 返回 true 表示调用方要求中断。
	// 用 std::function 而不是 SDK 的 abort_callback，是为了让 core/ 保持不依赖 foobar2000 SDK
	//（组件侧用 lambda 包一层 abort_callback，探针侧留空即可）。
	std::function<bool()> abort_requested;
};

struct HttpResult {
	bool ok = false;
	std::string error;
	HttpResponse response;
};

HttpResult http_post_form(const std::string & url, const std::string & body,
	CookieJar & jar, const HttpRequestOptions & options);

HttpResult http_get(const std::string & url, CookieJar & jar,
	const HttpRequestOptions & options);

// 流式 GET：与 http_get 的区别是**不把响应体攒进内存**，而是保持连接，
// 由调用方按需 read()。音频播放正需要这种：
//  · 整包读取对 Hi-Res（单曲可达 176 MB）既慢又占内存；
//  · 而"每次 Range 取 1 MB"的写法会在每块边界重新握手 —— Hi-Res 约 1 秒一块，
//    握手延迟会直接变成播放卡顿。
// 保持一条连接顺序读，两种问题都避开；需要跳转时再发一次 Range 重开即可。
class HttpStream {
public:
	HttpStream();
	~HttpStream();
	HttpStream(const HttpStream &) = delete;
	HttpStream & operator=(const HttpStream &) = delete;

	// 打开并从 start_offset 开始读取。start_offset > 0 时发 Range 请求；
	// 服务端若忽略 Range（回 200 + 整包），内部会把前面 start_offset 字节读掉丢弃，
	// 对外语义仍然正确（只是慢）。
	// 失败返回 false，error 里是原因；成功后可用 status()/total_size()。
	bool open(const std::string & url, CookieJar & jar, const HttpRequestOptions & options,
		uint64_t start_offset, std::string * error);

	// 读取至多 bytes 字节。内部循环直到填满或到达 EOF —— file::read 的约定是
	// 只有 EOF 时才允许少读，音频解码器依赖这一点。
	// 返回实际字节数，0 = EOF，-1 = 出错（error 里有原因）。
	int64_t read(void * buffer, size_t bytes, std::string * error);

	void close();
	bool is_open() const;
	int status() const;
	// 连接内已交付到的绝对偏移。
	uint64_t position() const;
	// 资源总长度；未知为 -1。
	int64_t total_size() const;
	// 服务端忽略了 Range（回 200 整包）。命中时前向跳转要读掉前面的字节。
	bool range_ignored() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace netease

