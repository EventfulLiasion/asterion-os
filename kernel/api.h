#ifndef ASTERION_KERNEL_API_H
#define ASTERION_KERNEL_API_H

#include <stdint.h>
#include "../include/boot_info.h"
#include "../drivers/serial.h"

void kernel_early_log(serial_level_t level, const char *message);
void kernel_set_boot_tsc(uint64_t timestamp);
void kernel_main(const boot_info_t *info);

#endif
