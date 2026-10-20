/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_window_snapshot.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    uint32_t back[64],front[16],out[64];
    for(unsigned i=0;i<64;++i)back[i]=0x102030;
    for(unsigned i=0;i<16;++i)front[i]=0x405060+i;
    PwWindowSnapshot s={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(s),.count=2,.clip_count=2};
    s.windows[0]=(PwWindowRow){.hwnd=2,.pid=2,.generation=2,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_SHAPED,
        .shape_count=2};
    s.windows[1]=(PwWindowRow){.hwnd=1,.pid=1,.generation=1,.flags=PW_WINDOW_VISIBLE,
        .right=8,.bottom=8};
    PwWindowImage images[]={
        {.hwnd=1,.pid=1,.generation=1,.width=8,.height=8,.stride=32,.pixels=back,.bytes=sizeof(back)},
        {.hwnd=2,.pid=2,.generation=2,.width=4,.height=4,.stride=16,.pixels=front,.bytes=sizeof(front)}};
    /* Oracle is an L-shaped membership predicate, independent of paint/clipping
     * code. Translate across all four output edges and compare every pixel. */
    for(int y=-4;y<=8;++y)for(int x=-4;x<=8;++x) {
        PwWindowRow *row=&s.windows[0];
        row->left=x;row->top=y;row->right=x+4;row->bottom=y+4;
        s.clips[0]=(PwWindowClip){x,y,x+4,y+1};
        s.clips[1]=(PwWindowClip){x,y+1,x+1,y+4};
        assert(!pw_windows_compose(out,8,8,&s,images,2));
        for(int py=0;py<8;++py)for(int px=0;px<8;++px) {
            int dx=px-x,dy=py-y;
            int inside=dx>=0 && dx<4 && dy>=0 && dy<4 && (dx==0 || dy==0);
            assert(out[py*8+px]==(inside?front[dy*4+dx]:back[py*8+px]));
        }
    }
    s.windows[0].left=s.windows[0].top=0;s.windows[0].right=s.windows[0].bottom=4;
    s.windows[0].shape_count=0;
    assert(!pw_windows_compose(out,8,8,&s,images,2) && out[0]==back[0]);
    /* A shape-clearing update restores the retained image without repaint. */
    s.windows[0].flags=PW_WINDOW_VISIBLE;
    assert(!pw_windows_compose(out,8,8,&s,images,2) && out[0]==front[0]);
    s.windows[0].shape_count=1;out[0]=99;
    assert(pw_windows_compose(out,8,8,&s,images,2)<0 && errno==EINVAL && out[0]==99);
    s.windows[0].flags|=PW_WINDOW_SHAPED;s.windows[0].shape_start=2;
    assert(pw_windows_compose(out,8,8,&s,images,2)<0 && errno==EINVAL && out[0]==99);
    s.windows[0].shape_start=0;s.clips[0]=(PwWindowClip){1,0,0,1};
    assert(pw_windows_compose(out,8,8,&s,images,2)<0 && errno==EINVAL && out[0]==99);
    s.clips[0]=(PwWindowClip){0};s.overflow=1;
    assert(pw_windows_compose(out,8,8,&s,images,2)<0 && errno==EOVERFLOW && out[0]==99);
    puts("window shapes: PASS 169 translations, 10816 oracle pixels, empty/cleared shape, malformed/overflow rejection");
}
