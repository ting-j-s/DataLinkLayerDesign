# Lab1 数据链路层 Go-Back-N 滑动窗口协议 — 验收说明

## 1. 协议类型

**带 ACK 搭载（Piggybacking）的 Go-Back-N 全双工滑动窗口协议。**

- A/B 两端使用同一份代码，均可同时收发。
- 序号空间 256（MAX_SEQ=255），窗口大小 8，满足 WINDOW_SIZE < 序号空间。

## 2. 帧格式

```
DATA Frame:  KIND(1) | ACK(1) | SEQ(1) | DATA(256) | CRC(4)
ACK  Frame:  KIND(1) | ACK(1) | CRC(4)

调试输出中 `Send DATA %d %d` 打印的是 `seq` 和 `ack`，便于观察协议行为；但实际 C 结构体 `struct FRAME` 字段顺序为 `kind, ack, seq, data, padding`，其中 `padding` 存放 CRC32 校验值。
```

- **KIND**: 1=DATA, 2=ACK, 3=NAK
- **ACK**: 累计确认，值为接收方下一个期望帧号（即 k 表示 k 之前全部正确收到）
- **SEQ**: 发送方当前帧序号（8-bit, 0~255）
- **CRC**: CRC32 校验，覆盖帧头和数据

## 3. 核心机制

### 3.1 发送方滑动窗口

| 变量 | 含义 |
|------|------|
| `ack_expected` | 最早未确认帧（窗口下沿） |
| `next_frame_to_send` | 下一个可用序号（窗口上沿） |
| `out_buf[256][256]` | 发送缓存，每帧独立存储 |

- 窗口内未确认帧数: `outstanding = (next_frame_to_send - ack_expected) % 256`
- 窗口满（outstanding >= 8）时 `disable_network_layer()`
- 窗口有空位时 `enable_network_layer()`

### 3.2 累计 ACK

- ACK=k 表示序号 k 之前的所有帧均已被接收方正确接收。
- 发送方收到有效 ACK 后，连续推进 `ack_expected` 直到等于 ACK 值。
- ACK 合法性判断使用 `ack_acceptable()`: ACK 必须在 (ack_expected, ack_expected + outstanding] 区间内。

### 3.3 超时回退重传

- 仅对 `ack_expected`（窗口最老帧）维护一个定时器。
- 超时后从 `ack_expected` 到 `next_frame_to_send - 1` 全部重传（Go-Back-N 语义）。

### 3.4 接收方

- 仅接受 `seq == frame_expected` 的 DATA 帧。
- 乱序帧直接丢弃，不缓存。
- 每收到一个 DATA 帧都回复 ACK（搭载在 DATA 帧或独立 ACK 帧）。

### 3.5 CRC 错误处理

- CRC 校验失败的帧直接丢弃，不递交给网络层。

## 4. 编译

```bash
cd Lab1-linux
make clean
make
```

要求: GCC，`-O2 -Wall`，0 error 0 warning。

## 5. 验收测试

### 5.1 快速自测（短时间）

```bash
bash run_acceptance_tests.sh quick
```

列出 3 组短测命令，约 30 秒/组。**短测不替代正式 20 分钟测试。**

### 5.2 表 3 正式性能测试（每组 20 分钟）

```bash
bash run_acceptance_tests.sh formal
```

| 组 | 参数 | 说明 |
|----|------|------|
| 1 | `-u -d1 -t1200` | 无误码信道普通传输 |
| 2 | `-d1 -t1200` | 默认误码率(1e-5)普通传输 |
| 3 | `-u -f -d1 -t1200` | 无误码信道 + 洪水模式 |
| 4 | `-f -d1 -t1200` | 默认误码率(1e-5) + 洪水模式 |
| 5 | `-f -b1e-4 -d1 -t1200` | 误码率 1e-4 + 洪水模式 |

**操作步骤**（每组）:
1. 终端 1（A端）: 执行 A 端命令（启动并等待连接）
2. 终端 2（B端）: 等待 A 端监听到端口后，执行 B 端命令
3. 观察 20 分钟，确认双方均 `Quit.` 正常退出

## 6. 测试通过判据

1. 程序持续输出: `.... xxx packets received, xxxx bps, xx.xx%, Err xx (...)`
2. A/B 两端都正常 `Quit.`
3. 无误码信道中 `Err` 为 0
4. 不出现以下错误:
   - `Network Layer received a bad packet from data link layer`
   - `Network Layer: incorrect packet length`
   - `Physical Layer Sending Queue overflow`
   - `Memory used by 'protocol.lib' is corrupted by your program`
5. 有误码信道允许 `Bad CRC Checksum` 和 `DATA timeout`
6. 超时后应从 `ack_expected` 开始连续重传窗口内所有未确认帧
7. 洪水模式下无死锁，利用率稳定

## 7. 线路利用率

线路利用率的计算方法（指导书定义）：

```
利用率 = 实际吞吐量 / 信道带宽
```

- 信道带宽: 8000 bps
- 实际吞吐量: 程序每 2.16 秒输出的 `bps` 值
- 日志中每 8 个包统计一次，显示当前累计 bps 和百分比

示例: 日志显示 `7557 bps, 94.47%` 表示利用率为 94.47%。

## 8. 常见异常

| 异常信息 | 含义 |
|----------|------|
| `Bad CRC Checksum` | 帧校验失败，物理层误码导致，协议已正确丢弃 |
| `DATA timeout` | 定时器超时，触发 Go-Back-N 重传 |
| `TCP Disconnected.` | 对端进程退出或网络断开 |

## 9. 验收环境

- 操作系统: Linux (Debian/Ubuntu)
- 编译器: GCC
- 实验库: protocol.lib v4.0
- 信道模型: 8000 bps, 270ms 传播时延

## 10. 提交前清理

```bash
cd Lab1-linux
make clean          # 删除 .o 和 datalink 可执行文件
rm -f *.log         # 删除运行日志（如不需要提交）
rm -f settings.local.json  # 删除本地配置文件
```
