# 本插件由DeepSeek100%构建

# foo_netease

foobar2000 的网易云音乐组件：登录账号、浏览歌单、直接播放。

[![Stars](https://img.shields.io/github/stars/HarumiEna/foo_netease?style=flat-square&label=Stars&color=blue)](https://github.com/HarumiEna/foo_netease/stargazers) [![Forks](https://img.shields.io/github/forks/HarumiEna/foo_netease?style=flat-square&label=Forks&color=blue)](https://github.com/HarumiEna/foo_netease/forks) [![Downloads](https://img.shields.io/github/downloads/HarumiEna/foo_netease/total?style=flat-square&label=Downloads&color=green)](https://github.com/HarumiEna/foo_netease/releases)

- 版本 **0.22.0** ｜ Windows ｜ foobar2000 **v2.x**（x64 / Win32）
- 需要 foobar2000 2.0 或更高版本
- 第三方个人作品，与网易云音乐官方无关

## 功能

- **登录**：扫码登录，登录状态保存在本地
- **可停靠面板**：每日推荐 / 漫游（私人 FM）/ 华语私人雷达 / 最近播放 / 私人雷达 / 我的歌单 / 搜索 / 搜索歌单 / 歌单链接；配色跟随 foobar2000 的明暗主题。单击选中，双击发送到当前播放列表；右键可以添加、替换、添加到新的播放列表、下一首播放，或在新窗口里单独显示某个歌单。文字过长时悬停可看完整内容
- **歌单浏览窗口**：浏览歌单与曲目，添加或替换到当前播放列表
- **播放列表自动刷新**：用面板右键「添加到新的播放列表」建出来的列表，之后在 foobar2000 的播放列表管理器里点中它，就会自动更新一次内容；内容没有变化时不会改动列表，也不会打乱你当前选中的条目
- **播放**：组件自己拉流，不需要官方客户端；支持云盘 Hi-Res；状态栏显示真实码率 / 采样率 / 位深 / 编码
- **下一首播放**：播放列表右键把歌曲排进 foobar2000 的播放队列，随机播放下也生效
- **切换音质**：播放列表右键为这首歌选择合适的档位（标准 / 较高 / 极高 / 无损 / Hi-Res），当前账号不支持的档位不会列出；只改这一条，列表里的位置和序号不变，正在播放的那首会接着播、进度不丢
- **漫游电台**：无限续播，只记录确实播过的曲目，歌快结束时自动补下一批
- **专辑封面**：列表里每个条目显示它自己那首歌的封面；设置里可以让没有封面来源的条目（例如本地文件）跟随正在播放显示封面
- **歌词**：播放时把歌词写到 foobar2000 配置目录的 `lyrics` 文件夹（ESLyric 等按本地歌词目录读取的插件直接可用，无需配置），有逐字版权时写逐字歌词；同时把歌词提供给按标签读取歌词的播放界面，组件自己的 `%netease_lyric%` / `%netease_lyric_enhanced%` / `%netease_yrc%` 字段与歌词窗口照旧。很久没再用到的歌词文件会自动清理，设置里也能一键全部清空
- **界面缩放**：默认跟随系统 DPI，也可以在设置里手动放大（100% / 125% / 150% / 175% / 200%），改完立即生效
- 标题格式字段 `%netease_no%`：歌曲在歌单里的原始序号

## 截图

![可停靠面板](foo_netease/screenshots/panel.png)
![歌单浏览](foo_netease/screenshots/browse.png)
![设置页](foo_netease/screenshots/prefs.png)
![扫码登录](foo_netease/screenshots/login-qr.png)

## 安装

参数设置 → 组件 → 安装… → 选择 `.fb2k-component` → 重启 foobar2000。

## 已知限制

- **不会同步到网易云账号**：在 foobar2000 里播的歌只进组件自己的「最近播放」
- 歌单内容来自网易云接口，登录状态失效时需要重新扫码

## 说明

- 仅供学习与个人使用，请遵守网易云音乐的服务条款
- 接口来自社区公开的逆向资料，可能随服务端调整而失效
- 不提供、不分发任何音乐内容
