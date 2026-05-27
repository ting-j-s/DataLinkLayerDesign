#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"
/*
MAX_SEQ  = 255    // 序号空间 0..255
WINDOW_SIZE = 8   // 发送窗口大小
NBUF = 256        // 缓冲区槽位数
DATA_TIMER = 2000 // 超时 2000ms
*/
#define DATA_TIMER 2000

/* Sliding window Go-Back-N protocol constants */
#define MAX_SEQ 255		   /* 8-bit sequence number range: 0..255 */
#define WINDOW_SIZE 8	   /* sender window size */
#define NBUF (MAX_SEQ + 1) /* total slots in sequence space (256) */

typedef unsigned char seq_nr;
/*
3.1 帧结构体 (17-23行)

struct FRAME {
	unsigned char kind;      // 1=DATA, 2=ACK
	unsigned char ack;       // 累计确认号
	unsigned char seq;       // 发送序号
	unsigned char data[256]; // 数据载荷
	unsigned int  padding;   // 存放 CRC32（不是填充）
};
*/
struct FRAME
{
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;	/* cumulative ack: next expected seq */
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int padding;
};
/*
// 发送方
ack_expected = 0       // 窗口下沿：最早未确认帧
next_frame_to_send = 0 // 窗口上沿：下一个可用的序号
out_buf[256][256]      // 发送缓冲区：每个序号存一份完整包

// 接收方
frame_expected = 0     // 期望接收的下一个序号
phl_ready             // 物理层是否就绪
*/

/* Sender state */
static seq_nr ack_expected = 0;				 /* send_base: oldest unacked frame */
static seq_nr next_frame_to_send = 0;		 /* next available seq number */
static unsigned char out_buf[NBUF][PKT_LEN]; /* buffered outgoing packets */

/* Receiver state */
static seq_nr frame_expected = 0; /* next expected seq number */

static int phl_ready = 0;

/*
窗口内未确认帧数
 * Count outstanding frames in sender window: frames in [ack_expected, next_frame_to_send).
 */
static int outstanding_frames(void)
{
	return (next_frame_to_send - ack_expected + NBUF) % NBUF;
}

/*
ACK 合法性判断：
ack 必须在 (ack_expected, ack_expected + outstanding] 区间内
即 ACK 必须确认至少 1 个、最多 outstanding 个未确认帧。
 * Check if an ACK is valid for the current sender window.
 * ACK=k means all frames before k have been received (cumulative ACK).
 * Returns true iff ack is in (ack_expected, ack_expected + outstanding].
 */
static bool ack_acceptable(seq_nr ack)
{
	int outstanding = outstanding_frames();
	int distance;

	if (outstanding == 0)
		return false;

	distance = (ack - ack_expected + NBUF) % NBUF;

	return distance > 0 && distance <= outstanding;
}
// 计算 CRC32 后调用物理层发送
static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

/*
发送指定序号的数据帧：

从 out_buf[seq] 取数据
搭载 frame_expected 作为 ACK（Piggybacking）
如果该帧是窗口最老帧（seq == ack_expected），启动定时器
 * Send a data frame with given sequence number and piggybacked ACK.
 * Only starts a timer if this is the oldest unacked frame (ack_expected).
 */
static void send_data_frame_n(seq_nr seq, seq_nr ack)
{
	struct FRAME s;

	s.kind = FRAME_DATA;
	s.seq = seq;
	s.ack = ack;
	memcpy(s.data, out_buf[seq], PKT_LEN);

	dbg_frame("Send DATA %d %d, ID %d\n", s.seq, s.ack, *(short *)s.data);

	put_frame((unsigned char *)&s, 3 + PKT_LEN);

	if (seq == ack_expected)
		start_timer(0, DATA_TIMER);
}
// 发送纯 ACK 帧
static void send_ack_frame(void)
{
	struct FRAME s;

	s.kind = FRAME_ACK;
	s.ack = frame_expected;

	dbg_frame("Send ACK  %d\n", s.ack);

	put_frame((unsigned char *)&s, 2);
}
// 主循环
int main(int argc, char **argv)
{
	int event, arg;
	struct FRAME f; // 栈上复用，每轮覆盖
	int len = 0;

	protocol_init(argc, argv); // TCP连接建立、参数解析
	lprintf("Designed by Jiang Yanjun, build: " __DATE__ "  " __TIME__ "\n");

	disable_network_layer(); // 初始关闭网络层，等物理层就绪后才开放，防止往空的发送队列写数据但发不出去

	for (;;)
	{
		event = wait_for_event(&arg);
		/*
		1. 等事件 — wait_for_event(&arg) 阻塞，直到以下五种之一发生：
		NETWORK_LAYER_READY	流控开放 + 网络层有包要发
		PHYSICAL_LAYER_READY	物理层发送队列低于水位
		FRAME_RECEIVED	收到完整帧（已过 270ms 延迟）
		DATA_TIMEOUT	窗口最老帧 2000ms 未确认
		*/

		/*
		处理事件 — 四种响应：

		NETWORK_LAYER_READY：取包存入 out_buf → 发送（搭载 frame_expected 作为 ACK）→ next_frame_to_send++
		PHYSICAL_LAYER_READY：置 phl_ready = 1
		FRAME_RECEIVED：CRC 错则丢弃；是 ACK 则推进发送窗口；是 DATA 则处理搭载 ACK + 序号匹配则递交网络层 + 回复 ACK
		DATA_TIMEOUT：从 ack_expected 到 next_frame_to_send-1 全部重传
		*/
		switch (event)
		{
		case NETWORK_LAYER_READY:
			get_packet(out_buf[next_frame_to_send]);
			send_data_frame_n(next_frame_to_send, frame_expected);
			next_frame_to_send = (next_frame_to_send + 1) % NBUF;
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char *)&f, sizeof f);
			if (len < 5 || crc32((unsigned char *)&f, len) != 0)
			{
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				break;
			}

			/* ACK frame: cumulative ACK, advance ack_expected */
			if (f.kind == FRAME_ACK)
			{
				dbg_frame("Recv ACK  %d\n", f.ack);
				if (ack_acceptable(f.ack))
				{
					stop_timer(0);
					while (ack_expected != f.ack)
					{
						ack_expected = (ack_expected + 1) % NBUF;
					}
					if (ack_expected != next_frame_to_send)
						start_timer(0, DATA_TIMER);
				}
			}

			/* DATA frame */
			if (f.kind == FRAME_DATA)
			{
				dbg_frame("Recv DATA %d %d, ID %d\n", f.seq,
						  f.ack, *(short *)f.data);

				/* Piggybacked ACK: advance sender window */
				if (ack_acceptable(f.ack))
				{
					stop_timer(0);
					while (ack_expected != f.ack)
					{
						ack_expected = (ack_expected + 1) % NBUF;
					}
					if (ack_expected != next_frame_to_send)
						start_timer(0, DATA_TIMER);
				}

				/* Go-Back-N receiver: accept only expected seq */
				if (f.seq == frame_expected)
				{
					put_packet(f.data, len - 7);
					frame_expected = (frame_expected + 1) % NBUF;
				}
				send_ack_frame();
			}
			break;

		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", arg);
			/* Go-Back-N: retransmit all outstanding frames */
			{
				seq_nr s = ack_expected;
				while (s != next_frame_to_send)
				{
					send_data_frame_n(s, frame_expected);
					s = (s + 1) % NBUF;
				}
				if (ack_expected != next_frame_to_send)
					start_timer(0, DATA_TIMER);
			}
			break;
		}

		/*
		流控判断 — 窗口未满且物理层就绪 → enable_network_layer()，
		否则 disable_network_layer()。
		Control network layer flow based on sender window */
		if (phl_ready && outstanding_frames() < WINDOW_SIZE)
			enable_network_layer();
		else
			disable_network_layer();
	}
}
/*
for(;;) 内每轮就是三步：

wait_for_event — 阻塞等到一个事件（网络层就绪 / 物理层就绪 / 帧到达 / 超时）

switch(event) — 处理这个事件，更新 ack_expected、next_frame_to_send、frame_expected 等全局状态

流控 — phl_ready && 窗口未满 ? enable : disable_network_layer()

三步一轮，永不退出，直到 protocol.c 内部时钟到期打印 Quit. 并 exit(0)。
*/