/* Protocol-side acceptance test for Stage 7C multi-window lifecycle/focus. */
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
#include <unistd.h>

enum { WINDOW_COUNT = 3, WINDOW_W = 300, WINDOW_H = 200 };

struct window_state {
	bool created, hidden, destroyed;
	unsigned frames, presents, activations;
	uint64_t last_presentation;
};
struct agent {
	int fd;
	uint64_t wire_sequence;
	bool hello, input_sent;
	uint16_t last_type;
	unsigned created, destroyed;
	struct window_state windows[WINDOW_COUNT];
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage7c-agent: %s\n", message);
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
	ok = cw_message_encode(&encoded, type, 0U, agent->wire_sequence++, payload, length) &&
		send_all(agent->fd, encoded.data, encoded.length);
	cw_buffer_destroy(&encoded);
	return ok;
}

static bool send_click_window_two(struct agent *agent, uint64_t presentation_sequence)
{
	uint8_t enter[40] = {0};
	uint8_t button[48] = {0};

	/* Second toplevel is the second generic cascade slot: dest=[128,80]. */
	cw_store_u64_le(enter, 2U);
	cw_store_u64_le(enter + 8U, presentation_sequence);
	cw_store_i32_le(enter + 16U, 10);
	cw_store_i32_le(enter + 20U, 10);
	cw_store_i32_le(enter + 24U, 138);
	cw_store_i32_le(enter + 28U, 90);
	cw_store_u64_le(enter + 32U, 1U);
	if (!send_message(agent, CW_MESSAGE_POINTER_ENTER, enter, sizeof(enter)))
		return false;
	memcpy(button, enter, sizeof(enter));
	cw_store_u32_le(button + 40U, CW_POINTER_BUTTON_LEFT);
	cw_store_u32_le(button + 44U, CW_BUTTON_PRESSED);
	if (!send_message(agent, CW_MESSAGE_POINTER_BUTTON, button, sizeof(button)))
		return false;
	cw_store_u32_le(button + 44U, CW_BUTTON_RELEASED);
	return send_message(agent, CW_MESSAGE_POINTER_BUTTON, button, sizeof(button));
}

static bool send_ack(struct agent *agent, const CwWindowPresent *present)
{
	uint8_t ack[16];

	cw_store_u64_le(ack, present->window_id);
	cw_store_u64_le(ack + 8U, present->presentation_sequence);
	return send_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, ack, sizeof(ack));
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload)
{
	struct agent *agent = context;

	agent->last_type = header->type;

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

		return agent->hello && cw_decode_output_config(payload,
			header->payload_length, &config) && config.scale_numerator == 1U &&
			config.scale_denominator == 1U;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;
		struct window_state *window;

		if (!agent->hello || !cw_decode_window_create(payload, header->payload_length,
							       &create) ||
		    agent->created >= WINDOW_COUNT || create.window_id != agent->created + 1U ||
		    create.parent_window_id != 0U || create.surface_width != WINDOW_W ||
		    create.surface_height != WINDOW_H)
			return false;
		window = &agent->windows[agent->created++];
		window->created = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;

		if (!cw_decode_window_frame(payload, header->payload_length, &frame) ||
		    frame.window_id == 0U || frame.window_id > WINDOW_COUNT ||
		    !agent->windows[frame.window_id - 1U].created || frame.width != WINDOW_W ||
		    frame.height != WINDOW_H || frame.stride != WINDOW_W * 4U ||
		    frame.pixel_bytes != (uint64_t)WINDOW_W * WINDOW_H * 4U)
			return false;
		agent->windows[frame.window_id - 1U].frames++;
		return true;
	}
	case CW_MESSAGE_WINDOW_DAMAGE: {
		CwWindowDamage damage;

		return cw_decode_window_damage(payload, header->payload_length, &damage) &&
		       damage.window_id != 0U && damage.window_id <= WINDOW_COUNT;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;
		struct window_state *window;

		if (!cw_decode_window_present(payload, header->payload_length, &present) ||
		    present.window_id == 0U || present.window_id > WINDOW_COUNT ||
		    !(window = &agent->windows[present.window_id - 1U])->created ||
		    !send_ack(agent, &present))
			return false;
		if (present.visible) {
			const int32_t expected_x = 80 + (int32_t)(present.window_id - 1U) * 48;

			if (present.source_x != 0 || present.source_y != 0 ||
			    present.source_w != WINDOW_W || present.source_h != WINDOW_H ||
			    present.destination_x != expected_x || present.destination_y != 80 ||
			    present.destination_w != WINDOW_W || present.destination_h != WINDOW_H)
				return false;
		} else if (present.window_id != 1U || present.source_w != 0 ||
			   present.source_h != 0 || present.destination_w != 0 ||
			   present.destination_h != 0) {
			return false;
		} else {
			window->hidden = true;
		}
		window->presents++;
		window->last_presentation = present.presentation_sequence;
		if (present.window_id == 2U && present.visible && !agent->input_sent) {
			agent->input_sent = true;
			return send_click_window_two(agent, present.presentation_sequence);
		}
		return true;
	}
	case CW_MESSAGE_WINDOW_ACTIVATE: {
		CwWindowActivate activate;

		if (!cw_decode_window_activate(payload, header->payload_length, &activate) ||
		    activate.window_id == 0U || activate.window_id > WINDOW_COUNT ||
		    !agent->windows[activate.window_id - 1U].created)
			return false;
		agent->windows[activate.window_id - 1U].activations++;
		return true;
	}
	case CW_MESSAGE_WINDOW_DESTROY: {
		const uint64_t id = header->payload_length == 8U ? cw_load_u64_le(payload) : 0U;
		struct window_state *window;

		if (id == 0U || id > WINDOW_COUNT || !(window = &agent->windows[id - 1U])->created ||
		    window->destroyed)
			return false;
		window->destroyed = true;
		agent->destroyed++;
		return true;
	}
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
	long port = 44612L;
	char *end;

	if (argc == 2) {
		port = strtol(argv[1], &end, 10);
		if (*end != '\0' || port < 1L || port > 65535L)
			fail("usage: stage7c-agent [port]");
	} else if (argc != 1) {
		fail("usage: stage7c-agent [port]");
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
	if (!send_message(&agent, CW_MESSAGE_HELLO, hello, sizeof(hello)))
		fail("send HELLO");
	cw_decoder_init(&decoder);
	while (agent.destroyed != WINDOW_COUNT) {
		ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);

		if (received <= 0 || !cw_decoder_feed(&decoder, bytes, (size_t)received,
						       on_message, &agent)) {
			fprintf(stderr, "stage7c-agent: last=%s created=%u destroyed=%u "
					"presents=[%u,%u,%u] hidden=%u decoder=%s\n",
					cw_message_type_name(agent.last_type), agent.created,
					agent.destroyed, agent.windows[0].presents,
					agent.windows[1].presents, agent.windows[2].presents,
					agent.windows[0].hidden ? 1U : 0U,
					cw_decoder_error_string(cw_decoder_error(&decoder)));
			fail("receive/decode");
		}
	}
	cw_decoder_destroy(&decoder);
	if (agent.created != WINDOW_COUNT || !agent.windows[0].hidden ||
	    agent.windows[0].frames < 2U || agent.windows[1].frames == 0U ||
	    agent.windows[2].frames == 0U || !agent.input_sent ||
	    agent.windows[1].activations == 0U)
		fail("lifecycle or activation assertions");
	close(agent.fd);
	puts("stage7c multi-window lifecycle integration: PASS");
	return EXIT_SUCCESS;
}
