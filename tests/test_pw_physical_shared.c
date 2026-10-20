/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../native/pw_physical_shared.h"
#include <assert.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
/* POSIX oracle for the allocator's name/ownership/alignment contract, not a
 * claim about console backing. Physical syscalls are measured separately. */
static char last_name[64];
static int refuse;
int physhm_open(const char *name, int flags, unsigned mode, int type)
{
    assert(type == 0x0c && mode == 0600);
    assert(flags == (O_CREAT | O_EXCL | O_RDWR));
    if (refuse) { errno = ENOSYS; return -1; }
    strcpy(last_name, name);
    return shm_open(name, flags, mode);
}
int physhm_unlink(const char *name) { return shm_unlink(name); }
#ifndef PW_PHYSICAL_SHARED_ORACLE_ONLY
int main(void)
{
    assert(pw_physical_shared_create(0) == -1 && errno == EINVAL);
    assert(pw_physical_shared_create(INT64_MAX) == -1 && errno == EINVAL);
    refuse = 1;
    assert(pw_physical_shared_create(1) == -1 && errno == ENOSYS);
    refuse = 0;
    int fd = pw_physical_shared_create(16385);
    assert(fd >= 0);
    struct stat st; assert(!fstat(fd, &st) && st.st_size == 32768);
    assert(shm_open(last_name, O_RDWR, 0600) == -1 && errno == ENOENT);
    unsigned char *map = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(map != MAP_FAILED);
    for (unsigned i = 0; i < 32768; ++i) assert(map[i] == 0);
    map[16385] = 0x51;
    assert(!close(fd));
    assert(map[16385] == 0x51);
    assert(!munmap(map, 32768));
    puts("pw_physical_shared: ok");
    return 0;
}
#endif
