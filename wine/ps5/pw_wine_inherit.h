/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_INHERIT_H
#define PW_WINE_INHERIT_H
#include <stddef.h>
#include <string.h>
/* What a helper inherits of its spawner's environment (pw_wine_process.c's spawn paths; host test:
 * tests/test_pw_wine_inherit.c).
 *
 * The desktop and display (WINE_PS5_DESKTOP, without which a helper's Wine reported an 800x600 monitor), Direct3D's and
 * DXVK's and VKD3D-Proton's settings, the CPU set, .NET's and SDL's, and the debug switches of the helper's RADV and
 * its compiler (RADV_DEBUG, RADV_PERFTEST, ACO_DEBUG; unset unless a test sets them; RADV_DEBUG=shaders still printed
 * nothing in a helper, zink-sprite-2026-10-06-08, which is not understood). Mesa's Gallium and Zink switches and its
 * log, for a helper's OpenGL (Mesa's WGL over Zink): GALLIUM_DRIVER the title sets for it, ZINK_DESCRIPTORS,
 * GALLIUM_THREAD or ZINK_DEBUG a test sets (before them, a test's ZINK_DESCRIPTORS=lazy never reached Mesa,
 * zink-sprite-2026-10-06-03). Not the rest of MESA_: the title's shader cache folder is a native path for its own RADV.
 * Not what the loader sets for itself (the server socket, the child markers, the GPU broker's and input page's
 * descriptors, the open-file threshold) nor what is the title's alone (its standard output file). The helper
 * scheduling settings reach every helper and the memory helper, whose loaders apply them. The GPU broker client's
 * switches live in the helper that presents: WINE_PS5_ASYNC_PRESENT (graphics patch 0024) and WINE_PS5_PRESENT_LINK
 * (0026); set only in the title, they never reached the client before they were named here. The language the
 * guided setup chose for the prefix (WINE_PS5_LOCALE): every Windows program but the shell's Explorer takes it (patch
 * 1230). */
static inline int pw_wine_inherited(const char *entry)
{
    static const char *const names[]={"WINE_PS5_DESKTOP=","WINE_PS5_VIEW=","WINE_PS5_WINDOW_SURFACES=","WINE_PS5_REFRESH=","WINE_PS5_HID=","WINE_PS5_LOCALE=",
        "WINE_PS5_HELPER_SCHED=","WINE_PS5_HELPER_PRIORITY=","WINE_PS5_HELPER_CPUS=",
        "WINE_PS5_HELPER_CPUS_WIDEN=",   /* the loader places the helper on the title's CPUs (src/pw_cpu_placement.c) */
        "WINE_PS5_ASYNC_PRESENT=",   /* the broker WSI presents from its own thread (graphics patch 0024) */
        "WINE_PS5_PRESENT_LINK=",    /* presents over a connection of their own (graphics patch 0026) */
        "WINE_PS5_SCHED_CENSUS=",
        "WINE_PS5_PAYLOAD_DMEM=","WINE_PS5_NATIVE_SIGNALS=","WINEARCH=",   /* wow64: a 32-bit program runs in its helper (docs/IA32_DESIGN.md, M3) */
        "WINETEST_",  /* watchdog, slice and threading controls must reach payload conformance tests */
        "WINE_PS5_OPENGL=","WINE_D3D_CONFIG=","WINE_CPU_TOPOLOGY=","WINEESYNC=","WINEFSYNC=","DXVK_","VKD3D_",
        "DOTNET_","SDL_","WINE_ADDITIONAL_CERTS_DIR=","WINE_PS5_DNS=","WINE_PS5_SERVICES=","RADV_DEBUG=","RADV_PERFTEST=",
        "ACO_DEBUG=","GALLIUM_","ZINK_","MESA_LOG="};
    for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)
        if(!strncmp(entry,names[i],strlen(names[i])))return 1;
    return 0;
}
/* The block a helper's bootstrap carries: each inherited NAME=value of environment, NUL-terminated, then an empty one;
 * an entry that does not fit in size is left out. Returns the bytes used before the final NUL. */
static inline size_t pw_wine_inherit_environment(char *const *environment,char *out,size_t size)
{
    size_t used=0;
    for(char *const *entry=environment;entry && *entry;++entry) {
        const size_t length=strlen(*entry)+1;
        if(!pw_wine_inherited(*entry) || !strchr(*entry,'=') || used+length+1>size)continue;
        memcpy(out+used,*entry,length);used+=length;
    }
    out[used]=0;
    return used;
}
#endif
