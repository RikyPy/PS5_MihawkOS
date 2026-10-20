/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_window_snapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>
int main(void)
{
 uint32_t blue[16],green[16],red[4],desktop[36],out[36];
 for(unsigned i=0;i<16;++i){blue[i]=1;green[i]=2;}for(unsigned i=0;i<4;++i)red[i]=3;for(unsigned i=0;i<36;++i)desktop[i]=4;
 PwWindowSnapshot snapshot={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(snapshot),.count=4,.sequence=1,.windows={
   {.hwnd=102,.flags=PW_WINDOW_VISIBLE,.pid=64,.windows_pid=64,.left=2,.top=2,.right=4,.bottom=4,.generation=3},
   {.hwnd=201,.flags=PW_WINDOW_VISIBLE,.pid=32,.windows_pid=32,.left=1,.top=1,.right=5,.bottom=5,.generation=2},
   {.hwnd=101,.flags=PW_WINDOW_VISIBLE,.pid=64,.windows_pid=64,.right=4,.bottom=4,.generation=1},
   {.hwnd=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_DESKTOP,.pid=60,.windows_pid=60,.right=6,.bottom=6,.generation=4}}};
 PwWindowImage images[]={ {101,4,4,16,0,0,0,64,1,blue,sizeof(blue),0,0}, {201,4,4,16,0,0,0,32,2,green,sizeof(green),0,0},
                         {102,2,2,8,0,0,0,64,3,red,sizeof(red),0,0}, {1,6,6,24,0,0,0,60,4,desktop,sizeof(desktop),0,0}};
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4));assert(out[0]==1 && out[7]==2 && out[14]==3 && out[35]==4);
 /* Same-process windows can straddle a second process in authoritative Z order. */
 PwWindowRow tmp=snapshot.windows[0];snapshot.windows[0]=snapshot.windows[2];snapshot.windows[2]=tmp;
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4) && out[14]==1);
 snapshot.windows[0].flags=0;
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4) && out[14]==2);
 /* A stale surface must not paint a reused HWND or a different process. */
 snapshot.windows[1].generation=99;
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4) && out[7]==4);
 snapshot.windows[1].generation=2;snapshot.windows[1].pid=65;
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4) && out[7]==4);
 snapshot.windows[1].pid=32;snapshot.windows[1].left=-2;snapshot.windows[1].top=-2;snapshot.windows[1].right=2;snapshot.windows[1].bottom=2;
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4) && out[0]==2 && out[2]==4);
 /* Extreme signed coordinates cannot wrap into an in-bounds source read. */
 snapshot.windows[1].left=INT_MIN;snapshot.windows[1].right=INT_MAX;snapshot.windows[1].top=INT_MIN;snapshot.windows[1].bottom=INT_MAX;
 assert(!pw_windows_compose(out,6,6,&snapshot,images,4) && out[0]==4);
 snapshot.overflow=1;out[0]=99;
 assert(pw_windows_compose(out,6,6,&snapshot,images,4)<0 && errno==EOVERFLOW && out[0]==99);
 snapshot.overflow=0;images[0].bytes=4;
 assert(pw_windows_compose(out,6,6,&snapshot,images,4)<0 && errno==EINVAL && out[0]==99);
 puts("pw_windows_compose isolated host checks: PASS");
}
