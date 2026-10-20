/* SPDX-License-Identifier: GPL-3.0-or-later
 * Qualify the ordinary descriptor contract of physical shared objects before
 * replacing any Wine backing. Firmware 10.01 AudioOut's known four-argument
 * physhm_open type 0x0c; no budget changes, kernel inspection or foreign mappings.
 * At most 2 MiB, one process, mappings released before resize. The Linux build
 * uses POSIX shm as a reference for the same operations. Protection checks only
 * transition/read/restore; they do not claim a forbidden write was faulted.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef PW_PHYSICAL_SHARED_PS5
extern int physhm_open(const char *, int, unsigned, int);
extern int physhm_unlink(const char *);
extern int sceKernelAvailableFlexibleMemorySize(size_t *);
extern int sceKernelGetResidentFmemCount(int, uint64_t *, uint64_t *);
#endif

enum { PAGE = 16384, INITIAL = 1024 * 1024, GROWN = 2 * INITIAL };
static unsigned checks, failures;
static void check(int okay, const char *name)
{
    ++checks;
    if (!okay) ++failures;
    printf("physical-contract check=%s result=%s errno=%d\n", name, okay ? "PASS" : "FAIL", okay ? 0 : errno);
    fflush(stdout);
}
static void census(const char *phase)
{
#ifdef PW_PHYSICAL_SHARED_PS5
    uint64_t resident = 0, mapped = 0;
    size_t flexible = 0;
    int f = sceKernelAvailableFlexibleMemorySize(&flexible);
    int r = sceKernelGetResidentFmemCount(getpid(), &resident, &mapped);
    printf("physical-contract phase=%s flexible_status=%#x available=%zu resident_status=%#x pages=%llu\n",
           phase, f, flexible, r, (unsigned long long)resident);
    fflush(stdout);
#else
    printf("physical-contract phase=%s oracle=posix-shm\n", phase);
#endif
}
static int object_open(const char *name, int flags)
{
#ifdef PW_PHYSICAL_SHARED_PS5
    return physhm_open(name, flags, 0600, 0x0c);
#else
    return shm_open(name, flags, 0600);
#endif
}
static int object_unlink(const char *name)
{
#ifdef PW_PHYSICAL_SHARED_PS5
    return physhm_unlink(name);
#else
    return shm_unlink(name);
#endif
}
static int zero(const unsigned char *map, size_t from, size_t to)
{
    for (size_t i = from; i < to; ++i) if (map[i]) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *tag = NULL;
    for (int i = 0; i < argc; ++i)
        if (!strncmp(argv[i], "ps5phys-", 8)) tag = argv[i];
    if (!tag || strlen(tag) > 40 || strspn(tag, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(tag)) return 2;
    char name[64]; snprintf(name, sizeof(name), "/%s", tag);
    int fd = -1, owned = 0;
    unsigned char *map = MAP_FAILED, *alias = MAP_FAILED;
    size_t mapped = 0;
    struct stat st;
    const uint32_t marker = 0x1250abcd, changed = 0x6250abcd;
    uint32_t got = 0;
    census("baseline");
    fd = object_open(name, O_RDWR | O_CREAT | O_EXCL);
    check(fd >= 0, "create"); if (fd < 0) goto done;
    owned = 1;
    /* No persistent named object if a later unsupported mapping terminates the
     * probe. The fd retains ownership; native process teardown frees its maps. */
    int unlinked = !object_unlink(name);
    check(unlinked, "unlink-live"); if (!unlinked) goto done;
    owned = 0;
    int sized = !ftruncate(fd, INITIAL);
    check(sized, "size-initial"); if (!sized) goto done;
    check(!fstat(fd, &st) && st.st_size == INITIAL, "stat-size");
    map = mmap(NULL, INITIAL, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "map-shared"); if (map == MAP_FAILED) goto done;
    mapped = INITIAL;
    check(zero(map, 0, INITIAL), "initial-zero-all-bytes");
    ssize_t put = pwrite(fd, &marker, sizeof(marker), 27);
    check(put == sizeof(marker), "pwrite-unaligned");
    memcpy(&got, map + 27, sizeof(got));
    check(put == sizeof(marker) && got == marker, "pwrite-visible-in-map");
    memcpy(map + 27, &changed, sizeof(changed));
    ssize_t read_count = pread(fd, &got, sizeof(got), 27);
    check(read_count == sizeof(got) && got == changed, "mapped-write-visible-in-pread");
    check(lseek(fd, 27, SEEK_SET) == 27, "seek");
    read_count = read(fd, &got, sizeof(got));
    check(read_count == sizeof(got) && got == changed, "sequential-read");
    check(!fsync(fd), "fsync");
    check(!msync(map, PAGE, MS_SYNC), "msync");
    int locked = !flock(fd, LOCK_SH | LOCK_NB);
    check(locked, "flock-shared"); if (locked) check(!flock(fd, LOCK_UN), "flock-unlock");
    alias = mmap(NULL, INITIAL, PROT_READ, MAP_SHARED, fd, 0);
    check(alias != MAP_FAILED, "read-only-alias");
    if (alias != MAP_FAILED) {
        memcpy(map + 27, &marker, sizeof(marker));
        memcpy(&got, alias + 27, sizeof(got));
        check(got == marker, "read-only-alias-sees-write");
        check(!munmap(alias, INITIAL), "unmap-read-only"); alias = MAP_FAILED;
    }
    check(!mprotect(map, PAGE, PROT_READ), "protect-read-only");
    memcpy(&got, map + 27, sizeof(got)); check(got == marker, "protected-read");
    int restored = !mprotect(map, PAGE, PROT_READ | PROT_WRITE);
    check(restored, "restore-read-write");
    alias = mmap(NULL, INITIAL, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(alias != MAP_FAILED, "map-private");
    if (alias != MAP_FAILED) {
        memcpy(alias + 27, &changed, sizeof(changed));
        memcpy(&got, map + 27, sizeof(got)); check(got == marker, "private-write-isolated");
        check(!munmap(alias, INITIAL), "unmap-private"); alias = MAP_FAILED;
    }
    census("initial-held");
    if (munmap(map, mapped)) { check(0, "unmap-before-grow"); goto done; }
    map = MAP_FAILED; mapped = 0;
    int grown = !ftruncate(fd, GROWN); check(grown, "grow"); if (!grown) goto done;
    map = mmap(NULL, GROWN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "map-grown"); if (map == MAP_FAILED) goto done;
    mapped = GROWN;
    memcpy(&got, map + 27, sizeof(got)); check(got == marker, "grow-retains-data");
    check(zero(map, INITIAL, GROWN), "grown-tail-zero");
    map[INITIAL + 17] = 0x62;
    census("grown-held");
    if (munmap(map, mapped)) { check(0, "unmap-before-shrink"); goto done; }
    map = MAP_FAILED; mapped = 0;
    int shrunk = !ftruncate(fd, INITIAL); check(shrunk, "shrink"); if (!shrunk) goto done;
    check(!fstat(fd, &st) && st.st_size == INITIAL, "shrunk-stat-size");
    grown = !ftruncate(fd, GROWN); check(grown, "regrow"); if (!grown) goto done;
    map = mmap(NULL, GROWN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "map-regrown"); if (map == MAP_FAILED) goto done;
    mapped = GROWN;
    check(zero(map, INITIAL, GROWN), "regrown-tail-zero");
    int missing = object_open(name, O_RDWR);
    check(missing < 0 && errno == ENOENT, "unlinked-name-absent");
    if (missing >= 0) close(missing);
    check(!close(fd), "close-live-fd"); fd = -1;
    memcpy(&got, map + 27, sizeof(got)); check(got == marker, "map-survives-last-fd");
done:
    if (map != MAP_FAILED) check(!munmap(map, mapped), "unmap-final");
    if (fd >= 0) check(!close(fd), "close-final");
    if (owned) check(!object_unlink(name), "unlink-final");
    census("released");
    printf("PS5_PHYSICAL_CONTRACT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
