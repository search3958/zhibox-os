/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux-shim: 最小 libc 実装 (macOS 上で静的 Linux バイナリを作るため)
 *   - syscall を直接発行する
 *   - fprintf は fb-gradient.c が必要とする書式 (%s %u %d %x %c %%) のみ対応
 */
#include <shim.h>

void *stderr = (void *)STDERR_FILENO;

ssize_t read(int fd, void *buf, size_t count)
{
	return zh_syscall(ZH_SYS_read, fd, (long)buf, (long)count);
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return zh_syscall(ZH_SYS_write, fd, (long)buf, (long)count);
}

int open(const char *path, int flags, ...)
{
	return (int)zh_syscall(ZH_SYS_open, (long)path, flags, 0);
}

int close(int fd)
{
	return (int)zh_syscall(ZH_SYS_close, fd, 0, 0);
}

int ioctl(int fd, unsigned long request, ...)
{
	__builtin_va_list ap;
	void *arg;

	__builtin_va_start(ap, request);
	arg = __builtin_va_arg(ap, void *);
	__builtin_va_end(ap);

	return (int)zh_syscall(ZH_SYS_ioctl, fd, (long)request, (long)arg);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	long ret = zh_syscall6(ZH_SYS_mmap, (long)addr, (long)length, prot,
			       flags, fd, offset);

	/* Linux はエラーを -errno で返す */
	return ret < 0 && ret > -4096 ? MAP_FAILED : (void *)ret;
}

int munmap(void *addr, size_t length)
{
	return (int)zh_syscall(ZH_SYS_munmap, (long)addr, (long)length, 0);
}

/* ------------------------------------------------------------- printf */

static char *put_unsigned(char *out, unsigned long value, unsigned int base,
			  int width, char pad)
{
	char tmp[24];
	unsigned int len = 0;
	unsigned int i;

	do {
		unsigned int digit = value % base;

		tmp[len++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
		value /= base;
	} while (value);
	while ((int)len < width) {
		tmp[len++] = pad;
	}
	for (i = len; i > 0; i--)
		*out++ = tmp[i - 1];
	return out;
}

static char *format_one(char *out, char spec, __builtin_va_list *ap)
{
	switch (spec) {
	case 's': {
		const char *s = __builtin_va_arg(*ap, const char *);
		if (!s)
			s = "(null)";
		while (*s)
			*out++ = *s++;
		break;
	}
	case 'c':
		*out++ = (char)__builtin_va_arg(*ap, int);
		break;
	case 'd': {
		int value = __builtin_va_arg(*ap, int);
		if (value < 0) {
			*out++ = '-';
			out = put_unsigned(out, (unsigned long)(-(long)value), 10, 0, ' ');
		} else {
			out = put_unsigned(out, (unsigned long)value, 10, 0, ' ');
		}
		break;
	}
	case 'u':
		out = put_unsigned(out, __builtin_va_arg(*ap, unsigned int),
				   10, 0, ' ');
		break;
	case 'x':
		out = put_unsigned(out, __builtin_va_arg(*ap, unsigned int),
				   16, 0, ' ');
		break;
	default:
		*out++ = '%';
		*out++ = spec;
		break;
	}
	return out;
}

int fprintf(void *stream, const char *format, ...)
{
	char buffer[256];
	char *out = buffer;
	__builtin_va_list ap;

	__builtin_va_start(ap, format);
	while (*format && out < buffer + sizeof(buffer) - 1) {
		if (*format != '%') {
			*out++ = *format++;
			continue;
		}
		format++;
		if (!*format)
			break;
		if (*format == '%') {
			*out++ = '%';
			format++;
			continue;
		}
		out = format_one(out, *format++, &ap);
	}
	__builtin_va_end(ap);

	return (int)write((int)(long)stream, buffer, (size_t)(out - buffer));
}

void perror(const char *s)
{
	char buffer[128];
	char *out = buffer;

	if (s && *s) {
		while (*s && out < buffer + sizeof(buffer) - 3)
			*out++ = *s++;
		*out++ = ':';
		*out++ = ' ';
	}
	*out++ = 'e';
	*out++ = 'r';
	*out++ = 'r';
	*out++ = 'o';
	*out++ = 'r';
	*out++ = '\n';

	write(STDERR_FILENO, buffer, (size_t)(out - buffer));
}
