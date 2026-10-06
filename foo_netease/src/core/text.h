#pragma once
// foo_netease —— 基础文本/字节编码工具。
//
// 这一层刻意不依赖 foobar2000 SDK，也不依赖 pfc：
// 同一个 core/ 目录既被组件（foo_netease.dll）使用，也被独立控制台探针
// 在没有 foobar2000 的环境里验证接口契约时也会用到。

#include <cstdint>
#include <string>
#include <vector>

namespace netease {

using Bytes = std::vector<uint8_t>;

std::string base64_encode(const uint8_t * data, size_t len);
std::string base64_encode(const Bytes & data);
bool base64_decode(const std::string & in, Bytes & out);

std::string hex_encode(const uint8_t * data, size_t len);
std::string hex_encode(const Bytes & data);

// application/x-www-form-urlencoded 的字段转义（不解码 +，够用即可）。
std::string url_encode(const std::string & s);

// 生成 n 个字符的随机 ASCII 串，取自 base62 字母表（weapi 的 secretKey 用）。
std::string random_base62(size_t n);

} // namespace netease

