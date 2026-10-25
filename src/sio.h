/*
 * The controller port (slot 1), a byte at a time: the pad and the memory
 * card share it (pad.c, save.c), each answering its own address.
 */
#pragma once

/* Selects slot 1 (the pad and the card both listen). */
void sio_select(void);
void sio_deselect(void);
/* One byte each way; waits for the device's acknowledge unless it's the
 * last byte. The byte back, or < 0 if nothing answered. */
int sio_xfer(int b, int ack);
