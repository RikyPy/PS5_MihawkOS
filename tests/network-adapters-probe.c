/* SPDX-License-Identifier: GPL-3.0-or-later
 * Asset-free NSI/IP Helper contract, identical PE32/PE64 source. No addresses,
 * adapter names or hardware identifiers are logged. DNS servers are queried
 * separately from interfaces: a family may have no configured DNS server. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
static unsigned checks, failures;
static void check(int ok, const char *what)
{
    char line[180]; ++checks;
    if (!ok) { ++failures; wsprintfA(line,"network-adapters: FAIL %s error=%lu\n",what,GetLastError()); OutputDebugStringA(line); }
}
static void query(ULONG family)
{
    ULONG size=0, count=0, result;
    char line[160];
    result=GetAdaptersAddresses(family,GAA_FLAG_INCLUDE_PREFIX|GAA_FLAG_SKIP_DNS_SERVER,NULL,NULL,&size);
    wsprintfA(line,"network-adapters: family=%lu size-status=%lu bytes=%lu\n",family,result,size); OutputDebugStringA(line);
    if (family!=AF_UNSPEC && result==ERROR_NO_DATA) { check(1,"family has no adapters"); return; }
    check(result==ERROR_BUFFER_OVERFLOW,"size-query status");
    check(size>0 && size<=1048576,"bounded size");
    if (result!=ERROR_BUFFER_OVERFLOW || !size || size>1048576) return;
    IP_ADAPTER_ADDRESSES *data=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size);
    check(data!=NULL,"query buffer");
    if (!data) return;
    result=GetAdaptersAddresses(family,GAA_FLAG_INCLUDE_PREFIX|GAA_FLAG_SKIP_DNS_SERVER,NULL,data,&size);
    check(result==NO_ERROR,"adapter query");
    if (!result) {
        IP_ADAPTER_ADDRESSES *a=data;
        for (; a && count<256; a=a->Next) {
            ++count;
            NET_LUID luid; NET_IFINDEX index=0;
            ULONG original=a->IfIndex ? a->IfIndex : a->Ipv6IfIndex;
            check(original!=0,"interface index");
            result=ConvertInterfaceIndexToLuid(original,&luid);
            check(result==NO_ERROR,"index to LUID");
            if (!result) {
                result=ConvertInterfaceLuidToIndex(&luid,&index);
                check(!result && index==original,"LUID roundtrip");
            }
        }
        check(count>0 && !a,"nonempty bounded list");
    }
    HeapFree(GetProcessHeap(),0,data);
}
void mainCRTStartup(void)
{
    WSADATA ws; char line[180];
    check(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock startup");
    HANDLE device=CreateFileW(L"\\\\.\\Nsi",0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    check(device!=INVALID_HANDLE_VALUE,"Nsi device");
    if(device!=INVALID_HANDLE_VALUE)CloseHandle(device);
    query(AF_UNSPEC);query(AF_INET);query(AF_INET6);
    WSACleanup();
    wsprintfA(line,"PS5_PROTON_NETWORK_ADAPTERS %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
    OutputDebugStringA(line);ExitProcess(failures!=0);
}
