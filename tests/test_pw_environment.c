/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_environment.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    char block[80]={0}, previous[80];size_t bytes=123;
    const char *items[]={"ROOT=selected","EMPTY=","UTF8=caf\xc3\xa9 \xce\xa9","PATH=C:\\Program Files\\Tools","EXPRESSION=a=b"};
    assert(!pw_environment_pack(block,sizeof(block),items,5,&bytes));
    assert(!pw_environment_validate(block,bytes));
    size_t position=0;
    for(unsigned i=0;i<5;++i) { assert(!strcmp(block+position,items[i]));position+=strlen(items[i])+1; }
    assert(bytes==position+1 && !block[bytes-1] && !block[bytes-2]);
    char exact[5];const char *one[]={"A="};
    assert(!pw_environment_pack(exact,4,one,1,&bytes) && bytes==4);
    memcpy(previous,block,sizeof(block));bytes=123;
    assert(pw_environment_pack(block,3,one,1,&bytes)<0 && errno==E2BIG && bytes==123 && !memcmp(block,previous,sizeof(block)));
    const char *bad[]={"NO_EQUALS"};
    assert(pw_environment_pack(block,sizeof(block),bad,1,&bytes)<0 && errno==EINVAL && bytes==123 && !memcmp(block,previous,sizeof(block)));
    bad[0]="=bad";assert(pw_environment_pack(block,sizeof(block),bad,1,&bytes)<0 && errno==EINVAL);
    bad[0]=NULL;assert(pw_environment_pack(block,sizeof(block),bad,1,&bytes)<0 && errno==EINVAL);
    assert(!pw_environment_pack(block,sizeof(block),NULL,0,&bytes) && bytes==2 && !pw_environment_validate(block,bytes));
    assert(pw_environment_validate("A=1\0",4)<0);
    assert(pw_environment_validate("A\0\0",3)<0);
    assert(pw_environment_validate("A=1\0\0B=2\0",10)<0);
    assert(pw_environment_validate("\0\0\0",3)<0);
    puts("test_pw_environment: ok");return 0;
}
