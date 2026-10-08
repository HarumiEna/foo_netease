# 本插件由DeepSeek100%构建

# foo_netease

foobar2000 的网易云音乐组件：登录账号、浏览歌单、直接播放。

[![Stars](https://img.shields.io/github/stars/HarumiEna/foo_netease?style=flat-square&label=Stars&color=blue)](https://github.com/HarumiEna/foo_netease/stargazers) [![Forks](https://img.shields.io/github/forks/HarumiEna/foo_netease?style=flat-square&label=Forks&color=blue)](https://github.com/HarumiEna/foo_netease/forks) [![Downloads](https://img.shields.io/github/downloads/HarumiEna/foo_netease/total?style=flat-square&label=Downloads&color=green)](https://github.com/HarumiEna/foo_netease/releases)

- 版本 **0.19.0** ｜ Windows ｜ foobar2000 **v2.x**（x64 / Win32）
- 第三方个人作品，与网易云音乐官方无关

## 功能

- **登录**：扫码登录；凭据用 DPAPI 加密后存本地
- **可停靠面板**（跟随明暗主题）：每日推荐 / 漫游（私人 FM）/ 华语私人雷达 / 最近播放 / 私人雷达 / 我的歌单 / 搜索 / 歌单链接；单击选中、双击发送，右键可添加或替换
- **歌单浏览窗口**：浏览歌单与曲目，添加或替换到当前播放列表
- **播放**：组件自己拉流，不依赖官方客户端，支持云盘 Hi-Res；状态栏显示真实码率 / 采样率 / 位深 / 编码
- **漫游电台**：无限续播，已播曲目自动移除
- **封面跟随正在播放**；**歌词**输出到 %netease_lyric% 并提供歌词窗口
- 元数据缓存、%netease_no% 保留歌单内序号、本机播放记录
- **界面缩放可调**：默认跟随系统 DPI，4K 屏觉得挤可在设置里再放大（100%/125%/150%/175%/200%），改完立即生效

## 截图

![可停靠面板](foo_netease/screenshots/panel.png)
![歌单浏览](foo_netease/screenshots/browse.png)
![设置页](foo_netease/screenshots/prefs.png)
![扫码登录](foo_netease/screenshots/login-qr.png)

## 安装

参数设置 → 组件 → 安装… → 选择 `.fb2k-component` → 重启 foobar2000。

## 已知限制

- **不会同步到网易云账号**：foobar2000 里播的歌只进组件自己的「最近播放」

## 说明

- 仅供学习与个人使用，请遵守网易云音乐的服务条款
- 接口来自社区公开的逆向资料，可能随服务端调整而失效
- 不提供、不分发任何音乐内容
