# SPDX-License-Identifier: GPL-3.0-or-later
# Pull patch 1128's guard against mappings that are not Wine's out of the applied virtual.c (the release that stays
# reserved, the holes made views, the refused commit undone, a refused placeholder commit, a reservation in a reserved
# area asked about first), so tests/test_wine_virtual_guard.c tests the patched code itself, not a copy.
#   python3 tests/extract_virtual_guard.py <applied virtual.c> <output .inc>
import sys
src = open(sys.argv[1]).read()
def function(signature):
    i = src.index(signature)
    return src[i:src.index('\n}\n', i) + 3]
saved = src.index('static struct { char *base; size_t size; BYTE vprot; } ps5_saved_vprot[64];')
start = src.index('static NTSTATUS map_view( struct file_view **view_ret')
branch = src.index('    if (alloc_type & MEM_REPLACE_PLACEHOLDER)\n    {', start)
branch_end = src.index('        return STATUS_SUCCESS;\n    }\n', branch) + len('        return STATUS_SUCCESS;\n    }\n')
out = [src[saved:src.index('\n}\n', saved) + 3],
       function('static BOOL set_vprot( struct file_view *view, void *base, size_t size, BYTE vprot )'),
       'static BOOL ps5_release_to_system;\n',
       function('static void unmap_area( void *start, size_t size )'),
       function('static void *reserved_area_mmap( void *start, size_t size, int prot )'),
       function('static void ps5_claim_native_holes(void)'),
       function('static NTSTATUS ps5_placeholder_refused( struct file_view *view )'),
       # map_view's placeholder replace, as it stands in map_view.
       'static NTSTATUS map_view_replace( struct file_view **view_ret, void *base, size_t size, unsigned int alloc_type,\n'
       '                                  unsigned int vprot )\n{\n' + src[branch:branch_end] +
       '    return STATUS_INVALID_PARAMETER;\n}\n']
open(sys.argv[2], 'w').write('\n'.join(out))
