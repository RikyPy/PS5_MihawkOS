/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_PAGES_H
#define PW_WINE_PAGES_H
#include <stddef.h>
#include <stdint.h>

/* The shared pages a helper maps itself (window pages, the frame page, the audio ring, the input page) go at
 * addresses the helper owns, inside one range it reserves before the program runs, never where the kernel chooses.
 *
 * Measured: the console puts a mapping that names no address in 0x2_0000_0000-0x2_FFFF_FFFF. The earlier emulator experiment (removed) reserved every
 * free address of its guest bands, and then a helper's mmap(NULL) of a physical shared object came back outside that
 * band (fb02920000, fb03420000 in released address-pool chunks; 0x10001830000 above the user limit) with success
 * and nothing mapped: the first write faulted inside a syscall and left Wine's USER lock held (runs of 2026-10-14,
 * record removed from the tree; in git history). 22 of 22 pages of other programs landed in the band and worked; 41 of 41 of
 * the emulator's did not. A fixed mapping over the process's own reservation is what the GPU client does and works.
 *
 * Pages are handed out first-fit in 16 KiB units; a freed range gets a reservation laid back over it, so the range
 * never offers the kernel a hole. A range that could not be laid back stays taken (lost): something may have landed in
 * it, and a fixed mapping never goes there again. Without a range (a host build, the title) mmap(NULL) is used as
 * before; a helper whose range could not be reserved (pw_wine_pages_no_range) maps nothing. The kernel is reached only
 * through PwWinePagesOps, so the policy is host-tested. */

enum {
    PW_WINE_PAGES_PAGE = 16384,
    PW_WINE_PAGES_MAX_BYTES = (size_t)128 << 20,  /* 64 MiB of live window pages, the 8 MiB frame page, the rings */
};

typedef struct PwWinePagesOps {
    void *context;
    /* Map bytes of fd at exactly address, over the range's reservation. 0, or nonzero (the range is then laid back). */
    int (*map)(void *context, uintptr_t address, size_t bytes, int protection, int fd);
    /* Put the reservation back over [address, +bytes). 0, or nonzero when it is not there (the range is lost). */
    int (*lay_back)(void *context, uintptr_t address, size_t bytes);
} PwWinePagesOps;

/* Own [base, +bytes), already reserved by the caller. 0, or -1 (bad range, or one is in use already). */
int pw_wine_pages_init(const PwWinePagesOps *ops, uintptr_t base, size_t bytes);
/* bytes of fd, shared, with this protection: an address inside the range, or (no range) where mmap puts it.
 * MAP_FAILED with errno (ENOSPC when the range is full, or a helper has none). */
void *pw_wine_pages_map(size_t bytes, int protection, int fd);
/* What pw_wine_pages_map returned. */
void pw_wine_pages_unmap(void *address, size_t bytes);
/* A free run of bytes inside the range, taken and left reserved for the caller's own fixed mapping there (an alias of
 * direct memory, pw_wine_alias.h): its address, or 0 (no range, none free). Nothing is mapped and the kernel is not
 * asked; the caller maps over the reservation and gives the run back with pw_wine_pages_give. */
uintptr_t pw_wine_pages_take(size_t bytes);
/* What pw_wine_pages_take returned, with whatever the caller mapped there: the reservation is laid back over it, as
 * pw_wine_pages_unmap does. A run that cannot be laid back stays taken (lost); an address outside the range is
 * ignored. */
void pw_wine_pages_give(uintptr_t address, size_t bytes);
/* The range, for the log: 0 when there is none. */
uintptr_t pw_wine_pages_base(size_t *bytes);
/* A helper's range could not be reserved: no shared page is mapped where the kernel would put it (a window page
 * mapped that way came back with nothing behind it, 2026-10-14). */
void pw_wine_pages_no_range(void);
/* Back to no range (tests). */
void pw_wine_pages_reset(void);

#endif
