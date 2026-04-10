#pragma once
#include <stdint.h>

typedef struct SysInfo {
    int      logical_cores;
    int      physical_cores;
    uint32_t l1_cache_kb;
    uint32_t l2_cache_kb;
    uint32_t l3_cache_kb;
    char     cpu_brand[64];

    /* Instruction set extensions */
    int has_mmx, has_sse, has_sse2, has_sse3, has_ssse3;
    int has_sse41, has_sse42;
    int has_avx, has_avx2;
    int has_avx512f, has_avx512bw, has_avx512vl, has_avx512dq;
    int has_fma, has_fma4;
    int has_bmi1, has_bmi2;
    int has_popcnt, has_lzcnt;
    int has_aes, has_sha;
    int has_f16c, has_movbe;
} SysInfo;

SysInfo sysinfo_probe(void);
void    sysinfo_print(const SysInfo *si);
