/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINDOW_SCENE_H
#define PW_WINDOW_SCENE_H
#include "pw_window_snapshot.h"
#include "pw_window_page.h"
/* Title-side ownership, used only under the broker display lock. At most 128
 * pages and 64 MiB shared backing; stable copies have the same bounded capacity.
 * HWND order/visibility/position are always supplied by Wine's snapshot. An
 * attach consumes fd on every outcome. Replacements are transactional. */
typedef struct PwWindowSurface {
    int32_t pid;          /* the window's owner (the snapshot row's), so composition and pruning follow the window */
    int32_t presenter;    /* GPU images: the process that presented them, which may not own the window */
    PwWindowPage *page;
    PwWindowPage header;
    uint32_t *pixels;
    uint32_t shown;
} PwWindowSurface;
typedef struct PwWindowScene {
    PwWindowSurface surfaces[128], gpu[128];
    size_t mapped_bytes,pixel_bytes,scratch_bytes;
    void *scratch;
} PwWindowScene;
#ifdef __cplusplus
extern "C" {
#endif
int pw_window_scene_attach(PwWindowScene *scene,int32_t pid,int fd,size_t bytes,const PwWindowSnapshot *snapshot);
/* Prune destroyed/reused lifetimes, retain hidden windows, then copy stable new
 * pixels. Returns changes, -1 on failure. Caller never paints live page memory. */
/* Copy a fence-completed GPU image into bounded title-owned retained pixels.
 * HWND/generation must name a window of the server snapshot (generations are
 * never reused); the presenter need not own it. Windows lets a process present
 * into another's window, and Chromium does: its GPU process presents into the
 * browser process's compositor window when DirectComposition is unavailable
 * (2026-10-10; record removed from the tree). The image is the owner's, so composition and
 * pruning follow the window; presenter's exit drops it (pw_window_scene_detach).
 * Input is borrowed only during this call, under the broker display lock.
 * Replacements preserve old pixels on validation/capacity/allocation failure. */
int pw_window_scene_gpu(PwWindowScene *scene,int32_t presenter,uint32_t hwnd,uint64_t generation,
                        const void *pixels,uint32_t width,uint32_t height,uint32_t stride,
                        unsigned bgra,const PwWindowSnapshot *snapshot);
int pw_window_scene_update(PwWindowScene *scene,const PwWindowSnapshot *snapshot);
size_t pw_window_scene_images(const PwWindowScene *scene,PwWindowImage *images,size_t capacity);
/* A process is gone: its windows' pages and images, and the GPU images it presented into any window. */
void pw_window_scene_detach(PwWindowScene *scene,int32_t pid);
void pw_window_scene_release(PwWindowScene *scene);
#ifdef __cplusplus
}
#endif
#endif
