/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_cursor.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
static PwWineCursor cursor;
static int done;
static void *publish(void *unused)
{
    (void)unused;
    for(int n=1;n<1000000;++n)pw_wine_cursor_store(&cursor,n,-n);
    __atomic_store_n(&done,1,__ATOMIC_RELEASE);
    return NULL;
}
int main(void)
{
    int x,y;
    pw_wine_cursor_read(&cursor,&x,&y);assert(!x && !y);
    pw_wine_cursor_store(&cursor,INT_MIN,INT_MAX);
    pw_wine_cursor_read(&cursor,&x,&y);assert(x==INT_MIN && y==INT_MAX);
    pw_wine_cursor_store(&cursor,0,0);pthread_t writer;
    assert(!pthread_create(&writer,NULL,publish,NULL));
    do {pw_wine_cursor_read(&cursor,&x,&y);assert(x==-y);}while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE));
    assert(!pthread_join(writer,NULL));
    pw_wine_cursor_read(&cursor,&x,&y);assert(x==999999 && y==-999999);
    puts("native cursor pair: PASS initial/signed limits/concurrent coherence/final publication");
}
