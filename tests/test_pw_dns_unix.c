/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winbase.h"
#include "windns.h"
#define USE_WS_PREFIX
#include "ws2def.h"
#include "ws2ipdef.h"
#include "dnsapi.h"
#include "pw_wine_process.h"
static unsigned checks,failed;
static int unavailable;
static void check(int okay,const char *what) { ++checks; if(!okay){++failed;printf("FAIL %s\n",what);} }
int pw_wine_dns_config(struct pw_dns_config *config)
{
    memset(config,0,sizeof(*config));
    if(unavailable)return -1;
    FILE *f=fopen("/etc/resolv.conf","r");if(!f)return -1;
    char line[512],address[256];
    while(config->count<4 && fgets(line,sizeof(line),f)) {
        if(sscanf(line,"nameserver %255s",address)!=1)continue;
        struct pw_dns_server *server=&config->servers[config->count];
        if(inet_pton(AF_INET,address,server->address)==1)server->family=4;
        else if(inet_pton(AF_INET6,address,server->address)==1)server->family=6;
        else continue;
        ++config->count;
    }
    fclose(f);return config->count ? 0:-1;
}
extern const unixlib_entry_t __wine_unix_call_funcs[];
int main(void)
{
    DWORD size=0;struct get_serverlist_params p={WS_AF_UNSPEC,NULL,&size};
    check(!__wine_unix_call_funcs[unix_get_serverlist](&p),"config size");
    check(size>=FIELD_OFFSET(DNS_ADDR_ARRAY,AddrArray[1]),"config bounded");
    DNS_ADDR_ARRAY *data=calloc(1,size);DWORD full=size; p.addrs=data;
    size=1;check(__wine_unix_call_funcs[unix_get_serverlist](&p)==ERROR_MORE_DATA && size==full,"config short buffer");
    check(!__wine_unix_call_funcs[unix_get_serverlist](&p) && data->AddrCount>0,"config readback");
    free(data);
    unavailable=1;check(__wine_unix_call_funcs[unix_get_serverlist](&p)==DNS_ERROR_NO_DNS_SERVERS,"missing config error");unavailable=0;
    WORD types[]={DNS_TYPE_A,DNS_TYPE_AAAA,DNS_TYPE_TEXT,DNS_TYPE_A,DNS_TYPE_A};
    for(unsigned i=0;i<5;++i){
        unsigned char buffer[65536]={0};DWORD len=sizeof(buffer);
        struct query_params q={i==4 ? "ps5proton-dns-probe.invalid":"example.com",types[i],i==3 ? DNS_QUERY_USE_TCP_ONLY:0,buffer,&len};
        NTSTATUS status=__wine_unix_call_funcs[unix_query](&q);
        check(i==4 ? status==DNS_ERROR_RCODE_NAME_ERROR:status==0,"query status");
        if(i!=4)check(len>12 && len<=sizeof(buffer),"real packet length");
    }
    printf("PW_DNS_UNIX %s checks=%u failures=%u\n",failed?"FAIL":"PASS",checks,failed);return failed!=0;
}
