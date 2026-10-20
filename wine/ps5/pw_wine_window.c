/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_window.h"
#include "pw_wine_pages.h"
#include "../../src/pw_window_page.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
/* Linked beside process transport in ntdll; host oracle supplies the same
 * descriptor ownership/adoption contract. Attach borrows fd and returns ACK. */
int pw_ps5_child_window(int fd,size_t bytes) __attribute__((weak));
static pthread_mutex_t window_lock=PTHREAD_MUTEX_INITIALIZER;
static int (*window_allocate)(size_t);
static struct { uint32_t hwnd;PwWindowPage *page;size_t bytes; } windows[128];
static size_t live_bytes;
enum { PW_WINDOW_MEMORY_LIMIT=64u<<20 };
void pw_wine_window_set_allocator(int (*allocate)(size_t)) {window_allocate=allocate;}
static void release_slot(unsigned n)
{
    if(windows[n].page) {pw_wine_pages_unmap(windows[n].page,windows[n].bytes);live_bytes-=windows[n].bytes;}
    windows[n].hwnd=0;windows[n].page=NULL;windows[n].bytes=0;
}
struct surface_alpha {
    uint32_t mask,opacity,stride;
    const void *shape;
    size_t bytes;
};
static int write_surface(PwWindowPage *page,const void *pixels,uint32_t stride,const struct surface_alpha *alpha)
{
    if(!alpha)return pw_window_page_write(page,pixels,stride);
    return pw_window_page_write_alpha(page,pixels,stride,alpha->mask,alpha->opacity,
                                      alpha->shape,alpha->stride,alpha->bytes);
}
static int present_surface(uint32_t hwnd,uint64_t generation,const void *pixels,
                           uint32_t width,uint32_t height,uint32_t stride,int32_t offset_x,int32_t offset_y,
                           const struct surface_alpha *alpha)
{
    const size_t bytes=pw_window_page_bytes(width,height);
    if(!hwnd || !generation || !pixels || !bytes || stride<width*4) {errno=EINVAL;return -1;}
    pthread_mutex_lock(&window_lock);
    unsigned slot=128,empty=128;
    for(unsigned n=0;n<128;++n) {
        if(windows[n].hwnd==hwnd) {slot=n;break;}
        if(!windows[n].hwnd && empty==128)empty=n;
    }
    if(slot==128)slot=empty;
    int status=-1;
    if(slot==128) {errno=ENOSPC;goto done;}
    PwWindowPage *page=windows[slot].page;
    if(!page || page->generation!=generation || page->width!=width || page->height!=height ||
       page->offset_x!=offset_x || page->offset_y!=offset_y ||
       page->flags!=(alpha ? PW_WINDOW_PAGE_PREMULTIPLIED : 0)) {
        if(!window_allocate || !pw_ps5_child_window) {errno=ENOTCONN;goto done;}
        /* During replacement old and new backing both remain live until ACK. */
        if(bytes>PW_WINDOW_MEMORY_LIMIT-live_bytes) {errno=ENOSPC;goto done;}
        int fd=window_allocate(bytes);if(fd<0)goto done;
        PwWindowPage *next=pw_wine_pages_map(bytes,PROT_READ|PROT_WRITE,fd);
        if(next==MAP_FAILED) {int saved=errno;close(fd);errno=saved;goto done;}
        int result=pw_window_page_init(next,bytes,hwnd,generation,width,height,offset_x,offset_y);
        /* Publish a complete first image before handing the descriptor over. */
        if(!result) {
            next->flags=alpha ? PW_WINDOW_PAGE_PREMULTIPLIED : 0;
            result=write_surface(next,pixels,stride,alpha);
        }
        if(!result)result=pw_ps5_child_window(fd,bytes);
        int saved=errno;close(fd);
        if(result) {pw_wine_pages_unmap(next,bytes);errno=saved;goto done;}
        release_slot(slot);windows[slot].hwnd=hwnd;windows[slot].page=next;windows[slot].bytes=bytes;live_bytes+=bytes;
        status=0;goto done;
    }
    status=write_surface(page,pixels,stride,alpha);
done:
    pthread_mutex_unlock(&window_lock);return status;
}
int pw_wine_window_present(uint32_t hwnd,uint64_t generation,const void *pixels,
                           uint32_t width,uint32_t height,uint32_t stride,int32_t offset_x,int32_t offset_y)
{
    return present_surface(hwnd,generation,pixels,width,height,stride,offset_x,offset_y,NULL);
}
int pw_wine_window_present_surface(uint32_t hwnd,uint64_t generation,const void *pixels,
                           uint32_t width,uint32_t height,uint32_t stride,int32_t offset_x,int32_t offset_y,
                           uint32_t alpha_mask,uint32_t opacity,const void *shape,
                           uint32_t shape_stride,size_t shape_bytes)
{
    const struct surface_alpha alpha={alpha_mask,opacity,shape_stride,shape,shape_bytes};
    if(!alpha_mask && opacity==255 && !shape && !shape_stride && !shape_bytes)
        return present_surface(hwnd,generation,pixels,width,height,stride,offset_x,offset_y,NULL);
    return present_surface(hwnd,generation,pixels,width,height,stride,offset_x,offset_y,&alpha);
}
void pw_wine_window_drop(uint32_t hwnd)
{
    pthread_mutex_lock(&window_lock);
    for(unsigned n=0;n<128;++n)if(windows[n].hwnd==hwnd) {release_slot(n);break;}
    pthread_mutex_unlock(&window_lock);
}
void pw_wine_window_release(void)
{
    pthread_mutex_lock(&window_lock);
    for(unsigned n=0;n<128;++n)release_slot(n);
    pthread_mutex_unlock(&window_lock);
}
