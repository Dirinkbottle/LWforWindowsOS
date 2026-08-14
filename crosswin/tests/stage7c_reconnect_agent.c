/* Verifies that a TCP peer disconnect/reconnect resynchronizes existing
 * mapped toplevels without tying their Weston lifetime to the transport. */
#define _POSIX_C_SOURCE 200809L
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { WINDOW_COUNT = 3, WINDOW_W = 300, WINDOW_H = 200 };
struct round {
	int fd;
	uint64_t wire_sequence;
	bool hello;
	uint8_t created, framed, presented;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage7c-reconnect-agent: %s\n", message);
	exit(EXIT_FAILURE);
}

static bool send_all(int fd, const uint8_t *bytes, size_t length)
{
	while (length != 0U) {
		ssize_t sent = send(fd, bytes, length, 0);
		if (sent > 0) { bytes += sent; length -= (size_t)sent; }
		else if (sent < 0 && errno == EINTR) continue;
		else return false;
	}
	return true;
}

static bool send_message(struct round *round, uint16_t type,
			 const uint8_t *payload, uint32_t length)
{
	CwBuffer encoded;
	bool ok;

	cw_buffer_init(&encoded);
	ok = cw_message_encode(&encoded, type, 0U, round->wire_sequence++, payload, length) &&
		send_all(round->fd, encoded.data, encoded.length);
	cw_buffer_destroy(&encoded);
	return ok;
}

static bool all_presented(const struct round *round)
{
	return round->presented == (uint8_t)((1U << WINDOW_COUNT) - 1U);
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload)
{
	struct round *round = context;

	switch (header->type) {
	case CW_MESSAGE_HELLO_ACK: {
		CwHelloAck ack;
		if (!cw_decode_hello_ack(payload, header->payload_length, &ack) ||
		    ack.selected_version != CW_PROTOCOL_VERSION)
			return false;
		round->hello = true;
		return true;
	}
	case CW_MESSAGE_OUTPUT_CONFIG: {
		CwOutputConfig config;

		return round->hello && cw_decode_output_config(payload,
			header->payload_length, &config) && config.scale_numerator == 1U &&
			config.scale_denominator == 1U;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;
		uint8_t bit;

		if (!round->hello || !cw_decode_window_create(payload, header->payload_length,
							       &create) || create.window_id == 0U ||
		    create.window_id > WINDOW_COUNT || create.parent_window_id != 0U ||
		    create.surface_width != WINDOW_W || create.surface_height != WINDOW_H)
			return false;
		bit = (uint8_t)(1U << (create.window_id - 1U));
		if (round->created & bit)
			return false;
		round->created |= bit;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;
		uint8_t bit;

		if (!cw_decode_window_frame(payload, header->payload_length, &frame) ||
		    frame.window_id == 0U || frame.window_id > WINDOW_COUNT ||
		    frame.width != WINDOW_W || frame.height != WINDOW_H ||
		    frame.stride != WINDOW_W * 4U ||
		    frame.pixel_bytes != (uint64_t)WINDOW_W * WINDOW_H * 4U)
			return false;
		bit = (uint8_t)(1U << (frame.window_id - 1U));
		if ((round->created & bit) == 0U)
			return false;
		round->framed |= bit;
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;
		uint8_t bit;
		uint8_t ack[16];

		if (!cw_decode_window_present(payload, header->payload_length, &present) ||
		    present.window_id == 0U || present.window_id > WINDOW_COUNT ||
		    !present.visible || present.source_w != WINDOW_W || present.source_h != WINDOW_H ||
		    present.destination_w != WINDOW_W || present.destination_h != WINDOW_H)
			return false;
		bit = (uint8_t)(1U << (present.window_id - 1U));
		if ((round->framed & bit) == 0U)
			return false;
		cw_store_u64_le(ack, present.window_id);
		cw_store_u64_le(ack + 8U, present.presentation_sequence);
		if (!send_message(round, CW_MESSAGE_WINDOW_PRESENT_ACK, ack, sizeof(ack)))
			return false;
		round->presented |= bit;
		return true;
	}
	case CW_MESSAGE_WINDOW_ACTIVATE: {
		CwWindowActivate activate;
		return cw_decode_window_activate(payload, header->payload_length, &activate) &&
		       activate.window_id != 0U && activate.window_id <= WINDOW_COUNT;
	}
	default:
		return false;
	}
}

static int connect_retry(uint16_t port)
{
	struct sockaddr_in address = { .sin_family = AF_INET };
	unsigned attempts;

	address.sin_port = htons(port);
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		return -1;
	for (attempts = 0U; attempts < 100U; ++attempts) {
		const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		if (fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
			return fd;
		if (fd >= 0)
			close(fd);
		(void)nanosleep(&delay, NULL);
	}
	return -1;
}

static void run_round(uint16_t port)
{
	struct round round = { .fd = -1, .wire_sequence = 1U };
	CwDecoder decoder;
	uint8_t hello[8] = {0};
	uint8_t bytes[8192];

	round.fd = connect_retry(port);
	if (round.fd < 0)
		fail("connect");
	cw_store_u16_le(hello, CW_PROTOCOL_VERSION);
	cw_store_u16_le(hello + 2U, CW_PROTOCOL_VERSION);
	cw_store_u32_le(hello + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
	if (!send_message(&round, CW_MESSAGE_HELLO, hello, sizeof(hello)))
		fail("send HELLO");
	cw_decoder_init(&decoder);
	while (!all_presented(&round)) {
		ssize_t received = recv(round.fd, bytes, sizeof(bytes), 0);
		if (received <= 0 || !cw_decoder_feed(&decoder, bytes, (size_t)received,
						       on_message, &round))
			fail("receive/decode");
	}
	cw_decoder_destroy(&decoder);
	close(round.fd);
}

int main(int argc, char **argv)
{
	long parsed = 44614L;
	char *end;
	const struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };

	if (argc == 2) {
		parsed = strtol(argv[1], &end, 10);
		if (*end != '\0' || parsed < 1L || parsed > 65535L)
			fail("usage: stage7c-reconnect-agent [port]");
	} else if (argc != 1) {
		fail("usage: stage7c-reconnect-agent [port]");
	}
	run_round((uint16_t)parsed);
	(void)nanosleep(&delay, NULL);
	run_round((uint16_t)parsed);
	puts("stage7c reconnect/resync integration: PASS");
	return EXIT_SUCCESS;
}
