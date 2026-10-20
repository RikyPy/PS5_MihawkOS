/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_wallpaper.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void put(unsigned char *p, uint32_t value, unsigned n) {
    for (unsigned i=0;i<n;++i) p[i]=(unsigned char)(value>>(8*i));
}
int main(void) {
    unsigned char bmp[78]={0};uint32_t pixels[6];
    memcpy(bmp,"BM",2);put(bmp+10,54,4);put(bmp+14,40,4);
    put(bmp+18,3,4);put(bmp+22,2,4);put(bmp+26,1,2);put(bmp+28,24,2);
    for(unsigned i=0;i<2;++i) for(unsigned x=0;x<3;++x) put(bmp+54+i*12+x*3,0x102030+i*0x100000+x,3);
    char path[]="/tmp/pw-wallpaper-XXXXXX";int fd=mkstemp(path);assert(fd>=0);
    assert(write(fd,bmp,sizeof(bmp))==sizeof(bmp));close(fd);
    assert(!pw_wine_wallpaper_load(path,pixels,3,2));assert(pixels[0]==0xff202030 && pixels[5]==0xff102032);
    put(bmp+22,(uint32_t)-2,4);FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bmp,1,sizeof(bmp),f)==sizeof(bmp));fclose(f);
    assert(!pw_wine_wallpaper_load(path,pixels,3,2));assert(pixels[0]==0xff102030 && pixels[5]==0xff202032);
    /* Downsampling uses pixel centers, not the top-left pixel of each cell. */
    assert(!pw_wine_wallpaper_load(path,pixels,1,1));assert(pixels[0]==0xff202031);
    f=fopen(path,"wb");assert(f);assert(fwrite(bmp,1,55,f)==55);fclose(f);
    assert(pw_wine_wallpaper_load(path,pixels,3,2)<0);for(unsigned i=0;i<6;++i)assert(!pixels[i]);
    assert(pw_wine_wallpaper_load(path,pixels,0,2)<0);
    unlink(path);assert(pw_wine_wallpaper_load(path,pixels,3,2)<0);
    puts("wallpaper: orientation, alignment, fill crop, truncation and bounds PASS");return 0;
}
