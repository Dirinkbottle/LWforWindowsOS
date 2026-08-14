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

enum output_placement {
	OUTPUT_RIGHT,
	OUTPUT_LEFT,
	OUTPUT_ABOVE,
	OUTPUT_BELOW,
};

struct agent {
	int fd;
	bool hello, created, initial_present, moved_present, released, destroyed;
	bool outside_output_test;
	uint32_t frames, presents;
	uint32_t expected_scale_numerator, expected_scale_denominator;
	enum output_placement placement;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage6-agent: %s\n", message);
	exit(EXIT_FAILURE);
}

static bool parse_scale(const char *text, uint32_t *numerator,
				uint32_t *denominator)
{
	char *slash;
	char *end;
	unsigned long parsed_numerator;
	unsigned long parsed_denominator;

	if (text == NULL || numerator == NULL || denominator == NULL ||
	    (slash = strchr(text, '/')) == NULL || slash == text || slash[1] == '\0')
		return false;
	*slash = '\0';
	parsed_numerator = strtoul(text, &end, 10);
	if (*end != '\0' || parsed_numerator == 0UL || parsed_numerator > UINT32_MAX) {
		*slash = '/';
		return false;
	}
	parsed_denominator = strtoul(slash + 1, &end, 10);
	*slash = '/';
	if (*end != '\0' || parsed_denominator == 0UL || parsed_denominator > UINT32_MAX)
		return false;
	*numerator = (uint32_t)parsed_numerator;
	*denominator = (uint32_t)parsed_denominator;
	return true;
}

static bool parse_placement(const char *text, enum output_placement *placement)
{
	if (text == NULL || placement == NULL)
		return false;
	if (strcmp(text, "right") == 0)
		*placement = OUTPUT_RIGHT;
	else if (strcmp(text, "left") == 0)
		*placement = OUTPUT_LEFT;
	else if (strcmp(text, "above") == 0)
		*placement = OUTPUT_ABOVE;
	else if (strcmp(text, "below") == 0)
		*placement = OUTPUT_BELOW;
	else
		return false;
	return true;
}

static bool send_motion(struct agent *agent, uint64_t wire_sequence,
			uint64_t presentation_sequence, int32_t client_x,
			int32_t client_y, int32_t output_x, int32_t output_y);

static bool placement_is_vertical(const struct agent *agent)
{
	return agent->placement == OUTPUT_ABOVE || agent->placement == OUTPUT_BELOW;
}

static bool send_drag_motion(struct agent *agent, uint64_t wire_sequence,
			     uint64_t presentation_sequence, unsigned step)
{
	int32_t output_x = 100;
	int32_t output_y = 90;

	if (!placement_is_vertical(agent)) {
		if (agent->placement == OUTPUT_RIGHT) {
			/* The last value is intentionally past the only remote output's
			 * right edge. The exporter must clamp it to the compositor desktop
			 * edge before passing it to libweston during an xdg move grab. */
			static const int32_t x[] = {-300, -699, -700, 280, 2300};
			output_x = x[step];
		} else {
			static const int32_t x[] = {1620, 2019, 2020, 280};
			output_x = x[step];
		}
	} else if (agent->placement == OUTPUT_ABOVE) {
		static const int32_t y[] = {790, 1089, 1090, 1270};
		output_x = 180;
		output_y = y[step];
	} else {
		static const int32_t y[] = {-290, 9, 10, 190};
		output_x = 180;
		output_y = y[step];
	}
	return send_motion(agent, wire_sequence, presentation_sequence,
			   100, 10, output_x, output_y);
}

static bool send_outside_output_motion_burst(struct agent *agent,
					     uint64_t presentation_sequence)
{
	uint64_t wire_sequence;

	/* Repeated WM_MOUSEMOVE messages are normal while Win32 capture is held at
	 * an outer desktop edge. The canonical view position is unchanged after the
	 * first one, so exactly one new WINDOW_PRESENT is permitted. */
	for (wire_sequence = 12U; wire_sequence < 76U; ++wire_sequence)
		if (!send_drag_motion(agent, wire_sequence, presentation_sequence, 4U))
			return false;
	return true;
}

static bool matches_drag_presentation(const struct agent *agent,
				      const CwWindowPresent *present, unsigned step)
{
	if (agent->placement == OUTPUT_RIGHT) {
		static const int32_t source_x[] = {400, 799};
		static const int32_t destination_x[] = {0, 0};
		static const int32_t width[] = {400, 1};
		return present->source_x == source_x[step] && present->source_y == 0 &&
			present->source_w == width[step] && present->source_h == 600 &&
			present->destination_x == destination_x[step] && present->destination_y == 80 &&
			present->destination_w == width[step] && present->destination_h == 600;
	}
	if (agent->placement == OUTPUT_LEFT) {
		static const int32_t source_w[] = {400, 1};
		static const int32_t destination_x[] = {1520, 1919};
		return present->source_x == 0 && present->source_y == 0 &&
			present->source_w == source_w[step] && present->source_h == 600 &&
			present->destination_x == destination_x[step] && present->destination_y == 80 &&
			present->destination_w == source_w[step] && present->destination_h == 600;
	}
	if (agent->placement == OUTPUT_ABOVE) {
		static const int32_t source_h[] = {300, 1};
		static const int32_t destination_y[] = {780, 1079};
		return present->source_x == 0 && present->source_y == 0 &&
			present->source_w == 800 && present->source_h == source_h[step] &&
			present->destination_x == 80 && present->destination_y == destination_y[step] &&
			present->destination_w == 800 && present->destination_h == source_h[step];
	}
	{
		static const int32_t source_y[] = {300, 599};
		static const int32_t source_h[] = {300, 1};
		return present->source_x == 0 && present->source_y == source_y[step] &&
			present->source_w == 800 && present->source_h == source_h[step] &&
			present->destination_x == 80 && present->destination_y == 0 &&
			present->destination_w == 800 && present->destination_h == source_h[step];
	}
}

static bool matches_outside_output_presentation(const struct agent *agent,
						 const CwWindowPresent *present)
{
	/* Right-side remote output is [1024, 2944). The move press is at x=1204
	 * while the view begins at x=1104, so clamping a capture point to x=2943
	 * puts the 800px surface at x=2843: its final 101px remains visible. */
	return agent->placement == OUTPUT_RIGHT && present->visible &&
		present->source_x == 0 && present->source_y == 0 &&
		present->source_w == 101 && present->source_h == 600 &&
		present->destination_x == 1819 && present->destination_y == 80 &&
		present->destination_w == 101 && present->destination_h == 600;
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
	case CW_MESSAGE_OUTPUT_CONFIG: {
		CwOutputConfig config;
		if (!agent->hello || !cw_decode_output_config(payload, header->payload_length,
							       &config) ||
		    config.scale_numerator != agent->expected_scale_numerator ||
		    config.scale_denominator != agent->expected_scale_denominator)
			fail("bad OUTPUT_CONFIG");
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
			if (!send_drag_motion(agent, 4, present.presentation_sequence, 0U))
				fail("drag motion");
			agent->presents++;
			return true;
		}
		if (agent->presents == 1) {
			if (!present.visible || !matches_drag_presentation(agent, &present, 0U) ||
			    !send_ack(agent, 5, present.presentation_sequence) ||
			    !send_drag_motion(agent, 6, present.presentation_sequence, 1U))
				fail("50/50 presentation");
			agent->moved_present = true;
			agent->presents++;
			return true;
		}
		if (agent->presents == 2) {
			if (!present.visible || !matches_drag_presentation(agent, &present, 1U) ||
			    !send_ack(agent, 7, present.presentation_sequence) ||
			    !send_drag_motion(agent, 8, present.presentation_sequence, 2U))
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
			    !send_drag_motion(agent, 10, present.presentation_sequence, 3U))
				fail("fully-local presentation");
			agent->presents++;
			return true;
		}
		if (agent->presents == 4) {
			if (!present.visible || present.source_x != 0 || present.source_y != 0 ||
			    present.source_w != 800 || present.source_h != 600 ||
			    present.destination_x != (placement_is_vertical(agent) ? 80 : 180) ||
			    present.destination_y != (placement_is_vertical(agent) ? 180 : 80) ||
			    present.destination_w != 800 || present.destination_h != 600 ||
			    !send_ack(agent, 11, present.presentation_sequence))
				fail("fully-remote presentation or release");
			if (agent->outside_output_test) {
				if (!send_outside_output_motion_burst(agent, present.presentation_sequence))
					fail("outside-output drag-motion burst");
			} else if (!send_button(agent, 12, present.presentation_sequence,
						CW_BUTTON_RELEASED, 100, 10,
						placement_is_vertical(agent) ? 180 : 280,
						placement_is_vertical(agent) ? 190 : 90)) {
				fail("fully-remote presentation or release");
			}
			agent->released = true;
			agent->presents++;
			return true;
		}
		if (agent->presents == 5 && agent->outside_output_test) {
			if (!matches_outside_output_presentation(agent, &present) ||
			    !send_ack(agent, 76, present.presentation_sequence) ||
			    !send_button(agent, 77, present.presentation_sequence,
					 CW_BUTTON_RELEASED, 100, 10, 2300, 90))
				fail("outside-output clamped presentation or release");
			agent->presents++;
			return true;
		}
		fail("unexpected extra WINDOW_PRESENT");
		return false;
	case CW_MESSAGE_WINDOW_ACTIVATE: {
		CwWindowActivate activate;
		if (!agent->created || !cw_decode_window_activate(payload,
			header->payload_length, &activate) || activate.window_id != 1U)
			fail("bad WINDOW_ACTIVATE");
		return true;
	}
	case CW_MESSAGE_WINDOW_DESTROY:
		if (!agent->moved_present || !agent->released ||
		    agent->presents != (agent->outside_output_test ? 6U : 5U) ||
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
	struct agent agent = {
		.expected_scale_numerator = 1U,
		.expected_scale_denominator = 1U,
	};
	struct sockaddr_in address = { .sin_family = AF_INET };
	CwDecoder decoder;
	uint8_t hello[8] = {0};
	uint8_t bytes[8192];
	char *end;
	long port = 44606;

	for (int index = 1; index < argc; ++index) {
		if (strcmp(argv[index], "--scale") == 0 && index + 1 < argc) {
			if (!parse_scale(argv[++index], &agent.expected_scale_numerator,
						 &agent.expected_scale_denominator))
				fail("usage: stage6-agent [port] [--scale numerator/denominator]");
			continue;
		}
		if (strcmp(argv[index], "--placement") == 0 && index + 1 < argc) {
			if (!parse_placement(argv[++index], &agent.placement))
				fail("usage: stage6-agent [port] [--scale numerator/denominator] "
				     "[--placement right|left|above|below]");
			continue;
		}
		if (strcmp(argv[index], "--test-outside-output") == 0) {
			agent.outside_output_test = true;
			continue;
		}
		port = strtol(argv[index], &end, 10);
		if (!*argv[index] || *end || port < 1 || port > 65535)
			fail("usage: stage6-agent [port] [--scale numerator/denominator] "
			     "[--placement right|left|above|below]");
	}
	if (agent.outside_output_test && agent.placement != OUTPUT_RIGHT)
		fail("--test-outside-output requires --placement right");
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
