/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host cost of the retained compositor with DWM effects: five cumulative
 * scenes at 1920x1080, minimum of 60 compositions each. A measurement, not a
 * test; tools/bench-window-effects.py compares revisions and records it. */
#include "pw_window_effects.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
enum{W=1920,H=1080};
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static PwWindowRow plain(uint32_t h,int l,int t,int r,int b){return (PwWindowRow){.hwnd=h,.pid=(int)h,.generation=h,.flags=PW_WINDOW_VISIBLE,.left=l,.top=t,.right=r,.bottom=b,.client_left=l,.client_top=t,.client_right=r,.client_bottom=b};}
int main(void){
 static uint32_t frame[W*H];uint32_t *root=malloc(W*H*4),*clear=calloc(W*H,4),*win=malloc(W*H*4);
 for(int i=0;i<W*H;++i){root[i]=0xff000000|(i*2654435761u>>8);win[i]=0xffc0c0c0;}
 PwWindowSnapshot *s=calloc(1,sizeof(*s));s->version=PW_WINDOW_SNAPSHOT_VERSION;s->size=sizeof(*s);
 PwWindowRow d=plain(1,0,0,W,H);d.flags|=PW_WINDOW_DESKTOP;
 PwWindowEffects fx={0};
 const char *names[]={"plain desktop + 2 opaque windows","+ rounded/shadow framed windows","+ acrylic Start 600x566","+ Mica taskbar 1920x48","+ full-screen Mica window"};
 for(int scene=0;scene<5;++scene){
  PwWindowRow rows[8];int n=0;
  PwWindowRow a=plain(2,200,150,1300,850),b=plain(3,400,250,1500,950);
  if(scene>=1){a.radius=8;a.shadow=2;b.radius=8;b.shadow=2;}
  PwWindowRow st=plain(4,660,454,1260,1020);st.radius=8;st.shadow=2;st.flags|=PW_WINDOW_GLASS|PW_WINDOW_BORDER;st.border=0xd6d9df;st.backdrop=PW_BACKDROP_ACRYLIC;st.solid_left=st.solid_right=660;st.solid_top=st.solid_bottom=454;
  PwWindowRow tb=plain(5,0,1032,W,H);tb.flags|=PW_WINDOW_GLASS;tb.backdrop=PW_BACKDROP_MICA;tb.solid_left=tb.solid_right=0;tb.solid_top=tb.solid_bottom=1032;
  PwWindowRow full=plain(6,0,0,W,1032);full.flags|=PW_WINDOW_GLASS;full.backdrop=PW_BACKDROP_MICA;full.solid_left=full.solid_right=0;full.solid_top=full.solid_bottom=0;
  if(scene>=4)rows[n++]=full;
  if(scene>=3)rows[n++]=tb;
  if(scene>=2)rows[n++]=st;
  rows[n++]=b;rows[n++]=a;rows[n++]=d;
  s->count=n;for(int i=0;i<n;++i)s->windows[i]=rows[i];
  PwWindowImage img[]={{.hwnd=1,.pid=1,.generation=1,.width=W,.height=H,.stride=W*4,.pixels=root,.bytes=W*H*4,.sequence=1},
   {.hwnd=2,.pid=2,.generation=2,.width=1100,.height=700,.stride=W*4,.pixels=win,.bytes=W*H*4},
   {.hwnd=3,.pid=3,.generation=3,.width=1100,.height=700,.stride=W*4,.pixels=win,.bytes=W*H*4},
   {.hwnd=4,.pid=4,.generation=4,.width=600,.height=566,.stride=W*4,.pixels=clear,.bytes=W*H*4},
   {.hwnd=5,.pid=5,.generation=5,.width=W,.height=48,.stride=W*4,.pixels=clear,.bytes=W*H*4},
   {.hwnd=6,.pid=6,.generation=6,.width=W,.height=1032,.stride=W*4,.pixels=clear,.bytes=W*H*4}};
  pw_windows_compose_effects(frame,W,H,s,img,6,&fx);
  double best=1e9;for(int r=0;r<60;++r){double t0=now();if(pw_windows_compose_effects(frame,W,H,s,img,6,&fx))return 1;double t=now()-t0;if(t<best)best=t;}
  printf("%s\t%.3f\n",names[scene],best);
 }
 return 0;}
