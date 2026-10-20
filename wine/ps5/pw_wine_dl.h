/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_DL_H
#define PW_WINE_DL_H
#include <stddef.h>
#include <stdint.h>

/* dlopen-style loading of Wine's Unix modules as PRX on PS5. A request for
 * "dir/name.so" loads "dir/name.prx", or "<module dir>/name.prx" when a
 * module directory is set. A bare "name.so" is looked up in the module
 * directory, then beside each module already loaded or adopted, so win32u's
 * dlopen("libfreetype.so") finds libfreetype.prx beside ntdll.prx. The
 * module's PRXDESC1 descriptor supplies every symbol, and its module_start,
 * when exported, is called once after load because the firmware does not. Loads are reference counted by path.
 * A module that exports pw_module_lookup(void *(*)(void *,const char *)) is handed pw_wine_dl_sym as it loads,
 * before module_start: a module linked without ntdll gets libkernel's dlsym, and the firmware's lookup finds no
 * application module's export (sceKernelDlsym on 12.02, tools/gen_prx_descriptor.py), so it looks Wine's up with this
 * (the GPU broker's client in a helper, for ntdll's pw_ps5_child_doomed).
 * The kernel calls are injected so the logic is host-testable. */
enum { PW_WINE_DL_MAX_MODULES=32,PW_WINE_DL_MAX_PATH=512,PW_WINE_DL_REPORT=1024 };
typedef struct PwWineDlOps {
    int32_t (*load_start)(const char *path,size_t argc,const void *argv,uint32_t flags,
                          const void *option,int *result);
    int (*module_info)(int32_t handle,void *info);
    int (*stop_unload)(int32_t handle,size_t argc,const void *argv,uint32_t flags,
                       const void *option,int *result);
    /* Optional: called with one line when an open fails, naming the module,
     * every path tried and why, so the console log says what went wrong; in a
     * helper (WINE_PS5_CHILD) also before and after each module load. */
    void (*report)(const char *line);
    /* Optional: a table handed to every module that exports pw_module_share,
     * as it loads (the open-file budget, which is one per process). */
    const void *share;
} PwWineDlOps;
#define PW_WINE_DL_DEFAULT ((void *)0)
typedef struct PwWineDlInfo {
    const char *path;          /* the path the module was opened or adopted as */
    const void *base;          /* its first segment */
} PwWineDlInfo;

void pw_wine_dl_configure(const PwWineDlOps *ops,const char *module_dir);
void *pw_wine_dl_open(const char *path);
/* handle PW_WINE_DL_DEFAULT searches every loaded module in load order. */
void *pw_wine_dl_sym(void *handle,const char *name);
int pw_wine_dl_close(void *handle);
/* Registers a module the title loaded itself (ntdll.prx, for instance) so
 * that dladdr can name it; returns a handle like pw_wine_dl_open. */
void *pw_wine_dl_adopt(const char *path,int32_t module_handle);
/* The module whose segments contain address, as dladdr reports it. */
int pw_wine_dl_addr(const void *address,PwWineDlInfo *info);
/* The last failure of this thread, cleared once read, as dlerror does. */
const char *pw_wine_dl_error(void);

#endif
