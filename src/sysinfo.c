#include "sysinfo.h"
#include <stdio.h>
#include <string.h>

#ifdef __x86_64__
#  include <cpuid.h>
#endif

/* ------------------------------------------------------------------ */
/* CPUID helpers                                                       */
/* ------------------------------------------------------------------ */

static void cpuid_brand(char brand[64]) {
#ifdef __x86_64__
    unsigned int r[12];
    __cpuid(0x80000002u, r[0],  r[1],  r[2],  r[3]);
    __cpuid(0x80000003u, r[4],  r[5],  r[6],  r[7]);
    __cpuid(0x80000004u, r[8],  r[9],  r[10], r[11]);
    memcpy(brand, r, 48);
    brand[48] = '\0';
    char *p = brand;
    while (*p == ' ') p++;
    if (p != brand) memmove(brand, p, strlen(p) + 1);
#else
    strncpy(brand, "unknown", 63);
    brand[63] = '\0';
#endif
}

static void detect_isa(SysInfo *si) {
#ifdef __x86_64__
    unsigned int eax, ebx, ecx, edx;

    /* ---- Leaf 1: standard features ---- */
    __cpuid_count(1, 0, eax, ebx, ecx, edx);

    si->has_mmx    = (edx >> 23) & 1;
    si->has_sse    = (edx >> 25) & 1;
    si->has_sse2   = (edx >> 26) & 1;
    si->has_sse3   = (ecx >>  0) & 1;
    si->has_ssse3  = (ecx >>  9) & 1;
    si->has_sse41  = (ecx >> 19) & 1;
    si->has_sse42  = (ecx >> 20) & 1;
    si->has_avx    = (ecx >> 28) & 1;
    si->has_fma    = (ecx >> 12) & 1;
    si->has_popcnt = (ecx >> 23) & 1;
    si->has_aes    = (ecx >> 25) & 1;
    si->has_f16c   = (ecx >> 29) & 1;
    si->has_movbe  = (ecx >> 22) & 1;

    /* ---- Leaf 7 sub-leaf 0: extended features ---- */
    __cpuid_count(7, 0, eax, ebx, ecx, edx);

    si->has_bmi1      = (ebx >>  3) & 1;
    si->has_avx2      = (ebx >>  5) & 1;
    si->has_bmi2      = (ebx >>  8) & 1;
    si->has_avx512f   = (ebx >> 16) & 1;
    si->has_avx512dq  = (ebx >> 17) & 1;
    si->has_avx512bw  = (ebx >> 30) & 1;
    si->has_avx512vl  = (ebx >> 31) & 1;
    si->has_sha       = (ebx >> 29) & 1;
    si->has_lzcnt     = (ecx >>  5) & 1;   /* LZCNT in ECX of leaf 7 */

    /* ---- Leaf 0x80000001: AMD extended ---- */
    __cpuid_count(0x80000001u, 0, eax, ebx, ecx, edx);
    si->has_fma4   = (ecx >> 16) & 1;  /* AMD FMA4 */
    /* LZCNT (ABM) */
    if ((ecx >> 5) & 1) si->has_lzcnt = 1;
#else
    (void)si;
#endif
}

/* ------------------------------------------------------------------ */
/* Sysfs helpers                                                       */
/* ------------------------------------------------------------------ */

static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int v = 0;
    (void)fscanf(f, "%d", &v);
    fclose(f);
    return v;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

SysInfo sysinfo_probe(void) {
    SysInfo si;
    memset(&si, 0, sizeof(si));

    cpuid_brand(si.cpu_brand);
    detect_isa(&si);

    /* logical cores via /proc/cpuinfo */
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            int phys_max = 0;
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "processor", 9) == 0)
                    si.logical_cores++;
                int v;
                if (sscanf(line, "cpu cores : %d", &v) == 1 && v > phys_max)
                    phys_max = v;
            }
            fclose(f);
            if (phys_max > 0) si.physical_cores = phys_max;
        }
    }
    if (si.logical_cores <= 0) si.logical_cores = 1;
    if (si.physical_cores <= 0) si.physical_cores = si.logical_cores;

    /* cache sizes from sysfs */
    for (int idx = 0; idx < 8; idx++) {
        char lvl_path[128], sz_path[128];
        snprintf(lvl_path, sizeof(lvl_path),
            "/sys/devices/system/cpu/cpu0/cache/index%d/level", idx);
        snprintf(sz_path, sizeof(sz_path),
            "/sys/devices/system/cpu/cpu0/cache/index%d/size", idx);
        int level = read_int_file(lvl_path);
        if (level == 0) break;
        FILE *f = fopen(sz_path, "r");
        if (!f) continue;
        uint32_t val = 0;
        char unit = 'K';
        (void)fscanf(f, "%u%c", &val, &unit);
        fclose(f);
        if (unit == 'M') val *= 1024;
        if (level == 1 && si.l1_cache_kb == 0) si.l1_cache_kb = val;
        else if (level == 2 && si.l2_cache_kb == 0) si.l2_cache_kb = val;
        else if (level == 3 && si.l3_cache_kb == 0) si.l3_cache_kb = val;
    }

    return si;
}

/* Print a flag if present */
#define F(name, field) if (si->field) printf(" " name);

void sysinfo_print(const SysInfo *si) {
    printf("  CPU      : %s\n",
           si->cpu_brand[0] ? si->cpu_brand : "unknown");
    printf("  Cores    : %d logical, %d physical\n",
           si->logical_cores, si->physical_cores);
    if (si->l1_cache_kb) printf("  L1 cache : %u KB\n", si->l1_cache_kb);
    if (si->l2_cache_kb) printf("  L2 cache : %u KB\n", si->l2_cache_kb);
    if (si->l3_cache_kb) printf("  L3 cache : %u KB\n", si->l3_cache_kb);

    printf("  ISA exts :");
    F("MMX",      has_mmx)
    F("SSE",      has_sse)
    F("SSE2",     has_sse2)
    F("SSE3",     has_sse3)
    F("SSSE3",    has_ssse3)
    F("SSE4.1",   has_sse41)
    F("SSE4.2",   has_sse42)
    F("AVX",      has_avx)
    F("AVX2",     has_avx2)
    F("AVX-512F", has_avx512f)
    F("AVX-512BW",has_avx512bw)
    F("AVX-512VL",has_avx512vl)
    F("AVX-512DQ",has_avx512dq)
    F("FMA3",     has_fma)
    F("FMA4",     has_fma4)
    F("BMI1",     has_bmi1)
    F("BMI2",     has_bmi2)
    F("POPCNT",   has_popcnt)
    F("LZCNT",    has_lzcnt)
    F("AES-NI",   has_aes)
    F("SHA",      has_sha)
    F("F16C",     has_f16c)
    F("MOVBE",    has_movbe)
    printf("\n");
}
