#ifndef ASTERION_KERNEL_AHCI_H
#define ASTERION_KERNEL_AHCI_H
#include "block_core.h"
#include "fs_core.h"
int ahci_init(block_device_t *device);
int storage_mount_aosfs(block_device_t *device, fs_volume_t *volume);
#endif
