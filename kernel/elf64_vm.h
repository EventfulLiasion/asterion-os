#ifndef ASTERION_KERNEL_ELF64_VM_H
#define ASTERION_KERNEL_ELF64_VM_H
#include "elf64_loader.h"
#include "vm.h"
elf64_status_t elf64_load_user_image(vm_space_t *space, const void *image,
                                     size_t image_size, elf64_loaded_image_t *loaded);
elf64_status_t elf64_unload_user_image(vm_space_t *space,
                                       elf64_loaded_image_t *loaded);
#endif
