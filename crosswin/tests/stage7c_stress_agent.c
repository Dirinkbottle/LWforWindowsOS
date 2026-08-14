/* 100 create/destroy cycles must keep monotonically increasing IDs and no
 * stale presentation object on the Stage 7C transport path. */
#define _POSIX_C_SOURCE 200809L
#include "../common/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

enum { CYCLES = 100, WINDOW_W = 300, WINDOW_H = 200 };
struct agent {
	int fd;
	uint64_t wire_sequence;
	unsigned created, destroyed;
	uint64_t current_id;
	bool hello, frame, present;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage7c-stress-agent: %s\n", message);
	exit(EXIT_FAILURE);
}

static bool send_all(int fd, const uint8_t *bytes, size_t length)
{
	while (length != 0U) {
		ssize_t sent = send(fd, bytes, length, 0);

		if (sent > 0) {
			bytes += sent;
			length -= (size_t)sent;
		} else if (sent < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

static bool send_ack(struct agent *agent, const CwWindowPresent *present)
{
	CwBuffer encoded;
	uint8_t payload[16];
	bool ok;

	cw_store_u64_le(payload, present->window_id);
	cw_store_u64_le(payload + 8U, present->presentation_sequence);
	cw_buffer_init(&encoded);
	ok = cw_message_encode(&encoded, CW_MESSAGE_WINDOW_PRESENT_ACK, 0U,
			       agent->wire_sequence++, payload, sizeof(payload)) &&
		send_all(agent->fd, encoded.data, encoded.length);
	cw_buffer_destroy(&encoded);
	return ok;
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload)
{
	struct agent *agent = context;

	switch (header->type) {
	case CW_MESSAGE_HELLO_ACK: {
		CwHelloAck ack;
		if (!cw_decode_hello_ack(payload, header->payload_length, &ack) ||
		    ack.selected_version != CW_PROTOCOL_VERSION)
			return false;
		agent->hello = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;
		if (!agent->hello || agent->current_id != 0U || agent->created >= CYCLES ||
		    !cw_decode_window_create(payload, header->payload_length, &create) ||
		    create.window_id != (uint64_t)agent->created + 1U ||
		    create.parent_window_id != 0U || create.surface_width != WINDOW_W ||
		    create.surface_height != WINDOW_H)
			return false;
		agent->current_id = create.window_id;
		agent->created++;
		agent->frame = false;
		agent->present = false;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;
		if (!cw_decode_window_frame(payload, header->payload_length, &frame) ||
		    frame.window_id != agent->current_id || frame.width != WINDOW_W ||
		    frame.height != WINDOW_H || frame.stride != WINDOW_W * 4U ||
		    frame.pixel_bytes != (uint64_t)WINDOW_W * WINDOW_H * 4U)
			return false;
		agent->frame = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;
		if (!cw_decode_window_present(payload, header->payload_length, &present) ||
		    present.window_id != agent->current_id || !agent->frame || !present.visible ||
		    present.source_x != 0 || present.source_y != 0 || present.source_w != WINDOW_W ||
		    present.source_h != WINDOW_H || present.destination_w != WINDOW_W ||
		    present.destination_h != WINDOW_H || !send_ack(agent, &present))
			return false;
		agent->present = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_ACTIVATE: {
		CwWindowActivate activate;
		return cw_decode_window_activate(payload, header->payload_length, &activate) &&
		       activate.window_id == agent->current_id;
	}
	case CW_MESSAGE_WINDOW_DESTROY:
		if (header->payload_length != 8U || cw_load_u64_le(payload) != agent->current_id ||
		    !agent->frame || !agent->present)
			return false;
		agent->destroyed++;
		agent->current_id = 0U;
		return true;
	default:
		return false;
	}
}

int main(int argc, char **argv)
{
	struct agent agent = { .fd = -1, .wire_sequence = 1U };
	struct sockaddr_in address = { .sin_family = AF_INET };
	CwDecoder decoder;
	uint8_t hello[8] = {0};
	uint8_t bytes[8192];
	long port = 44613L;
	char *end;

	if (argc == 2) {
		port = strtol(argv[1], &end, 10);
		if (*end != '\0' || port < 1L || port > 65535L)
			fail("usage: stage7c-stress-agent [port]");
	} else if (argc != 1) {
		fail("usage: stage7c-stress-agent [port]");
	}
	address.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		fail("inet_pton");
	agent.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (agent.fd < 0 || connect(agent.fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		fail("connect");
	cw_store_u16_le(hello, CW_PROTOCOL_VERSION);
	cw_store_u16_le(hello + 2U, CW_PROTOCOL_VERSION);
	cw_store_u32_le(hello + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
	{
		CwBuffer encoded;
		bool ok;

		cw_buffer_init(&encoded);
		ok = cw_message_encode(&encoded, CW_MESSAGE_HELLO, 0U, agent.wire_sequence++,
				       hello, sizeof(hello)) &&
			send_all(agent.fd, encoded.data, encoded.length);
		cw_buffer_destroy(&encoded);
		if (!ok)
			fail("send HELLO");
	}
	cw_decoder_init(&decoder);
	while (agent.destroyed != CYCLES) {
		ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);
		if (received <= 0 || !cw_decoder_feed(&decoder, bytes, (size_t)received,
						       on_message, &agent))
			fail("receive/decode");
	}
	cw_decoder_destroy(&decoder);
	if (agent.created != CYCLES || agent.current_id != 0U)
		fail("cycle count");
	close(agent.fd);
	puts("stage7c 100-window lifecycle stress: PASS");
	return EXIT_SUCCESS;
}
