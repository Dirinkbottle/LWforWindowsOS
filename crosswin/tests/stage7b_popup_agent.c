/* Protocol-side acceptance test for the Stage 7B xdg_popup hierarchy. */
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

struct window_state {
	uint64_t id, parent;
	uint32_t width, height;
	bool frame, present, destroyed;
};
struct agent {
	int fd;
	uint64_t wire_sequence;
	bool hello, popup_crossed, input_sent;
	struct window_state windows[3];
	uint32_t create_count, destroy_count;
};

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: stage7b-popup-agent: %s\n", message);
	exit(EXIT_FAILURE);
}

static bool send_all(int fd, const uint8_t *data, size_t length)
{
	while (length != 0U) {
		ssize_t written = send(fd, data, length, 0);
		if (written > 0) {
			data += written;
			length -= (size_t)written;
		} else if (written < 0 && errno == EINTR) {
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

static struct window_state *find_window(struct agent *agent, uint64_t id)
{
	uint32_t index;

	for (index = 0U; index < agent->create_count; ++index)
		if (agent->windows[index].id == id)
			return &agent->windows[index];
	return NULL;
}

static bool frame_pixel_is(const CwWindowFrame *frame, uint32_t x, uint32_t y,
			   uint8_t blue, uint8_t green, uint8_t red, uint8_t alpha)
{
	const uint8_t *pixel;

	if (x >= frame->width || y >= frame->height)
		return false;
	pixel = frame->pixels + (size_t)y * frame->stride + (size_t)x * 4U;
	return pixel[0] == blue && pixel[1] == green && pixel[2] == red &&
		pixel[3] == alpha;
}

static bool send_popup_input(struct agent *agent, uint64_t presentation_sequence)
{
	uint8_t location[40] = {0};
	uint8_t button[48] = {0};

	/* Popup 2 is clipped at source x=74 and starts at remote-local y=120.
	 * Thus client [20,30] maps to popup surface [94,30] at global [1044,150]. */
	cw_store_u64_le(location, 2U);
	cw_store_u64_le(location + 8U, presentation_sequence);
	cw_store_i32_le(location + 16U, 20);
	cw_store_i32_le(location + 20U, 30);
	cw_store_i32_le(location + 24U, 20);
	cw_store_i32_le(location + 28U, 150);
	cw_store_u64_le(location + 32U, 1U);
	if (!send_message(agent, CW_MESSAGE_POINTER_ENTER, location, sizeof(location)))
		return false;
	memcpy(button, location, sizeof(location));
	cw_store_u32_le(button + 40U, CW_POINTER_BUTTON_LEFT);
	cw_store_u32_le(button + 44U, CW_BUTTON_PRESSED);
	if (!send_message(agent, CW_MESSAGE_POINTER_BUTTON, button, sizeof(button)))
		return false;
	cw_store_u32_le(button + 44U, CW_BUTTON_RELEASED);
	return send_message(agent, CW_MESSAGE_POINTER_BUTTON, button, sizeof(button));
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
		struct window_state *window;
		static const uint32_t expected_width[] = { ROOT_W, POPUP_W, NESTED_W };
		static const uint32_t expected_height[] = { ROOT_H, POPUP_H, NESTED_H };

		if (!agent->hello || agent->create_count == 3U ||
		    !cw_decode_window_create(payload, header->payload_length, &create) ||
		    create.window_id != agent->create_count + 1U ||
		    create.parent_window_id != (agent->create_count == 0U ? 0U : agent->create_count) ||
		    create.surface_width != expected_width[agent->create_count] ||
		    create.surface_height != expected_height[agent->create_count])
			return false;
		window = &agent->windows[agent->create_count++];
		window->id = create.window_id;
		window->parent = create.parent_window_id;
		window->width = create.surface_width;
		window->height = create.surface_height;
		return true;
	}
	case CW_MESSAGE_WINDOW_FRAME: {
		CwWindowFrame frame;
		struct window_state *window;

		if (!cw_decode_window_frame(payload, header->payload_length, &frame) ||
		    !(window = find_window(agent, frame.window_id)) ||
		    frame.width != window->width || frame.height != window->height ||
		    frame.stride != window->width * 4U ||
		    frame.pixel_bytes != (uint64_t)frame.stride * frame.height)
			return false;
		if ((window->id == 1U &&
		     (!frame_pixel_is(&frame, 0U, 0U, 0x80U, 0x80U, 0x80U, 0xffU) ||
		      !frame_pixel_is(&frame, 50U, 50U, 0x70U, 0x20U, 0x30U, 0xffU))) ||
		    (window->id == 2U && !frame_pixel_is(&frame, 0U, 0U, 0U, 0U, 0xc0U, 0xc0U)) ||
		    (window->id == 3U && !frame_pixel_is(&frame, 0U, 0U, 0U, 0xc0U, 0U, 0xc0U)))
			return false;
		window->frame = true;
		return true;
	}
	case CW_MESSAGE_WINDOW_PRESENT: {
		CwWindowPresent present;
		struct window_state *window;
		uint8_t ack[16];

		if (!cw_decode_window_present(payload, header->payload_length, &present) ||
		    !(window = find_window(agent, present.window_id)) || !window->frame)
			return false;
		if (window->id == 2U && present.visible && present.source_x > 0)
			agent->popup_crossed = true;
		window->present = true;
		cw_store_u64_le(ack, present.window_id);
		cw_store_u64_le(ack + 8U, present.presentation_sequence);
		if (!send_message(agent, CW_MESSAGE_WINDOW_PRESENT_ACK, ack, sizeof(ack)))
			return false;
		if (window->id == 2U && !agent->input_sent) {
			/* Let the client bind the dedicated crosswin-proxy seat before
			 * injecting its deterministic click. */
			const struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
			(void)nanosleep(&delay, NULL);
			agent->input_sent = true;
			return send_popup_input(agent, present.presentation_sequence);
		}
		return true;
	}
	case CW_MESSAGE_WINDOW_DESTROY: {
		struct window_state *window;

		if (header->payload_length != 8U ||
		    !(window = find_window(agent, cw_load_u64_le(payload))) ||
		    window->destroyed || !window->frame || !window->present)
			return false;
		window->destroyed = true;
		agent->destroy_count++;
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
	uint8_t buffer[8192];
	long port = 44611L;
	char *end;

	if (argc == 2) {
		port = strtol(argv[1], &end, 10);
		if (*end != '\0' || port < 1L || port > 65535L)
			fail("usage");
	} else if (argc != 1) {
		fail("usage");
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
	while (agent.destroy_count != 3U) {
		ssize_t received = recv(agent.fd, buffer, sizeof(buffer), 0);
		if (received <= 0 || !cw_decoder_feed(&decoder, buffer, (size_t)received,
						      on_message, &agent))
			fail("receive/decode");
	}
	cw_decoder_destroy(&decoder);
	if (!agent.popup_crossed || !agent.input_sent)
		fail("popup did not cross the output boundary");
	close(agent.fd);
	puts("stage7b popup hierarchy integration: PASS");
	return EXIT_SUCCESS;
}
