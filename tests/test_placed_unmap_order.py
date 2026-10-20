#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The invariants wine/ps5/pw_wine_dmem.h relies on for placed GPU memory (VK_EXT_map_memory_placed, patch 0951): win32u
gives the committed range back to a bare reservation the engine forgets (patch 1130, through ntdll's
__wine_ps5_placed_range_bare, which holds the virtual mutex) before RADV maps its wired memory there, so the engine reads
that mapping as foreign; and winevulkan lays the reservation back
(VK_MEMORY_UNMAP_RESERVE_BIT_EXT) before NtFreeVirtualMemory releases the view. Read from the tree the last
tools/build-wine-ps5.sh applied; skipped without one."""
from pathlib import Path
import re
import unittest

VULKAN = Path(__file__).resolve().parents[1] / '.deps/wine-ps5/source/dlls/win32u/vulkan.c'


def body(source, name):
    start = re.search(r'^static \w+ ' + name + r'\(.*?\n\{\n', source, re.M | re.S)
    assert start, f'{name} not found in {VULKAN}'
    end = source.index('\n}\n', start.end())
    return source[start.end():end]


@unittest.skipUnless(VULKAN.exists(), 'no applied Wine tree')
class PlacedUnmapOrder(unittest.TestCase):
    def test_reserve_laid_back_before_the_view_is_freed(self):
        source = VULKAN.read_text()
        for name in ('win32u_vkFreeMemory', 'win32u_vkUnmapMemory2KHR'):
            with self.subTest(function=name):
                text = body(source, name)
                laid = text.index('VK_MEMORY_UNMAP_RESERVE_BIT_EXT')
                unmapped = text.index('p_vkUnmapMemory2KHR', laid)
                freed = text.index('NtFreeVirtualMemory( GetCurrentProcess(), &memory->vm_map')
                self.assertLess(unmapped, freed)

    def test_range_made_bare_before_the_placed_map(self):
        text = body(VULKAN.read_text(), 'win32u_vkMapMemory2KHR')
        committed = text.index('MEM_COMMIT, PAGE_READWRITE')
        bare = text.index('__wine_ps5_placed_range_bare( placed_info.pPlacedAddress', committed)
        self.assertLess(bare, text.index('device->p_vkMapMemory2KHR( device->host.device, &info, data )'))
        self.assertNotIn('__wine_ps5_dmem_file_region', text)   # the engine only under ntdll's virtual mutex


if __name__ == '__main__':
    unittest.main()
