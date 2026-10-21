/* SPDX-License-Identifier: GPL-2.0 */
/*
 * macOS ホストで x86_64-elf-gcc を使い、静的 Linux バイナリを作るための
 * 最小限の libc 代替 (freestanding)。
 *
 * ここで定義する構造体・定数は Linux の UAPI (uapi/linux/fb.h, uapi/linux/kd.h)
 * と ABI 互換。glibc には依存せず、syscall を直接発行する。
 */
#ifndef ZHIBOX_LINUX_SHIM_H
#define ZHIBOX_LINUX_SHIM_H

#include <stddef.h>
#include <stdint.h>

typedef long ssize_t;
typedef long off_t;

/* ------------------------------------------------------------ syscall */

static inline long zh_syscall6(long nr, long a1, long a2, long a3,
			       long a4, long a5, long a6)
{
	register long r10 __asm__("r10") = a4;
	register long r8 __asm__("r8") = a5;
	register long r9 __asm__("r9") = a6;
	long ret;

	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
			   "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}

#define ZH_SYS_read		0
#define ZH_SYS_write		1
#define ZH_SYS_open		2
#define ZH_SYS_close		3
#define ZH_SYS_mmap		9
#define ZH_SYS_munmap		11
#define ZH_SYS_ioctl		16
#define ZH_SYS_exit		60

static inline long zh_syscall(long nr, long a1, long a2, long a3)
{
	return zh_syscall6(nr, a1, a2, a3, 0, 0, 0);
}

/* ------------------------------------------------------------ 標準関数 */

#define STDIN_FILENO	0
#define STDOUT_FILENO	1
#define STDERR_FILENO	2

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int open(const char *path, int flags, ...);
int close(int fd);
int ioctl(int fd, unsigned long request, ...);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
void perror(const char *s);
int fprintf(void *stream, const char *format, ...);

extern void *stderr;

/* ------------------------------------------------------------ fcntl */
#define O_RDWR		02
#define O_NOCTTY	0400

/* ------------------------------------------------------------ mman */
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_SHARED	0x01
#define MAP_FAILED	((void *)-1)

/* ------------------------------------------------------------ fb / kd */
struct fb_bitfield {
	uint32_t offset;
	uint32_t length;
	uint32_t msb_right;
};

struct fb_fix_screeninfo {
	char id[16];
	unsigned long smem_start;
	uint32_t smem_len;
	uint32_t type;
	uint32_t type_aux;
	uint32_t visual;
	uint16_t xpanstep;
	uint16_t ypanstep;
	uint16_t ywrapstep;
	uint32_t line_length;
	unsigned long mmio_start;
	uint32_t mmio_len;
	uint32_t accel;
	uint16_t capabilities;
	uint16_t reserved[2];
};

struct fb_var_screeninfo {
	uint32_t xres;
	uint32_t yres;
	uint32_t xres_virtual;
	uint32_t yres_virtual;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t bits_per_pixel;
	uint32_t grayscale;
	struct fb_bitfield red;
	struct fb_bitfield green;
	struct fb_bitfield blue;
	struct fb_bitfield transp;
	uint32_t nonstd;
	uint32_t activate;
	uint32_t height;
	uint32_t width;
	uint32_t accel_flags;
	uint32_t pixclock;
	uint32_t left_margin;
	uint32_t right_margin;
	uint32_t upper_margin;
	uint32_t lower_margin;
	uint32_t hsync_len;
	uint32_t vsync_len;
	uint32_t sync;
	uint32_t vmode;
	uint32_t rotate;
	uint32_t colorspace;
	uint32_t reserved[4];
};

#define FBIOGET_VSCREENINFO	0x4600
#define FBIOGET_FSCREENINFO	0x4602

#define KDSETMODE		0x4B3A
#define KD_TEXT			0x00
#define KD_GRAPHICS		0x01

#endif /* ZHIBOX_LINUX_SHIM_H */
