#ifndef ZHIBOX_LINUX_INPUT_H
#define ZHIBOX_LINUX_INPUT_H

#include "shim.h"

struct input_event {
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

#define EV_SYN		0x00
#define EV_KEY		0x01
#define EV_REL		0x02

#define REL_X		0x00
#define REL_Y		0x01

#define BTN_LEFT	0x110

#define SYN_REPORT	0

#define EVIOCGBIT(ev, len) \
	_IOC(_IOC_READ, 'E', 0x20 + (ev), (len))

#endif /* ZHIBOX_LINUX_INPUT_H */
