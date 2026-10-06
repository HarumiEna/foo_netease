#include "stdafx.h"
#include "component_log.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <mutex>

namespace netease_log {

namespace {

std::mutex g_mutex;
std::string g_path;          // UTF-8 路径
bool g_ready = false;

// core_api::get_profile_path() 返回的是 "file://c:\...\foobar2000-v2" 这种形式，
// 这里把它还原成 Win32 能用的本地路径。
std::string local_profile_dir() {
	const char * raw = core_api::get_profile_path();
	if (!raw) return std::string();
	std::string path = raw;
	const std::string prefix = "file://";
	if (path.compare(0, prefix.size(), prefix) == 0) path = path.substr(prefix.size());
	for (char & c : path) {
		if (c == '/') c = '\\';
	}
	while (!path.empty() && path.back() == '\\') path.pop_back();
	return path;
}

std::wstring to_wide(const std::string & utf8) {
	if (utf8.empty()) return std::wstring();
	const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(need > 0 ? need : 0, L'\0');
	if (need > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), need);
	return out;
}

std::string timestamp() {
	SYSTEMTIME st{};
	GetLocalTime(&st);
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
}

// 日志文件超过 1 MB 就滚动一次，避免长期使用后无限增长。
void roll_if_needed(const std::string & path) {
	std::error_code ec;
	const auto size = std::filesystem::file_size(to_wide(path), ec);
	if (ec || size < 1024 * 1024) return;
	std::filesystem::rename(to_wide(path), to_wide(path + ".old"), ec);
}

} // namespace

void init() {
	std::lock_guard<std::mutex> lock(g_mutex);
	const std::string dir = local_profile_dir();
	if (dir.empty()) return;
	g_path = dir + "\\foo_netease.log";
	roll_if_needed(g_path);
	g_ready = true;
}

const std::string & file_path() { return g_path; }

std::string profile_dir() { return local_profile_dir(); }

void write(const std::string & line) {
	// 先写控制台：这是 foobar2000 里最自然的查看方式。
	console::print(line.c_str());

	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_ready || g_path.empty()) return;

	std::ofstream out(to_wide(g_path).c_str(), std::ios::binary | std::ios::app);
	if (!out) return;
	out << timestamp() << "  " << line << "\r\n";
}

void shutdown() {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_ready = false;
}

} // namespace netease_log

