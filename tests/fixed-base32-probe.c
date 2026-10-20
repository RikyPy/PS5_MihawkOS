/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A 32-bit program as old installers are linked: at 0x400000 with its relocations stripped (/fixed), so it can load
 * only there. In a helper that range held the carrier's own executable (0x400000-0x584000) until the payload loader
 * gave it back (native/proton_payload_child.c, carrier_release). It passes only when its image is at 0x400000, its
 * absolute references (a /fixed image keeps them as linked) read its own data, VirtualQuery calls that image MEM_IMAGE,
 * and every 64 KiB block from the image's end to 0x590000 that VirtualQuery calls free can be reserved there: Wine's
 * map of the carrier's old range agrees with the kernel's (blocks in use, Wine's own heaps on Linux, are counted). tools/run-console.py reads the last line. */
#include <windows.h>

static const char want[] = "fixed";
static const char *const absolute = want;   /* an absolute address in .rdata, never relocated */
static HANDLE out;

static void put(const char *text)
{
    DWORD written;
    WriteFile(out, text, (DWORD)lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void put_number(const char *label, unsigned value)
{
    char text[64], digits[12];
    int n = lstrlenA(lstrcpyA(text, label)), d = 0;
    do { digits[d++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (d) text[n++] = digits[--d];
    text[n++] = ' ';
    text[n] = 0;
    put(text);
}

int mainCRTStartup(void)
{
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    MEMORY_BASIC_INFORMATION image = {0};
    const int at_base = GetModuleHandleA(NULL) == (HMODULE)0x400000;
    const int reads = absolute[0] == 'f' && absolute[4] == 'd';
    const int mapped = VirtualQuery((void *)0x400000, &image, sizeof(image)) && image.Type == MEM_IMAGE &&
                       image.AllocationBase == (void *)0x400000;
    unsigned free_blocks = 0, reserved = 0, conflicts = 0, in_use = 0;
    ULONG_PTR address = ((ULONG_PTR)image.BaseAddress + image.RegionSize + 0xffff) & ~(ULONG_PTR)0xffff;
    for (; mapped && address < 0x590000; address += 0x10000) {
        MEMORY_BASIC_INFORMATION block = {0};
        if (!VirtualQuery((void *)address, &block, sizeof(block))) { conflicts++; continue; }
        if (block.State != MEM_FREE) { in_use++; continue; }
        free_blocks++;
        void *at = VirtualAlloc((void *)address, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
        if (at == (void *)address) reserved++;
        else conflicts++;
        if (at) VirtualFree(at, 0, MEM_RELEASE);
    }
    put_number("PS5_PROTON_FIXED_BASE32 free_blocks=", free_blocks);
    put_number("reserved=", reserved);
    put_number("conflicts=", conflicts);
    put_number("in_use=", in_use);
    put("\r\n");
    const int pass = at_base && reads && mapped && !conflicts;
    put(pass ? "PS5_PROTON_FIXED_BASE32 PASS: loaded at 0x400000, the carrier's old range reservable\r\n"
             : "PS5_PROTON_FIXED_BASE32 FAIL: not at 0x400000, or the range is not what VirtualQuery says\r\n");
    ExitProcess(pass ? 0 : 1);
}
