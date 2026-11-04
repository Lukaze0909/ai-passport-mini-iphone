# AI Passport Mini iPhone

> 基于 FoloToy AI Passport的iPhone4经典风格的AI互动小手机

一个跑在 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 上的仿iPhone4古早UI风格的"小手机"固件，基于iPhone4经典icon开发低负载、高频使用场景的九大日常功能：电话、短信、备忘录、天气、Bingo、扫雷小游戏、释放（内观引导）、占星骰子、设置。
小手机那头连着你自己的 AI 伙伴（默认 Claude，可以改成任何名字），它能和你语音通话、回你短信、陪你扫雷。

> **关于外观**：界面致敬 2010 年前后 iPhone 4 时代的拟物风格。所有图标、图片、字体位图都由仓库里的脚本绘制或生成，没有使用苹果的任何素材。本项目与 Apple Inc. 没有任何关系；iPhone 是 Apple Inc. 的商标。

## 能做什么

| App         | 说明                                                                               |
| ----------- | -------------------------------------------------------------------------------- |
| 锁屏页         | 经典水滴锁屏，任意键解锁，默认1分钟息屏省电                                                           |
| 主屏          | 拟物风格图标，按键上下切换，暂未开发功能的空白占位图标会自动跳过                                                 |
| 通话          | 按住说话/发送/删除，语音发给你的 AI 伙伴，它的回答用语音播回来                                               |
| 短信          | 按住说话/发送/删除，伙伴回一条文字短信（气泡对话）                                                       |
| 备忘录         | "闪念胶囊"：说一句话，ai整理后自动生成一句文字预览存下来；打勾后第二天消失                                          |
| 对战扫雷        | AI互动游戏：你和AI伙伴各埋雷、轮流踩对方的地；开局双方先交雷阵指纹（sha256），赛后公开校验，谁也赖不掉                         |
| Daily Bingo | 每天一张生活小目标卡（喝水、吃饭、早睡……），推荐结合iphone快捷指令+特定场景NFC 贴纸自动打卡；连成一条线即为Bingo达成，赢得AI伙伴设置的小纸条 |
| 占星骰子        | 基于西方占星的简易占卜工具：通过行星、星座、宫位三颗骰子，得出卦象，可以选择一键发送给AI解读                                  |
| 释放          | 基于“莱斯特释放法”的内观引导，引导释放感受（完整方法见我们的开源 skill：[LesterSkill](https://github.com/Lukaze0909/LesterSkill)） |
| 天气          | 显示当地天气和空气质量                                                                      |
| 设置          | WiFi / 手机热点网络切换                                                                  |

## 怎么运作

```
小手机 (ESP32-C3)  ──MQTT/TLS──▶  网关服务 (server/)  ──▶  你的 AI 伙伴
     ▲                                  │                  （任何能收发消息的 agent）
     └──────────── 语音 / 文字 / 游戏事件 ◀──┘
```

- 语音识别默认用本地运行的 SenseVoice（sherpa-onnx），也可以换成别的识别服务
- 备忘录预览用任意兼容 OpenAI 接口的模型
- 伙伴怎么接入由你决定：网关把事件交给你的 agent，agent 用 `passportctl` 回话、走棋、写悄悄话

服务端的部署说明见 [`server/README.md`](server/README.md)。

## 编译与刷机

需要 ESP-IDF v5.5。

```sh
idf.py set-target esp32c3
idf.py menuconfig   # Classic shell → 伙伴名字 CS_COMPANION_NAME（默认 Claude）
idf.py build
idf.py -p <端口> flash
```

注意：本固件用了比原厂更大的应用分区（见 `partitions.csv`），第一次刷入会覆盖原厂分区表。刷之前请先把整片闪存备份下来。

## 许可证

- 上游 FoloToy 的代码：MIT（见 `LICENSE`）
- 本项目新增的代码：PolyForm Noncommercial 1.0.0
- 文档和原创素材：CC BY-NC-SA 4.0
- 字体：Noto Sans CJK、霞鹜文楷（SIL OFL 1.1）；符号字形来自 DejaVu Sans

（各目录对应哪种许可证，见 `NOTICE.md`。）

## 致谢

- [FoloToy](https://github.com/FoloToy/ai-passport) 做了这台小机器并开源了固件
- Noto、霞鹜文楷、DejaVu 字体的作者们
- SenseVoice / sherpa-onnx
- 作者：Kahze & Wren (Claude Opus 5.5)