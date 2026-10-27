#include <stdint.h>
#include "cpu.h"

static void cpuid_leaf(uint32_t leaf, uint32_t subleaf,
                       uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static void store_u32_le(char *out, uint32_t value) {
    out[0] = (char)value; out[1] = (char)(value >> 8);
    out[2] = (char)(value >> 16); out[3] = (char)(value >> 24);
}

void cpu_identify(boot_info_t *info) {
    uint32_t a, b, c, d;
    cpuid_leaf(0, 0, &a, &b, &c, &d);
    store_u32_le(info->cpu_vendor, b); store_u32_le(info->cpu_vendor + 4, d);
    store_u32_le(info->cpu_vendor + 8, c); info->cpu_vendor[12] = 0;
    cpuid_leaf(1, 0, &a, &b, &c, &d);
    info->cpu_signature = a; info->cpu_feature_ecx = c; info->cpu_feature_edx = d;
    cpuid_leaf(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        for (uint32_t leaf = 0; leaf < 3; ++leaf) {
            cpuid_leaf(0x80000002u + leaf, 0, &a, &b, &c, &d);
            store_u32_le(info->cpu_brand + leaf * 16, a);
            store_u32_le(info->cpu_brand + leaf * 16 + 4, b);
            store_u32_le(info->cpu_brand + leaf * 16 + 8, c);
            store_u32_le(info->cpu_brand + leaf * 16 + 12, d);
        }
        info->cpu_brand[48] = 0;
    }
    cpuid_leaf(0x80000001u, 0, &a, &b, &c, &d);
    info->cpu_extended_feature_edx = d;
}
