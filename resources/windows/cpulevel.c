/* astra-cpulevel: report the x86-64 microarchitecture level of this machine.
 *
 * The Windows installer carries one ASTRA payload per level it was built for
 * (see ASTRA_WIN_VARIANTS in build-windows.sh) and runs this probe before
 * copying anything, to pick the fastest payload the CPU can execute. It is
 * its own 64-bit program, rather than a check inside the 32-bit setup, on
 * purpose: under x64 emulation on ARM a 32-bit process is not told about
 * AVX/AVX2 even when 64-bit processes are, and the question is what a 64-bit
 * ASTRA.exe will be allowed to run.
 *
 * The answer is the exit code: 10 + level, so 11 (plain x86-64) to 14
 * (x86-64-v4). Anything else means the probe itself did not run, which the
 * installer treats as level 1. No output and no dependencies beyond the C
 * runtime entry point; build-windows.sh compiles it -march=x86-64 -static.
 *
 * Levels as defined by the x86-64 psABI:
 *   v2  CMPXCHG16B LAHF/SAHF POPCNT SSE3 SSSE3 SSE4.1 SSE4.2
 *   v3  v2 + AVX AVX2 BMI1 BMI2 F16C FMA LZCNT MOVBE, YMM state enabled by OS
 *   v4  v3 + AVX512F/BW/CD/DQ/VL, ZMM and opmask state enabled by the OS
 */
#include <cpuid.h>

static unsigned long long xcr0(void)
{
    unsigned int eax, edx;
    /* xgetbv, spelled as bytes so no -mxsave is needed to assemble it. */
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | eax;
}

static int has_all(unsigned int reg, unsigned int mask)
{
    return (reg & mask) == mask;
}

static int cpu_level(void)
{
    unsigned int a, b, c, d;
    unsigned int ecx1 = 0, ebx7 = 0, ecx81 = 0;
    unsigned long long xcr = 0;

    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 1;
    ecx1 = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d))
        ebx7 = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d))
        ecx81 = c;

    /* v2: SSE3 (0), SSSE3 (9), CMPXCHG16B (13), SSE4.1 (19), SSE4.2 (20),
     * POPCNT (23); LAHF/SAHF in long mode is extended leaf ECX bit 0. */
    if (!has_all(ecx1, (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) |
                           (1u << 20) | (1u << 23)) ||
        !has_all(ecx81, 1u << 0))
        return 1;

    /* v3: FMA (12), MOVBE (22), OSXSAVE (27), AVX (28), F16C (29);
     * leaf 7 EBX: BMI1 (3), AVX2 (5), BMI2 (8); LZCNT is extended ECX bit 5.
     * OSXSAVE must be checked before xgetbv, which faults without it. */
    if (!has_all(ecx1, (1u << 12) | (1u << 22) | (1u << 27) | (1u << 28) |
                           (1u << 29)) ||
        !has_all(ebx7, (1u << 3) | (1u << 5) | (1u << 8)) ||
        !has_all(ecx81, 1u << 5))
        return 2;
    xcr = xcr0();
    if ((xcr & 0x6) != 0x6) /* XMM and YMM state saved by the OS */
        return 2;

    /* v4: leaf 7 EBX: AVX512F (16), DQ (17), CD (28), BW (30), VL (31);
     * XCR0 additionally needs opmask, ZMM_Hi256 and Hi16_ZMM (bits 5-7). */
    if (!has_all(ebx7, (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) |
                           (1u << 31)) ||
        (xcr & 0xe6) != 0xe6)
        return 3;

    return 4;
}

int main(void)
{
    return 10 + cpu_level();
}
