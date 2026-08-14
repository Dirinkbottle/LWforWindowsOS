/* Stage 7D protocol-side oracle for exact logical output clipping. */
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

enum geometry_case {
	CASE_RIGHT_HALF,
	CASE_RIGHT_ONE,
	CASE_LEFT_HALF,
	CASE_LEFT_ONE,
	CASE_ABOVE_HALF,
	CASE_ABOVE_ONE,
	CASE_BELOW_HALF,
	CASE_BELOW_ONE,
};

struct expected_presentation {
	int32_t source_x, source_y, source_w, source_h;
	int32_t destination_x, destination_y, destination_w, destination_h;
};

struct agent {
	int fd;
	uint64_t next_wire_sequence;
	uint32_t scale_numerator;
	uint32_t scale_denominator;
	enum geometry_case geometry_case;
	bool hello, config, created, framed, presented, destroyed;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage7d-geometry-agent: %s\n", message);
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

static bool parse_geometry_case(const char *text, enum geometry_case *out)
{
	static const struct {
		const char *name;
		enum geometry_case value;
	} cases[] = {
		{ "right-half", CASE_RIGHT_HALF }, { "right-one", CASE_RIGHT_ONE },
		{ "left-half", CASE_LEFT_HALF }, { "left-one", CASE_LEFT_ONE },
		{ "above-half", CASE_ABOVE_HALF }, { "above-one", CASE_ABOVE_ONE },
		{ "below-half", CASE_BELOW_HALF }, { "below-one", CASE_BELOW_ONE },
	};
	size_t index;

	if (text == NULL || out == NULL)
		return false;
	for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		if (strcmp(text, cases[index].name) == 0) {
			*out = cases[index].value;
			return true;
		}
	}
	return false;
}

static const struct expected_presentation *expected_for(enum geometry_case geometry_case)
{
	static const struct expected_presentation expected[] = {
		[CASE_RIGHT_HALF] = {400, 0, 400, 600, 0, 80, 400, 600},
		[CASE_RIGHT_ONE] = {799, 0, 1, 600, 0, 80, 1, 600},
		[CASE_LEFT_HALF] = {0, 0, 400, 600, 1520, 80, 400, 600},
		[CASE_LEFT_ONE] = {0, 0, 1, 600, 1919, 80, 1, 600},
		[CASE_ABOVE_HALF] = {0, 0, 800, 300, 80, 780, 800, 300},
		[CASE_ABOVE_ONE] = {0, 0, 800, 1, 80, 1079, 800, 1},
		[CASE_BELOW_HALF] = {0, 300, 800, 300, 80, 0, 800, 300},
		[CASE_BELOW_ONE] = {0, 599, 800, 1, 80, 0, 800, 1},
	};

	return &expected[geometry_case];
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
	ok = cw_message_encode(&encoded, type, 0U, agent->next_wire_sequence++,
			       payload, length) && send_all(agent->fd, encoded.data, encoded.length);
	cw_buffer_destroy(&encoded);
	return ok;
}

static bool send_present_ack(struct agent *agent, const CwWindowPresent *present)
{
	uint8_t payload[16];

	cw_store_u64_le(payload, present->window_id);
	cw_store_u64_le(payload + 8U, present->presentation_sequence);
	return send_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, payload, sizeof(payload));
}

static bool same_presentation(const CwWindowPresent *present,
			      const struct expected_presentation *expected)
{
	return present->visible && present->source_x == expected->source_x &&
		present->source_y == expected->source_y && present->source_w == expected->source_w &&
		present->source_h == expected->source_h &&
		present->destination_x == expected->destination_x &&
		present->destination_y == expected->destination_y &&
		present->destination_w == expected->destination_w &&
		present->destination_h == expected->destination_h;
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
							       &config) || agent->config ||
		    config.scale_numerator != agent->scale_numerator ||
		    config.scale_denominator != agent->scale_denominator)
			return false;
		agent->config = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;

		if (!agent->config || !cw_decode_window_create(payload, header->payload_length,
								 &create) || agent->created ||
		    create.window_id != 1U || create.parent_window_id != 0U ||
		    create.surface_width != 800U || create.surface_height != 600U)
			return false;
		agent->created = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;

		if (!agent->created || !cw_decode_window_frame(payload, header->payload_length,
							      &frame) || agent->framed || frame.window_id != 1U ||
		    frame.width != 800U || frame.height != 600U || frame.stride != 3200U ||
		    frame.pixel_bytes != UINT64_C(1920000))
			return false;
		agent->framed = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;

		if (!agent->framed || !cw_decode_window_present(payload, header->payload_length,
								  &present) || agent->presented ||
		    present.window_id != 1U || !same_presentation(&present,
								      expected_for(agent->geometry_case)) ||
		    !send_present_ack(agent, &present))
			return false;
		agent->presented = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_DESTROY:
		if (!agent->presented || header->payload_length != 8U ||
		    cw_load_u64_le(payload) != 1U)
			return false;
		agent->destroyed = true;
		return true;
	default:
		return false;
	}
}

int main(int argc, char **argv)
{
	struct agent agent = {
		.fd = -1,
		.next_wire_sequence = 1U,
		.scale_numerator = 1U,
		.scale_denominator = 1U,
	};
	struct sockaddr_in address = { .sin_family = AF_INET };
	CwDecoder decoder;
	uint8_t hello[8] = {0};
	uint8_t bytes[8192];
	char *end;
	long port = 44660L;
	bool got_case = false;
	int index;

	for (index = 1; index < argc; ++index) {
		if (strcmp(argv[index], "--scale") == 0 && index + 1 < argc) {
			if (!parse_scale(argv[++index], &agent.scale_numerator,
						 &agent.scale_denominator))
				fail("usage");
		} else if (strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
			if (!parse_geometry_case(argv[++index], &agent.geometry_case))
				fail("usage");
			got_case = true;
		} else {
			port = strtol(argv[index], &end, 10);
			if (*argv[index] == '\0' || *end != '\0' || port < 1L || port > 65535L)
				fail("usage");
		}
	}
	if (!got_case)
		fail("usage: stage7d-geometry-agent [port] --scale numerator/denominator "
	     "--case right-half|right-one|left-half|left-one|above-half|above-one|below-half|below-one");
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
	while (!agent.destroyed) {
		ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);

		if (received <= 0 || !cw_decoder_feed(&decoder, bytes, (size_t)received,
						       on_message, &agent))
			fail("receive/decode");
	}
	cw_decoder_destroy(&decoder);
	close(agent.fd);
	puts("stage7d exact logical clipping integration: PASS");
	return EXIT_SUCCESS;
}
