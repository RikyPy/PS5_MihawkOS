/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../src/pw_window_scene.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
static int object(size_t bytes,PwWindowPage **page,uint32_t hwnd,uint64_t gen,uint32_t width,uint32_t height)
{
    int fd=memfd_create("scene-test",MFD_CLOEXEC);assert(fd>=0 && !ftruncate(fd,bytes));
    *page=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(*page!=MAP_FAILED);
    assert(!pw_window_page_init(*page,bytes,hwnd,gen,width,height,0,0));return fd;
}
int main(void)
{
    PwWindowScene scene={0};PwWindowSnapshot snap={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(snap),.count=2,
        .windows={{.hwnd=2,.pid=22,.generation=2,.flags=PW_WINDOW_VISIBLE,.right=2,.bottom=2},
                  {.hwnd=1,.pid=11,.generation=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_DESKTOP,.right=4,.bottom=4}}};
    PwWindowPage *p,*q;size_t bytes=pw_window_page_bytes(4,4);int fd=object(bytes,&p,1,1,4,4);
    uint32_t blue[16],red[4]={3,3,3,3},out[16];for(unsigned i=0;i<16;++i)blue[i]=2;
    assert(!pw_window_page_write(p,blue,16));
    int bad=dup(fd);assert(pw_window_scene_attach(&scene,22,bad,bytes,&snap)<0 && errno==ESTALE);
    assert(fcntl(bad,F_GETFD)<0 && errno==EBADF);
    assert(!pw_window_scene_attach(&scene,11,dup(fd),bytes,&snap));
    int short_fd=memfd_create("short-window-test",MFD_CLOEXEC);assert(short_fd>=0 && !ftruncate(short_fd,128));
    assert(pw_window_scene_attach(&scene,11,short_fd,bytes,&snap)<0 && errno==EPROTO);
    assert(fcntl(short_fd,F_GETFD)<0 && errno==EBADF && scene.mapped_bytes==bytes);
    p->magic=0;assert(pw_window_scene_attach(&scene,11,dup(fd),bytes,&snap)<0 && errno==EPROTO && scene.mapped_bytes==bytes);
    p->magic=PW_WINDOW_PAGE_MAGIC;
    int child=object(bytes,&q,2,2,2,2);assert(!pw_window_page_write(q,red,8));
    assert(!pw_window_scene_attach(&scene,22,dup(child),bytes,&snap));
    assert(pw_window_scene_update(&scene,&snap)==2);
    PwWindowImage images[PW_WINDOW_IMAGE_LIMIT];size_t n=pw_window_scene_images(&scene,images,PW_WINDOW_IMAGE_LIMIT);assert(n==2);
    assert(!pw_windows_compose(out,4,4,&snap,images,n) && out[0]==3 && out[15]==2);
    /* Producer mutations do not change retained content before stable adoption. */
    q->sequence=3;((uint32_t *)(q+1))[0]=99;
    assert(!pw_window_scene_update(&scene,&snap));
    assert(!pw_windows_compose(out,4,4,&snap,images,n) && out[0]==3);
    snap.windows[0].flags=0;assert(!pw_window_scene_update(&scene,&snap));
    assert(!pw_windows_compose(out,4,4,&snap,images,n) && out[0]==2);
    snap.windows[0].generation=3;assert(pw_window_scene_update(&scene,&snap)==1 && scene.mapped_bytes==bytes);
    snap.overflow=1;assert(pw_window_scene_update(&scene,&snap)<0 && errno==EOVERFLOW);
    snap.overflow=0;pw_window_scene_detach(&scene,11);assert(!scene.mapped_bytes && !scene.pixel_bytes);
    pw_window_scene_release(&scene);close(fd);close(child);munmap(p,bytes);munmap(q,bytes);
    puts("window scene extent/ownership/retention/lifetime checks: PASS");
}
