/**
 * Copyright 2015 Rapid7
 * @brief Meterpreter-style Type/length/value packet handler
 * @file tlv.c
 */

#include <arpa/inet.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "tlv.h"
#include "uthash.h"

struct tlv_header {
	int len;
	uint32_t type;
};

struct tlv_packet {
	struct tlv_header h;
	char buf[];
};

struct tlv_handler {
	tlv_handler_cb cb;
	void *arg;
	UT_hash_handle hh;
	char method[];
};

struct tlv_dispatcher {
	struct tlv_handler *handlers;
};

struct tlv_dispatcher * tlv_dispatcher_new(void)
{
	struct tlv_dispatcher *td = calloc(1, sizeof(*td));

	return td;
}

struct tlv_packet *tlv_packet_new(uint32_t type, int initial_len)
{
	struct tlv_packet *p = calloc(1, sizeof(struct tlv_packet) +
			(initial_len ? initial_len : 64));
	if (p) {
		p->h.type = htonl(type);
		p->h.len = htonl(sizeof(struct tlv_header));
	}
	return p;
}

void tlv_packet_free(struct tlv_packet *p)
{
	free(p);
}

void *tlv_packet_data(struct tlv_packet *p)
{
	return &p->h;
}

int tlv_packet_len(struct tlv_packet *p)
{
	return ntohl(p->h.len);
}

void *tlv_packet_get_raw(struct tlv_packet *p, uint32_t value_type, int *len)
{
	*len = 0;
	size_t offset = 0;
	while (offset < p->h.len) {
		struct tlv_header *h = (struct tlv_header *)(p->buf + offset);
		uint32_t type = h->type & ~TLV_META_TYPE_COMPRESSED;
		if (type == value_type) {
			*len = h->len - sizeof(struct tlv_header);
			return h + 1;
		}
		offset += h->len;
	}
	return NULL;
}

char *tlv_packet_get_str(struct tlv_packet *p, uint32_t value_type)
{
	int len;
	char *str = tlv_packet_get_raw(p, value_type, &len);
	if (str != NULL) {
		if (len > 0) {
			str[len - 1] = '\0';
		} else {
			str = NULL;
		}
	}
	return str;
}

struct tlv_packet * tlv_packet_add_child(struct tlv_packet *p,
		const void *val, int len)
{
	int packet_len = tlv_packet_len(p);
	int new_len = packet_len +  len;
	p = realloc(p, new_len);
	if (p) {
		memcpy((void *)p + packet_len, val, len);
		p->h.len = htonl(new_len);
	}
	return p;
}

struct tlv_packet * tlv_packet_add_raw(struct tlv_packet *p, uint32_t type,
		const void *val, int len)
{
	int packet_len = tlv_packet_len(p);
	int new_len = packet_len + sizeof(struct tlv_header) + len;
	p = realloc(p, new_len);
	if (p) {
		struct tlv_header *hdr = (void *)p + packet_len;
		hdr->type = htonl(type);
		hdr->len = htonl(sizeof(struct tlv_header) + len);
		memcpy(hdr + 1, val, len);
		p->h.len = htonl(new_len);
	}
	return p;
}

struct tlv_packet * tlv_packet_add_str(struct tlv_packet *p,
		uint32_t type, const char *str)
{
	return tlv_packet_add_raw(p, type, str, strlen(str) + 1);
}

struct tlv_packet * tlv_packet_add_u32(struct tlv_packet *p,
		uint32_t type, uint32_t val)
{
	val = htonl(val);
	return tlv_packet_add_raw(p, type, &val, sizeof(val));
}

struct tlv_packet * tlv_packet_add_u64(struct tlv_packet *p,
		uint32_t type, uint64_t val)
{
	return tlv_packet_add_raw(p, type, &val, sizeof(val));
}

struct tlv_packet * tlv_packet_add_bool(struct tlv_packet *p,
		uint32_t type, bool val)
{
	char val_c = val;
	return tlv_packet_add_raw(p, type, &val_c, sizeof(val_c));
}

static uint32_t bitmask32(uint32_t bits)
{
	return bits % 32 == 0 ? 0 : htonl(0xffffffff << (32 - bits));
}

static void bitmask128(uint32_t bits, uint32_t mask[4])
{
	memset(mask, 0xff, 16);
	if (bits >= 96) {
		mask[3] = bitmask32(bits % 32);
	} else if (bits >= 64) {
		mask[2] = bitmask32(bits % 32);
		memset(mask + 3, 0, 4);
	} else if (bits >= 32) {
		mask[1] = bitmask32(bits % 32);
		memset(mask + 2, 0, 8);
	} else {
		mask[0] = bitmask32(bits % 32);
		memset(mask + 1, 0, 12);
	}
}

struct tlv_packet * tlv_packet_add_addr(struct tlv_packet *p,
	uint32_t addr_tlv, uint32_t mask_tlv, const struct addr *a)
{
	if (a->addr_type == ADDR_TYPE_IP) {
		p = tlv_packet_add_raw(p, addr_tlv, a->addr_data8, IP_ADDR_LEN);
		if (mask_tlv) {
			uint32_t mask = bitmask32(a->addr_bits);
			p = tlv_packet_add_raw(p, mask_tlv, &mask, IP_ADDR_LEN);
		}

	} else if (a->addr_type == ADDR_TYPE_IP6) {
		p = tlv_packet_add_raw(p, addr_tlv, a->addr_data8, IP6_ADDR_LEN);
		if (mask_tlv) {
			uint32_t mask[4];
			bitmask128(a->addr_bits, mask);
			p = tlv_packet_add_raw(p, mask_tlv, mask, IP6_ADDR_LEN);
		}
	}
	return p;
}

struct tlv_packet * tlv_packet_add_printf(struct tlv_packet *p,
		uint32_t type, char const *fmt, ...)
{
	va_list va;
	char *buffer = NULL;
	va_start(va, fmt);
	int printed = vasprintf(&buffer, fmt, va);
	if (printed != -1) {
		p = tlv_packet_add_raw(p, type, buffer, printed + 1);
		free(buffer);
	}
	va_end(va);
	return p;
}

struct tlv_packet * tlv_packet_response(struct tlv_handler_ctx *ctx, int initial_len)
{
	struct tlv_packet *p = tlv_packet_new(TLV_PACKET_TYPE_RESPONSE, initial_len);
	if (p) {
		p = tlv_packet_add_str(p, TLV_TYPE_METHOD, ctx->method);
		p = tlv_packet_add_str(p, TLV_TYPE_REQUEST_ID, ctx->id);
	}
	return p;
};

struct tlv_packet * tlv_packet_response_result(struct tlv_handler_ctx *ctx, int rc)
{
	struct tlv_packet *p = tlv_packet_response(ctx, tlv_packet_len(ctx->p) + 32);
	if (p) {
		p = tlv_packet_add_u32(p, TLV_TYPE_RESULT, rc);
	}
	return p;
}

int tlv_dispatcher_add_handler(struct tlv_dispatcher *td,
		const char *method, tlv_handler_cb cb, void *arg)
{
	struct tlv_handler *handler =
		calloc(1, sizeof(*handler) + strlen(method) + 1);
	if (handler == NULL) {
		return -1;
	}

	strcpy(handler->method, method);
	handler->cb = cb;
	handler->arg = arg;

	HASH_ADD_STR(td->handlers, method, handler);
	return 0;
}

void tlv_iter_extension_methods(struct tlv_dispatcher *td,
		const char *extension,
		void (*cb)(const char *method, void *arg), void *arg)
{
	struct tlv_handler *handler, *tmp;
	size_t extension_len = strlen(extension);
	HASH_ITER(hh, td->handlers, handler, tmp) {
		if (strncmp(handler->method, extension, extension_len) == 0) {
			cb(handler->method, arg);
		}
	}
}

static struct tlv_handler * find_handler(struct tlv_dispatcher *td,
		const char *method)
{
	struct tlv_handler *handler = NULL;
	HASH_FIND_STR(td->handlers, method, handler);
	return handler;
}

struct tlv_packet * tlv_process_request(struct tlv_dispatcher *td,
		struct tlv_packet *p)
{
	struct tlv_handler_ctx ctx = {
		.method = tlv_packet_get_str(p, TLV_TYPE_METHOD),
		.id = tlv_packet_get_str(p, TLV_TYPE_REQUEST_ID),
		.p = p
	};

	if (ctx.method == NULL || ctx.id == NULL) {
		return NULL;
	}

	struct tlv_handler *handler = find_handler(td, ctx.method);
	if (handler == NULL) {
		log_info("no handler found for method: '%s'", ctx.method);
		return tlv_packet_response_result(&ctx, TLV_RESULT_FAILURE);
	}

	log_debug("processing method: '%s' id: '%s'", ctx.method, ctx.id);
	struct tlv_packet *response = handler->cb(&ctx, handler->arg);
	if (response == NULL) {
		return tlv_packet_response_result(&ctx, TLV_RESULT_FAILURE);
	}
	return response;
}

struct tlv_packet * tlv_get_packet_buffer_queue(struct buffer_queue *q)
{
	/*
	 * Sanity check packet header and length
	 */
	struct tlv_header h;
	if (buffer_queue_len(q) < sizeof(struct tlv_header)) {
		return NULL;
	}

	buffer_queue_copy(q, &h, sizeof(struct tlv_header));
	h.type = ntohl(h.type);
	h.len = ntohl(h.len);
	if (h.len < 0 || h.len < sizeof(struct tlv_header)
			|| buffer_queue_len(q) < h.len) {
		return NULL;
	}

	/*
	 * Header is OK, read the rest of the packet
	 */
	struct tlv_packet *p = malloc(h.len);
	if (p) {
		p->h = h;
		buffer_queue_drain(q, sizeof(struct tlv_header));
		buffer_queue_remove(q, p->buf, h.len);
		p->h.len -= sizeof(struct tlv_header);
	}

	/*
	 * Sanity check sub-TLVs and byteswap
	 */
	int offset = 0;
	while (offset < p->h.len) {
		struct tlv_header *tlv = (struct tlv_header *)(p->buf + offset);
		tlv->type = htonl(tlv->type);
		tlv->len = htonl(tlv->len);
		/*
		 * Ensure the sub-TLV's fit within the packet
		 */
		if (tlv->len > (h.len - offset)
				|| tlv->len < sizeof(struct tlv_header)) {
			free(p);
			return NULL;
		}
		offset += tlv->len;
		if (tlv->len == 0) {
			break;
		}
	}

	return p;
}

void tlv_dispatcher_free(struct tlv_dispatcher *td)
{
	if (td) {
		struct tlv_handler *h, *h_tmp;
		HASH_ITER(hh, td->handlers, h, h_tmp) {
			free(h);
		}
		free(td);
	}
}