/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zh_font.c - 最小 TrueType パーサ / ラスタライザ
 *
 *   head / maxp / hhea / hmtx / cmap / loca / glyf のみを使う。
 *   アウトライン (2 次ベジェ) を 26.6 固定小数点の線分に分解し、
 *   4 倍垂直スーパーサンプリング + 水平方向の正確な被覆率で
 *   8bit の alpha マスクを作る。浮動小数点は使わない。
 */
#include "zh_font.h"

#include <string.h>

#define ZH_MAX_POINTS	512
#define ZH_MAX_EDGES	4096
#define ZH_MAX_DIM	128
#define ZH_CURVE_STEPS	10

struct zh_point {
	int32_t x;
	int32_t y;
	uint8_t on;
};

struct zh_edge {
	int32_t x0;
	int32_t y0;
	int32_t x1;
	int32_t y1;
};

static struct zh_point g_points[ZH_MAX_POINTS];
static uint8_t g_flags[ZH_MAX_POINTS];
static uint16_t g_ends[ZH_MAX_POINTS];
static int32_t g_cx[ZH_MAX_POINTS];
static int32_t g_cy[ZH_MAX_POINTS];
static uint8_t g_con[ZH_MAX_POINTS];
static struct zh_edge g_edges[ZH_MAX_EDGES];
static int32_t g_cross_x[ZH_MAX_EDGES];
static int8_t g_cross_dir[ZH_MAX_EDGES];
static uint32_t g_acc[ZH_MAX_DIM * ZH_MAX_DIM];
static int g_npoints;
static int g_ncontours;
static int g_nedges;

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t be16s(const uint8_t *p)
{
	return (int16_t)be16(p);
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static int tag_eq(const uint8_t *p, const char *tag)
{
	return p[0] == (uint8_t)tag[0] && p[1] == (uint8_t)tag[1] &&
	       p[2] == (uint8_t)tag[2] && p[3] == (uint8_t)tag[3];
}

static int in_range(uint32_t off, uint32_t len, uint32_t total)
{
	return off <= total && len <= total - off;
}

/* ------------------------------------------------------------ cmap */

static int cmap_rank(uint16_t platform, uint16_t encoding, int format)
{
	if (format != 4 && format != 12)
		return -1;
	if (platform == 3 && encoding == 10 && format == 12)
		return 5;
	if (platform == 3 && encoding == 1 && format == 4)
		return 4;
	if (platform == 0 && format == 12)
		return 3;
	if (platform == 0)
		return 2;
	return format == 4 ? 1 : 0;
}

static uint32_t cmap4_lookup(const struct zh_font *font, uint32_t cp)
{
	const uint8_t *cm = font->data + font->cmap_off;
	uint32_t need;
	uint16_t seg_x2;
	uint16_t segs;
	uint16_t i;
	const uint8_t *end;
	const uint8_t *start;
	const uint8_t *delta;
	const uint8_t *range;

	if (cp > 0xffff)
		return 0;
	if (font->cmap_len < 16)
		return 0;

	seg_x2 = be16(cm + 6);
	segs = (uint16_t)(seg_x2 / 2);
	if (!segs)
		return 0;
	need = 16u + (uint32_t)seg_x2 * 4u;
	if (font->cmap_len < need)
		return 0;

	end = cm + 14;
	start = end + seg_x2 + 2;
	delta = start + seg_x2;
	range = delta + seg_x2;

	for (i = 0; i < segs; i++) {
		uint16_t last = be16(end + (uint32_t)i * 2);
		uint16_t first;
		uint16_t d;
		uint16_t ro;
		uint32_t addr;
		uint32_t glyph;

		if (cp > last)
			continue;
		first = be16(start + (uint32_t)i * 2);
		if (cp < first)
			return 0;
		d = be16(delta + (uint32_t)i * 2);
		ro = be16(range + (uint32_t)i * 2);
		if (ro == 0)
			return (cp + d) & 0xffff;
		addr = (uint32_t)(range + (uint32_t)i * 2 - font->data) + ro +
		       2u * (cp - first);
		if (!in_range(addr, 2, font->size))
			return 0;
		glyph = be16(font->data + addr);
		if (glyph)
			glyph = (glyph + d) & 0xffff;
		return glyph;
	}
	return 0;
}

static uint32_t cmap12_lookup(const struct zh_font *font, uint32_t cp)
{
	const uint8_t *cm = font->data + font->cmap_off;
	uint32_t groups;
	uint32_t lo;
	uint32_t hi;

	if (font->cmap_len < 16)
		return 0;
	groups = be32(cm + 12);
	if (groups > (font->cmap_len - 16) / 12)
		return 0;

	lo = 0;
	hi = groups;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const uint8_t *g = cm + 16 + mid * 12;
		uint32_t first = be32(g);
		uint32_t last = be32(g + 4);

		if (cp < first)
			hi = mid;
		else if (cp > last)
			lo = mid + 1;
		else
			return be32(g + 8) + (cp - first);
	}
	return 0;
}

uint32_t zh_font_glyph_index(const struct zh_font *font, uint32_t codepoint)
{
	if (!font || !font->data)
		return 0;
	if (font->cmap_format == 12)
		return cmap12_lookup(font, codepoint);
	return cmap4_lookup(font, codepoint);
}

/* ------------------------------------------------------------ メトリクス */

int zh_font_advance(const struct zh_font *font, uint32_t glyph, int px)
{
	uint32_t index;
	uint32_t advance;

	if (!font || !font->units_per_em || px <= 0 ||
	    glyph >= font->num_glyphs || !font->num_hmetrics)
		return 0;

	index = glyph < font->num_hmetrics ? glyph : font->num_hmetrics - 1;
	if (!in_range(font->hmtx_off + index * 4u, 2, font->size))
		return 0;
	advance = be16(font->data + font->hmtx_off + index * 4u);
	return (int)(((uint64_t)advance * (uint32_t)px +
		      font->units_per_em / 2) / font->units_per_em);
}

int zh_font_ascent(const struct zh_font *font, int px)
{
	int64_t value;

	if (!font || !font->units_per_em || px <= 0)
		return 0;
	value = (int64_t)font->ascent * px;
	return (int)((value + font->units_per_em - 1) / font->units_per_em);
}

/* ------------------------------------------------------------ glyf */

static int parse_glyph(const struct zh_font *font, uint32_t glyph, int depth);

static int glyph_offset(const struct zh_font *font, uint32_t glyph,
			uint32_t *offset, uint32_t *length)
{
	uint32_t pos;
	uint32_t next;
	uint32_t start;
	uint32_t end;

	if (glyph >= font->num_glyphs)
		return -1;
	if (font->loca_long) {
		pos = glyph * 4u;
		if (!in_range(font->loca_off + pos, 8, font->size))
			return -1;
		start = be32(font->data + font->loca_off + pos);
		end = be32(font->data + font->loca_off + pos + 4);
	} else {
		pos = glyph * 2u;
		if (!in_range(font->loca_off + pos, 4, font->size))
			return -1;
		start = (uint32_t)be16(font->data + font->loca_off + pos) * 2u;
		next = (uint32_t)be16(font->data + font->loca_off + pos + 2) * 2u;
		end = next;
	}
	if (end < start || !in_range(font->glyf_off + start, end - start,
				     font->size))
		return -1;
	*offset = font->glyf_off + start;
	*length = end - start;
	return 0;
}

static int parse_simple(const uint8_t *g, uint32_t length,
			int16_t contours)
{
	const uint8_t *limit = g + length;
	const uint8_t *p;
	uint32_t total;
	uint32_t i;
	uint32_t instruction_length;
	int32_t x;
	int32_t y;
	int base = g_npoints;

	if (contours <= 0)
		return 0;
	if (10u + (uint32_t)contours * 2u > length)
		return -1;

	total = (uint32_t)be16(g + 10 + (uint32_t)(contours - 1) * 2u) + 1u;
	if (total == 0 || total > ZH_MAX_POINTS ||
	    base + (int)total > ZH_MAX_POINTS)
		return -1;
	if (g_ncontours + contours > ZH_MAX_POINTS)
		return -1;

	for (i = 0; i < (uint32_t)contours; i++)
		g_ends[g_ncontours + (int)i] =
			(uint16_t)(base + be16(g + 10 + i * 2u));

	p = g + 10 + (uint32_t)contours * 2u;
	if (p + 2 > limit)
		return -1;
	instruction_length = be16(p);
	p += 2;
	if (p + instruction_length > limit)
		return -1;
	p += instruction_length;

	/* フラグ (repeat 展開) */
	i = 0;
	while (i < total) {
		uint8_t flag;

		if (p >= limit)
			return -1;
		flag = *p++;
		g_flags[i] = flag;
		g_points[base + (int)i].on = (uint8_t)(flag & 1);
		i++;
		if (flag & 8) {
			uint8_t repeat;

			if (p >= limit)
				return -1;
			repeat = *p++;
			while (repeat--) {
				if (i >= total)
					break;
				g_flags[i] = flag;
				g_points[base + (int)i].on =
					(uint8_t)(flag & 1);
				i++;
			}
		}
	}

	/* X 座標 */
	x = 0;
	for (i = 0; i < total; i++) {
		uint8_t flag = g_flags[i];

		if (flag & 2) {
			int32_t delta;

			if (p >= limit)
				return -1;
			delta = *p++;
			x += (flag & 16) ? delta : -delta;
		} else if (!(flag & 16)) {
			if (p + 2 > limit)
				return -1;
			x += be16s(p);
			p += 2;
		}
		g_points[base + (int)i].x = x;
	}

	/* Y 座標 */
	y = 0;
	for (i = 0; i < total; i++) {
		uint8_t flag = g_flags[i];

		if (flag & 4) {
			int32_t delta;

			if (p >= limit)
				return -1;
			delta = *p++;
			y += (flag & 32) ? delta : -delta;
		} else if (!(flag & 32)) {
			if (p + 2 > limit)
				return -1;
			y += be16s(p);
			p += 2;
		}
		g_points[base + (int)i].y = y;
	}

	g_npoints = base + (int)total;
	g_ncontours += contours;
	return 0;
}

static int parse_composite(const struct zh_font *font, const uint8_t *g,
			   uint32_t length, int depth)
{
	const uint8_t *limit = g + length;
	const uint8_t *p = g + 10;
	int more = 1;

	while (more) {
		uint16_t flags;
		uint16_t component;
		int32_t dx = 0;
		int32_t dy = 0;
		int base;
		int i;

		if (p + 4 > limit)
			return -1;
		flags = be16(p);
		component = be16(p + 2);
		p += 4;

		if (flags & 0x0001) {
			if (p + 4 > limit)
				return -1;
			if (flags & 0x0002) {
				dx = be16s(p);
				dy = be16s(p + 2);
			}
			p += 4;
		} else {
			if (p + 2 > limit)
				return -1;
			if (flags & 0x0002) {
				dx = (int8_t)p[0];
				dy = (int8_t)p[1];
			}
			p += 2;
		}

		/* スケール付きコンポーネントは未対応 (稀) */
		if (flags & 0x0008) {
			if (p + 2 > limit)
				return -1;
			p += 2;
		}
		if (flags & 0x0040) {
			if (p + 4 > limit)
				return -1;
			p += 4;
		}
		if (flags & 0x0080) {
			if (p + 8 > limit)
				return -1;
			p += 8;
		}

		base = g_npoints;
		if (parse_glyph(font, component, depth + 1) < 0)
			return -1;
		for (i = base; i < g_npoints; i++) {
			g_points[i].x += dx;
			g_points[i].y += dy;
		}

		more = (flags & 0x0020) != 0;
	}
	return 0;
}

static int parse_glyph(const struct zh_font *font, uint32_t glyph, int depth)
{
	uint32_t offset;
	uint32_t length;
	const uint8_t *g;
	int16_t contours;

	if (depth > 5)
		return -1;
	if (glyph_offset(font, glyph, &offset, &length) < 0)
		return -1;
	if (length < 10)
		return 0;
	g = font->data + offset;
	contours = be16s(g);

	if (contours >= 0)
		return parse_simple(g, length, contours);
	return parse_composite(font, g, length, depth);
}

/* ------------------------------------------------------------ ラスタライズ */

static int floor_div(int32_t value, int32_t divisor)
{
	int32_t quotient = value / divisor;

	if ((value % divisor) && ((value < 0) != (divisor < 0)))
		quotient--;
	return quotient;
}

static int ceil_div(int32_t value, int32_t divisor)
{
	return -floor_div(-value, divisor);
}

static int32_t scale_26_6(int32_t value, int px, uint32_t upem)
{
	return (int32_t)(((int64_t)value * px * 64) / (int64_t)upem);
}

static int add_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
	if (y0 == y1)
		return 0;
	if (g_nedges >= ZH_MAX_EDGES)
		return -1;
	g_edges[g_nedges].x0 = x0;
	g_edges[g_nedges].y0 = y0;
	g_edges[g_nedges].x1 = x1;
	g_edges[g_nedges].y1 = y1;
	g_nedges++;
	return 0;
}

static void add_quad(int32_t x0, int32_t y0, int32_t cx, int32_t cy,
		     int32_t x1, int32_t y1)
{
	int32_t prev_x = x0;
	int32_t prev_y = y0;
	int i;

	for (i = 1; i <= ZH_CURVE_STEPS; i++) {
		int32_t u = ZH_CURVE_STEPS - i;
		int32_t v = i;
		int32_t den = ZH_CURVE_STEPS * ZH_CURVE_STEPS;
		int32_t x = (x0 * u * u + 2 * cx * u * v + x1 * v * v) / den;
		int32_t y = (y0 * u * u + 2 * cy * u * v + y1 * v * v) / den;

		if (add_edge(prev_x, prev_y, x, y) < 0)
			return;
		prev_x = x;
		prev_y = y;
	}
}

static void process_contour(int first, int last)
{
	int count = last - first + 1;
	int start = -1;
	int i;
	int begin;
	int32_t sx;
	int32_t sy;
	int32_t px;
	int32_t py;
	int32_t ctrl_x = 0;
	int32_t ctrl_y = 0;
	int pending = 0;

	for (i = 0; i < count; i++) {
		if (g_con[first + i]) {
			start = i;
			break;
		}
	}

	if (start >= 0) {
		sx = g_cx[first + start];
		sy = g_cy[first + start];
		begin = start + 1;
	} else {
		if (count < 2)
			return;
		sx = (g_cx[first] + g_cx[first + 1]) / 2;
		sy = (g_cy[first] + g_cy[first + 1]) / 2;
		begin = 1;
	}

	px = sx;
	py = sy;

	for (i = 0; i < count; i++) {
		int index = first + (begin + i) % count;
		int32_t x = g_cx[index];
		int32_t y = g_cy[index];

		if (g_con[index]) {
			if (pending) {
				add_quad(px, py, ctrl_x, ctrl_y, x, y);
				pending = 0;
			} else {
				add_edge(px, py, x, y);
			}
			px = x;
			py = y;
		} else {
			if (pending) {
				int32_t mid_x = (ctrl_x + x) / 2;
				int32_t mid_y = (ctrl_y + y) / 2;

				add_quad(px, py, ctrl_x, ctrl_y, mid_x, mid_y);
				px = mid_x;
				py = mid_y;
			}
			ctrl_x = x;
			ctrl_y = y;
			pending = 1;
		}
	}

	if (pending)
		add_quad(px, py, ctrl_x, ctrl_y, sx, sy);
	else
		add_edge(px, py, sx, sy);
}

static void add_span(int32_t xa, int32_t xb, int row, int bx, int bw)
{
	int32_t left;
	int32_t right;
	int px0;
	int px1;
	int x;

	if (xa > xb) {
		int32_t tmp = xa;

		xa = xb;
		xb = tmp;
	}

	px0 = floor_div(xa, 64);
	px1 = floor_div(xb, 64);
	if (px0 < bx)
		px0 = bx;
	if (px1 > bx + bw - 1)
		px1 = bx + bw - 1;

	for (x = px0; x <= px1; x++) {
		left = x * 64;
		right = left + 64;
		if (xa > left)
			left = xa;
		if (xb < right)
			right = xb;
		if (right > left)
			g_acc[(uint32_t)row * (uint32_t)bw +
			      (uint32_t)(x - bx)] += (uint32_t)(right - left);
	}
}

int zh_font_render(const struct zh_font *font, uint32_t glyph, int px,
		   uint8_t *scratch, size_t scratch_len,
		   int *bw, int *bh, int *bx, int *by)
{
	uint32_t offset;
	uint32_t length;
	const uint8_t *g;
	int32_t x_min;
	int32_t y_min;
	int32_t x_max;
	int32_t y_max;
	int left;
	int right;
	int top;
	int bottom;
	int box_x;
	int box_y;
	int box_w;
	int box_h;
	int contour;
	int row;
	int sub;
	int i;

	if (!font || !font->data || !scratch || !bw || !bh || !bx || !by)
		return -1;
	if (px <= 0 || px > 64)
		return -1;
	*bw = 0;
	*bh = 0;
	*bx = 0;
	*by = 0;
	if (glyph >= font->num_glyphs || !font->units_per_em)
		return 0;

	if (glyph_offset(font, glyph, &offset, &length) < 0 || length < 10)
		return 0;
	g = font->data + offset;

	x_min = be16s(g + 2);
	y_min = be16s(g + 4);
	x_max = be16s(g + 6);
	y_max = be16s(g + 8);

	g_npoints = 0;
	g_ncontours = 0;
	if (parse_glyph(font, glyph, 0) < 0 || g_npoints == 0)
		return 0;

	left = scale_26_6(x_min, px, font->units_per_em);
	right = scale_26_6(x_max, px, font->units_per_em);
	top = -scale_26_6(y_max, px, font->units_per_em);
	bottom = -scale_26_6(y_min, px, font->units_per_em);

	box_x = floor_div(left, 64) - 1;
	box_y = floor_div(top, 64) - 1;
	box_w = ceil_div(right, 64) + 1 - box_x;
	box_h = ceil_div(bottom, 64) + 1 - box_y;
	if (box_w <= 0 || box_h <= 0 || box_w > ZH_MAX_DIM ||
	    box_h > ZH_MAX_DIM)
		return -1;
	if ((size_t)box_w * (size_t)box_h > scratch_len)
		return -1;

	for (i = 0; i < g_npoints; i++) {
		g_cx[i] = scale_26_6(g_points[i].x, px, font->units_per_em);
		g_cy[i] = -scale_26_6(g_points[i].y, px, font->units_per_em);
		g_con[i] = g_points[i].on;
	}

	g_nedges = 0;
	for (contour = 0; contour < g_ncontours; contour++) {
		int first = contour == 0 ? 0 : g_ends[contour - 1] + 1;
		int last = g_ends[contour];

		if (last >= first)
			process_contour(first, last);
	}
	if (g_nedges == 0)
		return 0;

	memset(g_acc, 0, (size_t)box_w * (size_t)box_h * sizeof(g_acc[0]));

	for (row = 0; row < box_h; row++) {
		for (sub = 0; sub < 4; sub++) {
			int32_t y = (int32_t)(box_y + row) * 64 + sub * 16 + 8;
			int crossings = 0;
			int wind = 0;
			int32_t span_start = 0;
			int a;
			int b;

			for (i = 0; i < g_nedges; i++) {
				int32_t y0 = g_edges[i].y0;
				int32_t y1 = g_edges[i].y1;
				int32_t x0;
				int32_t x1;

				if (!((y0 <= y && y < y1) ||
				      (y1 <= y && y < y0)))
					continue;
				x0 = g_edges[i].x0;
				x1 = g_edges[i].x1;
				g_cross_x[crossings] =
					x0 + (int32_t)(((int64_t)(y - y0) *
							(x1 - x0)) / (y1 - y0));
				g_cross_dir[crossings] = (int8_t)(y1 > y0 ? 1 : -1);
				crossings++;
				if (crossings >= ZH_MAX_EDGES)
					break;
			}

			for (a = 1; a < crossings; a++) {
				int32_t key_x = g_cross_x[a];
				int8_t key_dir = g_cross_dir[a];

				b = a - 1;
				while (b >= 0 && g_cross_x[b] > key_x) {
					g_cross_x[b + 1] = g_cross_x[b];
					g_cross_dir[b + 1] = g_cross_dir[b];
					b--;
				}
				g_cross_x[b + 1] = key_x;
				g_cross_dir[b + 1] = key_dir;
			}

			for (a = 0; a < crossings; a++) {
				int previous = wind;

				wind += g_cross_dir[a];
				if (previous == 0 && wind != 0)
					span_start = g_cross_x[a];
				else if (previous != 0 && wind == 0)
					add_span(span_start, g_cross_x[a], row,
						 box_x, box_w);
			}
		}
	}

	for (i = 0; i < box_w * box_h; i++) {
		uint32_t alpha = g_acc[i];

		alpha = (alpha * 255u + 128u) / 256u;
		if (alpha > 255u)
			alpha = 255u;
		scratch[i] = (uint8_t)alpha;
	}

	*bw = box_w;
	*bh = box_h;
	*bx = box_x;
	*by = box_y;
	return 1;
}

int zh_font_init(struct zh_font *font, const void *data, size_t size)
{
	const uint8_t *d = data;
	uint32_t tables;
	uint32_t i;
	uint32_t head_off = 0;
	uint32_t head_len = 0;
	uint32_t maxp_off = 0;
	uint32_t maxp_len = 0;
	uint32_t hhea_off = 0;
	uint32_t hhea_len = 0;
	uint32_t cmap_base = 0;
	uint32_t selected_sub = 0;
	int selected_format = 0;
	int best_rank = -1;

	if (!font || !d || size < 12)
		return -1;
	if (!((d[0] == 0 && d[1] == 1 && d[2] == 0 && d[3] == 0) ||
	      (d[0] == 't' && d[1] == 'r' && d[2] == 'u' && d[3] == 'e')))
		return -1;

	tables = be16(d + 4);
	if (!tables || 12u + tables * 16u > size)
		return -1;

	memset(font, 0, sizeof(*font));
	font->data = d;
	font->size = (uint32_t)size;

	for (i = 0; i < tables; i++) {
		const uint8_t *record = d + 12 + i * 16;
		uint32_t offset = be32(record + 8);
		uint32_t length = be32(record + 12);

		if (!in_range(offset, length, (uint32_t)size))
			continue;
		if (tag_eq(record, "head")) {
			head_off = offset;
			head_len = length;
		} else if (tag_eq(record, "maxp")) {
			maxp_off = offset;
			maxp_len = length;
		} else if (tag_eq(record, "hhea")) {
			hhea_off = offset;
			hhea_len = length;
		} else if (tag_eq(record, "hmtx")) {
			font->hmtx_off = offset;
			font->hmtx_len = length;
		} else if (tag_eq(record, "cmap")) {
			font->cmap_off = offset;
			font->cmap_len = length;
		} else if (tag_eq(record, "loca")) {
			font->loca_off = offset;
			font->loca_len = length;
		} else if (tag_eq(record, "glyf")) {
			font->glyf_off = offset;
			font->glyf_len = length;
		}
	}

	if (head_len < 54 || maxp_len < 6 || hhea_len < 36 ||
	    !font->hmtx_off || !font->cmap_off || !font->loca_off ||
	    !font->glyf_off)
		return -1;

	font->units_per_em = be16(d + head_off + 18);
	font->loca_long = be16s(d + head_off + 50) != 0;
	font->num_glyphs = be16(d + maxp_off + 4);
	font->num_hmetrics = be16(d + hhea_off + 34);
	font->ascent = be16s(d + hhea_off + 4);
	font->descent = be16s(d + hhea_off + 6);

	if (!font->units_per_em || !font->num_glyphs || !font->num_hmetrics)
		return -1;

	/* cmap サブテーブル選択 */
	cmap_base = font->cmap_off;
	{
		const uint8_t *cm = d + cmap_base;
		uint32_t count;
		uint32_t encoding;

		if (font->cmap_len < 4)
			return -1;
		count = be16(cm + 2);
		if (count > (font->cmap_len - 4) / 8)
			count = (font->cmap_len - 4) / 8;
		for (encoding = 0; encoding < count; encoding++) {
			const uint8_t *record = cm + 4 + encoding * 8;
			uint16_t platform = be16(record);
			uint16_t id = be16(record + 2);
			uint32_t sub = be32(record + 4);
			int format;
			int rank;
			uint32_t need;

			if (!in_range(sub, 2, font->cmap_len))
				continue;
			format = be16(cm + sub);
			if (format == 4) {
				uint16_t seg_x2;

				if (!in_range(sub, 14, font->cmap_len))
					continue;
				seg_x2 = be16(cm + sub + 6);
				need = 16u + (uint32_t)seg_x2 * 4u;
			} else if (format == 12) {
				if (!in_range(sub, 16, font->cmap_len))
					continue;
				need = 16u;
			} else {
				continue;
			}
			if (need > font->cmap_len - sub)
				continue;
			rank = cmap_rank(platform, id, format);
			if (rank > best_rank) {
				best_rank = rank;
				selected_sub = sub;
				selected_format = format;
			}
		}
	}

	if (best_rank < 0)
		return -1;
	font->cmap_off = cmap_base + selected_sub;
	font->cmap_len -= selected_sub;
	font->cmap_format = selected_format;
	return 0;
}
