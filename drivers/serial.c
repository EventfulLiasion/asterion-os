#include <stdint.h>
#include "serial.h"

static int enabled;

static inline void out8(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0,%1" :: "a"(value), "Nd"(port));
}

static inline uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}

void serial_init(void) {
    if (in8(0x3FD) == 0xFF) return;
    out8(0x3F9, 0); out8(0x3FB, 0x80);
    out8(0x3F8, 3); out8(0x3F9, 0);
    out8(0x3FB, 3); out8(0x3FA, 0xC7); out8(0x3FC, 0x0B);
    enabled = 1;
}

static void serial_write_char(char c) {
    if (!enabled) return;
    for (uint32_t i = 0; i < 20000; ++i) {
        if (in8(0x3FD) & 0x20) { out8(0x3F8, (uint8_t)c); return; }
    }
}

static void serial_write(const char *text) {
    while (*text) serial_write_char(*text++);
}

void serial_write_bytes(const char *data, unsigned long length) {
    if (!data) return;
    for (unsigned long i = 0; i < length; ++i) serial_write_char(data[i]);
}

void serial_log(serial_level_t level, const char *message) {
    static const char *const labels[] = {"DEBUG", "INFO", "WARN", "ERROR", "PANIC"};
    if ((unsigned)level >= sizeof(labels) / sizeof(labels[0])) level = SERIAL_ERROR;
    serial_write("["); serial_write(labels[level]); serial_write("] ");
    serial_write(message ? message : "(null)");
    serial_write("\r\n");
}
