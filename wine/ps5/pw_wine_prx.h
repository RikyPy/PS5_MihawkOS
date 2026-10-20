/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PRX_H
#define PW_WINE_PRX_H
#include <stddef.h>
#include <stdint.h>

/* PRX export descriptors for Wine's Unix modules on PS5. On firmware 12.02
 * sceKernelDlsym does not resolve an application PRX's exports and
 * module_start is not run, so each module carries a static, relocated table
 * of {name, address} pairs that the loader finds by scanning the module's
 * segments. The layout is the PRXDESC1 format the console already accepts
 * for application modules: "PRXDESC1", version 1, a count, then the pairs,
 * 16-byte aligned. Everything here is pure and host-testable. */
#define PW_PRX_MAGIC UINT64_C(0x3143534544585250) /* "PRXDESC1" little-endian */
enum {
    PW_PRX_VERSION=1,PW_PRX_ALIGN=16,PW_PRX_MAX_EXPORTS=4096,PW_PRX_MAX_SEGMENTS=4,
    PW_PRX_MAX_NAME=256,PW_PRX_MODULE_INFO_BYTES=0x160,
    PW_PRX_OK=0,PW_PRX_ERR_ARGUMENT=-1,PW_PRX_ERR_MODULE_INFO=-2,PW_PRX_ERR_NO_DESCRIPTOR=-3
};
typedef struct PwPrxExport { const char *name; const void *address; } PwPrxExport;
typedef struct PwPrxDescriptor {
    uint64_t magic;uint32_t version,count;
    PwPrxExport exports[];
} PwPrxDescriptor;
typedef struct PwPrxSegment { const void *address;uint32_t size,protection; } PwPrxSegment;

/* Parses a SceKernelModuleInfo record: 64-bit size at 0 (0x160 as the
 * caller set it, or 0: the console clears it on success), name at 8,
 * up to four {address, 32-bit size, 32-bit protection} segments at 0x108 and
 * their count at 0x148. */
int pw_prx_parse_module_info(const void *info,char name[PW_PRX_MAX_NAME],
                             PwPrxSegment segments[PW_PRX_MAX_SEGMENTS],uint32_t *count);
/* 1 for a segment a payload's loader wires (native/proton_payload_child.c): every one it cannot write. Text and read-only
 * data are paged in from the fake-signed SELF, which a payload cannot authenticate again once the kernel evicted a page
 * (klog "pager error"; the helper died 17-19 s into a .NET game, 2026-10-06 (record removed from the tree; in git history), and ran 204 s with
 * every segment wired, -03); writable data and bss are ordinary memory once relocated. */
static inline int pw_prx_wired(uint32_t protection) { return !(protection&2u); }
/* Before the module is unloaded, unlock (munlock) takes every segment of its info record the loader wired, so the
 * kernel never tears down wired memory ("unwire timeout"). How many unlock refused, or -1 when the record does not
 * parse. */
int pw_prx_unwire(const void *info,int (*unlock)(const void *,size_t));
/* websrv's elfldr starts every payload inside Sony's carrier executable (NPXS40112's eboot.bin), which the kernel places
 * at 0x400000 and whose code never runs: elfldr takes the process over at exec. A 32-bit program linked /fixed there
 * (an old installer) can load nowhere else, so the payload loader gives the image back (native/proton_payload_child.c).
 * When this info record's lowest segment starts at base and every segment ends at or below 4 GiB, each is unmapped
 * through unmap on 16 KiB pages and *end is the image's end; carrier-image-2026-10-19-03 measured four segments,
 * 0x400000-0x584000, after which threads, signals and ntdll.prx's load were unchanged. How many unmap refused, or -1
 * for any other module or an unreadable record (nothing unmapped). */
int pw_prx_release_low_image(const void *info,uintptr_t base,int (*unmap)(void *,size_t),uintptr_t *end);
/* Finds the first valid descriptor in the readable segments. A candidate is
 * valid only if the whole table, every name (terminated within 256 bytes)
 * and every address lie inside the module's own segments. */
int pw_prx_find_descriptor(const PwPrxSegment *segments,uint32_t count,
                           const PwPrxDescriptor **descriptor);
const void *pw_prx_lookup(const PwPrxDescriptor *descriptor,const char *name);

#endif
