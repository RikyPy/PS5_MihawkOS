/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_window_page.h"
#include <errno.h>
#include <string.h>
size_t pw_window_page_bytes(uint32_t width,uint32_t height)
{
    if(!width || width>PW_WINDOW_WIDTH || !height || height>PW_WINDOW_HEIGHT) {errno=EINVAL;return 0;}
    size_t bytes=sizeof(PwWindowPage)+(size_t)width*height*4;
    return (bytes+PW_WINDOW_PAGE_ALIGN-1)&~(size_t)(PW_WINDOW_PAGE_ALIGN-1);
}
int pw_window_page_init(PwWindowPage *page,size_t bytes,uint32_t hwnd,uint64_t generation,
                        uint32_t width,uint32_t height,int32_t offset_x,int32_t offset_y)
{
    size_t needed=pw_window_page_bytes(width,height);
    if(!page || !hwnd || !generation || !needed || bytes!=needed) {errno=EINVAL;return -1;}
    *page=(PwWindowPage){.magic=PW_WINDOW_PAGE_MAGIC,.version=PW_WINDOW_PAGE_VERSION,
        .bytes=(uint32_t)bytes,.hwnd=hwnd,.generation=generation,.width=width,.height=height,
        .stride=width*4,.offset_x=offset_x,.offset_y=offset_y};
    return 0;
}
int pw_window_page_header(const PwWindowPage *page,size_t bytes,PwWindowPage *header)
{
    if(!page || !header || bytes<sizeof(*page)) {errno=EINVAL;return -1;}
    PwWindowPage copy;memcpy(&copy,page,sizeof(copy));
    const size_t needed=pw_window_page_bytes(copy.width,copy.height);
    if(!needed || copy.magic!=PW_WINDOW_PAGE_MAGIC || copy.version!=PW_WINDOW_PAGE_VERSION ||
       !copy.hwnd || !copy.generation || bytes!=needed || copy.bytes!=needed || copy.stride!=copy.width*4 ||
       (copy.flags&~PW_WINDOW_PAGE_PREMULTIPLIED) ||
       copy.reserved[0] || copy.reserved[1] || copy.reserved[2]) {errno=EPROTO;return -1;}
    *header=copy;return 0;
}
int pw_window_page_write(PwWindowPage *page,const void *pixels,uint32_t stride)
{
    PwWindowPage header;
    if(!page || !pixels || pw_window_page_header(page,page->bytes,&header) || stride<header.stride) {errno=EINVAL;return -1;}
    uint32_t seq=__atomic_load_n(&page->sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&page->sequence,seq|1,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for(uint32_t y=0;y<header.height;++y)
        memcpy((char *)(page+1)+(size_t)y*header.stride,(const char *)pixels+(size_t)y*stride,header.stride);
    uint32_t next=(seq|1)+1;if(!next)next=2;
    __atomic_store_n(&page->sequence,next,__ATOMIC_RELEASE);
    return 0;
}
int pw_window_page_read(const PwWindowPage *page,const PwWindowPage *header,
                        void *scratch,size_t bytes,uint32_t previous,uint32_t *sequence)
{
    const size_t needed=header ? (size_t)header->width*header->height*4 : 0;
    if(!page || !header || !scratch || !sequence || !needed ||
       !pw_window_page_bytes(header->width,header->height) || bytes<needed || header->stride!=header->width*4) {errno=EINVAL;return -1;}
    const uint32_t seq=__atomic_load_n(&page->sequence,__ATOMIC_ACQUIRE);
    if(!seq || (seq&1) || seq==previous)return 0;
    memcpy(scratch,page+1,needed);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(__atomic_load_n(&page->sequence,__ATOMIC_ACQUIRE)!=seq)return 0;
    *sequence=seq;return 1;
}

int pw_window_page_write_alpha(PwWindowPage *page,const void *pixels,uint32_t stride,
                               uint32_t alpha_mask,uint32_t opacity,
                               const void *shape,uint32_t shape_stride,size_t shape_bytes)
{
    PwWindowPage header;
    if(!page || !pixels || ((uintptr_t)pixels&3) || stride%4 ||
       pw_window_page_header(page,page->bytes,&header) || stride<header.stride ||
       header.flags!=PW_WINDOW_PAGE_PREMULTIPLIED || opacity>255 ||
       (alpha_mask && alpha_mask!=0xff000000u) ||
       (shape && (shape_stride<(header.width+7)/8 ||
                  (uint64_t)shape_stride*header.height>shape_bytes)) ||
       (!shape && (shape_stride || shape_bytes))) {errno=EINVAL;return -1;}
    uint32_t seq=__atomic_load_n(&page->sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&page->sequence,seq|1,__ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for(uint32_t y=0;y<header.height;++y) {
        const uint32_t *source=(const uint32_t *)((const char *)pixels+(size_t)y*stride);
        const unsigned char *mask=shape ? (const unsigned char *)shape+(size_t)y*shape_stride : NULL;
        uint32_t *dest=(uint32_t *)(page+1)+(size_t)y*header.width;
        for(uint32_t x=0;x<header.width;++x) {
            if(mask && !(mask[x/8]&(0x80u>>(x%8)))) {dest[x]=0;continue;}
            uint32_t value=source[x],alpha=alpha_mask ? value>>24 : 255;
            uint32_t pixel=((alpha*opacity+127)/255)<<24;
            for(unsigned shift=0;shift<24;shift+=8)
                pixel|=((((value>>shift)&255)*opacity+127)/255)<<shift;
            dest[x]=pixel;
        }
    }
    uint32_t next=(seq|1)+1;if(!next)next=2;
    __atomic_store_n(&page->sequence,next,__ATOMIC_RELEASE);
    return 0;
}
