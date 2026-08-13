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

struct agent {
	int fd;
	bool hello, created, initial_present, moved_present, released, destroyed;
	uint32_t frames, presents;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage6-agent: %s\n", message);
	exit(EXIT_FAILURE);
}

static bool send_all(int fd, const uint8_t *bytes, size_t length)
{
	while (length > 0) {
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

static bool send_message(struct agent *agent, uint16_t type, uint64_t sequence,
			 const uint8_t *payload, uint32_t length)
{
	CwBuffer encoded;
	bool ok;

	cw_buffer_init(&encoded);
	ok = cw_message_encode(&encoded, type, 0, sequence, payload, length) &&
		send_all(agent->fd, encoded.data, encoded.length);
	cw_buffer_destroy(&encoded);
	return ok;
}

static bool send_ack(struct agent *agent, uint64_t wire_sequence,
		     uint64_t presentation_sequence)
{
	uint8_t payload[16];

	cw_store_u64_le(payload, 1);
	cw_store_u64_le(payload + 8, presentation_sequence);
	return send_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, wire_sequence,
				payload, sizeof(payload));
}

static void store_location(uint8_t payload[40], uint64_t presentation_sequence,
			   int32_t client_x, int32_t client_y,
			   int32_t output_x, int32_t output_y)
{
	cw_store_u64_le(payload, 1);
	cw_store_u64_le(payload + 8, presentation_sequence);
	cw_store_i32_le(payload + 16, client_x);
	cw_store_i32_le(payload + 20, client_y);
	cw_store_i32_le(payload + 24, output_x);
	cw_store_i32_le(payload + 28, output_y);
	cw_store_u64_le(payload + 32, 1000);
}

static bool send_button(struct agent *agent, uint64_t wire_sequence,
			uint64_t presentation_sequence, uint32_t state,
			int32_t client_x, int32_t client_y,
			int32_t output_x, int32_t output_y)
{
	uint8_t payload[48] = {0};

	store_location(payload, presentation_sequence, client_x, client_y,
		       output_x, output_y);
	cw_store_u32_le(payload + 40, CW_POINTER_BUTTON_LEFT);
	cw_store_u32_le(payload + 44, state);
	return send_message(agent, CW_MESSAGE_POINTER_BUTTON, wire_sequence,
				payload, sizeof(payload));
}

static bool send_motion(struct agent *agent, uint64_t wire_sequence,
			uint64_t presentation_sequence, int32_t client_x,
			int32_t client_y, int32_t output_x, int32_t output_y)
{
	uint8_t payload[48] = {0};

	store_location(payload, presentation_sequence, client_x, client_y,
		       output_x, output_y);
	cw_store_u32_le(payload + 40, 1);
	return send_message(agent, CW_MESSAGE_POINTER_MOTION, wire_sequence,
				payload, sizeof(payload));
}

static bool on_message(void *context, const CwHeader *header,
		       const uint8_t *payload)
{
	struct agent *agent = context;
	CwWindowPresent present;

	switch (header->type) {
	case CW_MESSAGE_HELLO_ACK: {
		CwHelloAck hello;
		if (!cw_decode_hello_ack(payload, header->payload_length, &hello) ||
		    hello.selected_version != CW_PROTOCOL_VERSION)
			fail("bad HELLO_ACK");
		agent->hello = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;
		if (!agent->hello || !cw_decode_window_create(payload,
			header->payload_length, &create) || create.window_id != 1 ||
		    create.surface_width != 800 || create.surface_height != 600)
			fail("bad WINDOW_CREATE");
		agent->created = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;
		if (!agent->created || !cw_decode_window_frame(payload,
			header->payload_length, &frame) || frame.window_id != 1 ||
		    frame.width != 800 || frame.height != 600 || frame.stride != 3200 ||
		    frame.pixel_format != CW_PIXEL_FORMAT_BGRA8888 ||
		    frame.pixel_bytes != 1920000)
			fail("bad WINDOW_FRAME");
		agent->frames++;
		if (agent->frames > 1)
			fail("drag must not retransmit WINDOW_FRAME");
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT:
		if (!cw_decode_window_present(payload, header->payload_length, &present) ||
		    !agent->created || agent->frames != 1 || present.window_id != 1)
			fail("bad WINDOW_PRESENT");
		if (agent->presents == 0) {
			const struct timespec delivery_delay = {0, 200000000L};
			if (!present.visible || present.source_x != 0 || present.source_y != 0 ||
			    present.source_w != 800 || present.source_h != 600 ||
			    present.destination_x != 80 || present.destination_y != 80 ||
			    present.destination_w != 800 || present.destination_h != 600 ||
			    !send_ack(agent, 2, present.presentation_sequence) ||
			    !send_button(agent, 3, present.presentation_sequence,
					 CW_BUTTON_PRESSED, 100, 10, 180, 90))
				fail("initial present or press");
			agent->initial_present = true;
			/* Let the client consume the real wl_pointer.button serial and
			 * issue xdg_toplevel.move before the first motion arrives. */
			if (nanosleep(&delivery_delay, NULL) < 0)
				fail("nanosleep");
			if (!send_motion(agent, 4, present.presentation_sequence,
					 100, 10, -300, 90))
				fail("drag motion");
			agent->presents++;
			return true;
		}
		if (agent->presents == 1) {
			if (!present.visible || present.source_x != 400 || present.source_y != 0 ||
			    present.source_w != 400 || present.source_h != 600 ||
			    present.destination_x != 0 || present.destination_y != 80 ||
			    present.destination_w != 400 || present.destination_h != 600 ||
			    !send_ack(agent, 5, present.presentation_sequence) ||
			    !send_motion(agent, 6, present.presentation_sequence,
					 100, 10, -699, 90))
				fail("50/50 presentation");
			agent->moved_present = true;
			agent->presents++;
			return true;
		}
		if (agent->presents == 2) {
			if (!present.visible || present.source_x != 799 || present.source_y != 0 ||
			    present.source_w != 1 || present.source_h != 600 ||
			    present.destination_x != 0 || present.destination_y != 80 ||
			    present.destination_w != 1 || present.destination_h != 600 ||
			    !send_ack(agent, 7, present.presentation_sequence) ||
			    !send_motion(agent, 8, present.presentation_sequence,
					 100, 10, -700, 90))
				fail("one-pixel remote presentation");
			agent->presents++;
			return true;
		}
		if (agent->presents == 3) {
			if (present.visible || present.source_x != 0 || present.source_y != 0 ||
			    present.source_w != 0 || present.source_h != 0 ||
			    present.destination_x != 0 || present.destination_y != 0 ||
			    present.destination_w != 0 || present.destination_h != 0 ||
			    !send_ack(agent, 9, present.presentation_sequence) ||
			    !send_motion(agent, 10, present.presentation_sequence,
					 100, 10, 280, 90))
				fail("fully-local presentation");
			agent->presents++;
			return true;
		}
		if (agent->presents == 4) {
			if (!present.visible || present.source_x != 0 || present.source_y != 0 ||
			    present.source_w != 800 || present.source_h != 600 ||
			    present.destination_x != 180 || present.destination_y != 80 ||
			    present.destination_w != 800 || present.destination_h != 600 ||
			    !send_ack(agent, 11, present.presentation_sequence) ||
			    !send_button(agent, 12, present.presentation_sequence,
					 CW_BUTTON_RELEASED, 100, 10, 280, 90))
				fail("fully-remote presentation or release");
			agent->released = true;
			agent->presents++;
			return true;
		}
		fail("unexpected extra WINDOW_PRESENT");
		return false;
	case CW_MESSAGE_WINDOW_DESTROY:
		if (!agent->moved_present || !agent->released || agent->presents != 5 ||
		    agent->frames != 1 ||
		    header->payload_length != 8 || cw_load_u64_le(payload) != 1)
			fail("bad WINDOW_DESTROY");
		agent->destroyed = true;
		return true;
	default:
		return false;
	}
}

int main(int argc, char **argv)
{
	struct agent agent = {0};
	struct sockaddr_in address = { .sin_family = AF_INET };
	CwDecoder decoder;
	uint8_t hello[8] = {0};
	uint8_t bytes[8192];
	char *end;
	long port = 44606;

	if (argc == 2) {
		port = strtol(argv[1], &end, 10);
		if (!*argv[1] || *end || port < 1 || port > 65535)
			fail("usage: stage6-agent [port]");
	} else if (argc != 1) {
		fail("usage: stage6-agent [port]");
	}
	address.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		return EXIT_FAILURE;
	agent.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (agent.fd < 0 || connect(agent.fd, (struct sockaddr *)&address,
				 sizeof(address)) < 0)
		fail("connect");
	cw_store_u16_le(hello, CW_PROTOCOL_VERSION);
	cw_store_u16_le(hello + 2, CW_PROTOCOL_VERSION);
	cw_store_u32_le(hello + 4, CW_PIXEL_FORMAT_MASK_BGRA8888);
	if (!send_message(&agent, CW_MESSAGE_HELLO, 1, hello, sizeof(hello)))
		fail("HELLO send");
	cw_decoder_init(&decoder);
	while (!agent.destroyed) {
		ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);
		if (received <= 0 || !cw_decoder_feed(&decoder, bytes,
				(size_t)received, on_message, &agent))
			fail("receive/decode");
	}
	cw_decoder_destroy(&decoder);
	close(agent.fd);
	puts("stage6 real xdg_toplevel.move integration: PASS");
	return EXIT_SUCCESS;
}
