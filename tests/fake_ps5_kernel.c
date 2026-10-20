/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The model behind tests/fake_ps5_kernel.h. A sorted table of entries, each backed by the host's memory at the same
 * address; everything here calls the host through __real_*, never the wrapped names. Not thread-safe: the replays make
 * their calls from one thread. */
#define _GNU_SOURCE
#include "fake_ps5_kernel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void *__real_mmap(void *, size_t, int, int, int, off_t);
int __real_mprotect(void *, size_t, int);
int __real_open(const char *, int, ...);
ssize_t __real_pread(int, void *, size_t, off_t);
ssize_t __real_pwrite(int, const void *, size_t, off_t);

enum { ENTRIES = 16384, CALLS = 16384, KLOG = 1 << 16, REPORT = 1 << 14, OBJECTS = 256 };
#define DMEM_BYTES ((size_t)64 << 30)
#define DMEM_PAGES (DMEM_BYTES / FK_PAGE)
#define GIB ((size_t)1 << 30)
enum { E_INVAL = (int)0x80020016, E_NOMEM = (int)0x8002000c, E_ACCES = (int)0x8002000d };

static FkEntry table[ENTRIES];
static unsigned count;
static char maker[16] = "wine";
static unsigned hazards;
static char report[REPORT];
static size_t report_used;
static char klog[KLOG];
static size_t klog_used;
static uintptr_t placement_floor = 0x200000000;
static int lay_ignored, phantom, refuse_cpu_once, gpu_started, window_ready, into_reservations;
static size_t budget, charged;
static int dmem_fd = -1;
static uint8_t *dmem_taken, *dmem_granted;   /* one bit per 16 KiB page */
static size_t dmem_cursor;
static void (*hooks[FK_OP_COUNT])(void);
static int fails[FK_OP_COUNT];
static struct { int op; uintptr_t at; size_t bytes; } calls[CALLS];
static unsigned call_count;
static struct { dev_t dev; ino_t ino; char maker[16]; } objects[OBJECTS];
static unsigned object_count;
static struct { uintptr_t text, end; } modules[64];

static void hazard(unsigned bit, const char *format, ...)
{
    va_list arguments;
    hazards |= bit;
    if (report_used + 256 >= sizeof(report)) return;
    va_start(arguments, format);
    const int length = vsnprintf(report + report_used, sizeof(report) - report_used - 1, format, arguments);
    va_end(arguments);
    if (length > 0) report_used += (size_t)length < sizeof(report) - report_used - 2 ? (size_t)length
                                                                                   : sizeof(report) - report_used - 2;
    report[report_used++] = '\n';
    report[report_used] = 0;
}

static void note_call(int op, uintptr_t at, size_t bytes)
{
    if (call_count < CALLS) calls[call_count] = (typeof(calls[0])){ op, at, bytes };
    call_count++;
}

/* Runs op's hook; 1 when the call must fail (fk_fail_next). */
static int fire(int op)
{
    void (*hook)(void) = hooks[op];
    char saved[16];

    if (hook) {
        hooks[op] = NULL;
        memcpy(saved, maker, sizeof(saved));
        fk_as("intruder");
        hook();
        memcpy(maker, saved, sizeof(saved));
    }
    if (!fails[op]) return 0;
    fails[op] = 0;
    return 1;
}

static uintptr_t round_up(uintptr_t value, size_t align) { return (value + align - 1) & ~(uintptr_t)(align - 1); }
static int aligned(uintptr_t value) { return !(value % FK_PAGE); }
static int inside(uintptr_t at, size_t bytes) { return at >= FK_LOW && bytes <= FK_HIGH - at; }
static int offset_kind(int kind) { return kind == FK_DIRECT || kind == FK_FILE || kind == FK_PHYSHM || kind == FK_JIT; }

/* The first entry that ends above at. */
static unsigned first_after(uintptr_t at)
{
    unsigned low = 0, high = count;
    while (low < high) {
        const unsigned mid = (low + high) / 2;
        if (table[mid].end <= at) low = mid + 1; else high = mid;
    }
    return low;
}

static void insert_at(unsigned index, const FkEntry *entry)
{
    if (count == ENTRIES) { fprintf(stderr, "fake kernel: table full\n"); abort(); }
    memmove(&table[index + 1], &table[index], (count - index) * sizeof(table[0]));
    table[index] = *entry;
    count++;
}

/* No entry straddles at. */
static void split_at(uintptr_t at)
{
    const unsigned i = first_after(at);
    if (i < count && table[i].start < at) {
        FkEntry right = table[i];
        if (offset_kind(right.kind)) right.offset += (int64_t)(at - table[i].start);
        right.start = at;
        table[i].end = at;
        insert_at(i + 1, &right);
    }
}

static size_t anon_bytes(uintptr_t low, uintptr_t high)
{
    size_t bytes = 0;
    for (unsigned i = first_after(low); i < count && table[i].start < high; i++)
        if (table[i].kind == FK_ANON) {
            const uintptr_t from = table[i].start > low ? table[i].start : low;
            const uintptr_t to = table[i].end < high ? table[i].end : high;
            bytes += to - from;
        }
    return bytes;
}

/* The host's memory at [at, +bytes) back to the window's: nothing behind it. */
static void host_bare(uintptr_t at, size_t bytes)
{
    if (__real_mmap((void *)at, bytes, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) !=
        (void *)at) {
        perror("fake kernel: host_bare");
        abort();
    }
}

/* [low, high) holds no entry any more; the host's memory there is bare. */
static void remove_range(uintptr_t low, uintptr_t high)
{
    unsigned i;

    split_at(low);
    split_at(high);
    charged -= anon_bytes(low, high);
    i = first_after(low);
    while (i < count && table[i].start < high) {
        memmove(&table[i], &table[i + 1], (count - i - 1) * sizeof(table[0]));
        count--;
    }
    host_bare(low, high - low);
}

static int host_protection(int kind, unsigned protection)
{
    return (int)(protection & (kind == FK_JIT ? 7u : 3u));
}

/* A new entry over [at, +bytes), which holds none now, backed on the host. 0, or the host's errno. */
static int add_entry(int kind, uintptr_t at, size_t bytes, unsigned protection, int fd, int64_t offset, int shared,
                     const char *by)
{
    FkEntry entry = { at, at + bytes, kind, protection, 0, offset, "" };
    int flags = MAP_FIXED | (shared ? MAP_SHARED : MAP_PRIVATE);
    void *mapped;

    if (kind == FK_RESERVED || kind == FK_PRX) {
        flags = MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | (kind == FK_RESERVED ? MAP_NORESERVE : 0);
        fd = -1;
        offset = 0;
    } else if (kind == FK_ANON) {
        flags |= MAP_ANONYMOUS;
        fd = -1;
        offset = 0;
    } else if (kind == FK_DIRECT) {
        fd = dmem_fd;
        flags = MAP_FIXED | MAP_SHARED;
    }
    mapped = __real_mmap((void *)at, bytes, kind == FK_RESERVED ? PROT_NONE : host_protection(kind, protection), flags,
                         fd, (off_t)offset);
    if (mapped != (void *)at) return errno ? errno : ENOMEM;
    snprintf(entry.maker, sizeof(entry.maker), "%s", by);
    insert_at(first_after(at), &entry);
    if (kind == FK_ANON) charged += bytes;
    return 0;
}

/* Judges every entry in [low, high) before op changes it (teardown: it is removed or replaced). */
static void judge(uintptr_t low, uintptr_t high, const char *op, int teardown)
{
    for (unsigned i = first_after(low); i < count && table[i].start < high; i++) {
        const FkEntry *entry = &table[i];
        const int other = entry->maker[0] && strcmp(entry->maker, maker);
        const int reservation = entry->kind == FK_RESERVED;
        if (other && (teardown || !reservation))
            hazard(FK_OWNER, "FK_OWNER: %s by %s over %#lx-%#lx (kind %d, protection %#x) made by %s", op, maker,
                   (unsigned long)entry->start, (unsigned long)entry->end, entry->kind, entry->protection, entry->maker);
        if (teardown && (entry->wired || (other && (entry->protection & 0x30))))
            hazard(FK_UNWIRE, "FK_UNWIRE: %s by %s tears down %#lx-%#lx (protection %#x, wired %u) made by %s", op, maker,
                   (unsigned long)entry->start, (unsigned long)entry->end, entry->protection, entry->wired,
                   entry->maker);
    }
}

static int any_entry(uintptr_t low, uintptr_t high)
{
    const unsigned i = first_after(low);
    return i < count && table[i].start < high;
}

static int fully_mapped(uintptr_t low, uintptr_t high, int reservations_count)
{
    uintptr_t cursor = low;
    for (unsigned i = first_after(low); i < count && table[i].start < high && cursor < high; i++) {
        if (table[i].start > cursor || (!reservations_count && table[i].kind == FK_RESERVED)) return 0;
        cursor = table[i].end;
    }
    return cursor >= high;
}

/* Where a mapping with no fixed address goes: the first fit at or above from and the floor, in free space, and with
 * fk_place_in_reservations in reservations too (make_room then clears them). */
static uintptr_t first_fit(uintptr_t from, size_t bytes, size_t align)
{
    const size_t step = align < FK_PAGE ? FK_PAGE : align;
    uintptr_t at = round_up(from > placement_floor ? from : placement_floor, step);
    unsigned i = first_after(at);

    for (;;) {
        while (into_reservations && i < count && table[i].kind == FK_RESERVED) i++;
        const uintptr_t limit = i < count ? table[i].start : FK_HIGH;
        if (limit >= at && limit - at >= bytes) return at;
        if (i >= count) return 0;
        at = round_up(table[i].end, step);
        i++;
    }
}
static void make_room(uintptr_t at, size_t bytes)
{
    if (into_reservations) remove_range(at, at + bytes);
}

/* What a descriptor maps: JIT memory, physical shared memory, or a file. */
static int fd_kind(int fd)
{
    char path[64], target[128];
    ssize_t length;

    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    length = readlink(path, target, sizeof(target) - 1);
    if (length <= 0) return -1;
    target[length] = 0;
    if (!strncmp(target, "/memfd:pw-jit", 13)) return FK_JIT;
    if (!strncmp(target, "/memfd:pw-physhm", 16)) return FK_PHYSHM;
    return FK_FILE;
}

static void object_maker_check(int fd)
{
    struct stat object;
    if (fstat(fd, &object)) return;
    for (unsigned i = 0; i < object_count; i++)
        if (objects[i].dev == object.st_dev && objects[i].ino == object.st_ino && !strcmp(objects[i].maker, "title") &&
            strcmp(maker, "title"))
            hazard(FK_TITLEMEM, "FK_TITLEMEM: %s maps a physical shared object %s made", maker, objects[i].maker);
}

/* mmap and sceKernelMmap. 0 with *out, or an errno. */
static int map_common(void *address, size_t bytes, int protection, int flags, int fd, off_t offset, void **out)
{
    const int fixed = flags & MAP_FIXED, anonymous = flags & MAP_ANONYMOUS;
    const int kind = anonymous ? FK_ANON : fd_kind(fd);
    uintptr_t at = (uintptr_t)address;
    unsigned kept;
    int status;

    if (!bytes || kind < 0) return kind < 0 ? EBADF : EINVAL;
    bytes = round_up(bytes, FK_PAGE);
    if (fixed) {
        if (!aligned(at)) return EINVAL;
        if (!inside(at, bytes)) return ENOMEM;
        if (fire(FK_OP_MMAP)) return ENOMEM;
        if (kind == FK_JIT) {
            struct stat object;
            if (fstat(fd, &object) || (size_t)object.st_size != bytes) return EINVAL;   /* K4 */
        }
    } else if (!(at = first_fit(inside(at, bytes) ? at : 0, bytes, FK_PAGE))) {
        return ENOMEM;
    }
    note_call(FK_OP_MMAP, at, bytes);
    if (kind == FK_PHYSHM) object_maker_check(fd);
    if (kind == FK_PHYSHM && phantom) {
        *out = (void *)at;
        return 0;
    }
    kept = (unsigned)protection & (kind == FK_JIT ? 7u : 3u);   /* K8 */
    if (kind == FK_ANON && budget && charged - (fixed ? anon_bytes(at, at + bytes) : 0) + bytes > budget) return ENOMEM;
    if (fixed) {
        judge(at, at + bytes, "fixed mmap", 1);
        remove_range(at, at + bytes);
    } else {
        make_room(at, bytes);
    }
    if ((status = add_entry(kind, at, bytes, kept, fd, offset, (flags & MAP_SHARED) != 0, maker))) return status;
    *out = (void *)at;
    return 0;
}

static int unmap_common(uintptr_t at, size_t bytes)
{
    size_t bare = 0;

    if (!aligned(at) || !bytes) return EINVAL;
    bytes = round_up(bytes, FK_PAGE);
    if (!inside(at, bytes)) return EINVAL;
    if (fire(FK_OP_MUNMAP)) return EINVAL;
    note_call(FK_OP_MUNMAP, at, bytes);
    for (unsigned i = first_after(at); i < count && table[i].start < at + bytes; i++)
        if (table[i].kind == FK_RESERVED) {
            const uintptr_t from = table[i].start > at ? table[i].start : at;
            const uintptr_t to = table[i].end < at + bytes ? table[i].end : at + bytes;
            bare += to - from;
        }
    if (gpu_started && bare >= GIB)
        hazard(FK_BIGBARE, "FK_BIGBARE: %s unmaps %#lx +%#zx holding %#zx bytes of bare reservation", maker,
               (unsigned long)at, bytes, bare);
    judge(at, at + bytes, "munmap", 1);
    remove_range(at, at + bytes);
    return 0;
}

static int protect_common(uintptr_t at, size_t bytes, int protection)
{
    const uintptr_t low = at & ~(uintptr_t)(FK_PAGE - 1), high = round_up(at + bytes, FK_PAGE);

    if (!bytes) return 0;
    if (!inside(low, high - low) || !fully_mapped(low, high, 1)) return ENOMEM;
    if (fire(FK_OP_MPROTECT)) return ENOMEM;
    note_call(FK_OP_MPROTECT, low, high - low);
    judge(low, high, "mprotect", 0);
    split_at(low);
    split_at(high);
    for (unsigned i = first_after(low); i < count && table[i].start < high; i++) {
        FkEntry *entry = &table[i];
        if (entry->kind == FK_RESERVED) continue;
        entry->protection = ((unsigned)protection & (entry->kind == FK_JIT ? 7u : 3u)) | (entry->protection & 0x30);
        if (__real_mprotect((void *)entry->start, entry->end - entry->start,
                            host_protection(entry->kind, entry->protection))) {
            perror("fake kernel: mprotect");
            abort();
        }
    }
    return 0;
}

static int sce(int error) { return error ? (int)(0x80020000u | (unsigned)error) : 0; }

/* The window, the table and every switch back to their start. */
void fk_init(void)
{
    if (!window_ready) {
        if (__real_mmap((void *)FK_LOW, FK_HIGH - FK_LOW, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0) != (void *)FK_LOW) {
            fprintf(stderr, "fake kernel: the window %#lx-%#lx is not free in this process\n", (unsigned long)FK_LOW,
                    (unsigned long)FK_HIGH);
            abort();
        }
        window_ready = 1;
    } else {
        host_bare(FK_LOW, FK_HIGH - FK_LOW);
    }
    if (dmem_fd < 0) {
        dmem_fd = memfd_create("fk-dmem", 0);
        dmem_taken = calloc(DMEM_PAGES / 8, 1);
        dmem_granted = calloc(DMEM_PAGES / 8, 1);
        if (dmem_fd < 0 || ftruncate(dmem_fd, (off_t)DMEM_BYTES) || !dmem_taken || !dmem_granted) abort();
    }
    memset(dmem_taken, 0, DMEM_PAGES / 8);
    memset(dmem_granted, 0, DMEM_PAGES / 8);
    dmem_cursor = 0;
    count = call_count = object_count = 0;
    hazards = 0;
    report_used = klog_used = 0;
    report[0] = klog[0] = 0;
    placement_floor = 0x200000000;
    lay_ignored = phantom = refuse_cpu_once = gpu_started = into_reservations = 0;
    budget = charged = 0;
    memset(hooks, 0, sizeof(hooks));
    memset(fails, 0, sizeof(fails));
    memset(modules, 0, sizeof(modules));
    fk_as("wine");
}

void fk_as(const char *who) { snprintf(maker, sizeof(maker), "%s", who); }
void fk_gpu_started(void) { gpu_started = 1; }
void fk_set_floor(uintptr_t floor) { placement_floor = floor; }
void fk_lay_ignored(int on) { lay_ignored = on; }
void fk_phantom_physhm(int on) { phantom = on; }
void fk_place_in_reservations(int on) { into_reservations = on; }
void fk_refuse_cpu_direct_once(void) { refuse_cpu_once = 1; }
void fk_flexible_budget(size_t bytes) { budget = bytes; }
size_t fk_charged(void) { return charged; }
void fk_on_next(int op, void (*fn)(void)) { hooks[op] = fn; }
void fk_fail_next(int op) { fails[op] = 1; }
unsigned fk_hazards(void) { return hazards; }
const char *fk_report(void) { return report; }
const char *fk_klog(void) { return klog; }
void fk_clear_hazards(void) { hazards = 0; report_used = 0; report[0] = 0; }
void fk_clear_calls(void) { call_count = 0; }

static int64_t direct_for(size_t bytes);
void fk_plant(int kind, uintptr_t at, size_t bytes, unsigned protection, const char *by, int wired)
{
    int64_t offset = 0;
    int fd = -1;

    if (kind == FK_DIRECT && (offset = direct_for(bytes)) < 0) abort();
    if (kind == FK_FILE || kind == FK_PHYSHM || kind == FK_JIT) {
        fd = memfd_create(kind == FK_JIT ? "pw-jit" : kind == FK_PHYSHM ? "pw-physhm" : "fk-file", 0);
        if (fd < 0 || ftruncate(fd, (off_t)bytes)) abort();
    }
    remove_range(at, at + bytes);
    if (add_entry(kind, at, bytes, protection, fd, offset, 1, by)) abort();
    if (fd >= 0) close(fd);
    table[first_after(at)].wired = (unsigned)wired;
}

uintptr_t fk_place(int kind, uintptr_t hint, size_t bytes, unsigned protection, const char *by)
{
    const uintptr_t at = first_fit(hint, round_up(bytes, FK_PAGE), FK_PAGE);
    if (at) fk_plant(kind, at, round_up(bytes, FK_PAGE), protection, by, 0);
    return at;
}

/* Direct memory: first fit over the 16 KiB pages from a cursor that moves on, so a release is not handed out again
 * at once. */
static int bit(const uint8_t *map, size_t page) { return map[page / 8] >> (page % 8) & 1; }
static void set_bits(uint8_t *map, size_t first, size_t pages, int on)
{
    for (size_t page = first; page < first + pages; page++)
        if (on) map[page / 8] |= (uint8_t)(1u << (page % 8)); else map[page / 8] &= (uint8_t)~(1u << (page % 8));
}
static int64_t direct_for(size_t bytes)
{
    const size_t pages = round_up(bytes, FK_PAGE) / FK_PAGE;
    for (int pass = 0; pass < 2; pass++)
        for (size_t first = pass ? 0 : dmem_cursor, run = 0, page = first; page < DMEM_PAGES; page++) {
            if (bit(dmem_taken, page)) { run = 0; first = page + 1; continue; }
            if (++run == pages) {
                set_bits(dmem_taken, first, pages, 1);
                dmem_cursor = first + pages;
                return (int64_t)(first * FK_PAGE);
            }
        }
    return -1;
}
int64_t fk_dmem_allocate(size_t bytes) { return direct_for(bytes); }
void fk_grant(int64_t offset, size_t bytes) { set_bits(dmem_granted, (size_t)offset / FK_PAGE, bytes / FK_PAGE, 1); }

uintptr_t fk_load_module(size_t text, size_t rodata, size_t data, unsigned char info[0x160])
{
    const size_t sizes[3] = { round_up(text, FK_PAGE), round_up(rodata, FK_PAGE), round_up(data, FK_PAGE) };
    const uint32_t protections[3] = { 4, 1, 3 }, segments = 3;
    const uintptr_t at = first_fit(0, sizes[0] + sizes[1] + sizes[2], FK_PAGE);
    uint64_t size = 0x160;
    uintptr_t cursor = at;

    if (!at) abort();
    make_room(at, sizes[0] + sizes[1] + sizes[2]);
    memset(info, 0, 0x160);
    memcpy(info, &size, 8);
    strcpy((char *)info + 8, "fake.prx");
    for (unsigned i = 0; i < segments; cursor += sizes[i++]) {
        const uint64_t address = cursor;
        const uint32_t bytes = (uint32_t)sizes[i];
        if (add_entry(FK_PRX, cursor, sizes[i], protections[i], -1, 0, 0, "prx")) abort();
        memcpy(info + 0x108 + i * 16, &address, 8);
        memcpy(info + 0x110 + i * 16, &bytes, 4);
        memcpy(info + 0x114 + i * 16, &protections[i], 4);
    }
    for (unsigned i = 0; i < 64; i++)
        if (!modules[i].text) { modules[i].text = at; modules[i].end = cursor; break; }
    memcpy(info + 0x148, &segments, 4);
    return at;
}

void fk_unload_module(uintptr_t text)
{
    for (unsigned i = 0; i < 64; i++)
        if (modules[i].text == text) {
            char saved[16];
            memcpy(saved, maker, sizeof(saved));
            fk_as("prx");
            judge(modules[i].text, modules[i].end, "module unload", 1);
            memcpy(maker, saved, sizeof(saved));
            remove_range(modules[i].text, modules[i].end);
            modules[i].text = 0;
            return;
        }
    abort();
}

int fk_entry(uintptr_t at, FkEntry *entry)
{
    const unsigned i = first_after(at);
    if (i >= count || table[i].start > at) return 0;
    *entry = table[i];
    return 1;
}

int fk_next(uintptr_t at, FkEntry *entry)
{
    const unsigned i = first_after(at);
    if (i >= count) return 0;
    *entry = table[i];
    return 1;
}

size_t fk_free_bytes(uintptr_t low, uintptr_t high)
{
    size_t bytes = 0;
    uintptr_t cursor = low;
    for (unsigned i = first_after(low); i < count && table[i].start < high; i++) {
        if (table[i].start > cursor) bytes += table[i].start - cursor;
        cursor = table[i].end;
    }
    return cursor < high ? bytes + (high - cursor) : bytes;
}

unsigned fk_calls(int op)
{
    unsigned n = 0;
    for (unsigned i = 0; i < call_count && i < CALLS; i++) n += calls[i].op == op;
    return n;
}

unsigned fk_touched(int op, uintptr_t low, uintptr_t high)
{
    unsigned n = 0;
    for (unsigned i = 0; i < call_count && i < CALLS; i++)
        n += calls[i].op == op && calls[i].at < high && low < calls[i].at + calls[i].bytes;
    return n;
}

size_t fk_largest(int op)
{
    size_t largest = 0;
    for (unsigned i = 0; i < call_count && i < CALLS; i++)
        if (calls[i].op == op && calls[i].bytes > largest) largest = calls[i].bytes;
    return largest;
}

/* The host functions the code under test calls, wrapped. */
void *__wrap_mmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset)
{
    void *out = MAP_FAILED;
    const int error = map_common(address, bytes, protection, flags, fd, offset, &out);
    if (error) { errno = error; return MAP_FAILED; }
    return out;
}
int __wrap_munmap(void *address, size_t bytes)
{
    const int error = unmap_common((uintptr_t)address, bytes);
    if (error) { errno = error; return -1; }
    return 0;
}
int __wrap_mprotect(void *address, size_t bytes, int protection)
{
    const int error = protect_common((uintptr_t)address, bytes, protection);
    if (error) { errno = error; return -1; }
    return 0;
}
static int wire(const void *address, size_t bytes, int delta)
{
    const uintptr_t low = (uintptr_t)address & ~(uintptr_t)(FK_PAGE - 1), high = round_up((uintptr_t)address + bytes, FK_PAGE);
    if (!fully_mapped(low, high, 0)) { errno = ENOMEM; return -1; }
    note_call(FK_OP_MLOCK, low, high - low);
    split_at(low);
    split_at(high);
    for (unsigned i = first_after(low); i < count && table[i].start < high; i++)
        if (delta > 0) table[i].wired++; else if (table[i].wired) table[i].wired--;
    return 0;
}
int __wrap_mlock(const void *address, size_t bytes) { return wire(address, bytes, 1); }
int __wrap_munlock(const void *address, size_t bytes) { return wire(address, bytes, -1); }
int __wrap_mincore(void *address, size_t bytes, unsigned char *vector)
{
    const uintptr_t low = (uintptr_t)address, high = round_up(low + bytes, FK_PAGE);
    if (!aligned(low)) { errno = EINVAL; return -1; }
    if (!fully_mapped(low, high, 0)) { errno = ENOMEM; return -1; }
    memset(vector, 1, (bytes + 4095) / 4096);
    return 0;
}
int __wrap_madvise(void *address, size_t bytes, int advice) { (void)address; (void)bytes; (void)advice; return 0; }
int __wrap_msync(void *address, size_t bytes, int flags) { (void)address; (void)bytes; (void)flags; return 0; }
int __wrap_open(const char *path, int flags, ...)
{
    va_list arguments;
    unsigned mode;
    if (!strncmp(path, "/dev/dmem", 9)) {
        hazard(FK_DMEMDEV, "FK_DMEMDEV: %s opens %s", maker, path);
        errno = EACCES;
        return -1;
    }
    va_start(arguments, flags);
    mode = va_arg(arguments, unsigned);
    va_end(arguments);
    return __real_open(path, flags, mode);
}
pid_t __wrap_fork(void)
{
    hazard(FK_DMEMDEV, "FK_DMEMDEV: %s forks", maker);
    errno = ENOSYS;
    return -1;
}
ssize_t __wrap_pread(int fd, void *buffer, size_t bytes, off_t offset)
{
    if (fd_kind(fd) == FK_JIT) { errno = ESPIPE; return -1; }   /* K5 */
    return __real_pread(fd, buffer, bytes, offset);
}
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t bytes, off_t offset)
{
    if (fd_kind(fd) == FK_JIT) { errno = ESPIPE; return -1; }
    return __real_pwrite(fd, buffer, bytes, offset);
}

/* libkernel. */
int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment)
{
    uintptr_t at = (uintptr_t)*address;
    const int fixed = flags & 0x10, no_overwrite = flags & 0x80;

    if (!bytes || bytes % FK_PAGE || !aligned(at)) return E_INVAL;
    if (fixed) {
        if (!inside(at, bytes)) return E_NOMEM;
        if (fire(no_overwrite ? FK_OP_RESERVE_NOOVERWRITE : FK_OP_RESERVE_FIXED)) return E_NOMEM;
        note_call(no_overwrite ? FK_OP_RESERVE_NOOVERWRITE : FK_OP_RESERVE_FIXED, at, bytes);
        if (no_overwrite && any_entry(at, at + bytes)) return E_NOMEM;
        if (lay_ignored && !no_overwrite && any_entry(at, at + bytes)) return 0;
        const int laid = !no_overwrite && any_entry(at, at + bytes);
        judge(at, at + bytes, "fixed reservation", 1);
        remove_range(at, at + bytes);
        return add_entry(FK_RESERVED, at, bytes, 0, -1, 0, 0, laid ? "" : maker) ? E_NOMEM : 0;
    }
    if (at && !inside(at, bytes)) return E_NOMEM;   /* a raw console hint the window does not hold */
    if (fire(FK_OP_RESERVE) || !(at = first_fit(at, bytes, alignment))) return E_NOMEM;
    note_call(FK_OP_RESERVE, at, bytes);
    make_room(at, bytes);
    if (add_entry(FK_RESERVED, at, bytes, 0, -1, 0, 0, maker)) return E_NOMEM;
    *address = (void *)at;
    return 0;
}

int sceKernelMapDirectMemory(void **address, size_t bytes, int protection, int flags, int64_t offset, size_t alignment)
{
    uintptr_t at = (uintptr_t)*address;
    int status;

    if ((protection & 4) || !bytes || bytes % FK_PAGE || offset % FK_PAGE || !aligned(at) || offset < 0 ||
        (size_t)offset + bytes > DMEM_BYTES)
        return E_INVAL;
    for (size_t page = (size_t)offset / FK_PAGE; page < ((size_t)offset + bytes) / FK_PAGE; page++)
        if (!bit(dmem_taken, page) && !bit(dmem_granted, page)) return E_ACCES;
    if (refuse_cpu_once && !(protection & 0x30)) {
        refuse_cpu_once = 0;
        return E_NOMEM;
    }
    if (flags & 0x10) {
        if (!inside(at, bytes)) return E_NOMEM;
        if (fire(FK_OP_MAP_DIRECT)) return E_NOMEM;
        judge(at, at + bytes, "fixed direct mapping", 1);
        remove_range(at, at + bytes);
    } else if (!(at = first_fit(inside(at, bytes) ? at : 0, bytes, alignment))) {
        return E_NOMEM;
    } else {
        make_room(at, bytes);
    }
    note_call(FK_OP_MAP_DIRECT, at, bytes);
    if ((status = add_entry(FK_DIRECT, at, bytes, (unsigned)protection & 0x33, -1, offset, 1, maker))) return sce(status);
    *address = (void *)at;
    return 0;
}

int sceKernelAllocateMainDirectMemory(size_t bytes, size_t alignment, int type, int64_t *offset)
{
    (void)alignment; (void)type;
    return (*offset = direct_for(bytes)) < 0 ? E_NOMEM : 0;
}

/* Released direct memory is unmapped wherever it is mapped (the engine's adopt alone left holes, 2026-10-14; record
 * removed from the tree). */
int sceKernelReleaseDirectMemory(int64_t offset, size_t bytes)
{
    const size_t first = (size_t)offset / FK_PAGE, pages = bytes / FK_PAGE;

    if (offset % FK_PAGE || bytes % FK_PAGE || (size_t)offset + bytes > DMEM_BYTES) return E_INVAL;
    for (size_t page = first; page < first + pages; page++)
        if (!bit(dmem_taken, page)) return E_INVAL;
    note_call(FK_OP_RELEASE_DIRECT, (uintptr_t)offset, bytes);
    set_bits(dmem_taken, first, pages, 0);
    for (unsigned i = 0; i < count;) {
        const FkEntry entry = table[i];
        const int64_t low = entry.offset > offset ? entry.offset : offset;
        const int64_t high = entry.offset + (int64_t)(entry.end - entry.start) < offset + (int64_t)bytes
                                 ? entry.offset + (int64_t)(entry.end - entry.start) : offset + (int64_t)bytes;
        if (entry.kind != FK_DIRECT || low >= high) { i++; continue; }
        hazard(FK_MAPPED, "FK_MAPPED: %s releases direct memory %#lx +%#zx still mapped at %#lx-%#lx (made by %s)",
               maker, (unsigned long)offset, bytes, (unsigned long)(entry.start + (uintptr_t)(low - entry.offset)),
               (unsigned long)(entry.start + (uintptr_t)(high - entry.offset)), entry.maker);
        remove_range(entry.start + (uintptr_t)(low - entry.offset), entry.start + (uintptr_t)(high - entry.offset));
        i = 0;   /* the table changed: start over (released ranges are few) */
    }
    return 0;
}

int sceKernelMprotect(const void *address, size_t bytes, int protection)
{
    return sce(protect_common((uintptr_t)address, bytes, protection));
}
int sceKernelMunmap(void *address, size_t bytes) { return sce(unmap_common((uintptr_t)address, bytes)); }

int sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection)
{
    FkEntry entry;
    if (!fk_entry((uintptr_t)address, &entry)) return E_INVAL;
    *start = (void *)entry.start;
    *end = (void *)entry.end;
    *protection = entry.kind == FK_PHYSHM ? 0 : entry.protection;
    return 0;
}

/* NEXT: the entry holding address or the next one; EACCES past the last. Record: start, end, offset, protection at 24,
 * type at 28, flags at 32, name at 33. */
int sceKernelVirtualQuery(const void *address, int flags, void *record, size_t bytes)
{
    const unsigned i = first_after((uintptr_t)address);
    const FkEntry *entry = &table[i];
    int32_t protection, type;
    uint8_t kind_flags;

    if (flags != 1 || bytes != 72) return E_INVAL;
    if (i >= count) return E_ACCES;
    /* Physical shared memory reads as the reservation under it; so does (not measured) anonymous PROT_NONE. */
    const int bare = entry->kind == FK_RESERVED || entry->kind == FK_PHYSHM ||
                     (entry->kind == FK_ANON && !entry->protection);
    protection = bare ? 0 : (int32_t)entry->protection;
    type = bare ? 0 : entry->kind;
    kind_flags = bare ? 0 : (uint8_t)(0x10 | entry->kind);
    memset(record, 0, bytes);
    memcpy(record, &entry->start, 8);
    memcpy((char *)record + 8, &entry->end, 8);
    memcpy((char *)record + 16, &entry->offset, 8);
    memcpy((char *)record + 24, &protection, 4);
    memcpy((char *)record + 28, &type, 4);
    memcpy((char *)record + 32, &kind_flags, 1);
    snprintf((char *)record + 33, 31, "fk:%s", bare ? "" : entry->maker);
    return 0;
}

int sceKernelJitCreateSharedMemory(const char *name, size_t bytes, int max_protection, int *fd)
{
    (void)name; (void)max_protection;
    *fd = memfd_create("pw-jit", 0);
    if (*fd < 0 || ftruncate(*fd, (off_t)bytes)) return E_NOMEM;
    return 0;
}
int sceKernelMmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset, void **out)
{
    return sce(map_common(address, bytes, protection, flags, fd, offset, out));
}
int sceKernelDebugOutText(int channel, const char *text)
{
    const size_t length = strlen(text);
    (void)channel;
    if (klog_used + length + 1 < sizeof(klog)) {
        memcpy(klog + klog_used, text, length + 1);
        klog_used += length;
    }
    return 0;
}
int sceKernelUsleep(unsigned microseconds) { return usleep(microseconds); }
/* The payload SDK's grant of execute: the unlocked write of the kernel's map entry. */
int kernel_mprotect(pid_t pid, intptr_t address, size_t bytes, int protection)
{
    (void)pid;
    hazard(FK_SDKGRANT, "FK_SDKGRANT: %s asks the SDK for protection %#x at %#lx +%#zx", maker, protection,
           (unsigned long)address, bytes);
    return 0;
}
int kernel_set_vmem_protection(pid_t pid, intptr_t address, size_t bytes, int protection)
{
    return kernel_mprotect(pid, address, bytes, protection);
}
/* libkernel_sys's physical shared memory (native/pw_physical_shared.h): an object remembered with who made it. */
int physhm_open(const char *name, int flags, unsigned mode, int type)
{
    struct stat object;
    const int fd = memfd_create("pw-physhm", 0);
    (void)name; (void)flags; (void)mode; (void)type;
    if (fd >= 0 && !fstat(fd, &object) && object_count < OBJECTS) {
        objects[object_count].dev = object.st_dev;
        objects[object_count].ino = object.st_ino;
        snprintf(objects[object_count++].maker, sizeof(objects[0].maker), "%s", maker);
    }
    return fd;
}
int physhm_unlink(const char *name) { (void)name; return 0; }
