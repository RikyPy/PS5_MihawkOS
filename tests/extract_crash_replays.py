# SPDX-License-Identifier: GPL-3.0-or-later
# Pull the virtual.c code the crash replays run out of the applied tree (tests/test_crash_replays.c), in source order,
# so the replays test the patched code itself, not a copy: Wine's host-protection cache and counted calls, its page
# table and host-page protection (patch 1122), set_vprot and patch 1128's guard, the address pool's give-back,
# NtLockVirtualMemory, the free-range list (patch 1119), the execute fault hook (patch 1127), the placed range's
# hand-over (patch 1130) and the 4 KiB MMU's classify (patch 1134); and the GPU broker owner's shared_object (graphics patch 0012), which is C as written.
#   python3 tests/extract_crash_replays.py <applied virtual.c> <output .inc> <applied server.cpp> <output .inc>
import sys
src = open(sys.argv[1]).read()


def function(signature):
    i = src.index(signature)
    return src[i:src.index('\n}\n', i) + 3]


def between(start, end):
    i = src.index(start)
    return src[i:src.index(end, i)]


known = src.index('enum { HOST_PROT_LEAF_SHIFT')
branch = src.index('    if (alloc_type & MEM_REPLACE_PLACEHOLDER)\n    {', src.index('static NTSTATUS map_view( struct file_view **view_ret'))
branch_end = src.index('        return STATUS_SUCCESS;\n    }\n', branch) + len('        return STATUS_SUCCESS;\n    }\n')
hook = src.index('#define PS5_EXEC_CLUSTER')
out = [
    src[known:src.index('static size_t host_prot_page_size(void)', known)],
    function('static size_t host_prot_page_size(void)'),
    function('static unsigned char *host_prot_entry( size_t host_page, int create )'),
    function('static void host_prot_record( const void *addr, size_t len, unsigned char value )'),
    function('static BOOL host_prot_unchanged( const void *addr, size_t len, int prot )'),
    function('static inline void *counted_mmap( void *addr, size_t len, int prot, int flags, int fd, off_t off )'),
    function('static inline int counted_munmap( void *addr, size_t len )'),
    function('static inline int counted_mprotect( void *addr, size_t len, int prot )'),
    '#define mmap counted_mmap\n#define munmap counted_munmap\n#define mprotect counted_mprotect\n',
    function('static BYTE get_page_vprot( const void *addr )'),
    function('static BYTE get_host_page_vprot( const void *addr )'),
    function('static SIZE_T get_vprot_range_size( char *base, SIZE_T size, BYTE mask, BYTE *vprot )'),
    function('static void set_page_vprot( const void *addr, size_t size, BYTE vprot )'),
    function('static void set_page_vprot_bits( const void *addr, size_t size, BYTE set, BYTE clear )'),
    function('static int get_unix_prot( BYTE vprot )'),
    function('static inline int mprotect_exec( void *base, size_t size, int unix_prot )'),
    function('static int get_host_page_unix_prot( const void *addr, BYTE set, BYTE clear )'),
    function('static int mprotect_range( void *base, size_t size, BYTE set, BYTE clear )'),
    between('static struct { char *base; size_t size; BYTE vprot; } ps5_saved_vprot[64];', '#endif'),
    function('static BOOL set_vprot( struct file_view *view, void *base, size_t size, BYTE vprot )'),
    'static BOOL ps5_release_to_system;\n',
    function('static void unmap_area( void *start, size_t size )'),
    function('static void *reserved_area_mmap( void *start, size_t size, int prot )'),
    function('static void *alloc_free_area_in_range( struct alloc_area *area, char *base, char *end )'),
    function('static void ps5_claim_native_holes(void)'),
    function('static NTSTATUS ps5_placeholder_refused( struct file_view *view )'),
    'static NTSTATUS map_view_replace( struct file_view **view_ret, void *base, size_t size, unsigned int alloc_type,\n'
    '                                  unsigned int vprot )\n{\n' + src[branch:branch_end] +
    '    return STATUS_INVALID_PARAMETER;\n}\n',
    function('static struct range_entry *free_ranges_lower_bound( void *addr )'),
    function('static void free_ranges_insert_view( struct file_view *view )'),
    function('static void free_ranges_remove_view( struct file_view *view )'),
    between('#define PS5_ADDRESS_POOL_CHUNK', '\n'),
    '\nstatic char *ps5_pool_base, *ps5_pool_end;\n',
    function('static BOOL ps5_pool_release( const void *start, const void *end, size_t size, size_t align_mask )'),
    function('NTSTATUS WINAPI NtLockVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG unknown )'),
    function('NTSTATUS WINAPI NtUnlockVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG unknown )'),
    function('static inline int is_view_valloc( const struct file_view *view )'),
    src[hook:src.index('#endif', src.index('static BOOL ps5_fault_served'))],
    function('BOOL virtual_ps5_step_done(void)'),
    function('DECLSPEC_EXPORT int __wine_ps5_placed_range_bare( void *address, size_t bytes )'),
    function('static int ps5_mmu4k_classify( void *context, uintptr_t host_page, PwWineMmu4kPage *out )'),
]
open(sys.argv[2], 'w').write('\n'.join(out))
server = open(sys.argv[3]).read()
start = server.index('int (*shared_alloc)(size_t)=nullptr;')
open(sys.argv[4], 'w').write(server[start:server.index('\n}\n', server.index('int shared_object(uint64_t bytes)', start)) + 3])
