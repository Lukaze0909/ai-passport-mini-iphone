# server/ — AI Passport Mini iPhone 网关服务

小手机本身只负责界面和录音放音。它通过 MQTT 和这里的网关说话，网关再把事件交给你自己的 AI 伙伴（任何能跑命令行的 agent）。伙伴用 `passportctl` 回话、发短信、走扫雷、写 Bingo 悄悄话。

## 架构

```
小手机 (ESP32-C3)
   │  MQTT over TLS (8883)
   ▼
MQTT broker（例如 mosquitto）
   │  本机 1883
   ▼
gateway.py ──────────────▶ 伙伴钩子 COMPANION_HOOK_CMD（事件 JSON 从 stdin 传入）
   │  天气 / 扫雷 / Bingo /      或 outbox.jsonl（没配钩子时，每行一个事件）
   │  语音拼包 + 语音识别
   ▲                                   │
   └──── passportctl say/msg/mine/bingo ◀┘   伙伴读到事件后，用命令行回应
```

| 文件 | 作用 |
| --- | --- |
| `gateway.py` | 常驻进程：收设备事件、拼接语音（IMA-ADPCM → WAV）、语音识别、调用伙伴钩子、推天气、提供 `/passport-audio/` 下载 |
| `passportctl.py` / `passportctl` | 伙伴用的命令行（`say` `msg` `status` `clips` `mine …` `bingo …`） |
| `mines.py` | 对战扫雷的服务端（伙伴那一方的雷阵、sha256 承诺、裁判） |
| `bingo.py` | 每日 Bingo 卡：抽卡、打勾、连线、悄悄话、可选的打卡自动勾选 |
| `memo_worker.py` | 备忘录：语音识别 → LLM 整理成一句英文 → 发回设备（不经过伙伴） |
| `asr.py` | 可插拔语音识别：本地 SenseVoice（sherpa-onnx）或任意 OpenAI 兼容接口；带"复读机"过滤 |
| `passport_tts.mjs` | `say` 用的语音合成：OpenAI 兼容接口或 MiniMax，也可用 `TTS_CMD` 换成任意命令 |
| `config.py` | 所有设置都从环境变量 / `.env` 读，见 `.env.example` |
| `examples/` | Bingo 任务池示例、systemd 单元、mosquitto 配置、钩子脚本示例 |

## 部署

需要 Python 3.10+、ffmpeg（`say` 用）、Node 18+（`passport_tts.mjs` 用）。

1. **MQTT broker**（以 mosquitto 为例，配置见 `examples/mosquitto-passport.conf` / `.acl`）
   - 给设备开一个 TLS 监听（8883，证书可以用 Let's Encrypt），给网关开一个只听本机的 1883
   - `mosquitto_passwd -c /etc/mosquitto/passport.passwd passport-1`（设备，用户名 = `PASSPORT_DEVICE_ID`），再加一个 `passport-gateway`
   - 设备侧的 broker 地址、用户名、密码在固件的设置里填写
2. **安装**
   ```sh
   cd server
   python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
   cp .env.example .env    # 按注释填写
   ```
3. **语音识别模型**（默认本地 SenseVoice）：下载 sherpa-onnx 的 SenseVoice 模型（例如 `sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17`），把 `model.int8.onnx` 和 `tokens.txt` 放到 `models/sensevoice/`，或者在 `.env` 里改路径。不想跑本地模型就设 `PASSPORT_ASR=openai` 并填 `ASR_*`。
   自检：`.venv/bin/python asr.py some.wav`
4. **运行**：`.venv/bin/python gateway.py`；长期运行参考 `examples/passport-gateway.service`
5. **伙伴接入**：把 `passportctl` 软链到 PATH（venv 的话设 `PASSPORT_PYTHON=/path/.venv/bin/python`），再设置 `COMPANION_HOOK_CMD`，或者让你的 agent 盯着 `state/outbox.jsonl`
6. **测试**：`python3 -m pytest`，或者逐个 `python3 test_mines.py` / `test_bingo.py` / `test_asr.py`（不需要 broker）

`say` 的音频有两种下发方式：`PASSPORT_AUDIO_VIA=mqtt`（默认，ADPCM 分块走 MQTT，不需要公网 HTTP）或 `http`（设备从 `AUDIO_ORIGIN` 下载，网关在 `HTTP_BIND:HTTP_PORT` 提供文件，前面需要一个 TLS 反向代理）。

## 伙伴怎么回应

每个事件都带一个英文的 `hint` 字段，告诉伙伴发生了什么、该用哪条命令回应。

```sh
passportctl say "到家了吗？"            # 语音：TTS → 设备播放
passportctl msg "Dinner's on me."       # 短信气泡（只能英文/ASCII，≤280）
passportctl status                       # 设备状态 + 天气
passportctl mine layout A2,C1,D4         # 扫雷：埋雷（自动发送 sha256 承诺）
passportctl mine step C3 "Corners again?"  # 扫雷：踩对方一格，配一句英文内心独白（≤60）
passportctl mine state
passportctl bingo state                  # Bingo：今天的卡
passportctl bingo whisper "Proud of you." # 今天藏起来的悄悄话（连成第一条线时才出现）
passportctl bingo tick WATER
```

## 事件格式

钩子从 stdin 收到的（或 outbox 里每一行）都是一个 JSON 对象：

```json
{
  "id": "passport-1793725823885-a1b2c3",
  "kind": "passport.voice",
  "source": "passport",
  "device": "passport-1",
  "occurred_at": "2026-11-03T09:10:23.885+00:00",
  "payload": { "hint": "…", "...": "…" }
}
```

| kind | 什么时候 | payload 主要字段 |
| --- | --- | --- |
| `passport.voice` | 通话 / 短信 App 里发来一段语音 | `app`（`call`/`msg`）、`audio_path`、`seconds`、`missing_chunks`、`transcript`（识别失败或被过滤时为 null）、`asr` |
| `passport.mines` | 开局、轮到伙伴、结束、主人的雷阵补发 | `game`、`size`/`mines`/`first`（开局）、`board`（伙伴视角的文字棋盘）、`winner`、`your_layout`、`owner_layout`、`owner_layout_consistent` |
| `passport.bingo` | 今天的卡生成了（请写悄悄话）、第一次连线 | `day`、`size`、`cells`、`meanings`；连线时 `line`、`whisper`、`whisper_by`、`how`、`grid` |
| `passport.astro` | 占星骰子 | `planet`、`sign`、`house`、`symbols` |

钩子命令一次只运行一个（文件锁），退出码 0 表示送达；失败或没配置时事件写进 `PASSPORT_OUTBOX`。

## MQTT 主题

前缀 `<PASSPORT_TOPIC_PREFIX>/<PASSPORT_DEVICE_ID>/`：

| 主题 | 方向 | 内容 |
| --- | --- | --- |
| `status` | 设备 → 网关 | 设备状态 JSON（保存到 `state/status.json`） |
| `event` | 设备 → 网关 | `voice_begin/end/send/cancel`、`mine_*`、`bingo_*`、`astro_roll` |
| `audio` | 设备 → 网关 | 语音分块：12 字节头（clip id、序号、标志、ADPCM 预测值/索引）+ IMA-ADPCM |
| `command` | 网关 → 设备 | `play`、`msg`、`memo`、`mine_commit/result/companion_step/reveal`、`bingo_card` |
| `playchunk` | 网关 → 设备 | `say` 的 ADPCM 音频分块 |
| `weather` | 网关 → 设备 | 天气 JSON（retained） |

扫雷协议里双方叫 `owner`（拿着设备的人）和 `companion`（AI 伙伴）：设备发 `mine_new {first}`、`mine_ready`、`mine_step`、`mine_companion_result {cell, boom, winner}`、`mine_owner_reveal {layout}`、`mine_quit`。主人的雷阵在对局中从不离开设备。

## 隐私

语音片段、备忘录原文、游戏记录都只存在 `PASSPORT_STATE_DIR`（默认 `./state`）。语音片段默认保留 3 天、下发音频 1 小时（可调）。用云端识别 / LLM / TTS 时，相应的语音或文字会发给你配置的服务商。

---

## English summary

`server/` is the self-hosted gateway for the AI Passport Mini iPhone firmware. The device talks MQTT (TLS) to a broker; `gateway.py` subscribes to the device's topics, reassembles push-to-talk voice clips (IMA-ADPCM → WAV), transcribes them (local SenseVoice via sherpa-onnx, or any OpenAI-compatible `/audio/transcriptions` endpoint, with a guard that drops looping hallucinated transcripts), runs the minesweeper and daily Bingo logic, publishes weather (Open-Meteo, optional AQI), and hands events to your AI companion.

Events (`passport.voice`, `passport.mines`, `passport.bingo`, `passport.astro`) are neutral JSON with an English `hint`. They are piped to `COMPANION_HOOK_CMD` on stdin, or appended to `PASSPORT_OUTBOX` (JSONL) when no hook is set. The companion answers with the `passportctl` CLI (`say`, `msg`, `mine …`, `bingo …`). Everything is configured through environment variables; see `.env.example`. Tests: `python3 -m pytest` (no broker needed).

License: PolyForm Noncommercial 1.0.0 (see the repository's `NOTICE.md`).
