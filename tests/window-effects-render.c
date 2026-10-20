/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host oracle: render the window-effects probe's scene with the production
 * compositor, so tools/run-window-effects.py can judge it before any console
 * run. Writes 1920x1080 RGB PPM frames in the probe's phase order. */
#include "../src/pw_window_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W=1920, H=1080 };
static uint32_t rgb(unsigned r,unsigned g,unsigned b) {return 0xff000000u|(r<<16)|(g<<8)|b;}
static uint32_t *solid(unsigned w,unsigned h,uint32_t colour)
{
    uint32_t *p=malloc((size_t)w*h*4);
    for(size_t i=0;i<(size_t)w*h;++i)p[i]=colour;
    return p;
}
static int save(const char *path,const uint32_t *frame)
{
    FILE *f=fopen(path,"wb");if(!f)return -1;
    fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(size_t i=0;i<(size_t)W*H;++i) {
        unsigned char p[3]={(unsigned char)(frame[i]>>16),(unsigned char)(frame[i]>>8),(unsigned char)frame[i]};
        fwrite(p,1,3,f);
    }
    return fclose(f);
}
static PwWindowRow plain(uint32_t hwnd,int l,int t,int r,int b)
{
    return (PwWindowRow){.hwnd=hwnd,.pid=(int32_t)hwnd,.generation=hwnd,.flags=PW_WINDOW_VISIBLE,
        .left=l,.top=t,.right=r,.bottom=b,.client_left=l,.client_top=t,.client_right=r,.client_bottom=b};
}
static PwWindowImage image(const PwWindowRow *row,const uint32_t *pixels,uint32_t sequence)
{
    const uint32_t w=(uint32_t)(row->right-row->left),h=(uint32_t)(row->bottom-row->top);
    return (PwWindowImage){.hwnd=row->hwnd,.pid=row->pid,.generation=row->generation,.width=w,.height=h,
        .stride=w*4,.pixels=pixels,.bytes=(size_t)w*h*4,.sequence=sequence};
}
/* Acrylic over a non-uniform backdrop near the screen edge: the frame without
 * the window, then with it, for the reference model in the runner test. */
static int acrylic(const char *folder,uint32_t *frame,const PwWindowRow *desktop,const PwWindowImage *root_image,
                   PwWindowSnapshot *s,PwWindowEffects *effects)
{
    char path[4096];
    PwWindowRow glass=plain(30,700,500,1250,1000);
    glass.flags|=PW_WINDOW_GLASS;glass.backdrop=PW_BACKDROP_ACRYLIC;glass.radius=8;glass.shadow=PW_SHADOW_WINDOW;
    glass.solid_left=glass.solid_right=glass.client_left;glass.solid_top=glass.solid_bottom=glass.client_top;
    uint32_t *clear=calloc(550*500,4);
    PwWindowImage images[]={*root_image,image(&glass,clear,1)};
    s->count=1;s->windows[0]=*desktop;s->thumbnail_count=0;
    if(pw_windows_compose_effects(frame,W,H,s,images,1,effects))return 7;
    snprintf(path,sizeof(path),"%s/a1.ppm",folder);if(save(path,frame))return 8;
    s->count=2;s->windows[0]=glass;s->windows[1]=*desktop;
    if(pw_windows_compose_effects(frame,W,H,s,images,2,effects))return 9;
    snprintf(path,sizeof(path),"%s/a2.ppm",folder);if(save(path,frame))return 10;
    free(clear);
    return 0;
}
int main(int argc,char **argv)
{
    if(argc!=2 && argc!=3)return 2;
    char path[4096];
    static uint32_t frame[W*H];
    uint32_t *root=malloc((size_t)W*H*4);
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)root[y*W+x]=rgb(x*255/(W-1),y*255/(H-1),(x/40+y/40)%2 ? 200 : 60);
    PwWindowSnapshot *s=calloc(1,sizeof(*s));
    s->version=PW_WINDOW_SNAPSHOT_VERSION;s->size=sizeof(*s);
    PwWindowRow desktop=plain(1,0,0,W,H);desktop.flags|=PW_WINDOW_DESKTOP;
    PwWindowImage root_image=image(&desktop,root,1);
    PwWindowEffects effects={0};
    if(argc==3) {
        int status=acrylic(argv[1],frame,&desktop,&root_image,s,&effects);
        pw_window_effects_release(&effects);free(root);free(s);
        return status;
    }
    /* Phase 1: the desktop root alone. */
    s->count=1;s->windows[0]=desktop;
    if(pw_windows_compose_effects(frame,W,H,s,&root_image,1,&effects))return 3;
    snprintf(path,sizeof(path),"%s/1.ppm",argv[1]);if(save(path,frame))return 4;

    PwWindowRow back=plain(10,60,60,1860,1020),round=plain(11,150,150,450,400),compact=plain(12,550,150,800,300);
    PwWindowRow framed=plain(13,150,500,550,750),dropped=plain(14,900,600,1000,700),acrylic=plain(15,1100,150,1400,400);
    PwWindowRow mica=plain(16,1100,550,1400,800),destination=plain(17,1500,150,1800,400),source=plain(18,600,800,1000,1000);
    round.radius=8;round.shadow=PW_SHADOW_WINDOW;
    compact.radius=4;compact.shadow=PW_SHADOW_SMALL;compact.flags|=PW_WINDOW_BORDER;compact.border=0x0078d7;
    framed.radius=8;framed.shadow=PW_SHADOW_WINDOW;framed.flags|=PW_WINDOW_BORDER|PW_WINDOW_ACTIVE;framed.border=0x66757575;
    framed.client_left+=8;framed.client_top+=31;framed.client_right-=8;framed.client_bottom-=8;
    dropped.shadow=PW_SHADOW_SMALL;
    acrylic.flags|=PW_WINDOW_GLASS;acrylic.backdrop=PW_BACKDROP_ACRYLIC;
    acrylic.solid_left=acrylic.solid_right=acrylic.client_left;acrylic.solid_top=acrylic.solid_bottom=acrylic.client_top;
    mica.flags|=PW_WINDOW_GLASS;mica.backdrop=PW_BACKDROP_MICA;mica.material=PW_MATERIAL_DARK;
    mica.solid_left=mica.solid_right=mica.client_left;mica.solid_top=mica.solid_bottom=mica.client_top;
    uint32_t *glass=solid(300,250,0);
    for(int y=50;y<100;++y)for(int x=50;x<100;++x)glass[y*300+x]=0xffffffffu;
    uint32_t *split=solid(400,200,rgb(250,130,20));
    for(int y=0;y<200;++y)for(int x=200;x<400;++x)split[y*400+x]=rgb(20,200,220);
    PwWindowImage images[]={root_image,image(&back,solid(1800,960,rgb(40,90,160)),1),image(&round,solid(300,250,rgb(220,50,60)),1),
        image(&compact,solid(250,150,rgb(30,180,70)),1),image(&framed,solid(400,250,rgb(240,180,30)),1),
        image(&dropped,solid(100,100,rgb(128,128,128)),1),image(&acrylic,glass,1),image(&mica,solid(300,250,0),1),
        image(&destination,solid(300,250,rgb(128,128,128)),1),image(&source,split,1)};
    const PwWindowRow order[]={destination,acrylic,round,source,mica,dropped,framed,compact,back,desktop};
    s->count=sizeof(order)/sizeof(order[0]);memcpy(s->windows,order,sizeof(order));
    s->thumbnail_count=1;
    s->thumbnails[0]=(PwWindowThumbnail){.destination=17,.source=18,.destination_generation=17,.source_generation=18,
        .left=1520,.top=170,.right=1720,.bottom=320,.source_right=400,.source_bottom=200,.opacity=255};
    const char *phases[]={"effects","thumbnail-half","thumbnail-live","thumbnail-hidden","acrylic-dark"};
    for(unsigned n=0;n<5;++n) {
        if(n==1)s->thumbnails[0].opacity=128;
        if(n==2) {s->thumbnails[0].opacity=255;for(int y=0;y<200;++y)for(int x=0;x<200;++x)split[y*400+x]=rgb(150,40,190);images[9].sequence=2;}
        if(n==3)s->thumbnail_count=0;
        if(n==4)s->windows[1].material=PW_MATERIAL_DARK;
        if(pw_windows_compose_effects(frame,W,H,s,images,sizeof(images)/sizeof(images[0]),&effects)) {
            fprintf(stderr,"compose %s failed\n",phases[n]);return 5;
        }
        snprintf(path,sizeof(path),"%s/%u.ppm",argv[1],n+2);if(save(path,frame))return 6;
    }
    printf("rendered 6 frames mica_builds=%llu thumbnail_builds=%llu bytes=%zu\n",
           (unsigned long long)effects.mica_builds,(unsigned long long)effects.thumbnail_builds,effects.bytes);
    pw_window_effects_release(&effects);
    for(unsigned i=1;i<sizeof(images)/sizeof(images[0]);++i)free((void *)images[i].pixels);
    free(root);free(s);
    return 0;
}
