/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_window.h"
#include "../src/pw_window_page.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
static int calls,refuse;
static PwWindowPage *adopted;static size_t adopted_bytes;
static int allocate(size_t bytes)
{
    int fd=memfd_create("window-test",MFD_CLOEXEC);assert(fd>=0 && !ftruncate(fd,bytes));return fd;
}
int pw_ps5_child_window(int fd,size_t bytes)
{
    ++calls;assert(fcntl(fd,F_GETFD)>=0);
    if(refuse) {errno=ENOSPC;return -1;}
    PwWindowPage *p=mmap(NULL,bytes,PROT_READ,MAP_SHARED,fd,0),h;assert(p!=MAP_FAILED);
    assert(!pw_window_page_header(p,bytes,&h) && p->sequence==2);
    if(adopted)munmap(adopted,adopted_bytes);
    adopted=p;adopted_bytes=bytes;return 0;
}
int main(void)
{
    uint32_t pixels[4]={1,2,3,4};
    assert(pw_wine_window_present(1,1,pixels,2,2,8,0,0)<0);
    pw_wine_window_set_allocator(allocate);
    assert(!pw_wine_window_present(1,1,pixels,2,2,8,0,0) && calls==1);
    assert(!pw_wine_window_present(1,1,pixels,2,2,8,0,0) && calls==1 && adopted->sequence==4);
    refuse=1;assert(pw_wine_window_present(1,1,pixels,1,2,8,0,0)<0 && errno==ENOSPC);
    assert(adopted->width==2 && adopted->sequence==4);
    /* Refused replacement preserves the old producer, which still updates. */
    assert(!pw_wine_window_present(1,1,pixels,2,2,8,0,0) && adopted->sequence==6);
    refuse=0;assert(!pw_wine_window_present(1,1,pixels,1,2,8,-1,2) && adopted->width==1);
    pw_wine_window_drop(1);assert(!pw_wine_window_present(1,2,pixels,2,2,8,0,0) && adopted->generation==2);
    const unsigned char shape[2]={0x80,0x40};
    assert(!pw_wine_window_present_surface(1,2,pixels,2,2,8,0,0,0,128,shape,1,2));
    assert(adopted->flags==PW_WINDOW_PAGE_PREMULTIPLIED);
    const uint32_t *alpha_pixels=(const uint32_t *)(adopted+1);
    assert(alpha_pixels[0]==0x80000001 && !alpha_pixels[1] && !alpha_pixels[2] && alpha_pixels[3]==0x80000002);
    uint32_t sequence=adopted->sequence;
    assert(pw_wine_window_present_surface(1,2,pixels,2,2,8,0,0,0,128,shape,1,1)<0);
    assert(adopted->sequence==sequence);
    assert(!pw_wine_window_present(1,2,pixels,2,2,8,0,0) && !adopted->flags);
    for(unsigned n=2;n<=128;++n)assert(!pw_wine_window_present(n,n,pixels,2,2,8,0,0));
    assert(pw_wine_window_present(129,129,pixels,2,2,8,0,0)<0 && errno==ENOSPC);
    pw_wine_window_release();munmap(adopted,adopted_bytes);
    puts("window producer ownership/replacement/capacity checks: PASS");
}
