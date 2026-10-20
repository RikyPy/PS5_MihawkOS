/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The C library names behind pw_wine_compat on PS5. Linked into ntdll.so
 * (exported to win32u.so) and into wineserver; each signature is checked
 * against the SDK's FreeBSD headers. */
#include "pw_wine_compat.h"
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <net/if.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/extattr.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/thr.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>

int pipe(int fds[2]){return pw_compat_pipe(fds);}
int pipe2(int fds[2],int flags){return pw_compat_pipe2(fds,flags);}
int futimens(int fd,const struct timespec times[2]){return pw_compat_futimens(fd,times);}
int posix_fallocate(int fd,off_t offset,off_t length)
{return pw_compat_posix_fallocate(fd,offset,length);}

/* Title storage has no extended attributes. */
ssize_t extattr_get_fd(int fd,int ns,const char *name,void *data,size_t bytes)
{(void)fd;(void)ns;(void)name;(void)data;(void)bytes;return pw_compat_no_extattr();}
ssize_t extattr_get_file(const char *path,int ns,const char *name,void *data,size_t bytes)
{(void)path;(void)ns;(void)name;(void)data;(void)bytes;return pw_compat_no_extattr();}
ssize_t extattr_set_fd(int fd,int ns,const char *name,const void *data,size_t bytes)
{(void)fd;(void)ns;(void)name;(void)data;(void)bytes;return pw_compat_no_extattr();}
int extattr_delete_fd(int fd,int ns,const char *name)
{(void)fd;(void)ns;(void)name;return pw_compat_no_extattr();}

/* No mount table is visible to a title. */
int getmntinfo(struct statfs **mounts,int mode){(void)mode;*mounts=NULL;return 0;}

struct passwd *getpwuid(uid_t uid){return pw_compat_getpwuid(uid);}
int isatty(int fd){return pw_compat_isatty(fd);}
int posix_fadvise(int fd,off_t offset,off_t length,int advice)
{return pw_compat_posix_fadvise(fd,offset,length,advice);}
int fstatfs(int fd,struct statfs *buf){(void)fd;(void)buf;return pw_compat_no_fstatfs();}
int link(const char *from,const char *to){(void)from;(void)to;return pw_compat_no_link();}
int ptrace(int request,pid_t pid,caddr_t address,int data)
{(void)request;(void)pid;(void)address;(void)data;return (int)pw_compat_no_ptrace();}
mode_t umask(mode_t mask){return pw_compat_umask(mask);}
char *nl_langinfo(nl_item item){return (char *)(item==CODESET?pw_compat_codeset():"");}
clock_t times(struct tms *buffer){return pw_compat_times(buffer);}
int __xuname(int field_size,void *fields){return pw_compat_uname(fields,(size_t)field_size);}

speed_t cfgetospeed(const struct termios *t){return (speed_t)pw_compat_cfgetospeed(t);}
speed_t cfgetispeed(const struct termios *t){return (speed_t)pw_compat_cfgetispeed(t);}
int cfsetospeed(struct termios *t,speed_t speed){return pw_compat_cfsetospeed(t,speed);}
int cfsetispeed(struct termios *t,speed_t speed){return pw_compat_cfsetispeed(t,speed);}

/* Thread names are diagnostic only; the title's threads keep theirs. */
int thr_set_name(long id,const char *name){(void)id;(void)name;return 0;}

/* Set by ntdll's loader (pw_wine_dl_libc.c), which knows the loaded modules:
 * prints where the assertion fired and, in a helper, has the title end the
 * process without returning here (the thread may hold Wine's virtual mutex).
 * The server has none; it and the title abort as before. */
void (*pw_assert_trace)(const char *,const char *,const char *,int);

void __assert(const char *function,const char *file,int line,const char *expression)
{
    if(pw_assert_trace)pw_assert_trace(expression,function,file,line);
    else fprintf(stderr,"Assertion failed: %s (%s: %s: %d)\n",expression,function?function:"?",file,line);
    abort();
}
