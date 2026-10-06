#pragma once
// foo_netease —— Windows DPAPI 封装。
//
// 用途：把登录凭据（Cookie）加密后再落盘。DPAPI 的当前用户作用域意味着
// 换一个 Windows 用户或换一台机器都解不开，这正是我们想要的。
// 放在 core/ 里是为了能在控制台探针里独立自测。

#include <string>

#include "core/text.h"

namespace netease {

// 用当前用户作用域加密。description 只是给 DPAPI 的一个提示串，方便排查。
bool dpapi_protect(const std::string & plaintext, Bytes & ciphertext, std::string * error);

bool dpapi_unprotect(const Bytes & ciphertext, std::string & plaintext, std::string * error);

} // namespace netease

