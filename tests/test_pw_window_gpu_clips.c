/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_window_scene.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
int main(void)
{
    uint32_t frame[64],gpu[36],child[4],out[64];
    for(unsigned i=0;i<64;++i)frame[i]=1;
    for(unsigned i=0;i<36;++i)gpu[i]=2;
    for(unsigned i=0;i<4;++i)child[i]=3;
    PwWindowSnapshot s={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(s),.count=2,.clip_count=3};
    s.windows[0]=(PwWindowRow){.hwnd=2,.pid=32,.generation=2,
        .flags=PW_WINDOW_VISIBLE|PW_WINDOW_CLIENT|PW_WINDOW_CHILD,
        .left=3,.top=3,.right=5,.bottom=5,.client_left=3,.client_top=3,.client_right=5,.client_bottom=5,
        .clip_start=2,.clip_count=1};
    s.windows[1]=(PwWindowRow){.hwnd=1,.pid=64,.generation=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_CLIENT,
        .right=8,.bottom=8,.client_left=1,.client_top=1,.client_right=7,.client_bottom=7,
        .clip_start=0,.clip_count=2};
    /* Canonical Wine clips leave a GDI control at x=5..7 and the child hole. */
    s.clips[0]=(PwWindowClip){1,1,5,3};s.clips[1]=(PwWindowClip){1,5,5,7};
    s.clips[2]=(PwWindowClip){3,3,5,5};
    PwWindowImage images[3]={
        {.hwnd=1,.pid=64,.generation=1,.width=8,.height=8,.stride=32,.pixels=frame,.bytes=sizeof(frame)},
        {.hwnd=1,.pid=64,.generation=1,.plane=PW_WINDOW_GPU,.width=6,.height=6,.stride=24,.pixels=gpu,.bytes=sizeof(gpu)},
        {.hwnd=2,.pid=32,.generation=2,.plane=PW_WINDOW_GPU,.width=2,.height=2,.stride=8,.pixels=child,.bytes=sizeof(child)}};
    assert(!pw_windows_compose(out,8,8,&s,images,3));
    assert(out[0]==1 && out[1*8+1]==2 && out[1*8+5]==1 && out[3*8+3]==3 && out[5*8+1]==2);
    s.windows[0].flags&=~PW_WINDOW_VISIBLE;
    assert(!pw_windows_compose(out,8,8,&s,images,3) && out[3*8+3]==1);
    s.windows[0].flags|=PW_WINDOW_VISIBLE;
    images[2].pid=65;
    assert(!pw_windows_compose(out,8,8,&s,images,3) && out[3*8+3]==1);
    images[2].pid=32;
    s.windows[0].clip_count=0;
    assert(!pw_windows_compose(out,8,8,&s,images,3) && out[3*8+3]==1);
    s.windows[0].clip_count=2;out[0]=99;
    assert(pw_windows_compose(out,8,8,&s,images,3)<0 && errno==EINVAL && out[0]==99);
    s.windows[0].clip_count=1;s.clips[2].right=2;
    assert(pw_windows_compose(out,8,8,&s,images,3)<0 && errno==EINVAL && out[0]==99);
    s.clips[2].right=5;s.clip_count=PW_WINDOW_CLIP_LIMIT+1;
    assert(pw_windows_compose(out,8,8,&s,images,3)<0 && errno==EINVAL && out[0]==99);
    puts("GPU clip planes: PASS GDI frame/control preservation, nested cross-process content, hidden/stale/empty clips, bounds before output mutation");
}
