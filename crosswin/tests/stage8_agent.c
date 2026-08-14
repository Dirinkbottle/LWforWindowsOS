/* Stage 8 Linux-side protocol peer. It proves that a Windows-originated
 * wheel plus a focused KEY_A press/release reaches the dedicated Crosswin
 * Weston input seat. The paired Wayland client asserts receipt. */
#define _POSIX_C_SOURCE 200809L

#include "../common/protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
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
	uint64_t wire_sequence;
	uint64_t window_id;
	bool hello;
	bool output_config;
	bool created;
	bool framed;
	bool sent_input;
	bool destroyed;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage8-agent: %s\n", message);
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

static bool send_message(struct agent *agent, uint16_t type,
			 const uint8_t *payload, uint32_t length)
{
	CwBuffer encoded;
	bool ok;

	cw_buffer_init(&encoded);
	ok = cw_message_encode(&encoded, type, 0U, agent->wire_sequence++,
			       payload, length) &&
		send_all(agent->fd, encoded.data, encoded.length);
	cw_buffer_destroy(&encoded);
	return ok;
}

static bool send_hello(struct agent *agent)
{
	uint8_t payload[8] = {0};

	cw_store_u16_le(payload, CW_PROTOCOL_VERSION);
	cw_store_u16_le(payload + 2U, CW_PROTOCOL_VERSION);
	cw_store_u32_le(payload + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
	return send_message(agent, CW_MESSAGE_HELLO, payload, sizeof(payload));
}

static bool send_ack(struct agent *agent, const CwWindowPresent *present)
{
	uint8_t payload[16];

	cw_store_u64_le(payload, present->window_id);
	cw_store_u64_le(payload + 8U, present->presentation_sequence);
	return send_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, payload, sizeof(payload));
}

static bool send_wheel(struct agent *agent, const CwWindowPresent *present)
{
	uint8_t payload[48] = {0};
	int32_t client_x;
	int32_t client_y;

	if (!present->visible || present->destination_w <= 0 || present->destination_h <= 0)
		return false;
	client_x = present->destination_w / 2;
	client_y = present->destination_h / 2;
	cw_store_u64_le(payload, present->window_id);
	cw_store_u64_le(payload + 8U, present->presentation_sequence);
	cw_store_i32_le(payload + 16U, client_x);
	cw_store_i32_le(payload + 20U, client_y);
	cw_store_i32_le(payload + 24U, present->destination_x + client_x);
	cw_store_i32_le(payload + 28U, present->destination_y + client_y);
	cw_store_u64_le(payload + 32U, 1U);
	cw_store_i32_le(payload + 44U, -120); /* One Windows wheel notch down. */
	return send_message(agent, CW_MESSAGE_POINTER_WHEEL, payload, sizeof(payload));
}

static bool send_keyboard_focus(struct agent *agent, uint64_t window_id, bool focused)
{
	uint8_t payload[16] = {0};

	cw_store_u64_le(payload, window_id);
	cw_store_u32_le(payload + 8U, focused ? 1U : 0U);
	return send_message(agent, CW_MESSAGE_KEYBOARD_FOCUS, payload, sizeof(payload));
}

static bool send_key(struct agent *agent, uint64_t window_id, uint32_t state)
{
	uint8_t payload[24] = {0};

	cw_store_u64_le(payload, window_id);
	cw_store_u32_le(payload + 8U, 30U); /* Linux KEY_A */
	cw_store_u32_le(payload + 12U, state);
	cw_store_u64_le(payload + 16U, state == CW_KEY_PRESSED ? 2U : 3U);
	return send_message(agent, CW_MESSAGE_KEYBOARD_KEY, payload, sizeof(payload));
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
	case CW_MESSAGE_OUTPUT_CONFIG: {
		CwOutputConfig config;

		if (!agent->hello || !cw_decode_output_config(payload, header->payload_length,
							       &config) ||
		    config.logical_width == 0U || config.logical_height == 0U)
			return false;
		agent->output_config = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;

		if (!agent->output_config || !cw_decode_window_create(payload, header->payload_length,
								      &create) || create.parent_window_id != 0U ||
		    agent->created)
			return false;
		agent->window_id = create.window_id;
		agent->created = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;

		if (!agent->created || !cw_decode_window_frame(payload, header->payload_length,
								    &frame) || frame.window_id != agent->window_id)
			return false;
		agent->framed = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;

		if (!agent->framed || !cw_decode_window_present(payload, header->payload_length,
								      &present) || present.window_id != agent->window_id)
			return false;
		if (!send_ack(agent, &present))
			return false;
		/* A newly created xdg surface may first be exported as hidden while
		 * its initial configure/commit settles. ACK it, then wait for the
		 * first usable fragment before choosing an in-window test location. */
		if (!agent->sent_input && present.visible && present.destination_w > 0 &&
		    present.destination_h > 0) {
			if (!send_wheel(agent, &present) ||
			    !send_keyboard_focus(agent, present.window_id, true) ||
			    !send_key(agent, present.window_id, CW_KEY_PRESSED) ||
			    !send_key(agent, present.window_id, CW_KEY_RELEASED))
				return false;
			agent->sent_input = true;
		}
		return true;
	}
	case CW_MESSAGE_WINDOW_ACTIVATE: {
		CwWindowActivate activate;

		return cw_decode_window_activate(payload, header->payload_length, &activate) &&
		       activate.window_id == agent->window_id;
	}
	case CW_MESSAGE_WINDOW_DESTROY:
		if (!agent->sent_input || header->payload_length != 8U ||
		    cw_load_u64_le(payload) != agent->window_id)
			return false;
		agent->destroyed = true;
		return true;
	default:
		return false;
	}
}

static int connect_to(uint16_t port)
{
	struct sockaddr_in address = { .sin_family = AF_INET };
	unsigned attempt;

	address.sin_port = htons(port);
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		return -1;
	for (attempt = 0U; attempt < 100U; ++attempt) {
		struct timespec retry = { .tv_nsec = 10000000L };
		int fd = socket(AF_INET, SOCK_STREAM, 0);

		if (fd >= 0 && connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0)
			return fd;
		if (fd >= 0)
			close(fd);
		(void)nanosleep(&retry, NULL);
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct agent agent = { .fd = -1, .wire_sequence = 1U };
	CwDecoder decoder;
	uint8_t bytes[8192];
	char *end;
	long port = 44680L;
	int elapsed = 0;

	if (argc == 2) {
		port = strtol(argv[1], &end, 10);
		if (!*argv[1] || *end || port < 1L || port > 65535L)
			fail("usage: stage8-agent [port]");
	} else if (argc != 1) {
		fail("usage: stage8-agent [port]");
	}
	agent.fd = connect_to((uint16_t)port);
	if (agent.fd < 0 || !send_hello(&agent))
		fail("connect or hello");
	cw_decoder_init(&decoder);
	while (!agent.destroyed && elapsed < 10000) {
		struct pollfd poll_fd = { .fd = agent.fd, .events = POLLIN };
		int ready = poll(&poll_fd, 1U, 100);

		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0) {
			elapsed += 100;
			continue;
		}
		if ((poll_fd.revents & POLLIN) != 0) {
			ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);
			if (received <= 0 || !cw_decoder_feed(&decoder, bytes, (size_t)received,
									  on_message, &agent))
				fail("receive/decode");
		}
	}
	if (!agent.hello || !agent.output_config || !agent.created || !agent.framed ||
	    !agent.sent_input || !agent.destroyed)
		fail("timed out waiting for expected protocol sequence");
	if (!cw_decoder_finish(&decoder))
		fail("truncated decoder");
	cw_decoder_destroy(&decoder);
	close(agent.fd);
	printf("stage8 keyboard/wheel protocol agent: PASS\n");
	return EXIT_SUCCESS;
}
