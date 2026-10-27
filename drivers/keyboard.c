#include <stdint.h>
#include "keyboard.h"

#define I8042_STATUS 0x64
#define I8042_DATA   0x60
#define I8042_OBF    0x01
#define I8042_IBF    0x02
#define I8042_AUX    0x20
#define I8042_ERROR  0xC0
#define I8042_RESET  0xFE
#define I8042_POLL_LIMIT 100000u

static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port));
}

keyboard_poll_result_t keyboard_poll(uint8_t *scan_code) {
    if (!scan_code) return KEYBOARD_IO_ERROR;
    uint8_t status = in8(I8042_STATUS);
    if (status == 0xFF) return KEYBOARD_UNAVAILABLE;
    if (!(status & I8042_OBF)) return KEYBOARD_NO_DATA;
    uint8_t data = in8(I8042_DATA);
    if (status & I8042_ERROR) return KEYBOARD_IO_ERROR;
    if (status & I8042_AUX) return KEYBOARD_NO_DATA;
    *scan_code = data;
    return KEYBOARD_KEY;
}

keyboard_reset_result_t keyboard_request_reset(void) {
    for (uint32_t i = 0; i < I8042_POLL_LIMIT; ++i) {
        uint8_t status = in8(I8042_STATUS);
        if (status == 0xFF) return KEYBOARD_RESET_UNAVAILABLE;
        if (!(status & I8042_IBF)) {
            out8(I8042_STATUS, I8042_RESET);
            return KEYBOARD_RESET_SENT;
        }
        __asm__ volatile("pause");
    }
    return KEYBOARD_RESET_TIMEOUT;
}
