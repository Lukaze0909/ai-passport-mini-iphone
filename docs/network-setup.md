# 配网：把 WiFi 和服务器写进小手机

这份说明写给你，也写给你的 AI 助手。把它整份发给 AI，告诉它你的 WiFi 和服务器信息，它可以照着做完。

## 不配网会怎样

固件里没有内置任何网络配置，手机上也没有配网页面。不配网时：

| 功能 | 不配网 | 配网 + 自建服务 |
| --- | --- | --- |
| 锁屏、主屏、设置 | 能用 | 能用 |
| 本地功能（如占星骰子摇骰、释放引导） | 能用 | 能用 |
| 电话、短信、备忘录整理 | 不能用 | 能用 |
| 天气、对战扫雷、Bingo 同步、一键发给 AI 解读 | 不能用 | 能用 |

"能用"的联网功能都需要你先部署 [`server/`](../server/README.md)。

## 需要准备

- ESP-IDF v5.5（编译固件时已经装好）
- 一个 2.4 GHz WiFi，可选再加一个手机热点
- 一个 MQTT broker 和设备账号，部署方法见 [`server/README.md`](../server/README.md)

## 第一步：写配置文件

复制 [`server/examples/device-config.csv`](../server/examples/device-config.csv)，改名为 `device-config.csv`，把值换成你自己的：

| 键 | 含义 |
| --- | --- |
| `ssid0` / `pass0` | 家里 WiFi 名和密码（优先连） |
| `ssid1` / `pass1` | 手机热点名和密码（连不上家里时用，不用可删掉这两行） |
| `mqtt_uri` | broker 地址，例如 `mqtts://broker.example.com:8883`，证书要能被公共 CA 验证 |
| `mqtt_user` / `mqtt_pass` | 设备账号；主题前缀是 `passport/<mqtt_user>/` |
| `wifi`（`classic` 命名空间） | `1` 表示开机就打开 WiFi |

`device-config.csv` 和生成的 `device-nvs.bin` 里有明文密码，不要提交到 Git。

## 第二步：生成并写入

```sh
python $IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py \
  generate device-config.csv device-nvs.bin 0x6000
python -m esptool --chip esp32c3 -p <端口> write_flash 0x9000 device-nvs.bin
```

`0x9000` 和 `0x6000` 来自 [`partitions.csv`](../partitions.csv) 的 `nvs` 分区。

**这一步会清空这块分区里已有的内容**，包括锁屏时长、Bingo 进度等设置。短信和备忘录在单独的分区里，不受影响。要改配置时重新生成写入即可。

## 第三步：确认

重启小手机，打开 设置：

- 第一行显示"家里 ✓"或"热点 ✓"，说明 WiFi 连上了
- 下一行显示"已连上"，说明服务器连上了

显示"未配置网络"：配置没写进去，检查写入地址。一直"连接中…"：检查 WiFi 名和密码，确认是 2.4 GHz。服务器"未连上"：检查 `mqtt_uri`、账号和证书。
