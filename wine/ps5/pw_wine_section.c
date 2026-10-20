/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_section.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
/* A record taken for the fault in progress, not yet linked: the clock hand must not choose it as a victim. */
#define HELD UINT64_MAX
/* The retry generation of a fault resolved in a mapped direct chunk (cache generations never reach it). */
#define DIRECT_RETRY (UINT64_MAX - 1)
#define CHUNK ((uint64_t)PW_WINE_SECTION_CHUNK)
static int failed(PwWineSection *s, unsigned stage)
{
    s->failures++;
    s->failed_stage = stage;
    return -1;
}
static int aligned(PwWineSection *s, uintptr_t address, size_t bytes)
{
    return bytes && !(address % s->ops.page) && !(bytes % s->ops.page) && address + bytes > address;
}
int pw_wine_section_init(PwWineSection *s, const PwWineSectionOps *ops, PwWineSectionPage *pages,
                         size_t count)
{
    if (!s || !ops || !pages || !count || !ops->page || (ops->page & (ops->page - 1)) || !ops->retain ||
        !ops->close || !ops->map || !ops->protect || !ops->flush || !ops->unmap || !ops->reserve ||
        count > SIZE_MAX / sizeof(*pages) || count >= UINT32_MAX)
        return -1;
    memset(s, 0, sizeof(*s));
    memset(pages, 0, count * sizeof(*pages));
    s->ops = *ops;
    s->pages = pages;
    s->capacity = count;
    return 0;
}
int pw_wine_section_use_index(PwWineSection *s, uint32_t *buckets, size_t count)
{
    if (!s || !buckets || !count || (count & (count - 1)) || s->count || count > SIZE_MAX / sizeof(*buckets))
        return -1;
    memset(buckets, 0, count * sizeof(*buckets));
    s->buckets = buckets;
    s->bucket_mask = count - 1;
    return 0;
}
int pw_wine_section_use_direct(PwWineSection *s, int (*direct)(void *, int), unsigned (*protection)(void *, uintptr_t))
{
    if (!s || !direct || !protection || s->views)
        return -1;
    s->direct = direct;
    s->protection = protection;
    return 0;
}
PwWineSectionView *pw_wine_section_find(PwWineSection *s, uintptr_t address)
{
    for (PwWineSectionView *v = s->views; v; v = v->next)
        if (address >= v->base && address - v->base < v->bytes)
            return v;
    return NULL;
}
int pw_wine_section_add(PwWineSection *s, PwWineSectionView *view, uintptr_t base, size_t bytes, int fd,
                        uint64_t offset)
{
    if (!view || fd < 0 || !aligned(s, base, bytes) || offset % s->ops.page || offset + bytes < offset)
        return -1;
    for (PwWineSectionView *v = s->views; v; v = v->next)
        if (v == view || (base < v->base + v->bytes && v->base < base + bytes))
            return -1;
    /* A direct view's chunk bitmap is made here, never in a fault. */
    uint64_t *chunks = NULL;
    if (s->direct) {
        const uint64_t count = (offset + bytes - 1) / CHUNK - offset / CHUNK + 1;
        if (!(chunks = calloc((size_t)((count + 63) / 64), sizeof(*chunks))))
            return failed(s, 1);
    }
    int retained = s->ops.retain(s->ops.context, fd);
    if (retained < 0) {
        free(chunks);
        return failed(s, 1);
    }
    *view = (PwWineSectionView){
        .base = base, .bytes = bytes, .offset = offset, .fd = retained, .next = s->views, .chunks = chunks};
    s->views = view;
    /* Asked now, not at the first fault: the question admits this process while the caller's handle still holds the
     * section, and a view outlives the handle (pw_wine_dsection.h, Lifetime). A failure is asked again at the fault. */
    if (s->direct) {
        const int direct = s->direct(s->ops.context, retained);
        if (direct >= 0) view->direct = direct ? 1 : -1;
    }
    return 0;
}

/* Direct views: chunk c of the view is the c-th PW_WINE_SECTION_CHUNK of the section it touches, clipped to it. */
static uint64_t chunk_index(const PwWineSectionView *v, uintptr_t at)
{
    return (v->offset + (at - v->base)) / CHUNK - v->offset / CHUNK;
}
static int chunk_mapped(const PwWineSectionView *v, uint64_t c)
{
    return (int)(v->chunks[c / 64] >> (c % 64) & 1);
}
static uintptr_t chunk_span(const PwWineSectionView *v, uint64_t c, size_t *bytes)
{
    const uint64_t start = (v->offset / CHUNK + c) * CHUNK, end = start + CHUNK;
    const uint64_t from = start > v->offset ? start : v->offset;
    const uint64_t to = end < v->offset + v->bytes ? end : v->offset + v->bytes;
    *bytes = (size_t)(to - from);
    return v->base + (uintptr_t)(from - v->offset);
}
/* The title's answer failed: for now (RETRY, a few times in a row) or for good (-1). The view stays usable. */
static int asked_again(PwWineSection *s, PwWineSectionView *v, int error)
{
    if ((error == EINTR || error == EAGAIN || error == ENOMEM || error == ETIMEDOUT) &&
        ++v->retries < PW_WINE_SECTION_RETRIES) {
        s->retries++;
        return PW_WINE_SECTION_RETRY;
    }
    v->retries = 0;
    return failed(s, 8);
}
/* Back to a guard: [at, +bytes) of a direct chunk that was mapped. 0, or -1 with the view refused (its guard may be
 * gone, and a fixed mapping must never go over what took its place). */
static int unmap_direct(PwWineSection *s, PwWineSectionView *v, uintptr_t at, size_t bytes, unsigned stage)
{
    if (!bytes || (s->ops.lay && !s->ops.lay(s->ops.context, at, bytes)))
        return 0;
    if (s->ops.unmap(s->ops.context, at, bytes) || s->ops.reserve(s->ops.context, at, bytes)) {
        v->failed = 1;
        v->lost_guard = at;
        return failed(s, stage);
    }
    return 0;
}
static int direct_fault(PwWineSection *s, PwWineSectionView *v, uintptr_t address, unsigned protection, int bus,
                        PwWineSectionRetry *retry)
{
    const PwWineSectionOps *o = &s->ops;
    const uintptr_t at = address & ~(uintptr_t)(o->page - 1);
    const uint64_t c = chunk_index(v, at);
    if (chunk_mapped(v, c)) {
        /* Mapped: the program's own fault, unless the kernel's protection lags Windows' (once per access). */
        if (bus || (retry->address == at && retry->generation == DIRECT_RETRY))
            return 0;
        if (o->protect(o->context, at, o->page, protection))
            return failed(s, 6);
        *retry = (PwWineSectionRetry){at, DIRECT_RETRY};
        return 1;
    }
    size_t bytes, done = 0;
    const uintptr_t base = chunk_span(v, c, &bytes);
    /* The whole chunk is mapped on any touch, so a mapping that is not Wine's anywhere in it refuses the fault. */
    if (o->foreign && o->foreign(o->context, base, bytes))
        return failed(s, 9);
    /* Run by run of one protection: each mapped read-write, then protected at once. */
    while (done < bytes) {
        const unsigned want = s->protection(o->context, base + done);
        size_t run = o->page;
        while (done + run < bytes && s->protection(o->context, base + done + run) == want)
            run += o->page;
        const uint64_t offset = v->offset + (base + done - v->base);
        if (o->map(o->context, base + done, run, PW_WINE_SECTION_READ | PW_WINE_SECTION_WRITE, v->fd, offset)) {
            const int error = errno;
            if (unmap_direct(s, v, base, done, 4))
                return -1;
            /* Not ENXIO: the title's answer failed and the run is as it was. */
            if (error != ENXIO)
                return asked_again(s, v, error);
            /* The kernel refused the mapping, which may have taken the guard with it: only a no-overwrite reserve. */
            v->failed = 1;
            if (o->reserve(o->context, base + done, run))
                v->lost_guard = base + done;
            return failed(s, 5);
        }
        done += run;
        if (want != (PW_WINE_SECTION_READ | PW_WINE_SECTION_WRITE) &&
            o->protect(o->context, base + done - run, run, want))
            return unmap_direct(s, v, base, done, 4) ? -1 : failed(s, 6);
    }
    v->chunks[c / 64] |= (uint64_t)1 << (c % 64);
    v->retries = 0;
    s->direct_maps++;
    s->direct_bytes += bytes;
    *retry = (PwWineSectionRetry){at, DIRECT_RETRY};
    return 1;
}

int pw_wine_section_backing(PwWineSection *s, uintptr_t address, int *fd, uint64_t *offset, uintptr_t *low,
                            uintptr_t *high)
{
    PwWineSectionView *v = s && fd && offset && low && high ? pw_wine_section_find(s, address) : NULL;
    size_t bytes;
    if (!v || v->direct <= 0 || !v->chunks || !chunk_mapped(v, chunk_index(v, address)))
        return 0;
    *low = chunk_span(v, chunk_index(v, address), &bytes);
    *high = *low + bytes;
    *fd = v->fd;
    *offset = v->offset + (address - v->base);
    return 1;
}

static size_t bucket_of(const PwWineSection *s, uintptr_t at)
{
    return (size_t)(((uint64_t)(at / s->ops.page) * 0x9e3779b97f4a7c15ull) >> 24) & s->bucket_mask;
}
/* The cached (linked) page at this page-aligned address, or NULL. */
static PwWineSectionPage *find_page(PwWineSection *s, uintptr_t at)
{
    if (s->buckets) {
        for (uint32_t i = s->buckets[bucket_of(s, at)]; i; i = s->pages[i - 1].hnext)
            if (s->pages[i - 1].address == at)
                return &s->pages[i - 1];
        return NULL;
    }
    for (size_t i = 0; i < s->capacity; i++)
        if (s->pages[i].view && s->pages[i].generation != HELD && s->pages[i].address == at)
            return &s->pages[i];
    return NULL;
}
/* Put a filled page record on its address chain and its view's list. */
static void link_page(PwWineSection *s, PwWineSectionPage *page)
{
    const uint32_t index = (uint32_t)(page - s->pages) + 1;
    PwWineSectionView *v = page->view;
    if (s->buckets) {
        uint32_t *bucket = &s->buckets[bucket_of(s, page->address)];
        page->hnext = *bucket;
        *bucket = index;
    }
    page->vprev = 0;
    page->vnext = v->head;
    if (v->head)
        s->pages[v->head - 1].vprev = index;
    v->head = index;
    v->cached++;
}
static void unlink_page(PwWineSection *s, PwWineSectionPage *page)
{
    const uint32_t index = (uint32_t)(page - s->pages) + 1;
    PwWineSectionView *v = page->view;
    if (s->buckets) {
        uint32_t *link = &s->buckets[bucket_of(s, page->address)];
        while (*link && *link != index)
            link = &s->pages[*link - 1].hnext;
        if (*link == index)
            *link = page->hnext;
    }
    if (page->vprev)
        s->pages[page->vprev - 1].vnext = page->vnext;
    else
        v->head = page->vnext;
    if (page->vnext)
        s->pages[page->vnext - 1].vprev = page->vprev;
    v->cached--;
}
static int evict(PwWineSection *s, PwWineSectionPage *page)
{
    const PwWineSectionOps *o = &s->ops;
    if (!page->view)
        return 0;
    if (page->needs_flush && o->flush(o->context, page->address, o->page))
        return failed(s, 2);
    const int laid = o->lay && !o->lay(o->context, page->address, o->page);
    if (!laid && o->unmap(o->context, page->address, o->page))
        return failed(s, 3);
    PwWineSectionView *v = page->view;
    uintptr_t address = page->address;
    unlink_page(s, page);
    memset(page, 0, sizeof(*page));
    s->count--;
    s->evictions++;
    if (!laid && o->reserve(o->context, address, o->page))
    {
        /* The guard could not be restored. Refuse this view from now on;
         * MAP_FIXED must never overwrite an unknown new native mapping. */
        v->failed = 1;
        v->lost_guard = address;
        return failed(s, 4);
    }
    return 0;
}
static void release_held(PwWineSection *s, const size_t *slot, size_t n)
{
    for (size_t i = 0; i < n; i++)
        memset(&s->pages[slot[i]], 0, sizeof(s->pages[0]));
    s->count -= n;
}
int pw_wine_section_fault_run(PwWineSection *s, uintptr_t address, unsigned protection, unsigned access,
                              int bus, PwWineSectionRetry *retry, size_t run)
{
    PwWineSectionView *v = pw_wine_section_find(s, address);
    if (!v || !retry || !access || (access & (access - 1)) || !(protection & access))
        return 0;
    if (v->failed)
        return -1;
    if (s->direct && !v->direct) {
        const int direct = s->direct(s->ops.context, v->fd);
        if (direct < 0)
            return asked_again(s, v, errno);
        v->direct = direct ? 1 : -1;
    }
    if (v->direct > 0)
        return direct_fault(s, v, address, protection, bus, retry);
    const size_t page = s->ops.page;
    uintptr_t at = address & ~(uintptr_t)(page - 1);
    PwWineSectionPage *cached = find_page(s, at);
    if (cached)
    {
        if (bus || !(cached->protection & access) ||
            (retry->address == at && retry->generation == cached->generation))
            return 0;
        *retry = (PwWineSectionRetry){at, cached->generation};
        return 1;
    }
    /* How many pages this fault maps: the run asked for, but not past the view, not over a page that is already cached, and
     * not more than a quarter of the cache. */
    size_t m = run ? run : 1;
    const size_t room = (v->base + v->bytes - at) / page;
    if (m > room)
        m = room;
    if (m > PW_WINE_SECTION_MAX_RUN)
        m = PW_WINE_SECTION_MAX_RUN;
    if (m > s->capacity / 4)
        m = s->capacity / 4;
    if (!m)
        m = 1;
    for (size_t i = 1; i < m; i++)
        if (find_page(s, at + i * page))
        {
            m = i;
            break;
        }
    if (s->ops.foreign && s->ops.foreign(s->ops.context, at, m * page))
        return failed(s, 9);
    /* A record for each page: a free one, or the one the clock hand reaches. They are held (view set, not yet linked) so
     * neither the free search nor the hand takes them again. */
    size_t slot[PW_WINE_SECTION_MAX_RUN];
    for (size_t i = 0; i < m; i++)
    {
        PwWineSectionPage *p = &s->pages[s->hand];
        if (s->count < s->capacity)
        {
            while (p->view)
            {
                s->hand = (s->hand + 1) % s->capacity;
                p = &s->pages[s->hand];
            }
        }
        else
        {
            for (size_t steps = 0; p->generation == HELD && steps < s->capacity; steps++)
            {
                s->hand = (s->hand + 1) % s->capacity;
                p = &s->pages[s->hand];
            }
            if (evict(s, p))
            {
                release_held(s, slot, i);
                return -1;
            }
        }
        slot[i] = s->hand;
        p->view = v;
        p->address = at + i * page;
        p->generation = HELD;
        s->count++;
        s->hand = (s->hand + 1) % s->capacity;
    }
    if (s->ops.map(s->ops.context, at, m * page, protection, v->fd, v->offset + at - v->base))
    {
        /* A failed fixed file mapping may remove the original guard. Restore
         * only with no-overwrite reservation; never retry over an unknown page. */
        v->failed = 1;
        if (s->ops.reserve(s->ops.context, at, m * page))
            v->lost_guard = at;
        release_held(s, slot, m);
        return failed(s, 5);
    }
    for (size_t i = 0; i < m; i++)
    {
        PwWineSectionPage *p = &s->pages[slot[i]];
        *p = (PwWineSectionPage){.address = at + i * page, .view = v, .protection = protection,
                                 .needs_flush = !!(protection & PW_WINE_SECTION_WRITE), .generation = ++s->generation};
        link_page(s, p);
    }
    if (s->count > s->peak)
        s->peak = s->count;
    s->faults += m;
    if (m > 1)
        s->clustered += m - 1;
    *retry = (PwWineSectionRetry){at, s->pages[slot[0]].generation};
    return 1;
}
int pw_wine_section_fault(PwWineSection *s, uintptr_t address, unsigned protection, unsigned access,
                          int bus, PwWineSectionRetry *retry)
{
    return pw_wine_section_fault_run(s, address, protection, access, bus, retry, 1);
}
int pw_wine_section_protect(PwWineSection *s, uintptr_t base, size_t bytes, unsigned protection)
{
    PwWineSectionView *v = pw_wine_section_find(s, base);
    if (!v)
        return 0;
    if (!aligned(s, base, bytes) || bytes > v->bytes - (base - v->base) || v->failed)
        return -1;
    for (uint32_t i = v->head; i; i = s->pages[i - 1].vnext)
    {
        PwWineSectionPage *p = &s->pages[i - 1];
        if (p->address >= base && p->address - base < bytes)
        {
            if (s->ops.protect(s->ops.context, p->address, s->ops.page, protection))
                return failed(s, 6);
            p->protection = protection;
            p->needs_flush |= !!(protection & PW_WINE_SECTION_WRITE);
            p->generation = ++s->generation;
        }
    }
    if (v->direct > 0)
        for (uint64_t c = chunk_index(v, base), last = chunk_index(v, base + bytes - 1); c <= last; c++) {
            size_t span;
            uintptr_t from = chunk_span(v, c, &span), to = from + span;
            if (!chunk_mapped(v, c))
                continue;
            if (from < base)
                from = base;
            if (to > base + bytes)
                to = base + bytes;
            if (s->ops.protect(s->ops.context, from, to - from, protection))
                return failed(s, 6);
        }
    return 1;
}
int pw_wine_section_remove(PwWineSection *s, PwWineSectionView *v)
{
    PwWineSectionView **link = &s->views;
    while (*link && *link != v)
        link = &(*link)->next;
    if (!*link)
        return -1;
    if (v->lost_guard)
        return failed(s, 4);
    while (v->head)
        if (evict(s, &s->pages[v->head - 1]))
            return -1;
    if (v->chunks && v->direct > 0)
        for (uint64_t c = 0, count = chunk_index(v, v->base + v->bytes - 1) + 1; c < count; c++) {
            size_t span;
            const uintptr_t from = chunk_span(v, c, &span);
            if (!chunk_mapped(v, c))
                continue;
            if (unmap_direct(s, v, from, span, 3))
                return -1;
            v->chunks[c / 64] &= ~((uint64_t)1 << (c % 64));
            s->direct_bytes -= span;
        }
    /* Every page is now a guard: descriptor close cannot invalidate a live
     * shared mapping. The caller releases the complete address range. */
    *link = v->next;
    int fd = v->fd;
    v->fd = -1;
    v->next = NULL;
    free(v->chunks);
    v->chunks = NULL;
    if (s->ops.close(s->ops.context, fd))
        return failed(s, 7);
    return 0;
}
