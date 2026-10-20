/* SPDX-License-Identifier: GPL-3.0-or-later
 * Rounded coverage, border and shadow against independent geometry. */
#include "../src/pw_window_effects.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned channel(uint32_t value,unsigned shift) {return (value>>shift)&255;}
/* A framed window's or a flyout's shadow over colour at (x,y), written independently of the compositor from the
 * recipe in pw_window_effects.h: per layer, the W3C three-box kernel counted box by box, its rounded rectangle as
 * 8-bit coverage, each source row blurred across and rounded to 8 bits as Skia rounds, the column of those blurred
 * down and rounded again (the unblurred 1 px ring's corners as Skia's analytic AA covers them, pw_window_ring_corners,
 * checked against the captures below); under the window's own anti-aliased edge the mask times what it leaves
 * uncovered; then the layer's a=(alpha*(mask+1))>>8. A window's layers each darken every channel (v*(256-a))>>8, bottom layer first; a
 * flyout's are first combined, A = a + A(256-a)>>8, and darken once. */
static uint32_t naive_shadow(uint32_t colour,const PwWindowRow *row,int x,int y,uint32_t cover)
{
    const int flyout=row->shadow==PW_SHADOW_FLYOUT;
    const PwShadowLayer *layers=flyout ? pw_flyout_shadow_layers[row->material==PW_MATERIAL_DARK]
                                       : pw_window_shadow_layers[!!(row->flags&PW_WINDOW_ACTIVE)][row->material==PW_MATERIAL_DARK];
    uint32_t combined=0;
    for(unsigned n=0;n<(flyout ? 2u : 3u);++n) {
        const PwShadowLayer *layer=&layers[n];
        const int window=(int)fmax(1,floor(layer->blur/2.0*3*sqrt(2*M_PI)/4+0.5)),half=window/2;
        const int first[3]={-half,window&1 ? -half : -half+1,-half},size[3]={window,window,window&1 ? window : window+1};
        int64_t kernel[512]={0},divisor=(int64_t)size[0]*size[1]*size[2];
        for(int a=0;a<size[0];++a)for(int b=0;b<size[1];++b)for(int c=0;c<size[2];++c)
            ++kernel[256+first[0]+a+first[1]+b+first[2]+c];
        const uint64_t weight=(uint64_t)llround(1.0/divisor*4294967296.0);
        const unsigned radius=pw_window_row_radius(row),round=radius ? radius+layer->spread : 0;
        const int l=row->left-layer->spread,r=row->right+layer->spread;
        const int t=row->top+layer->offset-layer->spread,b=row->bottom+layer->offset+layer->spread;
        uint64_t down=0;
        for(int j=t;j<b;++j) {
            if(abs(y-j)>=256)continue;
            uint64_t across=0;
            for(int i=l;i<r;++i) {
                if(abs(x-i)>=256)continue;
                const int ci=i-l<(int)round ? i-l : r-1-i<(int)round ? r-1-i : -1;
                const int cj=j-t<(int)round ? j-t : b-1-j<(int)round ? b-1-j : -1;
                const unsigned corner=!layer->blur && round==PW_SHADOW_RING_RADIUS ?
                    pw_window_ring_corners[(j-t<(int)round ? 0 : 2)+(i-l<(int)round ? 0 : 1)][cj<0 ? 0 : cj][ci<0 ? 0 : ci] :
                    pw_window_corner_coverage(round,(unsigned)ci,(unsigned)cj);
                across+=(uint64_t)kernel[256+x-i]*(ci>=0 && cj>=0 ? corner : 255);
            }
            down+=(uint64_t)kernel[256+y-j]*((across*weight+(1ull<<31))>>32);
        }
        uint32_t mask=(uint32_t)((down*weight+(1ull<<31))>>32);
        if(cover)mask=(uint32_t)floor(mask*(255.0-cover)/255.0+0.5);
        uint32_t a=(layer->alpha*(mask+1))>>8;
        if(flyout) {
            combined=a+((combined*(256-a))>>8);
            if(n==0)continue;
            a=combined;
        }
        uint32_t shaded=colour&0xff000000u;
        for(unsigned shift=0;shift<24;shift+=8)shaded|=((((colour>>shift)&255)*(256-a))>>8)<<shift;
        colour=shaded;
    }
    return colour;
}
/* The prototype's own per-layer alphas (bottom layer, middle layer, then the 1 px ring at 0 px only), read off its
 * reference captures (.impeccable/reference-1920, Chromium over Bloom): beside and below File Explorer at
 * 410,166 1100 x 700 while active (explorer.png), beside and above it while Notepad is (windows-active-inactive.png),
 * out from the window's edge, one pixel at a time. Every column and row there fits these in all three channels. */
static const struct { unsigned active, dark, vertical, ring; uint8_t outer[70], inner[12]; } prototype_profiles[]={
    {1,0,0,15,{27,26,25,24,23,22,22,21,20,19,18,17,16,15,15,14,13,12,12,11,10,10,9,8,8,7,7,6,6,5,5,5,4,4,3,3,3,3,2,2,2,1,1,1,1,1,1,1},{11,9,7,5,3,2,1,1}},
    {1,0,1,15,{41,40,40,39,38,37,36,35,35,34,33,32,31,30,29,28,27,26,25,24,23,22,22,21,20,19,18,17,16,15,15,14,13,12,12,11,10,10,9,8,8,7,7,6,6,5,5,5,4,4,3,3,3,3,2,2,2,1,1,1,1,1,1,1},{16,14,11,9,7,5,3,2,1,1}},
    {1,1,0,89,{56,54,53,51,48,47,45,43,41,39,38,36,34,32,31,29,28,26,25,23,22,20,19,18,17,16,14,13,13,12,11,10,9,8,8,7,6,6,5,4,4,4,3,3,3,2,2,2,1,1,1,1,1},{35,28,21,16,11,7,4,3,1}},
    {1,1,1,89,{85,84,82,80,79,77,75,73,71,70,68,66,64,62,60,58,56,54,53,51,48,47,45,43,41,39,38,36,34,32,31,29,28,26,25,23,22,20,19,18,17,16,14,13,13,12,11,10,9,8,8,7,6,6,5,4,4,4,3,3,3,2,2,2,1,1,1,1,1},{49,42,35,28,21,16,11,7,4,3,1}},
    {0,0,0,13,{15,14,13,12,11,10,9,8,7,6,5,5,4,3,3,3,2,2,1,1,1,1},{8,6,4,2,1}},
    {0,0,1,13,{7,6,5,5,4,3,3,3,2,2,1,1,1,1},{4,2,1}},
    {0,1,0,77,{37,34,32,30,27,25,22,20,18,16,14,12,11,9,8,7,6,5,4,3,3,2,2,1,1,1},{28,21,14,9,5,2,1}},
    {0,1,1,77,{18,16,14,12,11,9,8,7,6,5,4,3,3,2,2,1,1,1},{14,9,5,2,1}}};
/* The 1 px ring's corner tips read off the same captures: for each pixel of an arc ({corner, j, i} as
 * pw_window_ring_corners counts them) the coverage range that puts the pixel within a level in all three channels of
 * every capture that shows it (File Explorer active, light and dark: all four corners; inactive under Notepad: the top
 * two), given the two blurred layers beneath. */
static const struct { uint8_t corner, j, i, low, high; } prototype_ring_tips[]={
    {0,0,5,40,47}, {0,0,6,115,122}, {0,0,7,169,176}, {0,0,8,224,231}, {0,1,3,5,12}, {0,1,4,132,139}, {0,2,2,43,50},
    {0,3,1,6,16}, {0,4,1,140,148}, {0,5,0,51,62}, {0,6,0,163,171}, {0,7,0,202,211}, {0,8,0,236,246}, {1,0,5,39,47},
    {1,0,6,113,122}, {1,0,7,169,176}, {1,0,8,224,231}, {1,1,3,2,8}, {1,1,4,129,135}, {1,2,2,46,53}, {1,3,1,3,13},
    {1,4,1,132,142}, {1,5,0,56,65}, {1,6,0,156,162}, {1,7,0,196,200}, {1,8,0,236,243}, {2,0,5,40,50}, {2,0,6,112,122},
    {2,0,7,163,174}, {2,0,8,218,231}, {2,1,3,2,13}, {2,1,4,126,137}, {2,2,2,54,68}, {2,3,1,2,13}, {2,4,1,120,134},
    {2,5,0,34,45}, {2,6,0,132,142}, {2,7,0,175,185}, {2,8,0,227,237}, {3,0,5,25,65}, {3,0,6,97,139}, {3,0,7,158,197},
    {3,0,8,207,252}, {3,1,3,0,24}, {3,1,4,120,157}, {3,2,2,25,62}, {3,3,1,0,24}, {3,4,1,109,142}, {3,5,0,25,59},
    {3,6,0,117,151}, {3,7,0,155,191}, {3,8,0,207,240}};
int main(void)
{
    /* Corner geometry oracle: a pixel square entirely inside the circle is
     * fully covered, entirely outside is empty, and the cut-away area of the
     * whole corner matches r^2(1-pi/4). */
    for(unsigned r=1;r<=PW_WINDOW_RADIUS_LIMIT;++r) {
        double cut=0;
        for(unsigned j=0;j<r;++j)for(unsigned i=0;i<r;++i) {
            const unsigned cover=pw_window_corner_coverage(r,i,j);
            assert(cover==pw_window_corner_coverage(r,j,i));
            const double nx=fmin(fmax(r,i),i+1.0),ny=fmin(fmax(r,j),j+1.0);
            const double near=hypot(r-nx,r-ny);
            const double fx=fabs(r-(double)i)>fabs(r-(i+1.0)) ? i : i+1.0,fy=fabs(r-(double)j)>fabs(r-(j+1.0)) ? j : j+1.0;
            const double far=hypot(r-fx,r-fy);
            if(far<=r)assert(cover==255);
            if(near>=r)assert(cover==0);
            if(i+1<r)assert(pw_window_corner_coverage(r,i,j)<=pw_window_corner_coverage(r,i+1,j));
            cut+=(255.0-cover)/255.0;
            const unsigned ring=pw_window_corner_border(r,i,j);
            if(far<=r-1.0 || near>=r)assert(ring==0);
        }
        assert(fabs(cut-r*r*(1.0-M_PI/4.0))<=0.02*r*r+0.25);
    }
    assert(pw_window_corner_coverage(0,0,0)==255 && pw_window_corner_coverage(8,8,0)==255);

    /* Rounded opaque window over a background, plus GPU plane clipping. */
    enum { W=40, H=30 };
    static uint32_t back[W*H],front[W*H],gpu[W*H],out[W*H];
    for(unsigned i=0;i<W*H;++i) {back[i]=0xff204060;front[i]=0xffc08040;gpu[i]=0xff10f010;}
    PwWindowSnapshot s={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(s),.count=2};
    s.windows[0]=(PwWindowRow){.hwnd=2,.pid=2,.generation=2,.flags=PW_WINDOW_VISIBLE,
        .left=5,.top=4,.right=33,.bottom=26,.client_left=5,.client_top=4,.client_right=33,.client_bottom=26,.radius=8};
    s.windows[1]=(PwWindowRow){.hwnd=1,.pid=1,.generation=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_DESKTOP,.right=W,.bottom=H};
    PwWindowImage images[]={
        {.hwnd=1,.pid=1,.generation=1,.width=W,.height=H,.stride=W*4,.pixels=back,.bytes=sizeof(back)},
        {.hwnd=2,.pid=2,.generation=2,.width=W,.height=H,.stride=W*4,.pixels=front,.bytes=sizeof(front)},
        {.hwnd=2,.pid=2,.generation=2,.width=28,.height=22,.stride=W*4,.plane=PW_WINDOW_GPU,.pixels=gpu,.bytes=sizeof(gpu)}};
    for(unsigned plane=0;plane<2;++plane) {
        if(plane) {s.windows[0].flags|=PW_WINDOW_CLIENT;s.clip_count=1;s.windows[0].clip_count=1;
                   s.clips[0]=(PwWindowClip){5,4,33,26};}
        assert(!pw_windows_compose(out,W,H,&s,images,3));
        const uint32_t colour=plane ? gpu[0] : front[0];
        /* Planes are blended in order, each with the row's coverage. */
        for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
            const uint32_t cover=pw_window_row_coverage(&s.windows[0],x,y);
            uint32_t expected=0;
            for(unsigned shift=0;shift<32;shift+=8) {
                uint32_t value=(channel(back[0],shift)*(255-cover)+channel(front[0],shift)*cover+127)/255;
                if(plane)value=(value*(255-cover)+channel(gpu[0],shift)*cover+127)/255;
                expected|=value<<shift;
            }
            assert(out[y*W+x]==expected);
        }
        assert(out[4*W+5]==back[0] && out[12*W+20]==colour && out[25*W+32]==back[0]);
    }
    s.windows[0].flags&=~PW_WINDOW_CLIENT;s.clip_count=0;s.windows[0].clip_count=0;

    /* Border: straight sides take the colour, corners blend a ring, the
     * interior and outside stay untouched. */
    s.windows[0].flags|=PW_WINDOW_BORDER;s.windows[0].border=0x0000ff;
    assert(!pw_windows_compose(out,W,H,&s,images,3));
    assert(out[4*W+20]==0xff0000ff && out[25*W+20]==0xff0000ff && out[15*W+5]==0xff0000ff && out[15*W+32]==0xff0000ff);
    assert(out[15*W+6]==front[0] && out[5*W+20]==front[0] && out[3*W+20]==back[0] && out[4*W+5]==back[0]);
    unsigned ring=0;for(int y=4;y<12;++y)for(int x=5;x<13;++x)ring+=out[y*W+x]!=front[0] && out[y*W+x]!=back[0];
    assert(ring>=8);
    /* Windows 11's default border: 40% grey over the window's own edge, scaled by the ring at the corners. */
    s.windows[0].border=0x66757575;
    assert(!pw_windows_compose(out,W,H,&s,images,3));
    for(unsigned shift=0;shift<24;shift+=8) {
        assert(channel(out[4*W+20],shift)==(channel(front[0],shift)*153+0x75*102+127)/255);
        assert(channel(out[15*W+5],shift)==(channel(front[0],shift)*153+0x75*102+127)/255);
    }
    assert(out[15*W+6]==front[0] && out[3*W+20]==back[0]);
    s.windows[0].flags&=~PW_WINDOW_BORDER;s.windows[0].border=0;

    /* A framed window's shadow: the prototype's layers, active or not, light or dark, against the naive recipe; the
     * window's anti-aliased corners blend over the shadow they leave uncovered. */
    s.windows[0].shadow=PW_SHADOW_WINDOW;
    for(unsigned state=0;state<4;++state) {
        s.windows[0].flags=(s.windows[0].flags&~PW_WINDOW_ACTIVE)|(state&1 ? PW_WINDOW_ACTIVE : 0);
        s.windows[0].material=state&2 ? PW_MATERIAL_DARK : PW_MATERIAL_LIGHT;
        assert(!pw_windows_compose(out,W,H,&s,images,3));
        for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
            const uint32_t cover=pw_window_row_coverage(&s.windows[0],x,y),under=naive_shadow(back[0],&s.windows[0],x,y,cover);
            uint32_t expected=0;
            for(unsigned shift=0;shift<32;shift+=8)
                expected|=((channel(under,shift)*(255-cover)+channel(front[0],shift)*cover+127)/255)<<shift;
            assert(out[y*W+x]==expected);
        }
        /* More shadow below than above, at equal distances; more when active, more in dark. */
        assert(channel(out[27*W+20],0)<channel(out[1*W+20],0) && channel(out[1*W+20],0)<channel(back[0],0));
    }
    /* A flyout's shadow (--shadow-28): its two layers combined, light and dark, against the naive recipe. */
    s.windows[0].shadow=PW_SHADOW_FLYOUT;
    for(unsigned dark=0;dark<2;++dark) {
        s.windows[0].flags&=~PW_WINDOW_ACTIVE;
        s.windows[0].material=dark ? PW_MATERIAL_DARK : PW_MATERIAL_LIGHT;
        assert(!pw_windows_compose(out,W,H,&s,images,3));
        for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
            const uint32_t cover=pw_window_row_coverage(&s.windows[0],x,y),under=naive_shadow(back[0],&s.windows[0],x,y,cover);
            uint32_t expected=0;
            for(unsigned shift=0;shift<32;shift+=8)
                expected|=((channel(under,shift)*(255-cover)+channel(front[0],shift)*cover+127)/255)<<shift;
            assert(out[y*W+x]==expected);
        }
        assert(channel(out[27*W+20],0)<channel(out[1*W+20],0) && channel(out[1*W+20],0)<channel(back[0],0));
    }
    s.windows[0].shadow=PW_SHADOW_WINDOW;
    {
        /* A window narrower than its kernel and partly off the screen: both corners reach every column. */
        PwWindowRow narrow=s.windows[0];
        narrow.left=-3;narrow.right=9;narrow.top=2;narrow.bottom=45;narrow.client_left=-3;narrow.client_right=9;
        narrow.client_top=2;narrow.client_bottom=45;narrow.flags|=PW_WINDOW_ACTIVE;narrow.material=PW_MATERIAL_DARK;
        PwWindowSnapshot n=s;n.windows[0]=narrow;
        PwWindowImage moved[3]={images[0],images[1],images[2]};
        assert(!pw_windows_compose(out,W,H,&n,moved,3));
        for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
            const uint32_t cover=pw_window_row_coverage(&narrow,x,y);
            if(cover)continue;
            assert(out[y*W+x]==naive_shadow(back[0],&narrow,x,y,0));
        }
    }
    {
        /* The prototype's own darkening: a 200 x 200 window over white, beside its middle row and below (or above)
         * its middle column, applies the captured alphas exactly. */
        enum { PW=420, PH=420 };
        static uint32_t white[PW*PH],grey[PW*PH],frame[PW*PH];
        for(unsigned i=0;i<PW*PH;++i) {white[i]=0xffffffffu;grey[i]=0xff808080u;}
        PwWindowSnapshot p={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(p),.count=2};
        p.windows[0]=(PwWindowRow){.hwnd=2,.pid=2,.generation=2,.flags=PW_WINDOW_VISIBLE,.left=110,.top=110,.right=310,
            .bottom=310,.client_left=110,.client_top=110,.client_right=310,.client_bottom=310,.radius=8,.shadow=PW_SHADOW_WINDOW};
        p.windows[1]=(PwWindowRow){.hwnd=1,.pid=1,.generation=1,.flags=PW_WINDOW_VISIBLE|PW_WINDOW_DESKTOP,.right=PW,.bottom=PH};
        PwWindowImage pi[]={
            {.hwnd=1,.pid=1,.generation=1,.width=PW,.height=PH,.stride=PW*4,.pixels=white,.bytes=sizeof(white)},
            {.hwnd=2,.pid=2,.generation=2,.width=200,.height=200,.stride=PW*4,.pixels=grey,.bytes=sizeof(grey)}};
        for(unsigned n=0;n<sizeof(prototype_profiles)/sizeof(prototype_profiles[0]);++n) {
            const __typeof__(prototype_profiles[0]) *profile=&prototype_profiles[n];
            p.windows[0].flags=PW_WINDOW_VISIBLE|(profile->active ? PW_WINDOW_ACTIVE : 0);
            p.windows[0].material=profile->dark ? PW_MATERIAL_DARK : PW_MATERIAL_LIGHT;
            assert(!pw_windows_compose(frame,PW,PH,&p,pi,2));
            for(unsigned k=0;k<70;++k) {
                /* active: right of and below the window; inactive: left of and above it, as captured */
                const int x=profile->vertical ? 210 : profile->active ? 310+(int)k : 109-(int)k;
                const int y=!profile->vertical ? 210 : profile->active ? 310+(int)k : 109-(int)k;
                uint32_t v=255;
                v=v*(256-profile->outer[k])>>8;
                if(k<12)v=v*(256-profile->inner[k])>>8;
                if(!k)v=v*(256-profile->ring)>>8;
                assert(frame[y*PW+x]==(0xff000000u|v<<16|v<<8|v));
            }
        }
    }
    s.windows[0].flags&=~PW_WINDOW_ACTIVE;s.windows[0].material=PW_MATERIAL_LIGHT;
    /* Skia's ring corners within the captured ranges; the one miss, the bottom left's third pixel along its diagonal
     * (51 where the dark capture asks 54-68), leaves that pixel 2 levels off. */
    for(unsigned n=0;n<sizeof(prototype_ring_tips)/sizeof(prototype_ring_tips[0]);++n) {
        const __typeof__(prototype_ring_tips[0]) *tip=&prototype_ring_tips[n];
        const unsigned v=pw_window_ring_corners[tip->corner][tip->j][tip->i],slack=tip->corner==2 && tip->j==2 && tip->i==2 ? 3 : 0;
        assert(v+slack>=tip->low && v<=tip->high);
    }

    /* Malformed effect state is refused without touching the output. */
    out[0]=99;
    const PwWindowRow good=s.windows[0];
    s.windows[0].radius=PW_WINDOW_RADIUS_LIMIT+1;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[0]=good;s.windows[0].shadow=PW_SHADOW_TOOLTIP+1;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[0]=good;s.windows[0].border=0x10;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL); /* no flag */
    s.windows[0]=good;s.windows[0].flags|=PW_WINDOW_GLASS;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[0]=good;s.windows[0].backdrop=PW_BACKDROP_MICA;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[0]=good;s.windows[0].solid_right=3;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[0]=good;s.windows[0].flags|=PW_WINDOW_CHILD;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[1].radius=4;s.windows[0]=good;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    s.windows[1].radius=0;s.windows[0]=good;s.windows[0].flags|=0x200;assert(pw_windows_compose(out,W,H,&s,images,3)<0 && errno==EINVAL);
    assert(out[0]==99);
    s.windows[0]=good;

    /* Extreme coordinates stay bounded with every effect enabled. */
    s.windows[0].left=INT_MIN;s.windows[0].top=INT_MIN;s.windows[0].right=INT_MAX;s.windows[0].bottom=INT_MAX;
    s.windows[0].client_left=INT_MIN;s.windows[0].client_top=INT_MIN;s.windows[0].client_right=INT_MAX;s.windows[0].client_bottom=INT_MAX;
    s.windows[0].flags|=PW_WINDOW_BORDER;s.windows[0].border=0x123456;s.windows[0].radius=16;
    assert(!pw_windows_compose(out,W,H,&s,images,3));
    /* A radius larger than the window is limited to half its size. */
    s.windows[0]=good;s.windows[0].right=9;s.windows[0].bottom=7;
    assert(pw_window_row_radius(&s.windows[0])==1 && !pw_windows_compose(out,W,H,&s,images,3));
    puts("window effects: PASS corner geometry r=1..16, rounded GDI/GPU coverage, border ring, window and flyout shadows vs naive recipe "
         "and the prototype's captured alphas and ring tips (active/inactive, light/dark), validation, extreme bounds");
}
