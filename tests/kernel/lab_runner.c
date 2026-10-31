/* SPDX-License-Identifier: Apache-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lab_control.h"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"FAIL kernel-lab:%d: %s (Windows error %lu)\n",__LINE__,#x,GetLastError()); return 1; } } while(0)

static HANDLE open_lab(void) {
    return CreateFileW(L"\\\\.\\OpenLdacKernelLab",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
}
static DWORD ioctl(HANDLE file,DWORD code,void *input,DWORD in_size,void *output,DWORD out_size) {
    OVERLAPPED ov={0};DWORD bytes=0,error=0;
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!ov.hEvent)return GetLastError();
    if(!DeviceIoControl(file,code,input,in_size,output,out_size,&bytes,&ov)) {
        error=GetLastError();
        if(error==ERROR_IO_PENDING) {
            if(WaitForSingleObject(ov.hEvent,5000)!=WAIT_OBJECT_0) {
                CancelIoEx(file,&ov);
                GetOverlappedResult(file,&ov,&bytes,TRUE); /* Real retirement before stack buffer goes away. */
                error=ERROR_TIMEOUT;
            } else error=GetOverlappedResult(file,&ov,&bytes,FALSE)?0:GetLastError();
        }
    }
    CloseHandle(ov.hEvent);return error;
}
static DWORD command(HANDLE file,unsigned cmd,unsigned value) {
    ol_lab_request r={sizeof(r),OL_LAB_ABI,cmd,value};
    return ioctl(file,OL_LAB_IOCTL,&r,sizeof(r),NULL,0);
}
static DWORD connect_lab(HANDLE file) {
    ol_native_request r={sizeof(r),OL_NATIVE_ABI};
    return ioctl(file,OL_IOCTL_CONNECT,&r,sizeof(r),NULL,0);
}
static int roots(HANDLE control,unsigned expected) {
    ol_native_request r={sizeof(r),OL_NATIVE_ABI};ol_native_status s;
    for(unsigned i=0;i<200;++i) {
        memset(&s,0,sizeof(s));
        if(ioctl(control,OL_IOCTL_QUERY,&r,sizeof(r),&s,sizeof(s)))return 0;
        if(s.size!=sizeof(s)||s.version!=OL_NATIVE_ABI)return 0;
        if(s.connections==expected && s.in_flight==0)return 1;
        Sleep(10); /* Bounded user-mode evidence wait; no driver polling timer. */
    }
    return 0;
}
static int cancel_read(HANDLE file) {
    BYTE buffer[OL_MAX_PACKET];OVERLAPPED ov={0};DWORD bytes=0,error,wait_result;
    BOOL pending,cancelled=FALSE,completed;
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(ov.hEvent!=NULL);
    completed=ReadFile(file,buffer,sizeof(buffer),&bytes,&ov);
    pending=!completed && GetLastError()==ERROR_IO_PENDING;
    if(!pending) { CloseHandle(ov.hEvent);CHECK(pending); }
    cancelled=CancelIoEx(file,&ov);
    wait_result=WaitForSingleObject(ov.hEvent,5000);
    if(wait_result!=WAIT_OBJECT_0)CancelIoEx(file,&ov);
    completed=GetOverlappedResult(file,&ov,&bytes,TRUE);error=completed?0:GetLastError();
    CloseHandle(ov.hEvent);
    CHECK(cancelled);CHECK(wait_result==WAIT_OBJECT_0);CHECK(error==ERROR_OPERATION_ABORTED);return 0;
}
static int write_packet(HANDLE file) {
    BYTE packet[2]={0x10,1};OVERLAPPED ov={0};DWORD bytes=0,error=0;
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(ov.hEvent!=NULL);
    if(!WriteFile(file,packet,sizeof(packet),&bytes,&ov)) {
        error=GetLastError();
        if(error==ERROR_IO_PENDING) {
            DWORD waited=WaitForSingleObject(ov.hEvent,5000);
            if(waited!=WAIT_OBJECT_0)CancelIoEx(file,&ov);
            error=GetOverlappedResult(file,&ov,&bytes,TRUE)?0:GetLastError();
            if(waited!=WAIT_OBJECT_0)error=ERROR_TIMEOUT;
        }
    }
    CloseHandle(ov.hEvent);CHECK(!error && bytes==sizeof(packet));return 0;
}
int main(int argc,char **argv) {
    HKEY key;DWORD enabled=0,type=0,n=sizeof(enabled);
    HANDLE control,file;
    ol_native_request request={sizeof(request),OL_NATIVE_ABI};ol_native_diagnostics diagnostics;
    if(argc!=2 || strcmp(argv[1],"--isolated-guest")) {
        fprintf(stderr,"VM-only synthetic-provider runner. Requires --isolated-guest and guest lab marker.\n");return 2;
    }
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\OpenLdacKernelLab",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return 2;
    LONG status=RegQueryValueExW(key,L"Enabled",NULL,&type,(BYTE*)&enabled,&n);RegCloseKey(key);
    if(status!=ERROR_SUCCESS || type!=REG_DWORD || enabled!=1)return 2;
    control=open_lab();CHECK(control!=INVALID_HANDLE_VALUE);
    CHECK(!command(control,OL_LAB_RESET,0));CHECK(roots(control,0));
    for(unsigned i=0;i<25;++i) {
        file=open_lab();CHECK(file!=INVALID_HANDLE_VALUE);
        CHECK(!connect_lab(file));CHECK(!write_packet(file));CHECK(!cancel_read(file));
        CHECK(CloseHandle(file));CHECK(roots(control,0));
    }
    CHECK(!command(control,OL_LAB_FAIL_OPEN,1));
    file=open_lab();CHECK(file!=INVALID_HANDLE_VALUE);CHECK(connect_lab(file)!=0);
    CHECK(CloseHandle(file));CHECK(roots(control,0));

    CHECK(!command(control,OL_LAB_KEEP_CALLBACKS,1));
    for(unsigned i=0;i<OL_MAX_CHANNELS;++i) {
        file=open_lab();CHECK(file!=INVALID_HANDLE_VALUE);CHECK(!connect_lab(file));
        CHECK(!command(control,OL_LAB_FAIL_CLOSE,1));CHECK(CloseHandle(file));
        CHECK(roots(control,i+1));
    }
    file=open_lab();CHECK(file!=INVALID_HANDLE_VALUE);CHECK(connect_lab(file)!=0);CHECK(CloseHandle(file));
    CHECK(roots(control,2));CHECK(!command(control,OL_LAB_RELEASE_CALLBACKS,0));CHECK(roots(control,0));
    CHECK(!command(control,OL_LAB_KEEP_CALLBACKS,0));

    for(unsigned reason=0;reason<=3;++reason) {
        file=open_lab();CHECK(file!=INVALID_HANDLE_VALUE);CHECK(!connect_lab(file));
        CHECK(!command(control,OL_LAB_REMOTE_DISCONNECT,reason));CHECK(CloseHandle(file));
        CHECK(roots(control,0));CHECK(!command(control,OL_LAB_RESET,0));
    }
    file=open_lab();CHECK(file!=INVALID_HANDLE_VALUE);CHECK(!connect_lab(file));
    CHECK(!command(control,OL_LAB_REMOVE,0));CHECK(CloseHandle(file));CHECK(roots(control,1));
    CHECK(!command(control,OL_LAB_RELEASE_CALLBACKS,0));CHECK(roots(control,0));
    CHECK(!command(control,OL_LAB_RESET,0));
    memset(&diagnostics,0,sizeof(diagnostics));
    CHECK(!ioctl(control,OL_IOCTL_DIAGNOSTICS,&request,sizeof(request),&diagnostics,sizeof(diagnostics)));
    CHECK(diagnostics.size==sizeof(diagnostics) && diagnostics.count==OL_EVENT_COUNT);
    CHECK(diagnostics.events[OL_EVENT_CONTRACT_FAULT]==0);
    CHECK(diagnostics.events[OL_EVENT_CALLBACK_ADD]==diagnostics.events[OL_EVENT_CALLBACK_RELEASE]);
    CHECK(CloseHandle(control));
    printf("{\"kind\":\"real_wdf_synthetic_provider_kernel_lab\",\"checks\":%u,"
           "\"callback_add\":%llu,\"callback_release\":%llu,\"roots_destroyed\":%llu,"
           "\"real_bluetooth_tested\":false,\"headset_audio_tested\":false,"
           "\"verifier_status_requires_external_evidence\":true}\n",checks,
           (unsigned long long)diagnostics.events[OL_EVENT_CALLBACK_ADD],
           (unsigned long long)diagnostics.events[OL_EVENT_CALLBACK_RELEASE],
           (unsigned long long)diagnostics.events[OL_EVENT_ROOT_DESTROY]);
    return 0;
}
