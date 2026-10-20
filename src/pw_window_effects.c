/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_window_effects.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void pw_window_effects_release(PwWindowEffects *effects)
{
    if(!effects)return;
    free(effects->mica);free(effects->mica_alt);free(effects->mica_alt_scratch);free(effects->blur[0]);free(effects->blur[1]);free(effects->source);
    for(unsigned i=0;i<PW_WINDOW_THUMBNAIL_LIMIT;++i)free(effects->thumbnails[i].pixels);
    for(unsigned i=0;i<PW_BACKDROP_CACHE_LIMIT;++i)free(effects->backdrops[i].pixels);
    memset(effects,0,sizeof(*effects));
}

/* 16x16 integer supersampling: deterministic on every compiler and CPU. A
 * subsample at (i+(2a+1)/32, j+(2b+1)/32) is inside when its squared distance
 * from (r,r), scaled by 32, is at most (32r)^2. */
static unsigned corner_samples(unsigned radius,unsigned i,unsigned j,unsigned inner)
{
    const int64_t outer2=(int64_t)32*radius*32*radius;
    const int64_t inner2=inner ? (int64_t)32*(radius-1)*32*(radius-1) : -1;
    unsigned count=0;
    for(unsigned b=0;b<16;++b)for(unsigned a=0;a<16;++a) {
        const int64_t dx=(int64_t)32*radius-(32*(int64_t)i+2*a+1);
        const int64_t dy=(int64_t)32*radius-(32*(int64_t)j+2*b+1);
        const int64_t d2=dx*dx+dy*dy;
        count+=d2<=outer2 && d2>inner2;
    }
    return count;
}
uint8_t pw_window_corner_coverage(unsigned radius,unsigned i,unsigned j)
{
    if(!radius || radius>PW_WINDOW_RADIUS_LIMIT+1 || i>=radius || j>=radius)return 255;
    return (uint8_t)((corner_samples(radius,i,j,0)*255+128)/256);
}
uint8_t pw_window_corner_border(unsigned radius,unsigned i,unsigned j)
{
    if(!radius || radius>PW_WINDOW_RADIUS_LIMIT || i>=radius || j>=radius)return 0;
    return (uint8_t)((corner_samples(radius,i,j,1)*255+128)/256);
}
unsigned pw_window_row_radius(const PwWindowRow *row)
{
    const int64_t width=(int64_t)row->right-row->left,height=(int64_t)row->bottom-row->top;
    int64_t radius=row->radius;
    if(radius>width/2)radius=width/2;
    if(radius>height/2)radius=height/2;
    return radius>0 ? (unsigned)radius : 0;
}
uint8_t pw_window_row_coverage(const PwWindowRow *row,int64_t x,int64_t y)
{
    const unsigned r=pw_window_row_radius(row);
    if(x<row->left || x>=row->right || y<row->top || y>=row->bottom)return 0;
    if(!r)return 255;
    const int64_t lx=x-row->left,rx=row->right-1-x,ty=y-row->top,by=row->bottom-1-y;
    const int64_t i=lx<r ? lx : rx<r ? rx : -1,j=ty<r ? ty : by<r ? by : -1;
    if(i<0 || j<0)return 255;
    return pw_window_corner_coverage(r,(unsigned)i,(unsigned)j);
}
float pw_window_rounded_distance(float px,float py,float l,float t,float r,float b,float radius)
{
    const float cx=(l+r)*0.5f,cy=(t+b)*0.5f,hx=(r-l)*0.5f-radius,hy=(b-t)*0.5f-radius;
    const float qx=fabsf(px-cx)-hx,qy=fabsf(py-cy)-hy;
    const float ox=qx>0 ? qx : 0,oy=qy>0 ? qy : 0;
    const float inside=qx>qy ? qx : qy;
    return sqrtf(ox*ox+oy*oy)+(inside<0 ? inside : 0)-radius;
}
uint8_t pw_window_shadow_alpha(unsigned shadow,float distance)
{
    if(shadow!=PW_SHADOW_SMALL || distance>=PW_SHADOW_SMALL_SIZE)return 0;
    if(distance<=0)return PW_SHADOW_SMALL_ALPHA;
    const float falloff=1.0f-distance/PW_SHADOW_SMALL_SIZE;
    return (uint8_t)(PW_SHADOW_SMALL_ALPHA*falloff*falloff+0.5f);
}

int pw_window_shadow_kernel(PwShadowKernel *kernel,unsigned blur)
{
    const int32_t possible=(int32_t)floor(blur*0.5*3*sqrt(2*3.14159265358979323846)/4+0.5);
    const int32_t window=possible>1 ? possible : 1;
    const int32_t border=window&1 ? 3*((window-1)/2) : 3*(window/2)-1;
    if(!kernel || border>PW_SHADOW_BORDER_LIMIT) {errno=EINVAL;return -1;}
    /* The boxes as [first offset, size]; their convolution, built one box at a time. */
    const int32_t boxes[3][2]={{-(window/2),window},{window&1 ? -(window/2) : -(window/2)+1,window},
                               {-(window/2),window&1 ? window : window+1}};
    uint32_t a[2*PW_SHADOW_BORDER_LIMIT+1]={1},b[2*PW_SHADOW_BORDER_LIMIT+1];
    int32_t low=0,count=1;
    for(unsigned k=0;k<3;++k) {
        memset(b,0,sizeof(b));
        for(int32_t i=0;i<count;++i)for(int32_t j=0;j<boxes[k][1];++j)b[i+j]+=a[i];
        low+=boxes[k][0];count+=boxes[k][1]-1;
        memcpy(a,b,sizeof(a));
    }
    memset(kernel,0,sizeof(*kernel));
    kernel->border=border;
    uint32_t sum=0;
    for(int32_t i=0;i<count;++i) {kernel->weights[i]=a[i];sum+=a[i];kernel->below[i+1]=sum;}
    kernel->divisor=sum;
    kernel->weight=(uint64_t)llround(1.0/sum*4294967296.0); /* as SkMaskBlurFilter computes it */
    return low==-border && count==2*border+1 ? 0 : (errno=EINVAL,-1);
}
uint32_t pw_window_shadow_span(const PwShadowKernel *kernel,int64_t p,int64_t a,int64_t b)
{
    /* sum of weights[p-j] for j in [a,b): the weights at offsets (p-b, p-a] */
    const int64_t border=kernel->border;
    const int64_t hi=p-a<-border-1 ? -border-1 : p-a>border ? border : p-a;
    const int64_t lo=p-b<-border-1 ? -border-1 : p-b>border ? border : p-b;
    return hi>lo ? kernel->below[hi+border+1]-kernel->below[lo+border+1] : 0;
}

static int64_t clamp64(int64_t value,int64_t low,int64_t high) {return value<low ? low : value>high ? high : value;}
int pw_window_blur(uint32_t *output,uint32_t *scratch,const uint32_t *source,uint32_t sw,uint32_t sh,
                   uint32_t stride,int32_t left,int32_t top,uint32_t area_w,uint32_t area_h,
                   unsigned scale,unsigned radius,unsigned passes)
{
    if(!output || !scratch || !source || !sw || !sh || stride<sw*4 || stride%4 || !area_w || !area_h ||
       area_w>4096 || area_h>4096 || !scale || scale>16 || radius>64 || passes>8) {errno=EINVAL;return -1;}
    const uint32_t width=(area_w+scale-1)/scale,height=(area_h+scale-1)/scale,n=scale*scale;
    const uint32_t pitch=stride/4;
    for(uint32_t by=0;by<height;++by)for(uint32_t bx=0;bx<width;++bx) {
        uint32_t sum[4]={0};
        const int64_t y0=(int64_t)top+(int64_t)by*scale,x0=(int64_t)left+(int64_t)bx*scale;
        if(y0>=0 && x0>=0 && y0+scale<=(int64_t)sh && x0+scale<=(int64_t)sw) {
            /* Interior block: the same sum without per-pixel clamping. */
            for(uint32_t dy=0;dy<scale;++dy) {
                const uint32_t *line=source+(size_t)(y0+dy)*pitch+(size_t)x0;
                for(uint32_t dx=0;dx<scale;++dx) {
                    const uint32_t value=line[dx];
                    sum[0]+=value&255;sum[1]+=(value>>8)&255;sum[2]+=(value>>16)&255;sum[3]+=value>>24;
                }
            }
        } else for(uint32_t dy=0;dy<scale;++dy) {
            const int64_t sy=clamp64(y0+dy,0,(int64_t)sh-1);
            const uint32_t *line=source+(size_t)sy*pitch;
            for(uint32_t dx=0;dx<scale;++dx) {
                const uint32_t value=line[clamp64(x0+dx,0,(int64_t)sw-1)];
                for(unsigned c=0;c<4;++c)sum[c]+=(value>>(8*c))&255;
            }
        }
        uint32_t pixel=0;for(unsigned c=0;c<4;++c)pixel|=((sum[c]+n/2)/n)<<(8*c);
        output[(size_t)by*width+bx]=pixel;
    }
    const uint32_t span=2*radius+1;
    for(unsigned pass=0;pass<passes && radius;++pass) {
        /* Horizontal into scratch, then vertical back into output. A sliding
         * sum with clamped edges is exactly the mean of 2r+1 clamped taps. */
        for(uint32_t y=0;y<height;++y) {
            const uint32_t *in=output+(size_t)y*width;uint32_t *out=scratch+(size_t)y*width;
            uint32_t sum[4]={0};
            for(int64_t k=-(int64_t)radius;k<=(int64_t)radius;++k) {
                const uint32_t value=in[clamp64(k,0,width-1)];
                for(unsigned c=0;c<4;++c)sum[c]+=(value>>(8*c))&255;
            }
            for(uint32_t x=0;x<width;++x) {
                uint32_t pixel=0;for(unsigned c=0;c<4;++c)pixel|=((sum[c]+span/2)/span)<<(8*c);
                out[x]=pixel;
                const uint32_t gone=in[clamp64((int64_t)x-radius,0,width-1)],next=in[clamp64((int64_t)x+radius+1,0,width-1)];
                for(unsigned c=0;c<4;++c)sum[c]+=((next>>(8*c))&255)-((gone>>(8*c))&255);
            }
        }
        for(uint32_t x=0;x<width;++x) {
            uint32_t sum[4]={0};
            for(int64_t k=-(int64_t)radius;k<=(int64_t)radius;++k) {
                const uint32_t value=scratch[(size_t)clamp64(k,0,height-1)*width+x];
                for(unsigned c=0;c<4;++c)sum[c]+=(value>>(8*c))&255;
            }
            for(uint32_t y=0;y<height;++y) {
                uint32_t pixel=0;for(unsigned c=0;c<4;++c)pixel|=((sum[c]+span/2)/span)<<(8*c);
                output[(size_t)y*width+x]=pixel;
                const uint32_t gone=scratch[(size_t)clamp64((int64_t)y-radius,0,height-1)*width+x];
                const uint32_t next=scratch[(size_t)clamp64((int64_t)y+radius+1,0,height-1)*width+x];
                for(unsigned c=0;c<4;++c)sum[c]+=((next>>(8*c))&255)-((gone>>(8*c))&255);
            }
        }
    }
    return 0;
}
uint32_t pw_window_blur_sample(const uint32_t *blurred,uint32_t width,uint32_t height,
                               int32_t left,int32_t top,unsigned scale,int64_t x,int64_t y)
{
    /* Fixed point 1/256: u = (x-left+0.5)/scale - 0.5. */
    const int64_t u=clamp64(((2*(x-left)+1)*128)/(int64_t)scale-128,0,((int64_t)width-1)*256);
    const int64_t v=clamp64(((2*(y-top)+1)*128)/(int64_t)scale-128,0,((int64_t)height-1)*256);
    const uint32_t x0=(uint32_t)(u>>8),y0=(uint32_t)(v>>8),fx=(uint32_t)(u&255),fy=(uint32_t)(v&255);
    const uint32_t x1=x0+1<width ? x0+1 : x0,y1=y0+1<height ? y0+1 : y0;
    const uint32_t a=blurred[(size_t)y0*width+x0],b=blurred[(size_t)y0*width+x1];
    const uint32_t c=blurred[(size_t)y1*width+x0],d=blurred[(size_t)y1*width+x1];
    uint32_t pixel=0xff000000u;
    for(unsigned shift=0;shift<24;shift+=8) {
        const uint32_t top_row=((a>>shift)&255)*(256-fx)+((b>>shift)&255)*fx;
        const uint32_t bottom_row=((c>>shift)&255)*(256-fx)+((d>>shift)&255)*fx;
        pixel|=((top_row*(256-fy)+bottom_row*fy+32768)>>16)<<shift;
    }
    return pixel;
}
void pw_window_material_row(uint32_t *out,const uint32_t *blurred,uint32_t width,uint32_t height,
                            int32_t left,int32_t top,unsigned scale,int64_t y,int64_t x0,uint32_t count,
                            uint32_t tint,uint32_t amount,unsigned recipe)
{
    uint8_t table[3][256];
    for(unsigned c=0;c<3;++c)for(unsigned v=0;v<256;++v) {
        const uint32_t u=recipe==PW_RECIPE_TASKBAR_LIGHT ? pw_window_taskbar_transfer(v) : v;
        table[c][v]=(uint8_t)((u*(255-amount)+((tint>>(8*c))&255)*amount+127)/255);
    }
    const int64_t v=clamp64(((2*(y-top)+1)*128)/(int64_t)scale-128,0,((int64_t)height-1)*256);
    const uint32_t y0=(uint32_t)(v>>8),fy=(uint32_t)(v&255),y1=y0+1<height ? y0+1 : y0;
    const uint32_t *upper=blurred+(size_t)y0*width,*lower=blurred+(size_t)y1*width;
    /* The column numerator grows by 256 per pixel: keep its quotient and
     * remainder instead of dividing. Truncating division of negative values
     * leaves negative remainders, so divide directly until the numerator has
     * been non-negative for one step. */
    int64_t numerator=(2*(x0-left)+1)*128,quotient=numerator/(int64_t)scale,remainder=numerator%(int64_t)scale;
    const int64_t step=256/(int64_t)scale,carry=256%(int64_t)scale;
    /* Blending the two blurred columns vertically first is the same integer as blending the two rows horizontally first
     * (each is the four samples times their weights), and a column pair's vertical blends serve every pixel between
     * them: they are taken once per pair, not per pixel. */
    uint32_t pair=UINT32_MAX,va[3]={0},vb[3]={0};
    for(uint32_t i=0;i<count;++i) {
        if(numerator<256) {quotient=numerator/(int64_t)scale;remainder=numerator%(int64_t)scale;}
        const int64_t u=clamp64(quotient-128,0,((int64_t)width-1)*256);
        numerator+=256;quotient+=step;remainder+=carry;
        if(remainder>=(int64_t)scale) {remainder-=scale;++quotient;}
        const uint32_t xa=(uint32_t)(u>>8),fx=(uint32_t)(u&255);
        if(xa!=pair) {
            const uint32_t xb=xa+1<width ? xa+1 : xa,a=upper[xa],b=upper[xb],cc=lower[xa],d=lower[xb];
            for(unsigned c=0;c<3;++c) {
                va[c]=((a>>8*c)&255)*(256-fy)+((cc>>8*c)&255)*fy;
                vb[c]=((b>>8*c)&255)*(256-fy)+((d>>8*c)&255)*fy;
            }
            pair=xa;
        }
        uint32_t blended=0;
        for(unsigned c=0;c<3;++c)blended|=((va[c]*(256-fx)+vb[c]*fx+32768)>>16)<<8*c;
        out[i]=blended;
    }
    /* Then the recipe over the blended row, one loop per recipe. */
    if(recipe==PW_RECIPE_MICA_ALT_LIGHT || recipe==PW_RECIPE_MICA_ALT_DARK) {
        for(uint32_t i=0;i<count;++i)out[i]=pw_window_mica_alt_pixel(out[i],tint,amount,recipe);
        return;
    }
    if(recipe==PW_RECIPE_TASKBAR_DARK)for(uint32_t i=0;i<count;++i)out[i]=pw_window_taskbar_saturate(out[i]);
    for(uint32_t i=0;i<count;++i)
        out[i]=0xff000000u|(uint32_t)table[0][out[i]&255]|(uint32_t)table[1][(out[i]>>8)&255]<<8|(uint32_t)table[2][(out[i]>>16)&255]<<16;
}
uint32_t pw_window_tint(uint32_t colour,uint32_t tint,uint32_t amount)
{
    uint32_t pixel=0xff000000u;
    for(unsigned shift=0;shift<24;shift+=8)
        pixel|=((((colour>>shift)&255)*(255-amount)+((tint>>shift)&255)*amount+127)/255)<<shift;
    return pixel;
}
int pw_window_scale(uint32_t *output,uint32_t dw,uint32_t dh,const uint32_t *source,
                    uint32_t sw,uint32_t sh,uint32_t stride)
{
    if(!output || !source || !dw || !dh || !sw || !sh || dw>4096 || dh>4096 || sw>4096 || sh>4096 ||
       stride<sw*4 || stride%4) {errno=EINVAL;return -1;}
    const uint32_t pitch=stride/4;
    for(uint32_t dy=0;dy<dh;++dy) {
        uint32_t y0=(uint32_t)((uint64_t)dy*sh/dh),y1=(uint32_t)((uint64_t)(dy+1)*sh/dh);if(y1<=y0)y1=y0+1;
        for(uint32_t dx=0;dx<dw;++dx) {
            uint32_t x0=(uint32_t)((uint64_t)dx*sw/dw),x1=(uint32_t)((uint64_t)(dx+1)*sw/dw);if(x1<=x0)x1=x0+1;
            uint64_t sum[4]={0};const uint64_t n=(uint64_t)(x1-x0)*(y1-y0);
            for(uint32_t sy=y0;sy<y1;++sy)for(uint32_t sx=x0;sx<x1;++sx) {
                const uint32_t value=source[(size_t)sy*pitch+sx];
                for(unsigned c=0;c<4;++c)sum[c]+=(value>>(8*c))&255;
            }
            uint32_t pixel=0;for(unsigned c=0;c<4;++c)pixel|=(uint32_t)((sum[c]+n/2)/n)<<(8*c);
            output[(size_t)dy*dw+dx]=pixel;
        }
    }
    return 0;
}

/* The canvas: the screen's width/8 x height/8, drawn MICA_ALT_MARGIN larger on every side (micaOf's drawImage at
 * -24,-24, w+48 x h+48), padded for the blur's reach. Four 16-bit premultiplied channels per pixel, two planes. */
static void mica_alt_canvas(uint32_t width,uint32_t height,uint32_t *cw,uint32_t *ch,uint32_t *pw,uint32_t *ph)
{
    *cw=width/PW_MICA_SCALE;*ch=height/PW_MICA_SCALE;
    *pw=*cw+2*PW_MICA_ALT_MARGIN+2*PW_MICA_ALT_RADIUS*PW_BLUR_PASSES;
    *ph=*ch+2*PW_MICA_ALT_MARGIN+2*PW_MICA_ALT_RADIUS*PW_BLUR_PASSES;
}
size_t pw_window_mica_alt_scratch(uint32_t width,uint32_t height)
{
    uint32_t cw,ch,pw,ph;mica_alt_canvas(width,height,&cw,&ch,&pw,&ph);
    return (size_t)pw*ph*4;  /* two planes of 4 x uint16 = 4 words per pixel */
}
/* One box pass of radius r along rows (step 1) or columns (step pitch), transparent beyond the plane, rounded. */
static void mica_alt_box(uint16_t *to,const uint16_t *from,uint32_t pw,uint32_t ph,int columns)
{
    const int64_t r=PW_MICA_ALT_RADIUS,span=2*r+1,lines=columns ? pw : ph,length=columns ? ph : pw;
    for(int64_t line=0;line<lines;++line) {
        uint32_t sum[4]={0};
        #define AT(k) ((columns ? (size_t)(k)*pw+(size_t)line : (size_t)line*pw+(size_t)(k))*4)
        for(int64_t k=0;k<=r && k<length;++k)for(unsigned c=0;c<4;++c)sum[c]+=from[AT(k)+c];
        for(int64_t k=0;k<length;++k) {
            for(unsigned c=0;c<4;++c)to[AT(k)+c]=(uint16_t)((sum[c]+span/2)/span);
            if(k-r>=0)for(unsigned c=0;c<4;++c)sum[c]-=from[AT(k-r)+c];
            if(k+r+1<length)for(unsigned c=0;c<4;++c)sum[c]+=from[AT(k+r+1)+c];
        }
        #undef AT
    }
}
int pw_window_mica_alt_source(uint32_t *output,uint32_t *scratch,const uint32_t *source,uint32_t sw,uint32_t sh,
                              uint32_t stride,int32_t left,int32_t top,uint32_t width,uint32_t height)
{
    if(!output || !scratch || !source || !sw || !sh || stride<sw*4 || stride%4 || width<PW_MICA_SCALE ||
       height<PW_MICA_SCALE || width>4096 || height>4096) {errno=EINVAL;return -1;}
    uint32_t cw,ch,pw,ph;mica_alt_canvas(width,height,&cw,&ch,&pw,&ph);
    const uint32_t dw=cw+2*PW_MICA_ALT_MARGIN,dh=ch+2*PW_MICA_ALT_MARGIN,pad=PW_MICA_ALT_RADIUS*PW_BLUR_PASSES,pitch=stride/4;
    uint16_t *a=(uint16_t *)scratch,*b=a+(size_t)pw*ph*4;
    memset(a,0,(size_t)pw*ph*8);
    /* The drawn wallpaper: canvas pixel (i,j) of the dw x dh drawing averages the screen area it covers, with the
     * fractional pixels at its edges weighted by their overlap (in 1/dw and 1/dh of a pixel). Fully opaque.
     * Separable, the same integers: a canvas row first sums its source rows (weight wy) into one line of the source
     * columns it reads, then each of its pixels sums its columns (weight wx) of that line. Summed per pixel this was
     * 8.5 of the 16 ms of the first frame with File Explorer open on the host. The line lives in plane b, which the
     * blur overwrites whole: 3 x width words, under an eighth of the plane. */
    const int64_t first=clamp64(left,0,(int64_t)sw-1),columns=clamp64((int64_t)left+width-1,0,(int64_t)sw-1)+1-first;
    uint32_t *sums=(uint32_t *)b;
    for(uint32_t j=0;j<dh;++j) {
        const uint64_t y0=(uint64_t)j*height,y1=(uint64_t)(j+1)*height;
        uint64_t rows=0;
        memset(sums,0,(size_t)columns*3*4);
        for(uint64_t y=y0/dh;y*dh<y1;++y) {
            const uint32_t wy=(uint32_t)((y1<(y+1)*dh ? y1 : (y+1)*dh)-(y0>y*dh ? y0 : y*dh));
            const uint32_t *line=source+(size_t)clamp64((int64_t)y+top,0,sh-1)*pitch+first;
            for(int64_t x=0;x<columns;++x) {
                const uint32_t value=line[x];
                sums[x]+=(value&255)*wy;sums[columns+x]+=((value>>8)&255)*wy;sums[2*columns+x]+=((value>>16)&255)*wy;
            }
            rows+=wy;
        }
        for(uint32_t i=0;i<dw;++i) {
            const uint64_t x0=(uint64_t)i*width,x1=(uint64_t)(i+1)*width;
            uint64_t sum[3]={0},across=0;
            for(uint64_t x=x0/dw;x*dw<x1;++x) {
                const uint64_t wx=(x1<(x+1)*dw ? x1 : (x+1)*dw)-(x0>x*dw ? x0 : x*dw);
                const size_t k=(size_t)(clamp64((int64_t)x+left,0,(int64_t)sw-1)-first);
                for(unsigned c=0;c<3;++c)sum[c]+=sums[c*columns+k]*wx;
                across+=wx;
            }
            const uint64_t total=across*rows;
            uint16_t *pixel=a+((size_t)(j+pad)*pw+(i+pad))*4;
            for(unsigned c=0;c<3;++c)pixel[c]=(uint16_t)((sum[c]+total/2)/total);
            pixel[3]=255;
        }
    }
    for(unsigned pass=0;pass<PW_BLUR_PASSES;++pass) {mica_alt_box(b,a,pw,ph,0);mica_alt_box(a,b,pw,ph,1);}
    /* The canvas window, over black (the JPEG it becomes has no alpha), through saturate(1.4). */
    static const int32_t m[3][3]={{1315,-286,-29},{-85,1114,-29},{-85,-286,1371}};  /* 1/1000, Filter Effects */
    for(uint32_t y=0;y<ch;++y)for(uint32_t x=0;x<cw;++x) {
        const uint16_t *pixel=a+((size_t)(y+pad+PW_MICA_ALT_MARGIN)*pw+(x+pad+PW_MICA_ALT_MARGIN))*4;
        uint32_t out=0xff000000u;
        for(unsigned i=0;i<3;++i) {
            /* channels are stored blue, green, red (0xAARRGGBB words); the matrix rows are red, green, blue */
            const int32_t red=pixel[2],green=pixel[1],blue=pixel[0];
            const int32_t v=(m[i][0]*red+m[i][1]*green+m[i][2]*blue+500)/1000;
            out|=(uint32_t)(v<0 ? 0 : v>255 ? 255 : v)<<(16-8*i);
        }
        output[(size_t)y*cw+x]=out;
    }
    return 0;
}
uint32_t pw_window_mica_alt_pixel(uint32_t sampled,uint32_t tint,uint32_t amount,unsigned recipe)
{
    /* W3C luminosity blend, SetLum(backdrop, Lum(grey)), in thousandths of a level, then the tint over it. */
    const int64_t target=1000*(int64_t)(recipe==PW_RECIPE_MICA_ALT_DARK ? PW_MICA_ALT_LUMINOSITY_DARK : PW_MICA_ALT_LUMINOSITY);
    int64_t c[3]={1000*(int64_t)((sampled>>16)&255),1000*(int64_t)((sampled>>8)&255),1000*(int64_t)(sampled&255)};
    const int64_t lum=(300*c[0]+590*c[1]+110*c[2])/1000,d=target-lum;
    int64_t low=INT64_MAX,high=INT64_MIN;
    for(unsigned i=0;i<3;++i) {c[i]+=d;if(c[i]<low)low=c[i];if(c[i]>high)high=c[i];}
    for(unsigned i=0;i<3;++i) {
        if(low<0 && target>low)c[i]=target+(c[i]-target)*target/(target-low);
        else if(high>255000 && high>target)c[i]=target+(c[i]-target)*(255000-target)/(high-target);
    }
    uint32_t pixel=0xff000000u;
    for(unsigned i=0;i<3;++i) {
        const int64_t t=(tint>>(16-8*i))&255;
        int64_t v=(c[i]*(255-(int64_t)amount)+t*(int64_t)amount*1000+127500)/255000;
        pixel|=(uint32_t)(v<0 ? 0 : v>255 ? 255 : v)<<(16-8*i);
    }
    return pixel;
}
