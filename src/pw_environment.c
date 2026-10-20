/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_environment.h"
#include <errno.h>
#include <string.h>
int pw_environment_pack(char *out,size_t capacity,const char *const *entries,
                        size_t count,size_t *bytes)
{
    size_t used=0;
    if(!out || !bytes || (count && !entries) || capacity<2) { errno=EINVAL;return -1; }
    for(size_t i=0;i<count;++i) {
        if(!entries[i]) { errno=EINVAL;return -1; }
        size_t length=strnlen(entries[i],capacity);
        if(length==capacity || length+2>capacity-used) { errno=E2BIG;return -1; }
        const char *equals=strchr(entries[i],'=');
        if(!equals || equals==entries[i]) { errno=EINVAL;return -1; }
        used+=length+1;
    }
    size_t offset=0;
    for(size_t i=0;i<count;++i) {
        size_t length=strlen(entries[i])+1;
        memcpy(out+offset,entries[i],length);offset+=length;
    }
    out[offset++]=0;
    if(!count)out[offset++]=0;
    *bytes=offset;return 0;
}
int pw_environment_validate(const char *block,size_t bytes)
{
    if(!block || bytes<2 || block[bytes-1] || block[bytes-2]) { errno=EINVAL;return -1; }
    if(!block[0]) {
        if(bytes==2)return 0;
        errno=EINVAL;return -1;
    }
    size_t offset=0;
    while(offset<bytes-1) {
        const char *end=memchr(block+offset,0,bytes-offset-1);
        if(!end || end==block+offset) { errno=EINVAL;return -1; }
        const char *equals=memchr(block+offset,'=',(size_t)(end-block-offset));
        if(!equals || equals==block+offset) { errno=EINVAL;return -1; }
        offset=(size_t)(end-block)+1;
    }
    if(offset==bytes-1)return 0;
    errno=EINVAL;return -1;
}
