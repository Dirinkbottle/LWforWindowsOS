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

enum {
	INITIAL_WIDTH = 800,
	INITIAL_HEIGHT = 600,
	RESIZED_WIDTH = 640,
	RESIZED_HEIGHT = 480,
	SQUARE = 32,
	MAX_DAMAGE_PIXELS = SQUARE * SQUARE * 2,
};

struct agent {
	int fd;
	bool hello;
	bool created;
	bool dropped_damage;
	bool requested_resync;
	bool received_resync;
	bool resized;
	bool damage_after_resize;
	bool destroyed;
	uint8_t *framebuffer;
	uint64_t frame_sequence;
	uint32_t frames;
	uint32_t damages;
	uint32_t applied_damages;
	uint32_t presents;
	uint32_t surface_width;
	uint32_t surface_height;
	uint32_t stride;
	size_t framebuffer_bytes;
	uint64_t next_wire_sequence;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage7a-agent: %s\n", message);
	exit(EXIT_FAILURE);
}

static uint32_t stage7a_pixel(int x, int y, uint64_t frame_sequence,
			      uint32_t width, uint32_t height)
{
	const unsigned step = (unsigned)(frame_sequence - 1U);
	const int square_x = (int)((step * 17U) % (width - SQUARE));
	const int square_y = 80 + (int)((step * 11U) % (height - SQUARE - 80));
	uint32_t color;

	if (((x / 40) + (y / 40)) % 2 == 0)
		color = 0x001b365dU;
	else
		color = 0x00457b9dU;
	if (x >= square_x && x < square_x + SQUARE &&
	    y >= square_y && y < square_y + SQUARE)
		color = 0x00ffe600U;
	return color;
}

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
	unsigned bit;

	crc ^= byte;
	for (bit = 0U; bit < 8U; ++bit)
		crc = (crc >> 1U) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
	return crc;
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_C(0xffffffff);
	size_t index;

	for (index = 0U; index < length; ++index)
		crc = crc32_update(crc, bytes[index]);
	return ~crc;
}

static uint32_t expected_crc32(uint64_t frame_sequence, uint32_t width,
			       uint32_t height)
{
	uint32_t crc = UINT32_C(0xffffffff);
	int x;
	int y;

	for (y = 0; y < (int)height; ++y) {
		for (x = 0; x < (int)width; ++x) {
			const uint32_t color = stage7a_pixel(x, y, frame_sequence, width, height);

			crc = crc32_update(crc, (uint8_t)(color & 0xffU));
			crc = crc32_update(crc, (uint8_t)((color >> 8U) & 0xffU));
			crc = crc32_update(crc, (uint8_t)((color >> 16U) & 0xffU));
			/* XRGB sources are opaque in Weston; the Stage 7B scene staging
			 * image normalizes their otherwise-unused byte to alpha=255. */
			crc = crc32_update(crc, UINT8_MAX);
		}
	}
	return ~crc;
}

static bool send_all(int fd, const uint8_t *bytes, size_t length)
{
	while (length > 0U) {
		const ssize_t sent = send(fd, bytes, length, 0);

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
			       payload, length) &&
		send_all(agent->fd, encoded.data, encoded.length);
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

static bool request_full_frame(struct agent *agent)
{
	uint8_t payload[16];

	cw_store_u64_le(payload, 1U);
	cw_store_u64_le(payload + 8U, agent->frame_sequence);
	agent->requested_resync = true;
	return send_message(agent, CW_MESSAGE_WINDOW_FRAME_REQUEST, payload, sizeof(payload));
}

static void verify_checksum(const struct agent *agent)
{
	const uint32_t actual = crc32_bytes(agent->framebuffer, agent->framebuffer_bytes);
	const uint32_t expected = expected_crc32(agent->frame_sequence,
					      agent->surface_width, agent->surface_height);

	if (actual != expected) {
		fprintf(stderr, "stage7a checksum mismatch frame=%llu actual=%08x expected=%08x\n",
			(unsigned long long)agent->frame_sequence, actual, expected);
		fail("reconstructed framebuffer differs from source");
	}
}

static bool apply_frame(struct agent *agent, const CwWindowFrame *frame)
{
	if (!agent->created || frame->window_id != 1U ||
	    frame->width != agent->surface_width || frame->height != agent->surface_height ||
	    frame->stride != agent->stride ||
	    frame->pixel_format != CW_PIXEL_FORMAT_BGRA8888 ||
	    frame->pixel_bytes != agent->framebuffer_bytes || frame->frame_sequence == 0U) {
		return false;
	}
	memcpy(agent->framebuffer, frame->pixels, agent->framebuffer_bytes);
	agent->frame_sequence = frame->frame_sequence;
	agent->frames++;
	if (agent->requested_resync)
		agent->received_resync = true;
	verify_checksum(agent);
	return true;
}

static bool damage_is_within_surface(const struct agent *agent,
				     const CwDamageRect *rect)
{
	const int64_t right = (int64_t)rect->x + rect->width;
	const int64_t bottom = (int64_t)rect->y + rect->height;

	return rect->x >= 0 && rect->y >= 0 && right <= agent->surface_width &&
	       bottom <= agent->surface_height &&
	       rect->stride == rect->width * 4U &&
	       rect->pixel_bytes == (uint64_t)rect->stride * rect->height;
}

static bool apply_damage(struct agent *agent, const CwWindowDamage *damage)
{
	CwDamageRect rects[CW_MAX_DAMAGE_RECTS];
	uint64_t pixels = 0U;
	uint32_t index;

	if (damage->window_id != 1U || damage->frame_sequence <= damage->base_frame_sequence) {
		return false;
	}
	agent->damages++;
	if (!agent->dropped_damage) {
		if (damage->base_frame_sequence != agent->frame_sequence) {
			return false;
		}
		agent->dropped_damage = true;
		return true; /* Simulate a lost TCP payload before applying any bytes. */
	}
	if (damage->base_frame_sequence != agent->frame_sequence) {
		return !agent->requested_resync && request_full_frame(agent);
	}
	for (index = 0U; index < damage->rect_count; ++index) {
		if (!cw_window_damage_rect_at(damage, index, &rects[index]) ||
		    !damage_is_within_surface(agent, &rects[index])) {
			return false;
		}
		pixels += rects[index].pixel_bytes;
	}
	if (pixels == 0U || pixels > (uint64_t)MAX_DAMAGE_PIXELS * 4U) {
		return false;
	}
	for (index = 0U; index < damage->rect_count; ++index) {
		const CwDamageRect *rect = &rects[index];
		uint32_t row;

		for (row = 0U; row < rect->height; ++row) {
			memcpy(agent->framebuffer + ((size_t)rect->y + row) * agent->stride +
			       (size_t)rect->x * 4U,
			       rect->pixels + (size_t)row * rect->stride, rect->stride);
		}
	}
	agent->frame_sequence = damage->frame_sequence;
	agent->applied_damages++;
	if (agent->resized)
		agent->damage_after_resize = true;
	verify_checksum(agent);
	return true;
}

static bool on_message(void *context, const CwHeader *header, const uint8_t *payload)
{
	struct agent *agent = context;

	switch (header->type) {
	case CW_MESSAGE_HELLO_ACK: {
		CwHelloAck hello;

		if (!cw_decode_hello_ack(payload, header->payload_length, &hello) ||
		    hello.selected_version != CW_PROTOCOL_VERSION)
			return false;
		agent->hello = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_CREATE: {
		CwWindowCreate create;

		if (!agent->hello || !cw_decode_window_create(payload, header->payload_length,
							       &create) || create.window_id != 1U ||
		    create.surface_width != INITIAL_WIDTH || create.surface_height != INITIAL_HEIGHT ||
		    agent->created)
			return false;
		agent->surface_width = create.surface_width;
		agent->surface_height = create.surface_height;
		agent->stride = agent->surface_width * 4U;
		agent->framebuffer_bytes = (size_t)agent->stride * agent->surface_height;
		agent->framebuffer = calloc(1U, agent->framebuffer_bytes);
		if (!agent->framebuffer)
			return false;
		agent->created = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;

		return cw_decode_window_frame(payload, header->payload_length, &frame) &&
		       apply_frame(agent, &frame);
	}
	case CW_MESSAGE_WINDOW_DAMAGE: {
		CwWindowDamage damage;

		return cw_decode_window_damage(payload, header->payload_length, &damage) &&
		       apply_damage(agent, &damage);
	}
	case CW_MESSAGE_WINDOW_RESIZE: {
		CwWindowResize resize;
		uint8_t *replacement;
		size_t bytes;

		if (!cw_decode_window_resize(payload, header->payload_length, &resize) ||
		    !agent->created || agent->resized || resize.window_id != 1U ||
		    resize.surface_width != RESIZED_WIDTH || resize.surface_height != RESIZED_HEIGHT)
			return false;
		bytes = (size_t)resize.surface_width * 4U * resize.surface_height;
		replacement = calloc(1U, bytes);
		if (!replacement)
			return false;
		free(agent->framebuffer);
		agent->framebuffer = replacement;
		agent->surface_width = resize.surface_width;
		agent->surface_height = resize.surface_height;
		agent->stride = resize.surface_width * 4U;
		agent->framebuffer_bytes = bytes;
		agent->frame_sequence = 0U;
		agent->resized = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;

		if (!cw_decode_window_present(payload, header->payload_length, &present) ||
		    !agent->created || present.window_id != 1U || !send_present_ack(agent, &present))
			return false;
		agent->presents++;
		return true;
	}
	case CW_MESSAGE_WINDOW_DESTROY:
		if (header->payload_length != 8U || cw_load_u64_le(payload) != 1U ||
		    agent->frames < 3U || !agent->dropped_damage || !agent->requested_resync ||
		    !agent->received_resync || agent->applied_damages < 3U ||
		    agent->damages < agent->applied_damages + 1U || !agent->resized ||
		    !agent->damage_after_resize || agent->presents < 5U)
			return false;
		agent->destroyed = true;
		return true;
	default:
		return false;
	}
}

int main(int argc, char **argv)
{
	struct agent agent = {.fd = -1, .next_wire_sequence = 1U};
	struct sockaddr_in address = {.sin_family = AF_INET};
	CwDecoder decoder;
	uint8_t hello[8] = {0};
	uint8_t bytes[8192];
	char *end;
	long port = 44608;

	if (argc == 2) {
		port = strtol(argv[1], &end, 10);
		if (!*argv[1] || *end || port < 1L || port > 65535L)
			fail("usage: stage7a-agent [port]");
	} else if (argc != 1) {
		fail("usage: stage7a-agent [port]");
	}
	address.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1)
		return EXIT_FAILURE;
	agent.fd = socket(AF_INET, SOCK_STREAM, 0);
	if (agent.fd < 0 || connect(agent.fd, (struct sockaddr *)&address,
					 sizeof(address)) < 0)
		fail("connect");
	cw_store_u16_le(hello, CW_PROTOCOL_VERSION);
	cw_store_u16_le(hello + 2U, CW_PROTOCOL_VERSION);
	cw_store_u32_le(hello + 4U, CW_PIXEL_FORMAT_MASK_BGRA8888);
	if (!send_message(&agent, CW_MESSAGE_HELLO, hello, sizeof(hello)))
		fail("HELLO send");
	cw_decoder_init(&decoder);
	while (!agent.destroyed) {
		const ssize_t received = recv(agent.fd, bytes, sizeof(bytes), 0);

		if (received <= 0 || !cw_decoder_feed(&decoder, bytes, (size_t)received,
						     on_message, &agent))
			fail("receive/decode");
	}
	cw_decoder_destroy(&decoder);
	free(agent.framebuffer);
	close(agent.fd);
	puts("stage7a damage/resync integration: PASS");
	return EXIT_SUCCESS;
}
