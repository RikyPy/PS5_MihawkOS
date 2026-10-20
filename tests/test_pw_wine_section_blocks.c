/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The section data test on a model of the console's /data (bfs): mappings of a hole are incoherent, real blocks are
 * shared; pw_wine_blocks gives each chunk blocks before its first mapping, and every alias then reads every write. */
#define PW_SECTION_INCOHERENT
#define PW_SECTION_BLOCKS
#include "test_pw_wine_section_data.c"
