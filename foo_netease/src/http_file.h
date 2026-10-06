#pragma once
// foo_netease —— 用我们自己的 HTTP 客户端承载远端音频流。
//
// 为什么需要它：把直链交给 foobar2000 内置 HTTP 输入时，云盘 / Hi-Res 歌曲会
// 报"不支持的格式或文件损坏"，而实测证明**用我们自己的请求能 100% 正确拿到数据**
// （HTTP 200 + 合法 FLAC 首字节，甚至不带任何请求头也行）。既然内置那条路拿不到，
// 就由我们自己发请求：请求头、URL 编码、超时、Range、abort 全部可控。
//
// 用法：把它作为 p_filehint 传给 input_entry::g_open_for_decoding()，
// 路径用真实 http 地址（这样 HTTP 输入会被选中，但它会用我们给的 file，
// 而不是自己去开 URL）。
//
// 实现基于 core/http 的 HttpStream：保持一条连接顺序读，避免"每 1 MB 重新握手"
// 在 Hi-Res 上变成卡顿；需要跳转时才发 Range 重开。

#include <string>

#include "core/http.h"

namespace netease {

// 返回一个只读、可 seek 的远端文件对象。
service_ptr_t<file> open_http_stream(const std::string & url, const CookieJar & jar);

} // namespace netease

