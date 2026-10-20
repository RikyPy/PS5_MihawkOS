/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Check native absence outside Sony's title-local process list. */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
/* Linked with -Wl,--wrap for both (tools/build-payload-loader.sh): the SDK's .so loader would grant execute by writing
 * the kernel's vm_map entry without its lock; this probe loads no .so, and any call is refused, not made. */
int __wrap_kernel_mprotect(int pid,unsigned long addr,unsigned long len,int prot){(void)pid;(void)addr;(void)len;(void)prot;errno=EPERM;return -1;}
int __wrap_kernel_set_vmem_protection(int pid,unsigned long addr,unsigned long len,int prot){(void)pid;(void)addr;(void)len;(void)prot;errno=EPERM;return -1;}
int main(int argc,char **argv) {
 int bad=0;
 for(int i=1;i<argc;++i) { int pid=atoi(argv[i]);if(pid<=1)return 2;errno=0;int status=kill(pid,0),error=errno;
 printf("payload-lifecycle pid=%d status=%d errno=%d absent=%d\n",pid,status,error,status<0 && error==ESRCH);fflush(stdout);bad|=!(status<0 && error==ESRCH); }
 return bad;
}
