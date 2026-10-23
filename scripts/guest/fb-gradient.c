/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fb-gradient.c - フレームバッファ・デスクトップのデモ
 *
 *   - /dev/fb0 に直接描画 (X11 / Wayland 不要)
 *   - evdev (/dev/input/event*) から PS/2 マウスを読む
 *   - window_ops を登録する簡易ウィンドウマネージャ (drag / raise / hover 通知)
 *   - q / Esc / Ctrl+C で終了
 *
 * WSL では gcc -static、macOS では linux-shim + x86_64-elf-gcc で
 * フリースタンディングな静的 ELF としてビルドする。
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "zh_font.h"
#include "zh_png.h"

#define TITLE_H		24
#define BORDER		2
#define CURSOR_W	10
#define CURSOR_H	16
#define ZH_WM_MAX_WINDOWS	8
#define ZH_GRADIENT_READY_PATH	"/tmp/gradient.ready"

struct window;

/*
 * ウィンドウの中身は「ops」で差し替える。
 * 新しい種類を追加するときは draw / tick / pointer を実装し、
 * desktop_create() で wm_add() するだけでよい。
 */
struct window_ops {
	const char *name;
	void (*draw)(struct window *win, int x, int y, int w, int h);
	void (*tick)(struct window *win);
	void (*pointer)(struct window *win, int x, int y, int inside);
};

struct window {
	int x, y, w, h;
	const char *title;
	uint32_t body;
	uint32_t bar;
	const struct window_ops *ops;
	void *data;
};

struct window_manager {
	struct window windows[ZH_WM_MAX_WINDOWS];
	struct window *stack[ZH_WM_MAX_WINDOWS];	/* bottom -> top */
	int count;
	struct window *pointer_window;
};

static struct window_manager wm;
static struct window *html_window;

static uint8_t *fb;
static uint8_t *background;
static uint8_t *scene;
static struct fb_fix_screeninfo finfo;
static struct fb_var_screeninfo vinfo;
static unsigned int pitch;
static unsigned int bytes_per_pixel;
static int screen_w;
static int screen_h;

static int mouse_fd = -1;
static int mouse_x;
static int mouse_y;
static struct window *mouse_grab;
static int grab_dx;
static int grab_dy;
static int scene_dirty;
static int cursor_x = -1000;
static int cursor_y = -1000;

static struct termios saved_tty;
static int tty_raw;

/* ------------------------------------------------------------ フォント / 壁紙 */

#define ZH_FONT_PATH		"/usr/share/zhibox/font.ttf"
#define ZH_WALLPAPER_PATH	"/usr/share/zhibox/wallpaper.png"
#define ZH_HTML_PATH		"/tmp/html.raw"
#define ZH_HTML_FALLBACK	"/usr/share/zhibox/html.raw"
#define ZH_HTML_VERSION_PATH	"/tmp/html.version"
#define ZH_HTML_INPUT_PATH	"/tmp/html.input"
#define ZH_HTML_INPUT_TMP	"/tmp/.html.input.tmp"

static struct zh_font ui_font;
static int font_ready;
static uint8_t *wallpaper_rgba;
static int wallpaper_w;
static int wallpaper_h;
static uint8_t text_mask[128 * 128];

/* ------------------------------------------------------------ HTML (WebKit)
 *
 * ゲスト Linux の init が rootfs.img をマウントして chroot し、Xvfb 上の
 * WebKitGTK で HTML+CSS を raw RGBA に描画する。その結果を /tmp/html.raw
 * から読み、ウィンドウ本文サイズへバイリニア縮小してキャッシュする。
 * ドラッグのたびに 640x400 から縮小しないよう、フレーム更新時だけ変換する。
 * ZH_HTML_FALLBACK はデバッグ用に明示的に埋め込んだ raw だけを読む。
 */
struct html_surface {
	uint8_t *data;
	size_t data_size;
	uint32_t version;
	uint8_t *rgba;
	int src_w;
	int src_h;
	int stride;
	int raw_ok;
	uint32_t *scaled;
	size_t scaled_bytes;
	int scaled_w;
	int scaled_h;
	int off_x;
	int off_y;
};

static struct html_surface html_view;
static int html_input_last_x = -100000;
static int html_input_last_y = -100000;
static int html_poll;

/* ------------------------------------------------------------ DRM 加速
 *
 * /dev/fb0 は virtio-gpu の GEM バッファを mmap したもの。書き込んだ内容は
 * DIRTYFB (drm_mode_fb_dirty_cmd) を投げるまでホスト側へ転送されない。
 * ここでは libdrm に依存せず、必要な UAPI 構造体だけを自前で定義する。
 * bochs など dirty 非対応のドライバでは ioctl が失敗するので、その場合は
 * drm_enabled を落として従来どおり直接描画にフォールバックする。
 */
struct zh_drm_clip_rect {
	uint16_t x1;
	uint16_t y1;
	uint16_t x2;
	uint16_t y2;
};

struct zh_drm_mode_modeinfo {
	uint32_t clock;
	uint16_t hdisplay;
	uint16_t hsync_start;
	uint16_t hsync_end;
	uint16_t htotal;
	uint16_t hskew;
	uint16_t vdisplay;
	uint16_t vsync_start;
	uint16_t vsync_end;
	uint16_t vtotal;
	uint16_t vscan;
	uint32_t vrefresh;
	uint32_t flags;
	uint32_t type;
	char name[32];
};

struct zh_drm_mode_card_res {
	uint64_t fb_id_ptr;
	uint64_t crtc_id_ptr;
	uint64_t connector_id_ptr;
	uint64_t encoder_id_ptr;
	uint32_t count_fbs;
	uint32_t count_crtcs;
	uint32_t count_connectors;
	uint32_t count_encoders;
	uint32_t min_width;
	uint32_t max_width;
	uint32_t min_height;
	uint32_t max_height;
};

struct zh_drm_mode_crtc {
	uint64_t set_connectors_ptr;
	uint32_t count_connectors;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t x;
	uint32_t y;
	uint32_t gamma_size;
	uint32_t mode_valid;
	struct zh_drm_mode_modeinfo mode;
};

struct zh_drm_mode_fb_dirty_cmd {
	uint32_t fb_id;
	uint32_t flags;
	uint32_t color;
	uint32_t num_clips;
	uint64_t clips_ptr;
};

#define ZH_DRM_IOCTL_MODE_GETRESOURCES \
	_IOWR('d', 0xA0, struct zh_drm_mode_card_res)
#define ZH_DRM_IOCTL_MODE_GETCRTC \
	_IOWR('d', 0xA1, struct zh_drm_mode_crtc)
#define ZH_DRM_IOCTL_MODE_DIRTYFB \
	_IOWR('d', 0xB1, struct zh_drm_mode_fb_dirty_cmd)

static int drm_fd = -1;
static uint32_t drm_fb_id;
static int drm_enabled;

static int damage_active;
static int damage_x1;
static int damage_y1;
static int damage_x2;
static int damage_y2;

static void damage_add(int x, int y, int w, int h);

/* 5x7 フォント (LSB が最上段、1 文字 = 5 列) */
static const char font_chars[] =
	" !-./0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t font_data[][5] = {
	{0x00, 0x00, 0x00, 0x00, 0x00},	/* ' ' */
	{0x00, 0x00, 0x5f, 0x00, 0x00},	/* '!' */
	{0x08, 0x08, 0x08, 0x08, 0x08},	/* '-' */
	{0x00, 0x60, 0x60, 0x00, 0x00},	/* '.' */
	{0x20, 0x10, 0x08, 0x04, 0x02},	/* '/' */
	{0x3e, 0x51, 0x49, 0x45, 0x3e},	/* '0' */
	{0x00, 0x42, 0x7f, 0x40, 0x00},	/* '1' */
	{0x42, 0x61, 0x51, 0x49, 0x46},	/* '2' */
	{0x21, 0x41, 0x45, 0x4b, 0x31},	/* '3' */
	{0x18, 0x14, 0x12, 0x7f, 0x10},	/* '4' */
	{0x27, 0x45, 0x45, 0x45, 0x39},	/* '5' */
	{0x3c, 0x4a, 0x49, 0x49, 0x30},	/* '6' */
	{0x01, 0x71, 0x09, 0x05, 0x03},	/* '7' */
	{0x36, 0x49, 0x49, 0x49, 0x36},	/* '8' */
	{0x06, 0x49, 0x49, 0x29, 0x1e},	/* '9' */
	{0x00, 0x36, 0x36, 0x00, 0x00},	/* ':' */
	{0x7e, 0x11, 0x11, 0x11, 0x7e},	/* 'A' */
	{0x7f, 0x49, 0x49, 0x49, 0x36},	/* 'B' */
	{0x3e, 0x41, 0x41, 0x41, 0x22},	/* 'C' */
	{0x7f, 0x41, 0x41, 0x22, 0x1c},	/* 'D' */
	{0x7f, 0x49, 0x49, 0x49, 0x41},	/* 'E' */
	{0x7f, 0x09, 0x09, 0x09, 0x01},	/* 'F' */
	{0x3e, 0x41, 0x49, 0x49, 0x7a},	/* 'G' */
	{0x7f, 0x08, 0x08, 0x08, 0x7f},	/* 'H' */
	{0x00, 0x41, 0x7f, 0x41, 0x00},	/* 'I' */
	{0x20, 0x40, 0x41, 0x3f, 0x01},	/* 'J' */
	{0x7f, 0x08, 0x14, 0x22, 0x41},	/* 'K' */
	{0x7f, 0x40, 0x40, 0x40, 0x40},	/* 'L' */
	{0x7f, 0x02, 0x0c, 0x02, 0x7f},	/* 'M' */
	{0x7f, 0x04, 0x08, 0x10, 0x7f},	/* 'N' */
	{0x3e, 0x41, 0x41, 0x41, 0x3e},	/* 'O' */
	{0x7f, 0x09, 0x09, 0x09, 0x06},	/* 'P' */
	{0x3e, 0x41, 0x51, 0x21, 0x5e},	/* 'Q' */
	{0x7f, 0x09, 0x19, 0x29, 0x46},	/* 'R' */
	{0x46, 0x49, 0x49, 0x49, 0x31},	/* 'S' */
	{0x01, 0x01, 0x7f, 0x01, 0x01},	/* 'T' */
	{0x3f, 0x40, 0x40, 0x40, 0x3f},	/* 'U' */
	{0x1f, 0x20, 0x40, 0x20, 0x1f},	/* 'V' */
	{0x3f, 0x40, 0x38, 0x40, 0x3f},	/* 'W' */
	{0x63, 0x14, 0x08, 0x14, 0x63},	/* 'X' */
	{0x07, 0x08, 0x70, 0x08, 0x07},	/* 'Y' */
	{0x61, 0x51, 0x49, 0x45, 0x43},	/* 'Z' */
};

/* ------------------------------------------------------------ 基本描画 */

static uint32_t channel(uint8_t value, struct fb_bitfield field)
{
	uint64_t maximum;
	uint64_t scaled;

	if (!field.length || field.length > 16 ||
	    field.offset + field.length > 32)
		return 0;

	maximum = (1ULL << field.length) - 1;
	scaled = ((uint64_t)value * maximum + 127) / 255;
	return (uint32_t)(scaled << field.offset);
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return channel(r, vinfo.red) | channel(g, vinfo.green) |
	       channel(b, vinfo.blue);
}

static void put_pixel(uint8_t *buffer, int x, int y, uint32_t color)
{
	uint8_t *pixel;

	if (x < 0 || y < 0 || x >= screen_w || y >= screen_h)
		return;

	pixel = buffer + (size_t)y * pitch + (size_t)x * bytes_per_pixel;
	if (bytes_per_pixel == 4) {
		*(uint32_t *)pixel = color;
	} else if (bytes_per_pixel == 3) {
		pixel[0] = (uint8_t)color;
		pixel[1] = (uint8_t)(color >> 8);
		pixel[2] = (uint8_t)(color >> 16);
	} else {
		*(uint16_t *)pixel = (uint16_t)color;
	}
}

static void buffer_copy(uint8_t *dst, const uint8_t *src, size_t length)
{
	size_t offset;

	for (offset = 0; offset + 4 <= length; offset += 4)
		*(uint32_t *)(dst + offset) = *(const uint32_t *)(src + offset);
	for (; offset < length; offset++)
		dst[offset] = src[offset];
}

static void fill_rect(uint8_t *buffer, int x, int y, int w, int h,
		      uint32_t color)
{
	int row;
	int column;

	for (row = 0; row < h; row++)
		for (column = 0; column < w; column++)
			put_pixel(buffer, x + column, y + row, color);
}

static void rect_outline(uint8_t *buffer, int x, int y, int w, int h,
			 uint32_t color)
{
	int i;

	for (i = 0; i < w; i++) {
		put_pixel(buffer, x + i, y, color);
		put_pixel(buffer, x + i, y + h - 1, color);
	}
	for (i = 0; i < h; i++) {
		put_pixel(buffer, x, y + i, color);
		put_pixel(buffer, x + w - 1, y + i, color);
	}
}

static const uint8_t *glyph_for(char c)
{
	size_t i;

	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');

	for (i = 0; i < sizeof(font_chars) - 1; i++)
		if (font_chars[i] == c)
			return font_data[i];
	return font_data[0];
}

static int text_width(const char *text, int scale)
{
	int length = 0;

	while (text[length])
		length++;
	return length * 6 * scale - scale;
}

static void draw_text(uint8_t *buffer, int x, int y, const char *text,
		      uint32_t color, int scale)
{
	int column;
	int row;
	int xpos = x;

	while (*text) {
		const uint8_t *glyph = glyph_for(*text++);

		for (column = 0; column < 5; column++) {
			for (row = 0; row < 7; row++) {
				if ((glyph[column] >> row) & 1)
					fill_rect(buffer, xpos + column * scale,
						  y + row * scale, scale, scale,
						  color);
			}
		}
		xpos += 6 * scale;
	}
}

/*
 * TTF のカバレッジ (alpha) を使って 1 ピクセルを合成する。
 * color は rgb() で作ったフレームバッファ形式の値。
 */
static void blend_pixel(uint8_t *buffer, int x, int y, uint32_t color,
			unsigned int alpha)
{
	uint8_t *pixel;
	uint32_t dst;
	uint32_t out;
	unsigned int rmax;
	unsigned int gmax;
	unsigned int bmax;
	unsigned int dr;
	unsigned int dg;
	unsigned int db;
	unsigned int sr;
	unsigned int sg;
	unsigned int sb;

	if (!alpha || x < 0 || y < 0 || x >= screen_w || y >= screen_h)
		return;

	pixel = buffer + (size_t)y * pitch + (size_t)x * bytes_per_pixel;
	if (bytes_per_pixel == 4) {
		dst = *(uint32_t *)pixel;
	} else if (bytes_per_pixel == 3) {
		dst = (uint32_t)pixel[0] | ((uint32_t)pixel[1] << 8) |
		      ((uint32_t)pixel[2] << 16);
	} else {
		dst = *(uint16_t *)pixel;
	}

	rmax = vinfo.red.length ? (1u << vinfo.red.length) - 1u : 0;
	gmax = vinfo.green.length ? (1u << vinfo.green.length) - 1u : 0;
	bmax = vinfo.blue.length ? (1u << vinfo.blue.length) - 1u : 0;
	dr = (dst >> vinfo.red.offset) & rmax;
	dg = (dst >> vinfo.green.offset) & gmax;
	db = (dst >> vinfo.blue.offset) & bmax;
	sr = (color >> vinfo.red.offset) & rmax;
	sg = (color >> vinfo.green.offset) & gmax;
	sb = (color >> vinfo.blue.offset) & bmax;

	out = ((dr * (255u - alpha) + sr * alpha + 127u) / 255u)
	      << vinfo.red.offset;
	out |= ((dg * (255u - alpha) + sg * alpha + 127u) / 255u)
	       << vinfo.green.offset;
	out |= ((db * (255u - alpha) + sb * alpha + 127u) / 255u)
	       << vinfo.blue.offset;

	if (bytes_per_pixel == 4) {
		*(uint32_t *)pixel = out;
	} else if (bytes_per_pixel == 3) {
		pixel[0] = (uint8_t)out;
		pixel[1] = (uint8_t)(out >> 8);
		pixel[2] = (uint8_t)(out >> 16);
	} else {
		*(uint16_t *)pixel = (uint16_t)out;
	}
}

static uint32_t next_codepoint(const char **text)
{
	const unsigned char *s = (const unsigned char *)*text;
	uint32_t code;
	int extra;
	int i;

	if (!s[0])
		return 0;
	if (s[0] < 0x80) {
		*text += 1;
		return s[0];
	}
	if ((s[0] & 0xe0) == 0xc0) {
		code = s[0] & 0x1f;
		extra = 1;
	} else if ((s[0] & 0xf0) == 0xe0) {
		code = s[0] & 0x0f;
		extra = 2;
	} else if ((s[0] & 0xf8) == 0xf0) {
		code = s[0] & 0x07;
		extra = 3;
	} else {
		*text += 1;
		return '?';
	}

	for (i = 1; i <= extra; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*text += 1;
			return '?';
		}
		code = (code << 6) | (s[i] & 0x3f);
	}
	*text += extra + 1;
	return code;
}

static int ui_text_width(const char *text, int px)
{
	const char *p = text;
	int width = 0;

	if (!font_ready)
		return text_width(text, px >= 36 ? 4 : (px >= 16 ? 2 : 1));

	while (*p) {
		uint32_t code = next_codepoint(&p);
		uint32_t glyph;

		if (!code)
			break;
		glyph = zh_font_glyph_index(&ui_font, code);
		width += zh_font_advance(&ui_font, glyph, px);
	}
	return width;
}

static void ui_text(uint8_t *buffer, int x, int y_top, const char *text,
		    uint32_t color, int px)
{
	const char *p = text;
	int baseline;
	int pen = x;

	if (!font_ready) {
		draw_text(buffer, x, y_top, text, color,
			  px >= 36 ? 4 : (px >= 16 ? 2 : 1));
		return;
	}

	baseline = y_top + zh_font_ascent(&ui_font, px);
	while (*p) {
		uint32_t code = next_codepoint(&p);
		uint32_t glyph;
		int bw = 0;
		int bh = 0;
		int bx = 0;
		int by = 0;
		int row;
		int column;

		if (!code)
			break;
		glyph = zh_font_glyph_index(&ui_font, code);
		if (zh_font_render(&ui_font, glyph, px, text_mask,
				   sizeof(text_mask), &bw, &bh, &bx, &by) == 1) {
			for (row = 0; row < bh; row++) {
				for (column = 0; column < bw; column++) {
					unsigned int alpha =
						text_mask[(size_t)row * bw + column];

					if (alpha)
						blend_pixel(buffer,
							    pen + bx + column,
							    baseline + by + row,
							    color, alpha);
				}
			}
		}
		pen += zh_font_advance(&ui_font, glyph, px);
	}
}

/* ------------------------------------------------------------ ウィンドウ */

static uint32_t bilinear(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
			 uint32_t fx, uint32_t fy)
{
	uint64_t top = (uint64_t)a * (65536u - fx) + (uint64_t)b * fx;
	uint64_t bottom = (uint64_t)c * (65536u - fx) + (uint64_t)d * fx;

	return (uint32_t)((top * (65536u - fy) + bottom * fy) >> 32);
}

/* 1920x1080 などの壁紙を画面いっぱい (cover) にバイリニア拡大縮小する */
static void draw_wallpaper(void)
{
	uint32_t scale_w;
	uint32_t scale_h;
	uint32_t scale;
	uint32_t scaled_w;
	uint32_t scaled_h;
	uint32_t step_x;
	uint32_t step_y;
	uint32_t start_x;
	uint32_t start_y;
	int off_x;
	int off_y;
	int x;
	int y;

	if (!wallpaper_rgba || wallpaper_w <= 0 || wallpaper_h <= 0)
		return;

	scale_w = (uint32_t)(((uint64_t)screen_w << 16) /
			     (uint32_t)wallpaper_w);
	scale_h = (uint32_t)(((uint64_t)screen_h << 16) /
			     (uint32_t)wallpaper_h);
	scale = (scale_w > scale_h ? scale_w : scale_h) + 1;
	scaled_w = (uint32_t)(((uint64_t)(uint32_t)wallpaper_w * scale) >> 16);
	scaled_h = (uint32_t)(((uint64_t)(uint32_t)wallpaper_h * scale) >> 16);
	off_x = ((int)scaled_w - screen_w) / 2;
	off_y = ((int)scaled_h - screen_h) / 2;
	if (off_x < 0)
		off_x = 0;
	if (off_y < 0)
		off_y = 0;

	step_x = (uint32_t)(((uint64_t)(uint32_t)wallpaper_w << 16) / scaled_w);
	step_y = (uint32_t)(((uint64_t)(uint32_t)wallpaper_h << 16) / scaled_h);
	start_x = (uint32_t)((((uint64_t)(uint32_t)off_x *
			       (uint32_t)wallpaper_w) << 16) / scaled_w);
	start_y = (uint32_t)((((uint64_t)(uint32_t)off_y *
			       (uint32_t)wallpaper_h) << 16) / scaled_h);

	for (y = 0; y < screen_h; y++) {
		uint32_t sy = start_y + (uint32_t)y * step_y;
		uint32_t fy = sy & 0xffffu;
		int y0 = (int)(sy >> 16);
		int y1;

		if (y0 >= wallpaper_h)
			y0 = wallpaper_h - 1;
		y1 = y0 + 1 < wallpaper_h ? y0 + 1 : y0;

		for (x = 0; x < screen_w; x++) {
			uint32_t sx = start_x + (uint32_t)x * step_x;
			uint32_t fx = sx & 0xffffu;
			int x0 = (int)(sx >> 16);
			int x1;
			const uint8_t *p00;
			const uint8_t *p10;
			const uint8_t *p01;
			const uint8_t *p11;
			uint32_t r;
			uint32_t g;
			uint32_t b;

			if (x0 >= wallpaper_w)
				x0 = wallpaper_w - 1;
			x1 = x0 + 1 < wallpaper_w ? x0 + 1 : x0;

			p00 = wallpaper_rgba +
			      ((size_t)y0 * (size_t)wallpaper_w + (size_t)x0) * 4u;
			p10 = wallpaper_rgba +
			      ((size_t)y0 * (size_t)wallpaper_w + (size_t)x1) * 4u;
			p01 = wallpaper_rgba +
			      ((size_t)y1 * (size_t)wallpaper_w + (size_t)x0) * 4u;
			p11 = wallpaper_rgba +
			      ((size_t)y1 * (size_t)wallpaper_w + (size_t)x1) * 4u;

			r = bilinear(p00[0], p10[0], p01[0], p11[0], fx, fy);
			g = bilinear(p00[1], p10[1], p01[1], p11[1], fx, fy);
			b = bilinear(p00[2], p10[2], p01[2], p11[2], fx, fy);

			/* ウィンドウと文字を読みやすくするため少し暗くする */
			r = (r * 205u + 127u) / 255u;
			g = (g * 205u + 127u) / 255u;
			b = (b * 205u + 127u) / 255u;

			put_pixel(background, x, y,
				  rgb((uint8_t)r, (uint8_t)g, (uint8_t)b));
		}
	}
}

static void build_background(void)
{
	uint32_t title_color;
	uint32_t footer_color;

	if (wallpaper_rgba) {
		draw_wallpaper();
		title_color = rgb(196, 210, 234);
		footer_color = rgb(200, 214, 234);
	} else {
		int x;
		int y;

		for (y = 0; y < screen_h; y++) {
			uint8_t r = (uint8_t)(16 + 22 * y / screen_h);
			uint8_t g = (uint8_t)(22 + 34 * y / screen_h);
			uint8_t b = (uint8_t)(50 + 92 * y / screen_h);
			uint32_t color = rgb(r, g, b);

			for (x = 0; x < screen_w; x++)
				put_pixel(background, x, y, color);
		}
		title_color = rgb(96, 128, 184);
		footer_color = rgb(72, 96, 140);
	}

	ui_text(background, (screen_w - ui_text_width("Zhibox OS", 40)) / 2,
		screen_h / 12, "Zhibox OS", title_color, 40);

	ui_text(background, 20, screen_h - 34,
		"DRAG TITLE BAR   Q TO QUIT", footer_color, 16);
}

static void draw_window(struct window *window)
{
	int x = window->x;
	int y = window->y;
	int w = window->w;
	int h = window->h;

	/* 影 */
	fill_rect(scene, x + 8, y + 8, w, h, rgb(4, 6, 14));

	/* 本体とタイトルバー */
	fill_rect(scene, x, y, w, h, window->body);
	fill_rect(scene, x, y, w, TITLE_H, window->bar);
	rect_outline(scene, x, y, w, h, rgb(12, 16, 28));
	rect_outline(scene, x, y, w, TITLE_H, window->bar);

	ui_text(scene, x + 10, y + 2, window->title, rgb(255, 255, 255), 18);

	if (window->ops && window->ops->draw) {
		int cx = x + 2 * BORDER + 6;
		int cy = y + TITLE_H + 2 * BORDER + 6;
		int cw = w - 4 * BORDER - 12;
		int ch = h - TITLE_H - 4 * BORDER - 12;

		if (cw > 0 && ch > 0)
			window->ops->draw(window, cx, cy, cw, ch);
	}
}

/* ------------------------------------------------------------ window manager */

static struct window *wm_add(const struct window_ops *ops, const char *title,
			     uint32_t body, uint32_t bar, void *data)
{
	struct window *win;

	if (wm.count >= ZH_WM_MAX_WINDOWS) {
		printf("gradient: window limit reached (%d)\n",
		       ZH_WM_MAX_WINDOWS);
		return NULL;
	}

	win = &wm.windows[wm.count];
	memset(win, 0, sizeof(*win));
	win->title = title;
	win->body = body;
	win->bar = bar;
	win->ops = ops;
	win->data = data;
	wm.stack[wm.count] = win;
	wm.count++;
	return win;
}

static void wm_raise(struct window *win)
{
	int position = -1;
	int i;

	for (i = 0; i < wm.count; i++)
		if (wm.stack[i] == win)
			position = i;
	if (position < 0 || position == wm.count - 1)
		return;

	for (i = position; i < wm.count - 1; i++)
		wm.stack[i] = wm.stack[i + 1];
	wm.stack[wm.count - 1] = win;
	scene_dirty = 1;
}

static struct window *wm_at(int x, int y)
{
	int i;

	for (i = wm.count - 1; i >= 0; i--) {
		struct window *win = wm.stack[i];

		if (x >= win->x && x < win->x + win->w &&
		    y >= win->y && y < win->y + win->h)
			return win;
	}
	return NULL;
}

static void wm_draw(void)
{
	int i;

	for (i = 0; i < wm.count; i++)
		draw_window(wm.stack[i]);
}

static void wm_tick(void)
{
	int i;

	for (i = 0; i < wm.count; i++) {
		struct window *win = wm.stack[i];

		if (win->ops && win->ops->tick)
			win->ops->tick(win);
	}
}

static void wm_pointer_update(void)
{
	struct window *hover = wm_at(mouse_x, mouse_y);

	if (wm.pointer_window != hover) {
		if (wm.pointer_window && wm.pointer_window->ops &&
		    wm.pointer_window->ops->pointer)
			wm.pointer_window->ops->pointer(wm.pointer_window,
						       -1, -1, 0);
		wm.pointer_window = hover;
	}
	if (hover && hover->ops && hover->ops->pointer)
		hover->ops->pointer(hover, mouse_x, mouse_y, 1);
}

static void build_scene(void)
{
	buffer_copy(scene, background, (size_t)pitch * screen_h);
	wm_draw();
}

static void blit_full(void)
{
	buffer_copy(fb, scene, (size_t)pitch * screen_h);
}

static void blit_region(int x, int y, int w, int h)
{
	int row;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > screen_w)
		w = screen_w - x;
	if (y + h > screen_h)
		h = screen_h - y;
	if (w <= 0 || h <= 0)
		return;

	for (row = 0; row < h; row++)
		buffer_copy(fb + (size_t)(y + row) * pitch +
				    (size_t)x * bytes_per_pixel,
			    scene + (size_t)(y + row) * pitch +
				    (size_t)x * bytes_per_pixel,
			    (size_t)w * bytes_per_pixel);
}

static void draw_cursor(void)
{
	uint32_t black = rgb(0, 0, 0);
	uint32_t white = rgb(255, 255, 255);
	int x;
	int y;

	for (y = 0; y < 10; y++)
		for (x = 0; x <= y; x++)
			put_pixel(fb, mouse_x + x, mouse_y + y,
				  (x == 0 || x == y || y == 9) ? black : white);

	for (y = 10; y < CURSOR_H; y++)
		for (x = 3; x <= 5; x++)
			put_pixel(fb, mouse_x + x, mouse_y + y,
				  (x == 3 || x == 5 || y == CURSOR_H - 1) ?
					  black : white);
}

static void cursor_hide(void)
{
	if (cursor_x < -100 || cursor_y < -100)
		return;
	blit_region(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
	damage_add(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
	cursor_x = -1000;
	cursor_y = -1000;
}

static void cursor_show(void)
{
	cursor_x = mouse_x;
	cursor_y = mouse_y;
	blit_region(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
	damage_add(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
	draw_cursor();
}

static void clamp_mouse(void)
{
	if (mouse_x < 0)
		mouse_x = 0;
	if (mouse_y < 0)
		mouse_y = 0;
	if (mouse_x > screen_w - 1)
		mouse_x = screen_w - 1;
	if (mouse_y > screen_h - 1)
		mouse_y = screen_h - 1;
}

static void mouse_press(void)
{
	struct window *win = wm_at(mouse_x, mouse_y);

	if (!win)
		return;

	wm_raise(win);
	if (mouse_y < win->y + TITLE_H) {
		mouse_grab = win;
		grab_dx = mouse_x - win->x;
		grab_dy = mouse_y - win->y;
	}
}

static void mouse_release(void)
{
	mouse_grab = NULL;
}

static int handle_mouse(void)
{
	struct input_event event;
	int changed = 0;

	while (mouse_fd >= 0 &&
	       read(mouse_fd, &event, sizeof(event)) == (ssize_t)sizeof(event)) {
		if (event.type == EV_REL) {
			if (event.code == REL_X) {
				mouse_x += event.value;
				changed = 1;
			} else if (event.code == REL_Y) {
				mouse_y += event.value;
				changed = 1;
			}
			clamp_mouse();
		} else if (event.type == EV_KEY && event.code == BTN_LEFT) {
			if (event.value)
				mouse_press();
			else
				mouse_release();
			changed = 1;
		}
	}

	if (mouse_grab && changed) {
		mouse_grab->x = mouse_x - grab_dx;
		mouse_grab->y = mouse_y - grab_dy;
		scene_dirty = 1;
	}

	return changed;
}

/* ------------------------------------------------------------ デバイス */

static void event_path(char *buffer, int number)
{
	const char *prefix = "/dev/input/event";
	int i = 0;

	while (prefix[i]) {
		buffer[i] = prefix[i];
		i++;
	}
	buffer[i++] = (char)('0' + number);
	buffer[i] = '\0';
}

static int open_mouse(void)
{
	char path[32];
	int number;

	for (number = 0; number < 8; number++) {
		unsigned long bits = 0;
		int fd;

		event_path(path, number);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(bits)), &bits) < 0 ||
		    !(bits & (1UL << REL_X))) {
			close(fd);
			continue;
		}
		return fd;
	}
	return -1;
}

static uint8_t *open_framebuffer(void)
{
	int fd;
	void *mapping;
	size_t length;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/fb0");
		return NULL;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		perror("FBIOGET_FSCREENINFO");
		close(fd);
		return NULL;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		perror("FBIOGET_VSCREENINFO");
		close(fd);
		return NULL;
	}

	screen_w = (int)vinfo.xres;
	screen_h = (int)vinfo.yres;
	bytes_per_pixel = vinfo.bits_per_pixel / 8;
	pitch = finfo.line_length;

	if (screen_w <= 0 || screen_h <= 0 || bytes_per_pixel < 2 ||
	    bytes_per_pixel > 4) {
		fprintf(stderr, "unsupported framebuffer layout\n");
		close(fd);
		return NULL;
	}

	length = (size_t)pitch * screen_h;
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (mapping == MAP_FAILED) {
		perror("mmap /dev/fb0");
		return NULL;
	}
	return mapping;
}

static uint8_t *alloc_buffer(size_t length)
{
	void *mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (mapping == MAP_FAILED)
		return NULL;
	return mapping;
}

static void release_buffer(void *mapping, size_t length)
{
	if (mapping)
		munmap(mapping, length);
}

static void *read_file(const char *path, size_t *size_out)
{
	int fd;
	off_t size;
	uint8_t *data;
	size_t total = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return NULL;

	size = lseek(fd, 0, SEEK_END);
	if (size <= 0) {
		close(fd);
		return NULL;
	}
	if (lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		return NULL;
	}

	data = alloc_buffer((size_t)size);
	if (!data) {
		close(fd);
		return NULL;
	}

	while (total < (size_t)size) {
		ssize_t chunk = read(fd, data + total, (size_t)size - total);

		if (chunk <= 0)
			break;
		total += (size_t)chunk;
	}
	close(fd);

	if (total != (size_t)size) {
		release_buffer(data, (size_t)size);
		return NULL;
	}
	*size_out = total;
	return data;
}

static void *asset_alloc(size_t length)
{
	return alloc_buffer(length);
}

static void load_assets(void)
{
	size_t size = 0;
	uint8_t *data;

	data = read_file(ZH_FONT_PATH, &size);
	if (data && zh_font_init(&ui_font, data, size) == 0)
		font_ready = 1;
	printf("gradient: font %s\n",
	       font_ready ? "loaded (TrueType)" : "unavailable (builtin)");

	data = read_file(ZH_WALLPAPER_PATH, &size);
	if (data && zh_png_decode(data, size, asset_alloc, &wallpaper_rgba,
				  &wallpaper_w, &wallpaper_h) == 0)
		printf("gradient: wallpaper %dx%d (PNG)\n",
		       wallpaper_w, wallpaper_h);
	else
		printf("gradient: wallpaper unavailable (gradient)\n");
}

/* ------------------------------------------------------------ HTML raw */

static uint32_t raw_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int html_raw_load(void)
{
	static const uint8_t magic[8] = {'Z', 'H', 'R', 'A', 'W', '1', 0, 0};
	size_t size = 0;
	uint8_t *data = read_file(ZH_HTML_PATH, &size);
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride;

	if (!data)
		data = read_file(ZH_HTML_FALLBACK, &size);
	if (!data)
		return -1;
	if (size < 24 || memcmp(data, magic, sizeof(magic)) != 0)
		goto invalid;

	width = raw_le32(data + 8);
	height = raw_le32(data + 12);
	format = raw_le32(data + 16);
	stride = raw_le32(data + 20);

	if (format != 0 || width == 0 || height == 0 ||
	    width > 8192 || height > 8192 || stride < width * 4)
		goto invalid;
	if (size < 24 + (size_t)stride * height)
		goto invalid;

	if (html_view.data)
		release_buffer(html_view.data, html_view.data_size);
	html_view.data = data;
	html_view.data_size = size;
	html_view.rgba = data + 24;
	html_view.src_w = (int)width;
	html_view.src_h = (int)height;
	html_view.stride = (int)stride;
	html_view.raw_ok = 1;
	return 0;

invalid:
	release_buffer(data, size);
	return -1;
}

static void prepare_html_cache(void)
{
	const struct window *window = html_window;
	int max_w;
	int max_h;
	int dw;
	int dh;
	int x;
	int y;
	int old_w = 0;
	int old_h = 0;
	uint32_t *scaled;

	if (!window || !html_view.raw_ok)
		return;

	max_w = window->w - 4 * BORDER - 12;
	max_h = window->h - TITLE_H - 4 * BORDER - 12;
	if (max_w <= 0 || max_h <= 0)
		return;

	/* アスペクト比を保って本文にフィットさせる */
	if ((long)max_w * html_view.src_h <= (long)max_h * html_view.src_w) {
		dw = max_w;
		dh = (html_view.src_h * dw + html_view.src_w / 2) /
		     html_view.src_w;
	} else {
		dh = max_h;
		dw = (html_view.src_w * dh + html_view.src_h / 2) /
		     html_view.src_h;
	}
	if (dw <= 0 || dh <= 0)
		return;

	scaled = (uint32_t *)alloc_buffer((size_t)dw * dh *
					  sizeof(*scaled));
	if (!scaled) {
		printf("gradient: html cache allocation failed\n");
		return;
	}
	old_w = html_view.scaled_w;
	old_h = html_view.scaled_h;

	/* 8.8 固定小数点のバイリニア補間 */
	for (y = 0; y < dh; y++) {
		int fy = ((2 * y + 1) * html_view.src_h * 256) / (2 * dh) - 128;
		int y0;
		int y1;
		int wy;
		const uint8_t *row0;
		const uint8_t *row1;

		if (fy < 0)
			fy = 0;
		y0 = fy >> 8;
		wy = fy & 255;
		y1 = y0 + 1;
		if (y1 >= html_view.src_h) {
			y1 = html_view.src_h - 1;
			y0 = y1;
			wy = 0;
		}
		row0 = html_view.rgba + (size_t)y0 * html_view.stride;
		row1 = html_view.rgba + (size_t)y1 * html_view.stride;

		for (x = 0; x < dw; x++) {
			int fx = ((2 * x + 1) * html_view.src_w * 256) /
				 (2 * dw) - 128;
			int x0;
			int x1;
			int wx;
			const uint8_t *p00;
			const uint8_t *p10;
			const uint8_t *p01;
			const uint8_t *p11;
			int w00;
			int w10;
			int w01;
			int w11;
			int r;
			int g;
			int b;

			if (fx < 0)
				fx = 0;
			x0 = fx >> 8;
			wx = fx & 255;
			x1 = x0 + 1;
			if (x1 >= html_view.src_w) {
				x1 = html_view.src_w - 1;
				x0 = x1;
				wx = 0;
			}

			p00 = row0 + (size_t)x0 * 4;
			p10 = row0 + (size_t)x1 * 4;
			p01 = row1 + (size_t)x0 * 4;
			p11 = row1 + (size_t)x1 * 4;
			w00 = (256 - wx) * (256 - wy);
			w10 = wx * (256 - wy);
			w01 = (256 - wx) * wy;
			w11 = wx * wy;

			r = (p00[0] * w00 + p10[0] * w10 +
			     p01[0] * w01 + p11[0] * w11) >> 16;
			g = (p00[1] * w00 + p10[1] * w10 +
			     p01[1] * w01 + p11[1] * w11) >> 16;
			b = (p00[2] * w00 + p10[2] * w10 +
			     p01[2] * w01 + p11[2] * w11) >> 16;
			scaled[(size_t)y * dw + x] =
				rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
		}
	}

	if (html_view.scaled)
		release_buffer(html_view.scaled, html_view.scaled_bytes);
	html_view.scaled = scaled;
	html_view.scaled_bytes = (size_t)dw * dh * sizeof(*scaled);
	html_view.scaled_w = dw;
	html_view.scaled_h = dh;
	html_view.off_x = (max_w - dw) / 2;
	html_view.off_y = (max_h - dh) / 2;
	if (old_w != dw || old_h != dh)
		printf("gradient: html %dx%d -> %dx%d (raw RGBA)\n",
		       html_view.src_w, html_view.src_h, dw, dh);
}

/* レンダラが書く 4 バイトのフレーム番号。raw の再読み込み判定だけに使う。 */
static uint32_t html_version_read(void)
{
	uint32_t version = html_view.version;
	size_t size = 0;
	uint8_t *data = read_file(ZH_HTML_VERSION_PATH, &size);

	if (data) {
		if (size >= 4)
			version = raw_le32(data);
		release_buffer(data, size);
	}
	return version;
}

static void html_refresh(void)
{
	uint32_t version = html_version_read();

	if (version == html_view.version)
		return;

	if (html_raw_load() == 0) {
		html_view.version = version;
		prepare_html_cache();
		scene_dirty = 1;
		printf("gradient: html frame %u\n", version);
	} else {
		/* 壊れたフレームで毎回リトライしないよう、番号だけ進める。 */
		html_view.version = version;
	}
}

/*
 * fb-gradient 側のマウス位置を WebKit の 640x400 座標へ変換し、
 * /tmp/html.input に書く。ゲスト側の render-html.py が XTEST で
 * ポインタを動かし、CSS :hover を発火させる。
 */
static char *append_int(char *out, int value)
{
	char digits[12];
	unsigned int number;
	int count = 0;

	if (value < 0) {
		*out++ = '-';
		number = (unsigned int)(-value);
	} else {
		number = (unsigned int)value;
	}
	do {
		digits[count++] = (char)('0' + number % 10);
		number /= 10;
	} while (number);
	while (count)
		*out++ = digits[--count];
	return out;
}

static void html_input_write(int x, int y)
{
	char buffer[32];
	char *end = buffer;
	int fd;

	if (x == html_input_last_x && y == html_input_last_y)
		return;
	html_input_last_x = x;
	html_input_last_y = y;

	fd = open(ZH_HTML_INPUT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	end = append_int(end, x);
	*end++ = ' ';
	end = append_int(end, y);
	*end++ = '\n';
	(void)write(fd, buffer, (size_t)(end - buffer));
	close(fd);
	(void)rename(ZH_HTML_INPUT_TMP, ZH_HTML_INPUT_PATH);
}

/* --------------------------------------------------------- window contents */

static void gradient_content_draw(struct window *win, int x, int y, int w, int h)
{
	int swatch_w = w - 4;
	int swatch_h = h - 4;
	int row;

	(void)win;
	if (swatch_w <= 0 || swatch_h <= 0)
		return;

	x += 2;
	y += 2;
	for (row = 0; row < swatch_h; row++) {
		uint8_t green = swatch_h > 1 ?
			(uint8_t)(255 * row / (swatch_h - 1)) : 255;
		uint32_t color = rgb(255, green, 0);
		int column;

		for (column = 0; column < swatch_w; column++)
			put_pixel(scene, x + column, y + row, color);
	}
	rect_outline(scene, x, y, swatch_w, swatch_h, rgb(12, 16, 28));
}

static void html_content_draw(struct window *win, int x, int y, int w, int h)
{
	int bx = x + html_view.off_x;
	int by = y + html_view.off_y;

	(void)win;
	if (html_view.scaled) {
		int row;
		int column;

		for (row = 0; row < html_view.scaled_h; row++)
			for (column = 0; column < html_view.scaled_w; column++)
				put_pixel(scene, bx + column, by + row,
					  html_view.scaled[
						  (size_t)row *
							  html_view.scaled_w +
						  column]);
		rect_outline(scene, bx - 1, by - 1,
			     html_view.scaled_w + 2, html_view.scaled_h + 2,
			     rgb(96, 128, 152));
	} else {
		const char *message = "HTML RENDER WAITING";
		int tw = ui_text_width(message, 16);

		ui_text(scene, x + (w - tw) / 2, y + (h - 16) / 2,
			message, rgb(200, 214, 234), 16);
	}
}

static void html_content_tick(struct window *win)
{
	(void)win;
	if (++html_poll < 10)
		return;
	html_poll = 0;
	html_refresh();
}

static void html_content_pointer(struct window *win, int x, int y, int inside)
{
	int left = win->x + 2 * BORDER + 6 + html_view.off_x;
	int top = win->y + TITLE_H + 2 * BORDER + 6 + html_view.off_y;
	int sx = -1;
	int sy = -1;

	if (inside && html_view.scaled && html_view.scaled_w > 0 &&
	    html_view.scaled_h > 0 &&
	    x >= left && x < left + html_view.scaled_w &&
	    y >= top && y < top + html_view.scaled_h) {
		sx = (x - left) * html_view.src_w / html_view.scaled_w;
		sy = (y - top) * html_view.src_h / html_view.scaled_h;
		if (sx < 0)
			sx = 0;
		if (sy < 0)
			sy = 0;
		if (sx >= html_view.src_w)
			sx = html_view.src_w - 1;
		if (sy >= html_view.src_h)
			sy = html_view.src_h - 1;
	}

	html_input_write(sx, sy);
}

struct text_content {
	const char *lines[4];
};

static void text_content_draw(struct window *win, int x, int y, int w, int h)
{
	const struct text_content *content = win->data;
	unsigned int i;

	(void)w;
	(void)h;
	if (!content)
		return;
	for (i = 0; i < 4; i++) {
		if (!content->lines[i])
			continue;
		ui_text(scene, x + 4, y + 4 + (int)i * 22,
			content->lines[i], rgb(226, 232, 244), 16);
	}
}

static const struct window_ops gradient_ops = {
	.name = "gradient",
	.draw = gradient_content_draw,
};

static const struct window_ops text_ops = {
	.name = "text",
	.draw = text_content_draw,
};

static const struct window_ops html_ops = {
	.name = "html",
	.draw = html_content_draw,
	.tick = html_content_tick,
	.pointer = html_content_pointer,
};


static void damage_add(int x, int y, int w, int h)
{
	int x2 = x + w;
	int y2 = y + h;

	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x2 > screen_w)
		x2 = screen_w;
	if (y2 > screen_h)
		y2 = screen_h;
	if (x >= x2 || y >= y2)
		return;

	if (!damage_active) {
		damage_x1 = x;
		damage_y1 = y;
		damage_x2 = x2;
		damage_y2 = y2;
		damage_active = 1;
		return;
	}

	if (x < damage_x1)
		damage_x1 = x;
	if (y < damage_y1)
		damage_y1 = y;
	if (x2 > damage_x2)
		damage_x2 = x2;
	if (y2 > damage_y2)
		damage_y2 = y2;
}

static void drm_dirty(const struct zh_drm_clip_rect *clip)
{
	struct zh_drm_mode_fb_dirty_cmd command;

	if (!drm_enabled)
		return;

	memset(&command, 0, sizeof(command));
	command.fb_id = drm_fb_id;
	command.num_clips = 1;
	command.clips_ptr = (uint64_t)(uintptr_t)clip;

	if (ioctl(drm_fd, ZH_DRM_IOCTL_MODE_DIRTYFB, &command) < 0)
		drm_enabled = 0;
}

static void damage_present(void)
{
	struct zh_drm_clip_rect clip;

	if (!damage_active)
		return;

	clip.x1 = (uint16_t)damage_x1;
	clip.y1 = (uint16_t)damage_y1;
	clip.x2 = (uint16_t)damage_x2;
	clip.y2 = (uint16_t)damage_y2;
	damage_active = 0;
	drm_dirty(&clip);
}

static void drm_init(void)
{
	struct zh_drm_mode_card_res resources;
	struct zh_drm_mode_crtc crtc;
	uint32_t crtcs[8];

	drm_fd = open("/dev/dri/card0", O_RDWR | O_NONBLOCK);
	if (drm_fd < 0) {
		printf("gradient: drm open errno=%d\n", errno);
		return;
	}

	memset(&resources, 0, sizeof(resources));
	if (ioctl(drm_fd, ZH_DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		printf("gradient: drm GETRESOURCES(1) errno=%d\n", errno);
		goto fail;
	}
	if (resources.count_crtcs == 0) {
		printf("gradient: drm GETRESOURCES: no crtc\n");
		goto fail;
	}

	memset(crtcs, 0, sizeof(crtcs));
	/* 1 回目で埋まった他のカウントを 0 に戻さないと、
	 * NULL ポインタへのコピーになって EFAULT になる */
	resources.fb_id_ptr = 0;
	resources.connector_id_ptr = 0;
	resources.encoder_id_ptr = 0;
	resources.count_fbs = 0;
	resources.count_connectors = 0;
	resources.count_encoders = 0;
	resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	resources.count_crtcs = 8;
	if (ioctl(drm_fd, ZH_DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0) {
		printf("gradient: drm GETRESOURCES(2) errno=%d\n", errno);
		goto fail;
	}
	if (resources.count_crtcs == 0) {
		printf("gradient: drm GETRESOURCES(2): no crtc\n");
		goto fail;
	}

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = crtcs[0];
	if (ioctl(drm_fd, ZH_DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) {
		printf("gradient: drm GETCRTC errno=%d\n", errno);
		goto fail;
	}
	if (!crtc.fb_id || !crtc.mode_valid)
		goto fail;

	drm_fb_id = crtc.fb_id;
	drm_enabled = 1;
	return;

fail:
	close(drm_fd);
	drm_fd = -1;
}

static const struct text_content about_content = {
	.lines = { "Zhibox OS", "OLK-5.10", "FB + EVDEV", "Q TO QUIT" },
};

static const struct text_content input_content = {
	.lines = { "MOUSE  : EVDEV", "DRAG   : TITLE",
		   "RAISE  : CLICK", "EXIT   : Q" },
};

static void place_window(struct window *win, int x, int y, int w, int h)
{
	if (!win)
		return;
	win->x = x;
	win->y = y;
	win->w = w;
	win->h = h;
}

/*
 * デスクトップの組み立て。window_ops を実装したら、ここで 1 行追加するだけで
 * 新しいウィンドウを登録できる。描画・重ね順・ドラッグ・hover 入力は wm_* が面倒を見る。
 */
static void desktop_create(void)
{
	struct window *win;
	int w;
	int h;

	w = screen_w / 3;
	h = screen_h / 3;
	if (w < 260)
		w = screen_w - 40;
	if (h < 140)
		h = screen_h / 2;
	if (w > 420)
		w = 420;
	if (h > 250)
		h = 250;

	win = wm_add(&gradient_ops, "GRADIENT", rgb(30, 38, 54),
		     rgb(52, 108, 176), NULL);
	place_window(win, screen_w / 12, screen_h / 8, w, h);

	win = wm_add(&text_ops, "ABOUT", rgb(34, 42, 58),
		     rgb(92, 74, 148), (void *)&about_content);
	place_window(win, screen_w / 2 + screen_w / 20, screen_h / 5, w, h);

	win = wm_add(&text_ops, "INPUT", rgb(30, 44, 40),
		     rgb(52, 132, 104), (void *)&input_content);
	place_window(win, screen_w / 5, screen_h / 2 + screen_h / 12, w, h);

	/* HTML (WebKit) ウィンドウ: 右下に 520x360 程度で配置 */
	html_window = wm_add(&html_ops, "WEBKIT", rgb(8, 14, 26),
			     rgb(38, 92, 120), NULL);
	if (!html_window)
		return;

	html_window->w = screen_w / 2 - 120;
	if (html_window->w < 300)
		html_window->w = screen_w - 40;
	if (html_window->w > 560)
		html_window->w = 560;
	html_window->h = html_window->w * 9 / 13;
	if (html_window->h > screen_h - 80)
		html_window->h = screen_h - 80;
	html_window->x = screen_w - html_window->w - 40;
	if (html_window->x < 0)
		html_window->x = 0;
	html_window->y = screen_h - html_window->h - 20;
	if (html_window->y < 0)
		html_window->y = 0;
}

/*
 * 最初のフレームを出し終えたことを init に知らせる。init はこのファイルを
 * 見てからゲスト WebKitGTK レンダラを起動する（gradient が先、WebKit が後）。
 */
static void signal_gradient_ready(void)
{
	int fd = open(ZH_GRADIENT_READY_PATH,
		      O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0) {
		printf("gradient: ready file open failed errno=%d\n", errno);
		return;
	}
	(void)write(fd, "1\n", 2);
	close(fd);
	printf("gradient: ready -> %s\n", ZH_GRADIENT_READY_PATH);
}

int main(void)
{
	struct timespec tick;
	int tty;
	int mouse_retries = 0;

	fb = open_framebuffer();
	if (!fb)
		return 1;

	drm_init();

	background = alloc_buffer((size_t)pitch * screen_h);
	scene = alloc_buffer((size_t)pitch * screen_h);
	if (!background || !scene) {
		perror("mmap buffer");
		return 1;
	}

	load_assets();
	if (html_raw_load() == 0)
		printf("gradient: html %dx%d (raw RGBA, JS off)\n",
		       html_view.src_w, html_view.src_h);
	else
		printf("gradient: html render unavailable (placeholder)\n");
	html_view.version = html_version_read();

	tty = open("/dev/tty0", O_RDWR);
	if (tty < 0)
		tty = STDIN_FILENO;

	if (tcgetattr(STDIN_FILENO, &saved_tty) == 0) {
		struct termios raw = saved_tty;

		raw.c_lflag &= ~(unsigned int)(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
			tty_raw = 1;
	}

	ioctl(tty, KDSETMODE, KD_GRAPHICS);

	mouse_fd = open_mouse();
	mouse_x = screen_w / 2;
	mouse_y = screen_h / 2;

	printf("gradient: %dx%d %ubpp\n", screen_w, screen_h,
	       vinfo.bits_per_pixel);
	printf("gradient: drm %s\n",
	       drm_enabled ? "acceleration on (virtio-gpu)" :
			     "unavailable (fbdev direct)");
	printf("gradient: mouse %s\n",
	       mouse_fd >= 0 ? "ready" : "not found (retrying)");
	printf("gradient: drag title bar / q to quit\n");

	desktop_create();
	prepare_html_cache();
	wm_pointer_update();
	build_background();
	build_scene();
	blit_full();
	damage_add(0, 0, screen_w, screen_h);
	cursor_show();
	damage_present();
	scene_dirty = 0;
	signal_gradient_ready();

	tick.tv_sec = 0;
	tick.tv_nsec = 10 * 1000 * 1000;

	while (1) {
		char key;
		int changed;

		if (mouse_fd < 0 && mouse_retries < 80 &&
		    ++mouse_retries % 25 == 0) {
			mouse_fd = open_mouse();
			if (mouse_fd >= 0)
				printf("gradient: mouse ready\n");
		}

		if (read(STDIN_FILENO, &key, 1) == 1 &&
		    (key == 'q' || key == 'Q' || key == 27 || key == 3))
			break;

		changed = handle_mouse();
		wm_pointer_update();
		wm_tick();
		if (changed || scene_dirty) {
			cursor_hide();
			if (scene_dirty) {
				build_scene();
				blit_full();
				damage_add(0, 0, screen_w, screen_h);
				scene_dirty = 0;
			}
			cursor_show();
			damage_present();
		}
		nanosleep(&tick, NULL);
	}

	cursor_hide();
	ioctl(tty, KDSETMODE, KD_TEXT);
	if (tty_raw)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_tty);
	if (mouse_fd >= 0)
		close(mouse_fd);
	if (drm_fd >= 0)
		close(drm_fd);
	printf("gradient: exit (cursor %d,%d)\n", mouse_x, mouse_y);
	return 0;
}
