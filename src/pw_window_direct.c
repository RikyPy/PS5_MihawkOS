/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_window_direct.h"
#include "pw_window_effects.h"

/* How far a shadow reaches beyond its window, on any side (pw_window_effects.h). */
enum { SHADOW_REACH = PW_SHADOW_WINDOW_REACH,
       FLYOUT_SHADOW_REACH = PW_SHADOW_FLYOUT_REACH + PW_SHADOW_FLYOUT_OFFSET };

static int intersects(int64_t left, int64_t top, int64_t right, int64_t bottom, uint32_t width, uint32_t height)
{
    return left < (int64_t)width && top < (int64_t)height && right > 0 && bottom > 0 && right > left && bottom > top;
}

static int direct_enabled = 1;

void pw_window_direct_set_enabled(int enabled)
{
    __atomic_store_n(&direct_enabled, enabled != 0, __ATOMIC_RELAXED);
}

int pw_window_direct_flip(const PwWindowSnapshot *snapshot, int32_t pid, uint32_t hwnd, uint64_t generation,
                          uint32_t frame_width, uint32_t frame_height, uint32_t screen_width, uint32_t screen_height)
{
    if (!__atomic_load_n(&direct_enabled, __ATOMIC_RELAXED)) return PW_DIRECT_OFF;
    if (!snapshot || snapshot->overflow || snapshot->count > PW_WINDOW_LIMIT ||
        snapshot->clip_count > PW_WINDOW_CLIP_LIMIT || snapshot->thumbnail_count > PW_WINDOW_THUMBNAIL_LIMIT)
        return PW_DIRECT_INCOMPLETE;
    uint32_t index = snapshot->count;
    for (uint32_t i = 0; i < snapshot->count; ++i) {
        const PwWindowRow *row = &snapshot->windows[i];
        if (row->pid == pid && row->hwnd == hwnd && row->generation == generation) { index = i; break; }
    }
    if (index == snapshot->count) return PW_DIRECT_NO_WINDOW;
    const PwWindowRow *row = &snapshot->windows[index];
    if ((row->flags & (PW_WINDOW_VISIBLE | PW_WINDOW_CLIENT)) != (PW_WINDOW_VISIBLE | PW_WINDOW_CLIENT))
        return PW_DIRECT_NO_WINDOW;
    if (row->client_left != 0 || row->client_top != 0 || row->client_right != (int32_t)screen_width ||
        row->client_bottom != (int32_t)screen_height)
        return PW_DIRECT_NOT_FULLSCREEN;
    if (frame_width != screen_width || frame_height != screen_height) return PW_DIRECT_SIZE;
    if ((row->flags & (PW_WINDOW_SHAPED | PW_WINDOW_GLASS | PW_WINDOW_UNSUPPORTED | PW_WINDOW_BORDER |
                       PW_WINDOW_DESKTOP)) || row->radius)
        return PW_DIRECT_EFFECTS;
    for (uint32_t i = 0; i < snapshot->thumbnail_count; ++i)
        if (snapshot->thumbnails[i].destination == hwnd) return PW_DIRECT_EFFECTS;
    /* The clips must tile the screen: each inside it, none overlapping another, together its whole area. */
    if (row->clip_start > snapshot->clip_count || row->clip_count > snapshot->clip_count - row->clip_start)
        return PW_DIRECT_INCOMPLETE;
    const PwWindowClip *clips = &snapshot->clips[row->clip_start];
    uint64_t area = 0;
    for (uint32_t i = 0; i < row->clip_count; ++i) {
        const PwWindowClip *a = &clips[i];
        if (a->left < 0 || a->top < 0 || a->right > (int32_t)screen_width || a->bottom > (int32_t)screen_height ||
            a->right <= a->left || a->bottom <= a->top)
            return PW_DIRECT_CLIPPED;
        for (uint32_t j = i + 1; j < row->clip_count; ++j) {
            const PwWindowClip *b = &clips[j];
            if (a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom)
                return PW_DIRECT_CLIPPED;
        }
        area += (uint64_t)(a->right - a->left) * (uint64_t)(a->bottom - a->top);
    }
    if (area != (uint64_t)screen_width * screen_height) return PW_DIRECT_CLIPPED;
    /* Rows are topmost first: everything before this one is drawn over it. */
    for (uint32_t i = 0; i < index; ++i) {
        const PwWindowRow *above = &snapshot->windows[i];
        if (!(above->flags & PW_WINDOW_VISIBLE)) continue;
        const int64_t reach = above->shadow == PW_SHADOW_FLYOUT ? FLYOUT_SHADOW_REACH : above->shadow ? SHADOW_REACH : 0;
        if (intersects((int64_t)above->left - reach, (int64_t)above->top - reach, (int64_t)above->right + reach,
                       (int64_t)above->bottom + reach, screen_width, screen_height))
            return PW_DIRECT_COVERED;
    }
    return PW_DIRECT_YES;
}

const char *pw_window_direct_reason(int reason)
{
    static const char *const words[PW_DIRECT_REASONS] = {"yes", "incomplete", "no-window", "not-fullscreen", "size",
                                                         "effects", "clipped", "covered", "off"};
    return reason >= 0 && reason < PW_DIRECT_REASONS ? words[reason] : "unknown";
}
