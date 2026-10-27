#ifndef ASTERION_KEYBOARD_H
#define ASTERION_KEYBOARD_H

#include <stdint.h>

typedef enum {
    KEYBOARD_KEY,
    KEYBOARD_NO_DATA,
    KEYBOARD_UNAVAILABLE,
    KEYBOARD_IO_ERROR
} keyboard_poll_result_t;

typedef enum {
    KEYBOARD_RESET_SENT,
    KEYBOARD_RESET_UNAVAILABLE,
    KEYBOARD_RESET_TIMEOUT
} keyboard_reset_result_t;

/* Nonblocking: returns immediately when the controller has no byte ready. */
keyboard_poll_result_t keyboard_poll(uint8_t *scan_code);
/* Waits only for a bounded time for the controller input buffer to clear. */
keyboard_reset_result_t keyboard_request_reset(void);

#endif
