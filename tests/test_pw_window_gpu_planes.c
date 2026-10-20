/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_window_snapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
int main(void)
{
    uint32_t frame[36],gpu[16],popup[4],out[64];
    for(unsigned i=0;i<36;++i)frame[i]=1;
    for(unsigned i=0;i<16;++i)gpu[i]=2;
    for(unsigned i=0;i<4;++i)popup[i]=3;
    PwWindowSnapshot s={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(s),.count=2,.clip_count=1};
    s.clips[0]=(PwWindowClip){2,3,6,6};
    s.windows[0]=(PwWindowRow){.hwnd=2,.pid=32,.generation=2,.flags=PW_WINDOW_VISIBLE,
        .left=3,.top=3,.right=5,.bottom=5};
    s.windows[1]=(PwWindowRow){.hwnd=1,.pid=64,.generation=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_CLIENT,
        .left=1,.top=1,.right=7,.bottom=7,.client_left=2,.client_top=3,.client_right=6,.client_bottom=6,.clip_count=1};
    PwWindowImage images[3]={
        {.hwnd=1,.pid=64,.generation=1,.width=6,.height=6,.stride=24,.pixels=frame,.bytes=sizeof(frame)},
        {.hwnd=1,.pid=64,.generation=1,.plane=PW_WINDOW_GPU,.width=4,.height=4,.stride=16,.pixels=gpu,.bytes=sizeof(gpu)},
        {.hwnd=2,.pid=32,.generation=2,.width=2,.height=2,.stride=8,.pixels=popup,.bytes=sizeof(popup)}};
    assert(!pw_windows_compose(out,8,8,&s,images,3));
    assert(out[1*8+1]==1); /* Real frame/caption remains. */
    assert(out[3*8+2]==2); /* GPU uses server client origin. */
    assert(out[3*8+3]==3); /* Other-process popup covers GPU. */
    assert(out[6*8+2]==1); /* Oversized GPU image cannot overwrite frame. */
    PwWindowRow top=s.windows[0];s.windows[0]=s.windows[1];s.windows[1]=top;
    assert(!pw_windows_compose(out,8,8,&s,images,3) && out[3*8+3]==2);
    s.windows[0].client_left=-2;s.windows[0].client_right=2;
    s.windows[0].client_top=-1;s.windows[0].client_bottom=3;
    s.windows[0].left=-3;s.windows[0].top=-2;
    s.clips[0]=(PwWindowClip){-2,-1,2,3};
    assert(!pw_windows_compose(out,8,8,&s,images,3) && out[0]==2);
    images[1].generation=3;
    assert(!pw_windows_compose(out,8,8,&s,images,3) && out[0]==1);
    images[1]=images[0];out[0]=99;
    assert(pw_windows_compose(out,8,8,&s,images,3)<0 && errno==EINVAL && out[0]==99);
    images[1].plane=3;
    assert(pw_windows_compose(out,8,8,&s,images,3)<0 && errno==EINVAL && out[0]==99);
    puts("GPU client planes: PASS frame retention, actual client clip, cross-process Z order, move, lifetime, malformed rejection");
}
