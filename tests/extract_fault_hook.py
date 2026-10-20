# SPDX-License-Identifier: GPL-3.0-or-later
# Pull the real PS5 fault-hook functions (patch 1127) and the upstream helpers they use out of the applied virtual.c,
# so tests/test_wine_fault_served.c tests the patched code itself, not a copy.
#   python3 tests/extract_fault_hook.py <applied virtual.c> <output .inc>
import sys
src = open(sys.argv[1]).read()
def function(signature):
    i = src.index(signature)
    j = src.index('\n}\n', i) + 3
    return src[i:j]
start = src.index('#define PS5_EXEC_CLUSTER')
end = src.index('#endif', src.index('static BOOL ps5_fault_served'))
known = src.index('enum { HOST_PROT_LEAF_SHIFT')
out = [src[known:src.index('\n', known) + 1],
       function('static int get_unix_prot( BYTE vprot )'),
       function('static inline int is_view_valloc( const struct file_view *view )'),
       src[start:end]]
open(sys.argv[2], 'w').write('\n'.join(out))
