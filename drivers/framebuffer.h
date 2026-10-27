#ifndef ASTERION_FRAMEBUFFER_H
#define ASTERION_FRAMEBUFFER_H

#include "../include/boot_info.h"
#include "serial.h"

void console_init(const boot_info_t *info);
void console_log(serial_level_t level, const char *message);
void console_render(const char *input, int shifted);
void console_clear(void);
int console_set_theme(char theme);

#endif
