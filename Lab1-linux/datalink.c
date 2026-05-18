#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER 2000

/* Sliding window Go-Back-N protocol constants */
#define MAX_SEQ 255       /* 8-bit sequence number range: 0..255 */
#define WINDOW_SIZE 8     /* sender window size */
#define NBUF (MAX_SEQ + 1) /* total slots in sequence space (256) */

typedef unsigned char seq_nr;

struct FRAME {
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;  /* cumulative ack: next expected seq */
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int padding;
};

/* Sender state */
static seq_nr ack_expected = 0;       /* send_base: oldest unacked frame */
static seq_nr next_frame_to_send = 0; /* next available seq number */
static unsigned char out_buf[NBUF][PKT_LEN]; /* buffered outgoing packets */

/* Receiver state */
static seq_nr frame_expected = 0;     /* next expected seq number */

static int phl_ready = 0;

/*
 * Count outstanding frames in sender window: frames in [ack_expected, next_frame_to_send).
 */
static int outstanding_frames(void)
{
	return (next_frame_to_send - ack_expected + NBUF) % NBUF;
}

/*
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

static void put_frame(unsigned char *frame, int len)
{
	*(unsigned int *)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

/*
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

static void send_ack_frame(void)
{
	struct FRAME s;

	s.kind = FRAME_ACK;
	s.ack = frame_expected;

	dbg_frame("Send ACK  %d\n", s.ack);

	put_frame((unsigned char *)&s, 2);
}

int main(int argc, char **argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;

	protocol_init(argc, argv);
	lprintf("Designed by Jiang Yanjun, build: " __DATE__ "  "__TIME__"\n");

	disable_network_layer();

	for (;;) {
		event = wait_for_event(&arg);

		switch (event) {
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
			if (len < 5 || crc32((unsigned char *)&f, len) != 0) {
				dbg_event("**** Receiver Error, Bad CRC Checksum\n");
				break;
			}

			/* ACK frame: cumulative ACK, advance ack_expected */
			if (f.kind == FRAME_ACK) {
				dbg_frame("Recv ACK  %d\n", f.ack);
				if (ack_acceptable(f.ack)) {
					stop_timer(0);
					while (ack_expected != f.ack) {
						ack_expected = (ack_expected + 1) % NBUF;
					}
					if (ack_expected != next_frame_to_send)
						start_timer(0, DATA_TIMER);
				}
			}

			/* DATA frame */
			if (f.kind == FRAME_DATA) {
				dbg_frame("Recv DATA %d %d, ID %d\n", f.seq,
					  f.ack, *(short *)f.data);

				/* Piggybacked ACK: advance sender window */
				if (ack_acceptable(f.ack)) {
					stop_timer(0);
					while (ack_expected != f.ack) {
						ack_expected = (ack_expected + 1) % NBUF;
					}
					if (ack_expected != next_frame_to_send)
						start_timer(0, DATA_TIMER);
				}

				/* Go-Back-N receiver: accept only expected seq */
				if (f.seq == frame_expected) {
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
				while (s != next_frame_to_send) {
					send_data_frame_n(s, frame_expected);
					s = (s + 1) % NBUF;
				}
				if (ack_expected != next_frame_to_send)
					start_timer(0, DATA_TIMER);
			}
			break;
		}

		/* Control network layer flow based on sender window */
		if (phl_ready && outstanding_frames() < WINDOW_SIZE)
			enable_network_layer();
		else
			disable_network_layer();
	}
}
