/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fb-gradient.c - フレームバッファ・デスクトップのデモ
 *
 *   - /dev/fb0 に直接描画 (X11 / Wayland 不要)
 *   - evdev (/dev/input/event*) から PS/2 マウスを読む
 *   - 簡易ウィンドウ: タイトルバーをドラッグで移動、クリックで前面化
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

#define TITLE_H		24
#define BORDER		2
#define CURSOR_W	10
#define CURSOR_H	16
#define WINDOW_COUNT	3

enum {
	WIN_GRADIENT,
	WIN_TEXT,
};

struct window {
	int x, y, w, h;
	const char *title;
	uint32_t body;
	uint32_t bar;
	int kind;
};

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
static int mouse_grab = -1;
static int grab_dx;
static int grab_dy;
static int scene_dirty;
static int cursor_x = -1000;
static int cursor_y = -1000;

static struct termios saved_tty;
static int tty_raw;

static struct window windows[WINDOW_COUNT];
static int stacking[WINDOW_COUNT] = {0, 1, 2};

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

/* ------------------------------------------------------------ ウィンドウ */

static void build_background(void)
{
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

	draw_text(background, (screen_w - text_width("ZHIBOX OS", 4)) / 2,
		  screen_h / 12, "ZHIBOX OS", rgb(96, 128, 184), 4);

	draw_text(background,
		  (screen_w - text_width("DRAG TITLE BAR   Q TO QUIT", 2)) / 2,
		  screen_h - 40, "DRAG TITLE BAR   Q TO QUIT",
		  rgb(72, 96, 140), 2);
}

static void draw_window(const struct window *window)
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

	draw_text(scene, x + 10, y + 5, window->title, rgb(255, 255, 255), 2);

	if (window->kind == WIN_GRADIENT) {
		int swatch_w = w - 4 * BORDER - 16;
		int swatch_h = h - TITLE_H - 4 * BORDER - 16;
		int row;

		for (row = 0; row < swatch_h; row++) {
			uint8_t green = (uint8_t)(255 * row / (swatch_h - 1));
			uint32_t color = rgb(255, green, 0);
			int column;

			for (column = 0; column < swatch_w; column++)
				put_pixel(scene, x + 2 * BORDER + 8 + column,
					  y + TITLE_H + 2 * BORDER + 8 + row,
					  color);
		}
		rect_outline(scene, x + 2 * BORDER + 8,
			     y + TITLE_H + 2 * BORDER + 8, swatch_w,
			     swatch_h, rgb(12, 16, 28));
	} else {
		const char *lines[] = {
			"ZHIBOX OS",
			"OLK-5.10",
			"FB + EVDEV",
			"Q TO QUIT",
		};
		unsigned int i;

		if (window->title[0] == 'I') {
			lines[0] = "MOUSE  : EVDEV";
			lines[1] = "DRAG   : TITLE";
			lines[2] = "RAISE  : CLICK";
			lines[3] = "EXIT   : Q";
		}

		for (i = 0; i < 4; i++)
			draw_text(scene, x + 2 * BORDER + 10,
				  y + TITLE_H + 2 * BORDER + 10 + (int)i * 22,
				  lines[i], rgb(226, 232, 244), 2);
	}
}

static void build_scene(void)
{
	int i;

	buffer_copy(scene, background, (size_t)pitch * screen_h);
	for (i = 0; i < WINDOW_COUNT; i++)
		draw_window(&windows[stacking[i]]);
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

static int window_at(int x, int y)
{
	int i;

	for (i = WINDOW_COUNT - 1; i >= 0; i--) {
		int index = stacking[i];
		const struct window *window = &windows[index];

		if (x >= window->x && x < window->x + window->w &&
		    y >= window->y && y < window->y + window->h)
			return index;
	}
	return -1;
}

static void raise_window(int index)
{
	int position = -1;
	int i;

	for (i = 0; i < WINDOW_COUNT; i++)
		if (stacking[i] == index)
			position = i;
	if (position < 0 || position == WINDOW_COUNT - 1)
		return;

	for (i = position; i < WINDOW_COUNT - 1; i++)
		stacking[i] = stacking[i + 1];
	stacking[WINDOW_COUNT - 1] = index;
	scene_dirty = 1;
}

static void mouse_press(void)
{
	int index = window_at(mouse_x, mouse_y);

	if (index < 0)
		return;

	raise_window(index);
	if (mouse_y < windows[index].y + TITLE_H) {
		mouse_grab = index;
		grab_dx = mouse_x - windows[index].x;
		grab_dy = mouse_y - windows[index].y;
	}
}

static void mouse_release(void)
{
	mouse_grab = -1;
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

	if (mouse_grab >= 0 && changed) {
		windows[mouse_grab].x = mouse_x - grab_dx;
		windows[mouse_grab].y = mouse_y - grab_dy;
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

static void layout_windows(void)
{
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

	windows[0].x = screen_w / 12;
	windows[0].y = screen_h / 8;
	windows[0].w = w;
	windows[0].h = h;
	windows[0].title = "GRADIENT";
	windows[0].body = rgb(30, 38, 54);
	windows[0].bar = rgb(52, 108, 176);
	windows[0].kind = WIN_GRADIENT;

	windows[1].x = screen_w / 2 + screen_w / 20;
	windows[1].y = screen_h / 5;
	windows[1].w = w;
	windows[1].h = h;
	windows[1].title = "ABOUT";
	windows[1].body = rgb(34, 42, 58);
	windows[1].bar = rgb(92, 74, 148);
	windows[1].kind = WIN_TEXT;

	windows[2].x = screen_w / 5;
	windows[2].y = screen_h / 2 + screen_h / 12;
	windows[2].w = w;
	windows[2].h = h;
	windows[2].title = "INPUT";
	windows[2].body = rgb(30, 44, 40);
	windows[2].bar = rgb(52, 132, 104);
	windows[2].kind = WIN_TEXT;
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

	layout_windows();
	build_background();
	build_scene();
	blit_full();
	damage_add(0, 0, screen_w, screen_h);
	cursor_show();
	damage_present();
	scene_dirty = 0;

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
