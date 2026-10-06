#pragma once
// foo_netease —— UTF-8 与宽字符互转。
//
// 约定：组件内部一律用 UTF-8（SDK 与 titleformat 都期望 UTF-8），
// 只有和 Win32 API / WTL 控件打交道时才转成宽字符。

#include <windows.h>

#include <string>

namespace netease {

inline std::wstring to_wide(const std::string & utf8) {
	if (utf8.empty()) return std::wstring();
	const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring out(need > 0 ? need : 0, L'\0');
	if (need > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), need);
	return out;
}

inline std::string to_utf8(const std::wstring & wide) {
	if (wide.empty()) return std::string();
	const int need = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string out(need > 0 ? need : 0, '\0');
	if (need > 0) WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), need, nullptr, nullptr);
	return out;
}

} // namespace netease

