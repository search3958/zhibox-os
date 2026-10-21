#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

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

static void draw_gradient(uint8_t *memory, size_t memory_length,
			 const struct fb_fix_screeninfo *fixed,
			 const struct fb_var_screeninfo *variable,
			 unsigned int bytes_per_pixel)
{
	unsigned int y;

	for (y = 0; y < variable->yres; y++) {
		unsigned int x;
	uint8_t green = variable->yres > 1 ?
			(uint8_t)((uint64_t)y * 255 / (variable->yres - 1)) : 0;
	uint32_t pixel = channel(255, variable->red) |
			 channel(green, variable->green) |
			 channel(0, variable->blue) |
			 channel(255, variable->transp);

		for (x = 0; x < variable->xres; x++) {
			size_t offset = (size_t)(y + variable->yoffset) *
				fixed->line_length +
				(size_t)(x + variable->xoffset) * bytes_per_pixel;
			unsigned int byte;

			if (offset + bytes_per_pixel > memory_length)
				return;
			for (byte = 0; byte < bytes_per_pixel; byte++)
				memory[offset + byte] = pixel >> (byte * 8);
		}
	}
}

int main(void)
{
	static const char prompt[] = "Red to yellow gradient. Press Enter to return.\r\n";
	static const char clear[] = "\033[0m\033[2J\033[H";
	struct fb_fix_screeninfo fixed;
	struct fb_var_screeninfo variable;
	int console = -1;
	int framebuffer = -1;
	void *memory = MAP_FAILED;
	unsigned int bytes_per_pixel;
	char input[32];
	ssize_t input_length;
	int result = 1;

	console = open("/dev/tty0", O_RDWR | O_NOCTTY);
	if (console < 0) {
		perror("open /dev/tty0");
		goto out;
	}

	framebuffer = open("/dev/fb0", O_RDWR);
	if (framebuffer < 0) {
		perror("open /dev/fb0");
		goto out;
	}
	if (ioctl(framebuffer, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    ioctl(framebuffer, FBIOGET_VSCREENINFO, &variable) < 0) {
		perror("read framebuffer mode");
		goto out;
	}
	bytes_per_pixel = (variable.bits_per_pixel + 7) / 8;
	if (!variable.xres || !variable.yres ||
	    (variable.bits_per_pixel != 16 && variable.bits_per_pixel != 24 &&
	     variable.bits_per_pixel != 32) ||
	    fixed.line_length < (variable.xres + variable.xoffset) * bytes_per_pixel) {
		fprintf(stderr, "Unsupported framebuffer mode: %ux%u at %u bpp\n",
			variable.xres, variable.yres, variable.bits_per_pixel);
		goto out;
	}

	memory = mmap(NULL, fixed.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      framebuffer, 0);
	if (memory == MAP_FAILED) {
		perror("mmap /dev/fb0");
		goto out;
	}
	if (write(STDOUT_FILENO, prompt, sizeof(prompt) - 1) < 0) {
		perror("write prompt");
		goto out;
	}
	if (ioctl(console, KDSETMODE, KD_GRAPHICS) < 0) {
		perror("switch to graphics mode");
		goto out;
	}

	draw_gradient(memory, fixed.smem_len, &fixed, &variable,
		      bytes_per_pixel);
	input_length = read(STDIN_FILENO, input, sizeof(input));
	if (input_length < 0) {
		perror("read prompt");
		goto out;
	}
	result = 0;

out:
	if (console >= 0) {
		ioctl(console, KDSETMODE, KD_TEXT);
		if (write(console, clear, sizeof(clear) - 1) < 0)
			result = 1;
	}
	if (memory != MAP_FAILED)
		munmap(memory, fixed.smem_len);
	if (framebuffer >= 0)
		close(framebuffer);
	if (console >= 0)
		close(console);
	return result;
}