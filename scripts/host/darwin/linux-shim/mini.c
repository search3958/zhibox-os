/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux-shim: 最小 libc 実装 (macOS 上で静的 Linux バイナリを作るため)
 *   - syscall を直接発行する
 *   - printf/fprintf は fb-gradient.c が必要とする書式のみ対応
 */
#include <shim.h>

int errno;
void *stdout = (void *)STDOUT_FILENO;
void *stderr = (void *)STDERR_FILENO;

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = s;

	while (n--)
		*p++ = (unsigned char)c;
	return s;
}

static long checked(long result)
{
	if (result < 0 && result > -4096) {
		errno = (int)-result;
		return -1;
	}
	return result;
}

ssize_t read(int fd, void *buf, size_t count)
{
	return checked(zh_syscall(ZH_SYS_read, fd, (long)buf, (long)count));
}

ssize_t write(int fd, const void *buf, size_t count)
{
	return checked(zh_syscall(ZH_SYS_write, fd, (long)buf, (long)count));
}

int open(const char *path, int flags, ...)
{
	return (int)checked(zh_syscall(ZH_SYS_open, (long)path, flags, 0));
}

int close(int fd)
{
	return (int)checked(zh_syscall(ZH_SYS_close, fd, 0, 0));
}

int ioctl(int fd, unsigned long request, ...)
{
	__builtin_va_list ap;
	void *arg;

	__builtin_va_start(ap, request);
	arg = __builtin_va_arg(ap, void *);
	__builtin_va_end(ap);

	return (int)checked(zh_syscall(ZH_SYS_ioctl, fd, (long)request,
				       (long)arg));
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	long result = checked(zh_syscall6(ZH_SYS_mmap, (long)addr,
					  (long)length, prot, flags, fd,
					  offset));

	return result < 0 ? MAP_FAILED : (void *)result;
}

int munmap(void *addr, size_t length)
{
	return (int)checked(zh_syscall(ZH_SYS_munmap, (long)addr, (long)length,
				       0));
}

int nanosleep(const struct timespec *request, struct timespec *remaining)
{
	return (int)checked(zh_syscall(ZH_SYS_nanosleep, (long)request,
				       (long)remaining, 0));
}

int tcgetattr(int fd, struct termios *termios_p)
{
	return (int)checked(zh_syscall(ZH_SYS_ioctl, fd, TCGETS,
				       (long)termios_p));
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
	(void)optional_actions;
	return (int)checked(zh_syscall(ZH_SYS_ioctl, fd, TCSETS,
				       (long)termios_p));
}

/* ------------------------------------------------------------- printf */

static char *put_unsigned(char *out, unsigned long value, unsigned int base)
{
	char temporary[24];
	unsigned int length = 0;
	unsigned int i;

	do {
		unsigned int digit = value % base;

		temporary[length++] =
			digit < 10 ? (char)('0' + digit) : (char)('a' + digit - 10);
		value /= base;
	} while (value);

	for (i = length; i > 0; i--)
		*out++ = temporary[i - 1];
	return out;
}

static char *format_one(char *out, const char *spec,
			__builtin_va_list *ap, int long_value)
{
	switch (*spec) {
	case 's': {
		const char *text = __builtin_va_arg(*ap, const char *);

		if (!text)
			text = "(null)";
		while (*text)
			*out++ = *text++;
		break;
	}
	case 'c':
		*out++ = (char)__builtin_va_arg(*ap, int);
		break;
	case 'd': {
		long value = long_value ?
			__builtin_va_arg(*ap, long) :
			(long)__builtin_va_arg(*ap, int);

		if (value < 0) {
			*out++ = '-';
			out = put_unsigned(out, (unsigned long)(-value), 10);
		} else {
			out = put_unsigned(out, (unsigned long)value, 10);
		}
		break;
	}
	case 'u':
		out = put_unsigned(out,
				   long_value ?
					   __builtin_va_arg(*ap, unsigned long) :
					   (unsigned long)__builtin_va_arg(*ap, unsigned int),
				   10);
		break;
	case 'x':
		out = put_unsigned(out,
				   long_value ?
					   __builtin_va_arg(*ap, unsigned long) :
					   (unsigned long)__builtin_va_arg(*ap, unsigned int),
				   16);
		break;
	default:
		*out++ = '%';
		*out++ = *spec;
		break;
	}
	return out;
}

static int vformat(int fd, const char *format, __builtin_va_list *ap)
{
	char buffer[256];
	char *out = buffer;

	while (*format && out < buffer + sizeof(buffer) - 1) {
		int long_value = 0;

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
		if (*format == 'l') {
			long_value = 1;
			format++;
		}
		if (!*format)
			break;
		out = format_one(out, format++, ap, long_value);
	}

	return (int)write(fd, buffer, (size_t)(out - buffer));
}

int printf(const char *format, ...)
{
	__builtin_va_list ap;
	int result;

	__builtin_va_start(ap, format);
	result = vformat(STDOUT_FILENO, format, &ap);
	__builtin_va_end(ap);
	return result;
}

int fprintf(void *stream, const char *format, ...)
{
	__builtin_va_list ap;
	int result;

	__builtin_va_start(ap, format);
	result = vformat((int)(long)stream, format, &ap);
	__builtin_va_end(ap);
	return result;
}

void perror(const char *s)
{
	char buffer[96];
	char *out = buffer;

	if (s && *s) {
		while (*s && out < buffer + sizeof(buffer) - 16)
			*out++ = *s++;
		*out++ = ':';
		*out++ = ' ';
	}
	*out++ = 'e';
	*out++ = 'r';
	*out++ = 'r';
	*out++ = 'o';
	*out++ = 'r';
	*out++ = ' ';
	out = put_unsigned(out, (unsigned long)(errno < 0 ? -errno : errno), 10);
	*out++ = '\n';

	write(STDERR_FILENO, buffer, (size_t)(out - buffer));
}
