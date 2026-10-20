/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_window_page.h"
#include "../src/pw_window_snapshot.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned scaled(unsigned value,unsigned opacity)
{
    return (unsigned)floor((double)value*(double)opacity/255.0+0.5);
}
int main(void)
{
    size_t bytes=pw_window_page_bytes(2,1);
    PwWindowPage *page=calloc(1,bytes);assert(page);
    assert(!pw_window_page_init(page,bytes,2,2,2,1,0,0));
    page->flags=PW_WINDOW_PAGE_PREMULTIPLIED;
    uint32_t input[2],background[2]={0xff204080,0xff204080},out[2],copy[2],seq;
    PwWindowSnapshot s={.version=PW_WINDOW_SNAPSHOT_VERSION,.size=sizeof(s),.count=2,
        .windows={{.hwnd=2,.pid=2,.generation=2,.flags=PW_WINDOW_VISIBLE,.right=2,.bottom=1},
                  {.hwnd=1,.pid=1,.generation=1,.flags=PW_WINDOW_VISIBLE,.right=2,.bottom=1}}};
    PwWindowImage images[]={
        {.hwnd=1,.pid=1,.generation=1,.width=2,.height=1,.stride=8,.pixels=background,.bytes=8},
        {.hwnd=2,.pid=2,.generation=2,.width=2,.height=1,.stride=8,.pixels=copy,.bytes=8,.flags=PW_WINDOW_PREMULTIPLIED}};
    /* Floating-point source-over reference over every alpha/opacity pair.
     * Premultiplied colour must not be multiplied by its own alpha again. */
    for(unsigned alpha=0;alpha<256;++alpha)for(unsigned opacity=0;opacity<256;++opacity) {
        input[0]=input[1]=(alpha<<24)|(scaled(200,alpha)<<16)|(scaled(100,alpha)<<8)|scaled(50,alpha);
        assert(!pw_window_page_write_alpha(page,input,8,0xff000000,opacity,NULL,0,0));
        assert(pw_window_page_read(page,page,copy,sizeof(copy),0,&seq)==1);
        assert(!pw_windows_compose(out,2,1,&s,images,2));
        unsigned a=scaled(alpha,opacity);
        uint32_t expected=0xff000000;
        for(unsigned shift=0;shift<24;shift+=8) {
            unsigned colour=scaled((input[0]>>shift)&255,opacity)+scaled((background[0]>>shift)&255,255-a);
            expected|=colour<<shift;
        }
        assert(out[0]==expected && out[1]==expected);
    }
    input[0]=input[1]=0x00102030;
    const unsigned char shape=0x80;
    assert(!pw_window_page_write_alpha(page,input,8,0,128,&shape,1,1));
    assert(pw_window_page_read(page,page,copy,sizeof(copy),0,&seq)==1);
    assert(copy[0]==0x80081018 && copy[1]==0);
    assert(!pw_windows_compose(out,2,1,&s,images,2) && out[1]==background[1]);
    uint32_t before=page->sequence,old=((uint32_t *)(page+1))[0];
    assert(pw_window_page_write_alpha(page,input,8,0,256,NULL,0,0)<0 && errno==EINVAL);
    assert(pw_window_page_write_alpha(page,input,8,0,128,&shape,0,1)<0 && errno==EINVAL);
    assert(pw_window_page_write_alpha(page,input,8,0,128,&shape,1,0)<0 && errno==EINVAL);
    assert(pw_window_page_write_alpha(page,input,8,1,128,NULL,0,0)<0 && errno==EINVAL);
    assert(page->sequence==before && ((uint32_t *)(page+1))[0]==old);
    images[1].flags=2;out[0]=99;
    assert(pw_windows_compose(out,2,1,&s,images,2)<0 && errno==EINVAL && out[0]==99);
    free(page);
    puts("window alpha: PASS 65536 alpha/opacity pairs, RGB normalization, shape mask, transactional validation");
}
