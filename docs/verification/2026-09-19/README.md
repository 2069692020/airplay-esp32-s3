# 验证取证归档 — 2026-09-19

这一天两轮工作的**原始**串口/网络/构建/烧录日志，以及产生它们的脚本。
配套的结论记录在仓库内的 `CLAUDE.md`（Landmines 一节）；作者私有的工程日志不发布。

## ⚠️ 全部文件已脱敏

内网 IP、MAC/BSSID、家庭 SSID、个人路径与用户名都已替换为 RFC 保留段 /
明显合成的值，映射是确定性的（同一原值→同一假值），所以日志之间的交叉关联仍成立。
`192.168.4.1` 未替换——那是 ESP32 SoftAP 的上游默认值。反向对照表只留在作者本地。

## 这批证据证明什么

| 文件 | 内容 |
|---|---|
| `soak_serial_main.log` | 两小时串口持续采集（8354 行）。**0 次** panic / backtrace / 栈溢出 / 堆损坏 / 看门狗 / assert |
| `soak_harness.log` / `.py` | 87 轮功能循环，730 PASS / 1 FAIL（该 FAIL 是作者自己烧录触发的重启）/ 0 SKIP；heap 净 −76 B |
| `soak_split.txt` / `soak_final.txt` / `soak_analysis.txt` | 上述日志的统计脚本与输出，含换镜像前后的 W/E 对比 |
| `verify_rtsp_clamp*.py/.txt` | 无鉴权 RTSP ANNOUNCE 打畸形 SDP，验证声道数/采样率/帧长钳位命中且不误杀合法流、设备不重启 |
| `verify_radio_*.py/.txt` `verify_server.py` | 本机假电台：验证停滞自愈不再无限重播（改前 3 次请求/45 s，改后 1 次并报错）、m3u 相对条目按歌单目录解析 |
| `verify_sd_cycles*.txt` | 8 轮 SD 播放↔AirPlay 的 I2S 交还循环 + 每轮 heap（改前 −18528 B，改后 −16 B） |
| `build_review*.log` `soak_build2.log` | 各次构建日志，`grep -c "warning:"` 均为 0 |
| `flash_*.log` `soak_flash_*.log` | 各次烧录日志（含"只烧改过的分区"的范围证明） |
| `read_nvs.log` | 全量重烧**前**把 NVS 读出来备份的日志（NVS 本身不入库） |

有意**不**归档：NVS 原始镜像（含裸 WiFi 凭据）、本机编译的 `.bin`（镜像里嵌着热点口令）、
以及作者定为不发布的两份本地 markdown 日志。
