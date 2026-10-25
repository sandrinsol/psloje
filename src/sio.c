/*
 * The controller port: see sio.h.
 */
#include <stdint.h>
#include <hwregs_c.h>
#include "sio.h"

int sio_xfer(int b, int ack) {
	int t = 0;
	while (!(SIO_STAT(0) & 1)) if (++t > 20000) return -1;   /* ready to send */
	SIO_DATA(0) = b;
	t = 0;
	while (!(SIO_STAT(0) & 2)) if (++t > 20000) return -1;   /* a byte back */
	int r = SIO_DATA(0);
	if (ack) {
		t = 0;
		while (!(SIO_STAT(0) & 0x200)) if (++t > 4000) return -2;   /* the device's /ACK */
		SIO_CTRL(0) |= 0x0010;
		IRQ_STAT = ~(1 << 7);
	}
	return r;
}

void sio_select(void) {
	SIO_MODE(0) = 0x000d;
	SIO_BAUD(0) = 0x0088;
	SIO_CTRL(0) = 0x1003;   /* port 1 selected, sending, /ACK raises a flag */
	for (volatile int i = 0; i < 400; i++);
}

void sio_deselect(void) {
	SIO_CTRL(0) = 0;
	for (volatile int i = 0; i < 400; i++);
}
