/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINDOW_SNAPSHOT_H
#define PW_WINDOW_SNAPSHOT_H
#include <stdint.h>
#include <stddef.h>
/* One server-authored snapshot, topmost first. Identity is native PID + full
 * HWND + lifetime generation. Surface producers never supply stacking or
 * visibility. Caller copies under the publisher's lock; no Wine object pointer
 * crosses into the broker. Regions/layered windows are identified explicitly; this record does not
 * itself render any pixels. An overflow is an explicit incomplete snapshot.
 *
 * Version 4 adds DWM surface state that the server resolves from the owning
 * process's dwmapi requests and window styles (docs/MIHAWK_OS.md, DWM surfaces):
 * corner radius, optional border, shadow class, system backdrop with its glass
 * area, and live thumbnails. All of it is visual: Wine's hit-testing, regions
 * and window state are unchanged. */
enum { PW_WINDOW_SNAPSHOT_VERSION=4, PW_WINDOW_LIMIT=128,
       PW_WINDOW_VISIBLE=1, PW_WINDOW_DESKTOP=2, PW_WINDOW_UNSUPPORTED=4,
       PW_WINDOW_CLIENT=8, PW_WINDOW_CHILD=16, PW_WINDOW_SHAPED=32,
       PW_WINDOW_BORDER=64, PW_WINDOW_GLASS=128, PW_WINDOW_ACTIVE=256,
       PW_WINDOW_CLIP_LIMIT=512, PW_WINDOW_THUMBNAIL_LIMIT=32 };
/* Resolved per row. RADIUS_LIMIT bounds the compositor's coverage tables. */
enum { PW_WINDOW_RADIUS_LIMIT=16,
       PW_BACKDROP_NONE=0, PW_BACKDROP_MICA=1, PW_BACKDROP_ACRYLIC=2, PW_BACKDROP_MICA_ALT=3, PW_BACKDROP_TASKBAR=4,
       PW_BACKDROP_FLYOUT_BASE=5, PW_BACKDROP_FLYOUT=6,
       PW_MATERIAL_LIGHT=0, PW_MATERIAL_DARK=1,
       PW_SHADOW_NONE=0, PW_SHADOW_SMALL=1, PW_SHADOW_WINDOW=2, PW_SHADOW_FLYOUT=3, PW_SHADOW_TOOLTIP=4 };
typedef struct PwWindowRow {
    uint32_t hwnd, flags;
    int32_t pid;
    uint32_t windows_pid, owner, reserved;
    int32_t left, top, right, bottom;
    int32_t client_left, client_top, client_right, client_bottom;
    uint32_t clip_start, clip_count;
    uint64_t generation;
    /* Whole-window region in screen coordinates, distinct from GPU-client clips.
     * SHAPED with zero rectangles is empty, never an implicit full rectangle. */
    uint32_t shape_start, shape_count;
    /* v4. radius rounds the visible rectangle's corners (0 = square). border is
     * 0xAARRGGBB, drawn only with PW_WINDOW_BORDER: AA 0 is opaque (an application's
     * DWMWA_BORDER_COLOR), otherwise the alpha it blends with over the window's own
     * edge (Windows 11's default frame border). With PW_WINDOW_GLASS the
     * window selected a backdrop: pixels inside its rectangle but outside the
     * solid rectangle (screen coordinates, possibly empty) carry premultiplied
     * alpha over the material; every other pixel stays opaque. material is the
     * theme the window's effects draw in (DWMWA_USE_IMMERSIVE_DARK_MODE): its glass and
     * its shadow. ACTIVE marks the foreground's active window, whose frame shadow
     * is the active one. */
    uint8_t radius, backdrop, material, shadow;
    uint32_t border;
    int32_t solid_left, solid_top, solid_right, solid_bottom;
} PwWindowRow;
/* A registered, visible DWM thumbnail. Destination is screen coordinates,
 * already offset from the destination's client origin; source is relative to
 * the source window's visible rectangle. The compositor clips the image to the
 * destination window and draws it immediately above that window's own planes. */
typedef struct PwWindowThumbnail {
    uint32_t destination, source;
    uint64_t destination_generation, source_generation;
    int32_t left, top, right, bottom;
    int32_t source_left, source_top, source_right, source_bottom;
    uint32_t opacity, reserved;
} PwWindowThumbnail;
typedef struct PwWindowClip { int32_t left,top,right,bottom; } PwWindowClip;
typedef struct PwWindowSnapshot {
    uint32_t version, size, count, overflow;
    uint64_t sequence;
    uint32_t clip_count, thumbnail_count;
    PwWindowRow windows[PW_WINDOW_LIMIT];
    PwWindowClip clips[PW_WINDOW_CLIP_LIMIT];
    PwWindowThumbnail thumbnails[PW_WINDOW_THUMBNAIL_LIMIT];
} PwWindowSnapshot;
/* This protocol is also used by the marked diagnostic's file rendezvous. */
typedef struct PwWindowProbeReply {
    uint32_t nonce, status;
    PwWindowSnapshot snapshot;
} PwWindowProbeReply;
typedef char pw_window_row_size[(sizeof(PwWindowRow) == 104) ? 1 : -1];
typedef char pw_window_thumbnail_size[(sizeof(PwWindowThumbnail) == 64) ? 1 : -1];
typedef char pw_window_snapshot_size[(sizeof(PwWindowSnapshot) == 23584) ? 1 : -1];
typedef char pw_window_reply_size[(sizeof(PwWindowProbeReply) == 23592) ? 1 : -1];
enum { PW_WINDOW_GDI=0, PW_WINDOW_GPU=1, PW_WINDOW_IMAGE_LIMIT=PW_WINDOW_LIMIT*2,
       PW_WINDOW_PREMULTIPLIED=1 };
/* Stable retained pixels, never a live writing page. Pixels remain owned by
 * the caller throughout compose. Stride/capacity are validated before drawing.
 * Output and all input buffers must be disjoint. sequence changes whenever the
 * pixels change; effect caches (Mica, thumbnails) key on it. */
typedef struct PwWindowImage {
    uint32_t hwnd, width, height, stride, plane;
    int32_t offset_x, offset_y;
    int32_t pid;
    uint64_t generation;
    const uint32_t *pixels;
    size_t bytes;
    uint32_t flags, sequence;
} PwWindowImage;
struct PwWindowEffects;
#ifdef __cplusplus
extern "C" {
#endif
/* Without an effects workspace, a snapshot needing a material or thumbnail is
 * refused (ENOMEM): content is never silently omitted. Corners, borders and
 * shadows need no workspace. */
int pw_windows_compose(uint32_t *output,uint32_t width,uint32_t height,
                       const PwWindowSnapshot *snapshot,
                       const PwWindowImage *images,size_t count);
int pw_windows_compose_effects(uint32_t *output,uint32_t width,uint32_t height,
                               const PwWindowSnapshot *snapshot,
                               const PwWindowImage *images,size_t count,
                               struct PwWindowEffects *effects);
#ifdef __cplusplus
}
#endif
#endif
