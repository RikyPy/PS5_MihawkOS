/* SPDX-License-Identifier: GPL-3.0-or-later
 * Live DWM thumbnails: area scaling, composition, caching and validation. */
#include "../src/pw_window_effects.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned channel(uint32_t value,unsigned shift) {return (value>>shift)&255;}
static uint32_t reference_scale(const uint32_t *src,unsigned sw,unsigned sh,unsigned dw,unsigned dh,unsigned dx,unsigned dy)
{
    /* Real-valued source span [dx*sw/dw,(dx+1)*sw/dw) floored, at least one pixel. */
    unsigned x0=dx*sw/dw,x1=(dx+1)*sw/dw,y0=dy*sh/dh,y1=(dy+1)*sh/dh;
    if(x1<=x0)x1=x0+1;
    if(y1<=y0)y1=y0+1;
    uint32_t pixel=0;
    for(unsigned c=0;c<32;c+=8) {
        unsigned long sum=0,n=0;
        for(unsigned y=y0;y<y1;++y)for(unsigned x=x0;x<x1;++x) {sum+=channel(src[y*sw+x],c);++n;}
        pixel|=(uint32_t)((sum+n/2)/n)<<c;
    }
    return pixel;
}
enum { W=120, H=80 };
static uint32_t root[W*H],source[W*H],dest[W*H],gpu[W*H],out[W*H],noise[64*48],scaled[64*48];
int main(void)
{
    srand(11);
    for(unsigned i=0;i<64*48;++i)noise[i]=(uint32_t)rand()^((uint32_t)rand()<<16);
    const unsigned sizes[][2]={{64,48},{32,24},{17,9},{64,1},{1,48},{100,70},{7,13}};
    for(unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);++n) {
        static uint32_t big[100*70];
        const unsigned dw=sizes[n][0],dh=sizes[n][1];
        uint32_t *o=dw*dh>64*48 ? big : scaled;
        assert(!pw_window_scale(o,dw,dh,noise,64,48,64*4));
        for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x)assert(o[y*dw+x]==reference_scale(noise,64,48,dw,dh,x,y));
    }
    assert(pw_window_scale(scaled,0,4,noise,64,48,256)<0 && errno==EINVAL);

    /* Source: left half red, right half blue, its GPU client a green square.
     * Destination: grey, with a thumbnail of the whole source window. */
    for(unsigned i=0;i<W*H;++i) {root[i]=0xff101010;dest[i]=0xff808080;gpu[i]=0xff00ff00;
        source[i]=(i%W)<20 ? 0xffff0000 : 0xff0000ff;}
    PwWindowSnapshot s={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(s),.count=3,.thumbnail_count=1};
    s.windows[0]=(PwWindowRow){.hwnd=3,.pid=3,.generation=3,.flags=PW_WINDOW_VISIBLE,
        .left=60,.top=10,.right=110,.bottom=70,.client_left=60,.client_top=10,.client_right=110,.client_bottom=70};
    s.windows[1]=(PwWindowRow){.hwnd=2,.pid=2,.generation=2,.flags=PW_WINDOW_VISIBLE,
        .left=0,.top=0,.right=40,.bottom=40,.client_left=0,.client_top=0,.client_right=40,.client_bottom=40};
    s.windows[2]=(PwWindowRow){.hwnd=1,.pid=1,.generation=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_DESKTOP,.right=W,.bottom=H};
    s.thumbnails[0]=(PwWindowThumbnail){.destination=3,.source=2,.destination_generation=3,.source_generation=2,
        .left=70,.top=20,.right=90,.bottom=40,.source_left=0,.source_top=0,.source_right=40,.source_bottom=40,.opacity=255};
    PwWindowImage images[]={
        {.hwnd=1,.pid=1,.generation=1,.width=W,.height=H,.stride=W*4,.pixels=root,.bytes=sizeof(root),.sequence=1},
        {.hwnd=2,.pid=2,.generation=2,.width=40,.height=40,.stride=W*4,.pixels=source,.bytes=sizeof(source),.sequence=1},
        {.hwnd=3,.pid=3,.generation=3,.width=50,.height=60,.stride=W*4,.pixels=dest,.bytes=sizeof(dest),.sequence=1}};
    out[0]=99;
    assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==ENOMEM && out[0]==99);
    PwWindowEffects effects={0};
    assert(!pw_windows_compose_effects(out,W,H,&s,images,3,&effects));
    /* A 40x40 source in 20x20 averages 2x2 blocks: halves stay pure. */
    assert(out[25*W+75]==0xffff0000 && out[25*W+85]==0xff0000ff && out[15*W+65]==0xff808080 && effects.thumbnail_builds==1);
    /* Static source: the cached image is reused. */
    assert(!pw_windows_compose_effects(out,W,H,&s,images,3,&effects) && effects.thumbnail_builds==1);
    /* Opacity blends once over the destination; source rect selects a part. */
    s.thumbnails[0].opacity=128;s.thumbnails[0].source_left=20;
    assert(!pw_windows_compose_effects(out,W,H,&s,images,3,&effects) && effects.thumbnail_builds==2);
    uint32_t half=0;for(unsigned c=0;c<32;c+=8)half|=((channel(0xff808080,c)*127+channel(0xff0000ff,c)*128+127)/255)<<c;
    assert(out[25*W+75]==half && out[25*W+85]==half);
    s.thumbnails[0].opacity=255;s.thumbnails[0].source_left=0;
    /* A changed source image rebuilds; the GPU plane is part of the source. */
    s.windows[1].flags|=PW_WINDOW_CLIENT;s.windows[1].client_left=20;s.windows[1].client_top=20;s.clip_count=1;
    s.windows[1].clip_count=1;s.clips[0]=(PwWindowClip){20,20,40,40};
    PwWindowImage four[]={images[0],images[1],images[2],
        {.hwnd=2,.pid=2,.generation=2,.width=20,.height=20,.stride=W*4,.plane=PW_WINDOW_GPU,.pixels=gpu,.bytes=sizeof(gpu),.sequence=1}};
    assert(!pw_windows_compose_effects(out,W,H,&s,four,4,&effects) && effects.thumbnail_builds==3);
    assert(out[35*W+85]==0xff00ff00 && out[25*W+85]==0xff0000ff && out[35*W+75]==0xffff0000);
    four[3].sequence=2;
    assert(!pw_windows_compose_effects(out,W,H,&s,four,4,&effects) && effects.thumbnail_builds==4);
    /* Clipped to the destination window, which also rounds it. */
    s.thumbnails[0].left=100;s.thumbnails[0].right=130;s.thumbnails[0].top=50;s.thumbnails[0].bottom=80;
    assert(!pw_windows_compose_effects(out,W,H,&s,four,4,&effects));
    assert(out[60*W+105]!=0xff808080 && out[75*W+105]==root[0] && out[60*W+112]==root[0]);
    s.windows[0].radius=8;
    assert(!pw_windows_compose_effects(out,W,H,&s,four,4,&effects));
    assert(out[69*W+109]==root[0]);
    s.windows[0].radius=0;
    /* A hidden destination draws no thumbnail; a source without images neither. */
    s.thumbnails[0]=(PwWindowThumbnail){.destination=3,.source=2,.destination_generation=3,.source_generation=2,
        .left=70,.top=20,.right=90,.bottom=40,.source_left=0,.source_top=0,.source_right=40,.source_bottom=40,.opacity=255};
    assert(!pw_windows_compose_effects(out,W,H,&s,four+2,1,&effects) && out[25*W+75]==0xff808080);
    /* Malformed or inconsistent thumbnails are refused before drawing. */
    const PwWindowThumbnail good=s.thumbnails[0];
    out[0]=99;
    s.thumbnails[0].source=3;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].source_generation=9;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].destination=1;s.thumbnails[0].destination_generation=1;
    assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].right=s.thumbnails[0].left;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].opacity=256;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].source_right=3000;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].source_left=-1;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnails[0].reserved=1;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    s.thumbnails[0]=good;s.thumbnail_count=PW_WINDOW_THUMBNAIL_LIMIT+1;assert(pw_windows_compose_effects(out,W,H,&s,four,4,&effects)<0 && errno==EINVAL);
    assert(out[0]==99);
    pw_window_effects_release(&effects);
    puts("window thumbnails: PASS area scale vs reference (7 sizes), live composition, cache keys, opacity, source rect, GPU plane, clipping/rounding, validation");
}
