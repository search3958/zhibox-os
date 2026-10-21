/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zh_png.c - 最小 PNG デコーダ
 *
 *   - 8bit / 非インターレース / color type 0,2,3,4,6
 *   - zlib (RFC 1950/1951) の inflate を内蔵 (stored / fixed / dynamic)
 *   - PNG フィルタ (None/Sub/Up/Average/Paeth) を復元
 *   - 出力は RGBA8888
 * 浮動小数点や外部ライブラリは使わない。
 */
#include "zh_png.h"

#include <string.h>

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------ inflate */

struct zh_zbuf {
	const uint8_t *in;
	size_t len;
	size_t pos;
	uint32_t bits;
	int count;
};

struct zh_huff {
	uint16_t count[16];
	uint16_t symbol[288];
};

static void zbuf_refill(struct zh_zbuf *z)
{
	while (z->count <= 24 && z->pos < z->len) {
		z->bits |= (uint32_t)z->in[z->pos++] << z->count;
		z->count += 8;
	}
}

static int zbuf_get(struct zh_zbuf *z, int n)
{
	int value;

	if (n == 0)
		return 0;
	zbuf_refill(z);
	if (z->count < n)
		return -1;
	value = (int)(z->bits & ((1u << n) - 1u));
	z->bits >>= n;
	z->count -= n;
	return value;
}

static void zbuf_align(struct zh_zbuf *z)
{
	int drop = z->count & 7;

	z->bits >>= drop;
	z->count -= drop;
}

static int zbuf_byte(struct zh_zbuf *z)
{
	if (z->count >= 8) {
		int value = (int)(z->bits & 0xffu);

		z->bits >>= 8;
		z->count -= 8;
		return value;
	}
	if (z->pos >= z->len)
		return -1;
	return z->in[z->pos++];
}

static void huff_build(struct zh_huff *h, const uint8_t *lengths, int count)
{
	uint16_t offsets[16];
	int i;
	int len;

	memset(h->count, 0, sizeof(h->count));
	for (i = 0; i < count; i++)
		h->count[lengths[i]]++;
	h->count[0] = 0;

	offsets[0] = 0;
	offsets[1] = 0;
	for (len = 1; len < 15; len++)
		offsets[len + 1] = (uint16_t)(offsets[len] + h->count[len]);

	for (i = 0; i < count; i++) {
		if (lengths[i])
			h->symbol[offsets[lengths[i]]++] = (uint16_t)i;
	}
}

static int huff_decode(struct zh_zbuf *z, const struct zh_huff *h)
{
	int code = 0;
	int first = 0;
	int index = 0;
	int len;

	for (len = 1; len <= 15; len++) {
		int bit = zbuf_get(z, 1);
		int count;

		if (bit < 0)
			return -1;
		code |= bit;
		count = h->count[len];
		if (code - first < count)
			return h->symbol[index + (code - first)];
		index += count;
		first = (first + count) << 1;
		code <<= 1;
	}
	return -1;
}

static const uint16_t length_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
	31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};

static const uint8_t length_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
	2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};

static const uint16_t distance_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
	193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
	6145, 8193, 12289, 16385, 24577,
};

static const uint8_t distance_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
	6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static void fixed_tables(struct zh_huff *lit, struct zh_huff *dist)
{
	uint8_t lengths[288];
	int i;

	for (i = 0; i < 144; i++)
		lengths[i] = 8;
	for (; i < 256; i++)
		lengths[i] = 9;
	for (; i < 280; i++)
		lengths[i] = 7;
	for (; i < 288; i++)
		lengths[i] = 8;
	huff_build(lit, lengths, 288);

	for (i = 0; i < 30; i++)
		lengths[i] = 5;
	huff_build(dist, lengths, 30);
}

static int dynamic_tables(struct zh_zbuf *z, struct zh_huff *lit,
			  struct zh_huff *dist)
{
	static const uint8_t order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
		11, 4, 12, 3, 13, 2, 14, 1, 15,
	};
	uint8_t code_lengths[19];
	uint8_t lengths[288 + 30];
	struct zh_huff code_huff;
	int hlit;
	int hdist;
	int hclen;
	int total;
	int i;

	hlit = zbuf_get(z, 5) + 257;
	hdist = zbuf_get(z, 5) + 1;
	hclen = zbuf_get(z, 4) + 4;
	if (hlit < 257 || hlit > 288 || hdist < 1 || hdist > 30 ||
	    hclen > 19)
		return -1;

	memset(code_lengths, 0, sizeof(code_lengths));
	for (i = 0; i < hclen; i++) {
		int value = zbuf_get(z, 3);

		if (value < 0)
			return -1;
		code_lengths[order[i]] = (uint8_t)value;
	}
	huff_build(&code_huff, code_lengths, 19);

	total = hlit + hdist;
	memset(lengths, 0, sizeof(lengths));
	i = 0;
	while (i < total) {
		int symbol = huff_decode(z, &code_huff);

		if (symbol < 0)
			return -1;
		if (symbol < 16) {
			lengths[i++] = (uint8_t)symbol;
		} else if (symbol == 16) {
			int repeat;
			uint8_t previous;

			if (i == 0)
				return -1;
			repeat = zbuf_get(z, 2) + 3;
			previous = lengths[i - 1];
			while (repeat-- && i < total)
				lengths[i++] = previous;
		} else if (symbol == 17) {
			int repeat = zbuf_get(z, 3) + 3;

			while (repeat-- && i < total)
				lengths[i++] = 0;
		} else if (symbol == 18) {
			int repeat = zbuf_get(z, 7) + 11;

			while (repeat-- && i < total)
				lengths[i++] = 0;
		} else {
			return -1;
		}
	}

	huff_build(lit, lengths, hlit);
	huff_build(dist, lengths + hlit, hdist);
	return 0;
}

static int inflate_block(struct zh_zbuf *z, struct zh_huff *lit,
			 struct zh_huff *dist, uint8_t *out, size_t out_size,
			 size_t *written)
{
	size_t offset = *written;

	for (;;) {
		int symbol = huff_decode(z, lit);

		if (symbol < 0)
			return -1;
		if (symbol < 256) {
			if (offset >= out_size)
				return -1;
			out[offset++] = (uint8_t)symbol;
		} else if (symbol == 256) {
			break;
		} else {
			int code = symbol - 257;
			int length;
			int distance_code;
			int distance;

			if (code < 0 || code >= 29)
				return -1;
			length = length_base[code];
			if (length_extra[code]) {
				int extra = zbuf_get(z, length_extra[code]);

				if (extra < 0)
					return -1;
				length += extra;
			}
			distance_code = huff_decode(z, dist);
			if (distance_code < 0 || distance_code >= 30)
				return -1;
			distance = distance_base[distance_code];
			if (distance_extra[distance_code]) {
				int extra = zbuf_get(z, distance_extra[distance_code]);

				if (extra < 0)
					return -1;
				distance += extra;
			}
			if (distance <= 0 || (size_t)distance > offset)
				return -1;
			if (offset + (size_t)length > out_size)
				return -1;
			while (length--) {
				out[offset] = out[offset - (size_t)distance];
				offset++;
			}
		}
	}

	*written = offset;
	return 0;
}

static int zlib_inflate(const uint8_t *input, size_t input_size, uint8_t *out,
			size_t out_size)
{
	struct zh_zbuf z;
	size_t written = 0;
	int final = 0;

	if (input_size < 6)
		return -1;
	if ((input[0] & 0x0f) != 8)
		return -1;

	memset(&z, 0, sizeof(z));
	z.in = input;
	z.len = input_size;
	z.pos = 2; /* zlib ヘッダ (2 byte) */

	while (!final) {
		int type;

		final = zbuf_get(&z, 1);
		type = zbuf_get(&z, 2);
		if (final < 0 || type < 0)
			return -1;

		if (type == 0) {
			int length;
			int inverse;

			zbuf_align(&z);
			length = zbuf_byte(&z);
			length |= zbuf_byte(&z) << 8;
			inverse = zbuf_byte(&z);
			inverse |= zbuf_byte(&z) << 8;
			if (length < 0 || inverse < 0 ||
			    (length ^ 0xffff) != inverse)
				return -1;
			while (length--) {
				int byte = zbuf_byte(&z);

				if (byte < 0 || written >= out_size)
					return -1;
				out[written++] = (uint8_t)byte;
			}
		} else if (type == 1 || type == 2) {
			struct zh_huff lit;
			struct zh_huff dist;

			if (type == 1)
				fixed_tables(&lit, &dist);
			else if (dynamic_tables(&z, &lit, &dist) < 0)
				return -1;
			if (inflate_block(&z, &lit, &dist, out, out_size,
					  &written) < 0)
				return -1;
		} else {
			return -1;
		}
	}

	return written == out_size ? 0 : -1;
}

/* ------------------------------------------------------------ PNG */

static int paeth(int a, int b, int c)
{
	int p = a + b - c;
	int pa = p > a ? p - a : a - p;
	int pb = p > b ? p - b : b - p;
	int pc = p > c ? p - c : c - p;

	if (pa <= pb && pa <= pc)
		return a;
	if (pb <= pc)
		return b;
	return c;
}

int zh_png_decode(const uint8_t *data, size_t size, zh_png_alloc_fn alloc,
		  uint8_t **rgba, int *width, int *height)
{
	static const uint8_t signature[8] = {
		0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a,
	};
	uint32_t image_width = 0;
	uint32_t image_height = 0;
	uint8_t bit_depth = 0;
	uint8_t color_type = 0;
	uint8_t compression = 0;
	uint8_t filter_method = 0;
	uint8_t interlace = 0;
	const uint8_t *palette = NULL;
	uint32_t palette_len = 0;
	size_t position;
	size_t idat_size = 0;
	uint8_t *compressed;
	uint8_t *raw;
	uint8_t *pixels;
	size_t idat_pos;
	size_t raw_size;
	int channels;
	size_t stride;
	uint32_t x;
	uint32_t y;

	if (!data || !alloc || !rgba || !width || !height)
		return -1;
	if (size < 33)
		return -1;
	if (memcmp(data, signature, 8) != 0)
		return -1;

	/* 最初のパス: IHDR / PLTE / IDAT の合計サイズ */
	position = 8;
	while (position + 12 <= size) {
		uint32_t length = be32(data + position);
		const uint8_t *type = data + position + 4;
		const uint8_t *body = data + position + 8;

		if (length > size - position - 12)
			return -1;
		if (memcmp(type, "IHDR", 4) == 0) {
			if (length != 13)
				return -1;
			image_width = be32(body);
			image_height = be32(body + 4);
			bit_depth = body[8];
			color_type = body[9];
			compression = body[10];
			filter_method = body[11];
			interlace = body[12];
		} else if (memcmp(type, "PLTE", 4) == 0) {
			palette = body;
			palette_len = length;
		} else if (memcmp(type, "IDAT", 4) == 0) {
			idat_size += length;
		} else if (memcmp(type, "IEND", 4) == 0) {
			break;
		}
		position += 12 + length;
	}

	if (!image_width || !image_height ||
	    image_width > 8192 || image_height > 8192)
		return -1;
	if (bit_depth != 8 || compression != 0 || filter_method != 0 ||
	    interlace != 0)
		return -1;

	switch (color_type) {
	case 0:
		channels = 1;
		break;
	case 2:
		channels = 3;
		break;
	case 3:
		channels = 1;
		if (!palette || palette_len < 3)
			return -1;
		break;
	case 4:
		channels = 2;
		break;
	case 6:
		channels = 4;
		break;
	default:
		return -1;
	}

	if (!idat_size)
		return -1;

	raw_size = (size_t)image_height * (1u + (size_t)image_width *
						  (size_t)channels);
	if (raw_size / (size_t)image_height !=
	    1u + (size_t)image_width * (size_t)channels)
		return -1;

	compressed = alloc(idat_size);
	raw = alloc(raw_size);
	pixels = alloc((size_t)image_width * (size_t)image_height * 4u);
	if (!compressed || !raw || !pixels)
		return -1;

	/* 2 パス目: IDAT を連結 */
	position = 8;
	idat_pos = 0;
	while (position + 12 <= size) {
		uint32_t length = be32(data + position);
		const uint8_t *type = data + position + 4;
		const uint8_t *body = data + position + 8;

		if (length > size - position - 12)
			return -1;
		if (memcmp(type, "IDAT", 4) == 0) {
			if (idat_pos + length > idat_size)
				return -1;
			memcpy(compressed + idat_pos, body, length);
			idat_pos += length;
		} else if (memcmp(type, "IEND", 4) == 0) {
			break;
		}
		position += 12 + length;
	}
	if (idat_pos != idat_size)
		return -1;

	if (zlib_inflate(compressed, idat_size, raw, raw_size) < 0)
		return -1;

	/* フィルタ復元 (in place) */
	stride = 1u + (size_t)image_width * (size_t)channels;
	for (y = 0; y < image_height; y++) {
		uint8_t *row = raw + (size_t)y * stride;
		uint8_t *current = row + 1;
		uint8_t *previous = y ? raw + (size_t)(y - 1) * stride + 1 : NULL;
		uint8_t filter = row[0];
		size_t i;
		size_t row_bytes = (size_t)image_width * (size_t)channels;

		for (i = 0; i < row_bytes; i++) {
			int a = i >= (size_t)channels ? current[i - (size_t)channels] : 0;
			int b = previous ? previous[i] : 0;
			int c = (previous && i >= (size_t)channels) ?
					previous[i - (size_t)channels] : 0;

			switch (filter) {
			case 0:
				break;
			case 1:
				current[i] = (uint8_t)(current[i] + a);
				break;
			case 2:
				current[i] = (uint8_t)(current[i] + b);
				break;
			case 3:
				current[i] = (uint8_t)(current[i] + ((a + b) >> 1));
				break;
			case 4:
				current[i] = (uint8_t)(current[i] + paeth(a, b, c));
				break;
			default:
				return -1;
			}
		}
	}

	/* RGBA へ変換 */
	for (y = 0; y < image_height; y++) {
		const uint8_t *row = raw + (size_t)y * stride + 1;

		for (x = 0; x < image_width; x++) {
			uint8_t *out = pixels + ((size_t)y * image_width + x) * 4u;

			if (color_type == 0) {
				uint8_t gray = row[x];
				out[0] = gray;
				out[1] = gray;
				out[2] = gray;
				out[3] = 255;
			} else if (color_type == 2) {
				const uint8_t *src = row + (size_t)x * 3u;
				out[0] = src[0];
				out[1] = src[1];
				out[2] = src[2];
				out[3] = 255;
			} else if (color_type == 3) {
				uint32_t index = row[x];
				if (index * 3u + 2u >= palette_len)
					return -1;
				out[0] = palette[index * 3u];
				out[1] = palette[index * 3u + 1u];
				out[2] = palette[index * 3u + 2u];
				out[3] = 255;
			} else if (color_type == 4) {
				const uint8_t *src = row + (size_t)x * 2u;
				out[0] = src[0];
				out[1] = src[0];
				out[2] = src[0];
				out[3] = src[1];
			} else {
				const uint8_t *src = row + (size_t)x * 4u;
				out[0] = src[0];
				out[1] = src[1];
				out[2] = src[2];
				out[3] = src[3];
			}
		}
	}

	*rgba = pixels;
	*width = (int)image_width;
	*height = (int)image_height;
	return 0;
}
