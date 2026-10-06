#pragma once

// foo_netease —— 网易云音乐 foobar2000 组件
//
// 预编译头。引入整个 foobar2000 SDK：foobar2000-lite+atl.h（ATL/WTL 基础）
// 加上 SDK/foobar2000-all.h（全部 SDK 接口），这是 Windows 平台 fb2k 组件的标准做法。
//
// 工程以 /utf-8 编译，因此源码中的中文字面量会以 UTF-8 进入二进制，
// 与 foobar2000 内部（pfc::string / titleformat）期望的编码一致。

#include <helpers/foobar2000+atl.h>

