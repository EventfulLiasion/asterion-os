#ifndef ASTERION_BOOT_VALIDATE_H
#define ASTERION_BOOT_VALIDATE_H

#include "boot_info.h"

/* Pure checks shared by the freestanding kernel and host-side tests. */
int boot_info_validate(const boot_info_t *info);

#endif
