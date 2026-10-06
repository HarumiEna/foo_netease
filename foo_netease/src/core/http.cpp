#include "core/http.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "winhttp.lib")

namespace netease {

namespace {

void set_error(std::string * error, const std::string & text) {
	if (error) *error = text;
}

std::string win32_message(DWORD code) {
	LPWSTR buffer = nullptr;
	const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
	std::string text;
	if (len && buffer) {
		const int need = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
		text.resize(need > 0 ? need : 0);
		if (need > 0) {
			WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len), text.data(), need, nullptr, nullptr);
		}
	}
	if (buffer) LocalFree(buffer);
	char code_buf[32];
	std::snprintf(code_buf, sizeof(code_buf), " (Win32 错误 %lu)", static_cast<unsigned long>(code));
	return text.empty() ? std::string("未知错误") + code_buf : text + code_buf;
}

std::wstring to_wide(const std::string & utf8) {
	if (utf8.empty()) return std::wstring();
	const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(need > 0 ? need : 0, L'\0');
	if (need > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), need);
	return out;
}

std::string to_utf8(const std::wstring & wide) {
	if (wide.empty()) return std::string();
	const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string out(need > 0 ? need : 0, '\0');
	if (need > 0) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), need, nullptr, nullptr);
	return out;
}

struct UrlParts {
	std::wstring host;
	std::wstring path;
	INTERNET_PORT port = 0;
	bool secure = false;
};

bool parse_url(const std::string & url, UrlParts & out, std::string * error) {
	const size_t scheme_end = url.find("://");
	if (scheme_end == std::string::npos) {
		set_error(error, "URL 缺少协议：" + url);
		return false;
	}
	const std::string scheme = url.substr(0, scheme_end);
	out.secure = (scheme == "https");
	if (scheme != "http" && scheme != "https") {
		set_error(error, "只支持 http/https：" + scheme);
		return false;
	}
	const size_t host_start = scheme_end + 3;
	size_t path_start = url.find('/', host_start);
	std::string authority = url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
	std::string path = path_start == std::string::npos ? "/" : url.substr(path_start);

	const size_t colon = authority.rfind(':');
	if (colon != std::string::npos) {
		out.port = static_cast<INTERNET_PORT>(std::atoi(authority.c_str() + colon + 1));
		authority = authority.substr(0, colon);
	}
	if (authority.empty()) {
		set_error(error, "URL 缺少主机名");
		return false;
	}
	out.host = to_wide(authority);
	out.path = to_wide(path);
	return true;
}

class WinHttpHandle {
public:
	~WinHttpHandle() { if (m_h) WinHttpCloseHandle(m_h); }
	WinHttpHandle() = default;
	WinHttpHandle(const WinHttpHandle &) = delete;
	WinHttpHandle & operator=(const WinHttpHandle &) = delete;
	HINTERNET * address() { return &m_h; }
	HINTERNET get() const { return m_h; }
	void reset(HINTERNET h) { if (m_h) WinHttpCloseHandle(m_h); m_h = h; }
private:
	HINTERNET m_h = nullptr;
};

std::string trim(const std::string & s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
	return s.substr(a, b - a);
}

std::string lower_ascii(std::string s) {
	for (char & c : s) {
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
	return s;
}

// 检查中断请求。返回 true 表示已要求中断，调用方应立即返回。
bool aborted(const HttpRequestOptions & options) {
	return options.abort_requested && options.abort_requested();
}

// 一次性取回整块响应头（CRLF 分隔）再自己拆。
// 这样能完整保留重复出现的 Set-Cookie，而不是只拿到第一条。
void read_response_headers(HINTERNET req, std::vector<HttpHeader> & out) {
	DWORD len = 0;
	WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
		nullptr, &len, WINHTTP_NO_HEADER_INDEX);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return;
	std::wstring raw(len / sizeof(wchar_t) + 2, L'\0');
	if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
			raw.data(), &len, WINHTTP_NO_HEADER_INDEX)) {
		return;
	}
	raw.resize(wcslen(raw.c_str()));
	const std::string block = to_utf8(raw);
	size_t pos = 0;
	while (pos < block.size()) {
		size_t eol = block.find("\r\n", pos);
		if (eol == std::string::npos) eol = block.size();
		const std::string line = block.substr(pos, eol - pos);
		pos = eol + 2;
		if (line.empty()) continue;
		const size_t sep = line.find(':');
		if (sep == std::string::npos) continue; // 状态行等
		const std::string name = trim(line.substr(0, sep));
		const std::string value = trim(line.substr(sep + 1));
		if (name.empty()) continue;
		out.push_back({ name, value });
	}
}

// 取一个响应头的值（大小写不敏感）；没有则返回空串。
std::string find_header(const std::vector<HttpHeader> & headers, const std::string & name) {
	const std::string want = lower_ascii(name);
	for (const auto & h : headers) {
		if (lower_ascii(h.name) == want) return h.value;
	}
	return std::string();
}

HttpResult request(const std::string & method, const std::string & url, const std::string & body,
	CookieJar & jar, const HttpRequestOptions & options) {
	HttpResult result;
	if (aborted(options)) {
		result.error = "已取消";
		return result;
	}

	UrlParts parts;
	if (!parse_url(url, parts, &result.error)) return result;

	WinHttpHandle session;
	session.reset(WinHttpOpen(L"foo_netease/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session.get()) {
		// 老系统不支持 AUTOMATIC_PROXY，退回默认代理配置。
		session.reset(WinHttpOpen(L"foo_netease/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	}
	if (!session.get()) {
		result.error = "WinHttpOpen 失败：" + win32_message(GetLastError());
		return result;
	}

	const int timeout = options.timeout_ms > 0 ? options.timeout_ms : 30000;
	WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);

	WinHttpHandle connect;
	connect.reset(WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));
	if (!connect.get()) {
		result.error = "WinHttpConnect 失败：" + win32_message(GetLastError());
		return result;
	}

	const std::wstring method_w = to_wide(method);
	WinHttpHandle req;
	req.reset(WinHttpOpenRequest(connect.get(), method_w.c_str(), parts.path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0));
	if (!req.get()) {
		result.error = "WinHttpOpenRequest 失败：" + win32_message(GetLastError());
		return result;
	}

	// 重定向与 Cookie 都由我们自己管，避免 WinHTTP 的隐式行为影响可测试性。
	DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
	WinHttpSetOption(req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));

	// 组装请求头（含 Cookie 罐里的内容）。
	std::string header_block;
	for (const auto & h : options.headers) {
		header_block += h.name + ": " + h.value + "\r\n";
	}
	const std::string cookie = jar.header_value();
	if (!cookie.empty()) header_block += "Cookie: " + cookie + "\r\n";

	const std::wstring headers_w = to_wide(header_block);
	const DWORD body_len = static_cast<DWORD>(body.size());

	if (aborted(options)) {
		result.error = "已取消";
		return result;
	}
	if (!WinHttpSendRequest(req.get(), headers_w.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers_w.c_str(),
			headers_w.empty() ? 0 : static_cast<DWORD>(headers_w.size()),
			body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data()), body_len, body_len, 0)) {
		result.error = "WinHttpSendRequest 失败：" + win32_message(GetLastError());
		return result;
	}
	if (!WinHttpReceiveResponse(req.get(), nullptr)) {
		result.error = "WinHttpReceiveResponse 失败：" + win32_message(GetLastError());
		return result;
	}

	DWORD status = 0, status_len = sizeof(status);
	if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX)) {
		result.error = "读取状态码失败：" + win32_message(GetLastError());
		return result;
	}
	result.response.status = static_cast<int>(status);

	// 一次性取回整块响应头（CRLF 分隔），再由我们自己拆。
	// 这样能完整保留重复出现的 Set-Cookie，而不是只拿到第一条。
	read_response_headers(req.get(), result.response.headers);
	for (const auto & h : result.response.headers) {
		if (lower_ascii(h.name) == "set-cookie") {
			result.response.set_cookies.push_back(h.value);
			jar.set_from_set_cookie(h.value);
		}
	}

	// 读取响应体。每次拿到一块之前都检查一次中断请求，
	// 这样"取消"最多延迟一个数据块的时间，而不是等满整个超时。
	for (;;) {
		if (aborted(options)) {
			result.error = "已取消";
			return result;
		}
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(req.get(), &available)) {
			result.error = "WinHttpQueryDataAvailable 失败：" + win32_message(GetLastError());
			return result;
		}
		if (available == 0) break;
		const size_t offset = result.response.body.size();
		result.response.body.resize(offset + available);
		DWORD read = 0;
		if (!WinHttpReadData(req.get(), result.response.body.data() + offset, available, &read)) {
			result.error = "WinHttpReadData 失败：" + win32_message(GetLastError());
			return result;
		}
		if (read == 0) break;
		result.response.body.resize(offset + read);
	}

	result.ok = true;
	return result;
}

} // namespace

// ---------------------------------------------------------------------------
// HttpStream：流式 GET。设计理由见 core/http.h 里的说明。
// ---------------------------------------------------------------------------

struct HttpStream::Impl {
	WinHttpHandle session;
	WinHttpHandle connect;
	WinHttpHandle req;
	std::function<bool()> abort_requested;
	int status = 0;
	int64_t total = -1;   // 资源总长度，未知为 -1
	uint64_t pos = 0;     // 已交付到的绝对偏移
	bool eof = false;
	bool range_ignored = false;
};

HttpStream::HttpStream() : m_impl(std::make_unique<Impl>()) {}
HttpStream::~HttpStream() = default;

bool HttpStream::is_open() const { return m_impl && m_impl->req.get() != nullptr; }
int HttpStream::status() const { return m_impl ? m_impl->status : 0; }
uint64_t HttpStream::position() const { return m_impl ? m_impl->pos : 0; }
int64_t HttpStream::total_size() const { return m_impl ? m_impl->total : -1; }
bool HttpStream::range_ignored() const { return m_impl && m_impl->range_ignored; }

void HttpStream::close() {
	if (!m_impl) return;
	m_impl->req.reset(nullptr);
	m_impl->connect.reset(nullptr);
	m_impl->session.reset(nullptr);
}

bool HttpStream::open(const std::string & url, CookieJar & jar, const HttpRequestOptions & options,
	uint64_t start_offset, std::string * error) {
	close();
	Impl & s = *m_impl;
	s.status = 0;
	s.total = -1;
	s.pos = 0;
	s.eof = false;
	s.range_ignored = false;
	s.abort_requested = options.abort_requested;

	auto fail = [&](const std::string & text) {
		set_error(error, text);
		close();
		return false;
	};

	if (aborted(options)) return fail("已取消");

	UrlParts parts;
	std::string parse_error;
	if (!parse_url(url, parts, &parse_error)) return fail(parse_error);

	s.session.reset(WinHttpOpen(L"foo_netease/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	if (!s.session.get()) {
		s.session.reset(WinHttpOpen(L"foo_netease/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
	}
	if (!s.session.get()) return fail("WinHttpOpen 失败：" + win32_message(GetLastError()));

	const int timeout = options.timeout_ms > 0 ? options.timeout_ms : 30000;
	WinHttpSetTimeouts(s.session.get(), timeout, timeout, timeout, timeout);

	s.connect.reset(WinHttpConnect(s.session.get(), parts.host.c_str(), parts.port, 0));
	if (!s.connect.get()) return fail("WinHttpConnect 失败：" + win32_message(GetLastError()));

	s.req.reset(WinHttpOpenRequest(s.connect.get(), L"GET", parts.path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0));
	if (!s.req.get()) return fail("WinHttpOpenRequest 失败：" + win32_message(GetLastError()));

	DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
	WinHttpSetOption(s.req.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));

	std::string header_block;
	for (const auto & h : options.headers) header_block += h.name + ": " + h.value + "\r\n";
	const std::string cookie = jar.header_value();
	if (!cookie.empty()) header_block += "Cookie: " + cookie + "\r\n";
	// start_offset > 0 时明确要断点续传，别让服务端按整包回。
	if (start_offset > 0) {
		header_block += "Range: bytes=" + std::to_string(start_offset) + "-\r\n";
	}

	const std::wstring headers_w = to_wide(header_block);
	if (aborted(options)) return fail("已取消");
	if (!WinHttpSendRequest(s.req.get(), headers_w.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers_w.c_str(),
			headers_w.empty() ? 0 : static_cast<DWORD>(headers_w.size()),
			WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
		return fail("WinHttpSendRequest 失败：" + win32_message(GetLastError()));
	}
	if (!WinHttpReceiveResponse(s.req.get(), nullptr)) {
		return fail("WinHttpReceiveResponse 失败：" + win32_message(GetLastError()));
	}

	DWORD status = 0, status_len = sizeof(status);
	if (!WinHttpQueryHeaders(s.req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len, WINHTTP_NO_HEADER_INDEX)) {
		return fail("读取状态码失败：" + win32_message(GetLastError()));
	}
	s.status = static_cast<int>(status);

	std::vector<HttpHeader> headers;
	read_response_headers(s.req.get(), headers);
	const std::string content_range = find_header(headers, "content-range");
	const std::string content_length = find_header(headers, "content-length");

	if (s.status == 206 && !content_range.empty()) {
		// Content-Range: bytes 0-1048575/12345678
		const size_t slash = content_range.rfind('/');
		if (slash != std::string::npos && content_range.substr(slash + 1) != "*") {
			s.total = std::strtoll(content_range.c_str() + slash + 1, nullptr, 10);
		}
	} else if (s.status == 200 && !content_length.empty()) {
		s.total = std::strtoll(content_length.c_str(), nullptr, 10);
	}

	if (s.status == 206) {
		s.pos = start_offset;
	} else if (s.status == 200) {
		s.pos = 0;
		s.range_ignored = start_offset > 0;
	} else {
		return fail("HTTP " + std::to_string(s.status));
	}

	// 服务端忽略 Range 时只能把前面的字节读掉丢弃，才能对齐到请求的偏移。
	while (s.pos < start_offset) {
		uint8_t scratch[16384];
		const uint64_t remain = start_offset - s.pos;
		const size_t want = static_cast<size_t>((std::min)(remain, static_cast<uint64_t>(sizeof(scratch))));
		std::string read_error;
		const int64_t got = read(scratch, want, &read_error);
		if (got <= 0) return fail("跳过前导字节失败：" + read_error);
	}
	return true;
}

int64_t HttpStream::read(void * buffer, size_t bytes, std::string * error) {
	if (!m_impl || !m_impl->req.get()) { set_error(error, "流未打开"); return -1; }
	Impl & s = *m_impl;
	if (bytes == 0) return 0;
	if (s.eof) return 0;

	uint8_t * out = static_cast<uint8_t *>(buffer);
	size_t done = 0;
	while (done < bytes) {
		if (s.abort_requested && s.abort_requested()) { set_error(error, "已取消"); return -1; }
		// 已知总长且已读满：直接 EOF，省一次往返。
		if (s.total >= 0 && static_cast<int64_t>(s.pos) >= s.total) { s.eof = true; break; }
		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(s.req.get(), &available)) {
			set_error(error, "WinHttpQueryDataAvailable 失败：" + win32_message(GetLastError()));
			return -1;
		}
		if (available == 0) { s.eof = true; break; }
		size_t want = static_cast<size_t>(available);
		if (want > bytes - done) want = bytes - done;
		if (want > 0xFFFFFFFFu) want = 0xFFFFFFFFu;
		DWORD got = 0;
		if (!WinHttpReadData(s.req.get(), out + done, static_cast<DWORD>(want), &got)) {
			set_error(error, "WinHttpReadData 失败：" + win32_message(GetLastError()));
			return -1;
		}
		if (got == 0) { s.eof = true; break; }
		done += got;
		s.pos += got;
	}
	return static_cast<int64_t>(done);
}

void CookieJar::set(const std::string & name, const std::string & value) {
	for (auto & kv : m_items) {
		if (kv.first == name) { kv.second = value; return; }
	}
	m_items.emplace_back(name, value);
}

void CookieJar::set_from_set_cookie(const std::string & line) {
	const size_t eq = line.find('=');
	if (eq == std::string::npos) return;
	const size_t semi = line.find(';');
	const std::string name = trim(line.substr(0, eq));
	if (name.empty() || lower_ascii(name) == "expires") return;
	const std::string value = line.substr(eq + 1, (semi == std::string::npos ? line.size() : semi) - eq - 1);
	set(name, value);
}

bool CookieJar::has(const std::string & name) const {
	for (const auto & kv : m_items) if (kv.first == name) return true;
	return false;
}

std::string CookieJar::get(const std::string & name) const {
	for (const auto & kv : m_items) if (kv.first == name) return kv.second;
	return std::string();
}

void CookieJar::remove(const std::string & name) {
	for (auto it = m_items.begin(); it != m_items.end(); ++it) {
		if (it->first == name) { m_items.erase(it); return; }
	}
}

void CookieJar::clear() { m_items.clear(); }

std::string CookieJar::header_value() const {
	std::string out;
	for (const auto & kv : m_items) {
		if (!out.empty()) out += "; ";
		out += kv.first + "=" + kv.second;
	}
	return out;
}

std::string CookieJar::serialize() const { return header_value(); }

void CookieJar::deserialize(const std::string & text) {
	clear();
	size_t pos = 0;
	while (pos < text.size()) {
		size_t semi = text.find(';', pos);
		if (semi == std::string::npos) semi = text.size();
		const std::string piece = trim(text.substr(pos, semi - pos));
		pos = semi + 1;
		if (piece.empty()) continue;
		const size_t eq = piece.find('=');
		if (eq == std::string::npos) continue;
		set(piece.substr(0, eq), piece.substr(eq + 1));
	}
}

HttpResult http_post_form(const std::string & url, const std::string & body,
	CookieJar & jar, const HttpRequestOptions & options) {
	return request("POST", url, body, jar, options);
}

HttpResult http_get(const std::string & url, CookieJar & jar,
	const HttpRequestOptions & options) {
	return request("GET", url, std::string(), jar, options);
}

} // namespace netease

