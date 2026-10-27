#ifndef ASTERION_SERIAL_H
#define ASTERION_SERIAL_H

typedef enum {
    SERIAL_DEBUG,
    SERIAL_INFO,
    SERIAL_WARN,
    SERIAL_ERROR,
    SERIAL_PANIC
} serial_level_t;

void serial_init(void);
void serial_log(serial_level_t level, const char *message);
void serial_write_bytes(const char *data, unsigned long length);

#endif
