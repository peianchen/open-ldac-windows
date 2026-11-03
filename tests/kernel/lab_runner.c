/* SPDX-License-Identifier: Apache-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "lab_control.h"

static unsigned checks;
static unsigned pending_cases;
static unsigned case_kind=~0u,case_release_before=~0u;
static unsigned file_retirement_waits;
static ULONGLONG file_retirement_max_ms;
static int wait_files(HANDLE control,unsigned expected);
static int roots(HANDLE control,unsigned expected);
#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"FAIL kernel-lab:%d: %s (Windows error %lu, pending cases %u, kind %u, release-before %u)\n", \
        __LINE__,#x,GetLastError(),pending_cases,case_kind,case_release_before); \
    fflush(stderr); ExitProcess(1); } } while(0)

static HANDLE open_lab(void) {
    return CreateFileW(L"\\\\.\\OpenLdacKernelLab",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
}
static HANDLE open_stream(HANDLE control) {
    if(!wait_files(control,1)) { SetLastError(ERROR_TIMEOUT);return INVALID_HANDLE_VALUE; }
    return open_lab();
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
static DWORD lab_status(HANDLE control,ol_lab_status *s) {
    ol_lab_request r={sizeof(r),OL_LAB_ABI,OL_LAB_QUERY,0};
    DWORD error;
    memset(s,0,sizeof(*s));
    error=ioctl(control,OL_LAB_IOCTL,&r,sizeof(r),s,sizeof(*s));
    if(!error && (s->size!=sizeof(*s)||s->version!=OL_LAB_ABI))return ERROR_REVISION_MISMATCH;
    return error;
}
static DWORD diag(HANDLE control,ol_native_diagnostics *d) {
    ol_native_request r={sizeof(r),OL_NATIVE_ABI};
    memset(d,0,sizeof(*d));
    return ioctl(control,OL_IOCTL_DIAGNOSTICS,&r,sizeof(r),d,sizeof(*d));
}
static int wait_open(HANDLE control,unsigned pending,unsigned cancelled) {
    ol_lab_status s;
    for(unsigned i=0;i<500;++i) {
        if(lab_status(control,&s)||s.size!=sizeof(s)||s.version!=OL_LAB_ABI)return 0;
        if(s.pending_open==pending && s.held_cancel_seen==cancelled)return 1;
        Sleep(10);
    }
    return 0;
}
typedef struct { HANDLE file; BOOL closed; DWORD error; } close_args;
typedef struct { HANDLE file,gate; OVERLAPPED *ov; BOOL cancelled; DWORD error; } cancel_args;
static DWORD WINAPI cancel_file_thread(void *opaque) {
    cancel_args *a=(cancel_args*)opaque;
    if(WaitForSingleObject(a->gate,5000)!=WAIT_OBJECT_0) { a->error=ERROR_TIMEOUT;return 0; }
    a->cancelled=CancelIoEx(a->file,a->ov);a->error=a->cancelled?0:GetLastError();return 0;
}
static DWORD WINAPI close_file_thread(void *opaque) {
    close_args *a=(close_args*)opaque;
    a->closed=CloseHandle(a->file);a->error=a->closed?0:GetLastError();return 0;
}
/* Use IOCP so the completion can be collected even after file cleanup/close.
 * The OVERLAPPED and input remain alive until the actual completion packet.
 * Hold=2 intentionally refuses immediate cancellation, then completes under
 * user control to exercise the production late-OPEN-success CLOSE path. */
static int pending_open_case(HANDLE control,unsigned kind,unsigned release_before) {
    HANDLE file,port,thread=NULL;DWORD bytes=0,error=0;ULONG_PTR key=0;OVERLAPPED *done=NULL,ov={0};
    ol_native_request input={sizeof(input),OL_NATIVE_ABI};
    ol_native_diagnostics before,after;ol_lab_status a,b;
    close_args closing={0};cancel_args racing={0};BOOL ok;
    unsigned held=kind>=2&&kind<=5,close_file=(kind==3||kind==5),fail=(kind==4||kind==5);
    case_kind=kind;case_release_before=release_before;
    CHECK(!diag(control,&before));CHECK(!lab_status(control,&a));
    CHECK(!command(control,OL_LAB_HOLD_OPEN,held?2:1));
    CHECK(!command(control,OL_LAB_KEEP_CALLBACKS,1));
    file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);
    port=CreateIoCompletionPort(file,NULL,17,1);CHECK(port!=NULL);
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(ov.hEvent!=NULL);
    ok=DeviceIoControl(file,OL_IOCTL_CONNECT,&input,sizeof(input),NULL,0,&bytes,&ov);
    error=ok?0:GetLastError();CHECK(!ok && error==ERROR_IO_PENDING);
    CHECK(wait_open(control,1,0));CHECK(WaitForSingleObject(ov.hEvent,0)==WAIT_TIMEOUT);
    CHECK(roots(control,1));
    if(release_before)CHECK(!command(control,OL_LAB_RELEASE_CALLBACKS,0));
    if(close_file) {
        closing.file=file;
        thread=CreateThread(NULL,0,close_file_thread,&closing,0,NULL);CHECK(thread!=NULL);
        CHECK(wait_open(control,1,1));
    } else if(kind==6) {
        racing.file=file;racing.ov=&ov;racing.gate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(racing.gate!=NULL);
        thread=CreateThread(NULL,0,cancel_file_thread,&racing,0,NULL);CHECK(thread!=NULL);
        CHECK(SetEvent(racing.gate)); /* Race retrieval against real user cancellation. */
    } else if(kind!=1) {
        CHECK(CancelIoEx(file,&ov));
        if(held)CHECK(wait_open(control,1,1));
    }
    if(held) {
        /* Cancellation does not imply completion or buffer/root retirement. */
        CHECK(WaitForSingleObject(ov.hEvent,0)==WAIT_TIMEOUT);CHECK(roots(control,1));
    }
    if(kind!=0) {
        DWORD completion=command(control,OL_LAB_COMPLETE_OPEN,fail?1:0);
        CHECK(!completion || (kind==6 && completion==ERROR_NO_MORE_ITEMS));
    }
    ok=GetQueuedCompletionStatus(port,&bytes,&key,&done,5000);error=ok?0:GetLastError();
    CHECK(done==&ov && key==17 && bytes==0);
    if(kind==6)CHECK((ok&&!error)||(!ok&&error==ERROR_OPERATION_ABORTED));
    else if(kind==1)CHECK(ok && !error);
    else if(fail)CHECK(!ok && error==ERROR_GEN_FAILURE);
    else CHECK(!ok && error==ERROR_OPERATION_ABORTED);
    CHECK(wait_open(control,0,0));
    /* No second packet is permitted for this one request. */
    done=NULL;ok=GetQueuedCompletionStatus(port,&bytes,&key,&done,0);
    CHECK(!ok && GetLastError()==WAIT_TIMEOUT && done==NULL);
    if(kind==6) {
        CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
        CHECK(racing.cancelled || racing.error==ERROR_NOT_FOUND);
        CHECK(CloseHandle(thread));CHECK(CloseHandle(racing.gate));CHECK(CloseHandle(file));
    } else if(close_file) {
        CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CHECK(closing.closed && !closing.error);
        CHECK(CloseHandle(thread));
    } else CHECK(CloseHandle(file));
    CHECK(CloseHandle(ov.hEvent));CHECK(CloseHandle(port));
    if(!release_before)CHECK(roots(control,1));
    CHECK(!command(control,OL_LAB_RELEASE_CALLBACKS,0));CHECK(roots(control,0));
    CHECK(wait_files(control,1)); /* IRP/root completion can precede EvtFileClose. */
    CHECK(!lab_status(control,&b));CHECK(!diag(control,&after));
    CHECK(b.open_deferred-a.open_deferred==1);
    if(kind==6)CHECK((b.open_cancelled-a.open_cancelled)+(b.open_control_completed-a.open_control_completed)==1);
    else {
        CHECK(b.open_cancelled-a.open_cancelled==(kind==0?1u:0u));
        CHECK(b.open_control_completed-a.open_control_completed==(kind==0?0u:1u));
    }
    CHECK(after.events[OL_EVENT_OPEN_SEND]-before.events[OL_EVENT_OPEN_SEND]==1);
    CHECK(after.events[OL_EVENT_OPEN_COMPLETE]-before.events[OL_EVENT_OPEN_COMPLETE]==1);
    CHECK(after.events[OL_EVENT_CALLBACK_ADD]-before.events[OL_EVENT_CALLBACK_ADD]==1);
    CHECK(after.events[OL_EVENT_CALLBACK_RELEASE]-before.events[OL_EVENT_CALLBACK_RELEASE]==1);
    CHECK(after.events[OL_EVENT_ROOT_DESTROY]-before.events[OL_EVENT_ROOT_DESTROY]==1);
    CHECK(after.events[OL_EVENT_CLOSE_SEND]-before.events[OL_EVENT_CLOSE_SEND]==((!fail&&!release_before)?(b.open_control_completed-a.open_control_completed):0));
    CHECK(after.events[OL_EVENT_CLOSE_COMPLETE]-before.events[OL_EVENT_CLOSE_COMPLETE]==((!fail&&!release_before)?(b.open_control_completed-a.open_control_completed):0));
    CHECK(after.events[OL_EVENT_CONTRACT_FAULT]==before.events[OL_EVENT_CONTRACT_FAULT]);
    CHECK(!command(control,OL_LAB_KEEP_CALLBACKS,0));CHECK(!command(control,OL_LAB_HOLD_OPEN,0));
    ++pending_cases;case_kind=~0u;case_release_before=~0u;return 0;
}
static int wait_files(HANDLE control,unsigned expected) {
    ol_lab_status s;
    ULONGLONG start=GetTickCount64();
    for(unsigned i=0;i<500;++i) {
        if(lab_status(control,&s))return 0;
        if(s.active_files==expected) {
            ULONGLONG elapsed=GetTickCount64()-start;
            if(elapsed>file_retirement_max_ms)file_retirement_max_ms=elapsed;
            return 1;
        }
        ++file_retirement_waits;Sleep(10);
    }
    return 0;
}
static int roots(HANDLE control,unsigned expected) {
    ol_native_request r={sizeof(r),OL_NATIVE_ABI};ol_native_status s;
    for(unsigned i=0;i<200;++i) {
        memset(&s,0,sizeof(s));
        if(ioctl(control,OL_IOCTL_QUERY,&r,sizeof(r),&s,sizeof(s)))return 0;
        if(s.size!=sizeof(s)||s.version!=OL_NATIVE_ABI)return 0;
        if(s.connections==expected && s.in_flight==0)return expected?1:wait_files(control,1);
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
    if((argc!=2&&argc!=3) || strcmp(argv[1],"--isolated-guest") ||
        (argc==3&&strcmp(argv[2],"--inspect-only"))) {
        fprintf(stderr,"VM-only synthetic-provider runner. Requires --isolated-guest and guest lab marker.\n");return 2;
    }
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\OpenLdacKernelLab",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return 2;
    LONG status=RegQueryValueExW(key,L"Enabled",NULL,&type,(BYTE*)&enabled,&n);RegCloseKey(key);
    if(status!=ERROR_SUCCESS || type!=REG_DWORD || enabled!=1)return 2;
    control=open_lab();CHECK(control!=INVALID_HANDLE_VALUE);
    if(argc==3) {
        ol_native_status s;
        CHECK(!ioctl(control,OL_IOCTL_QUERY,&request,sizeof(request),&s,sizeof(s)));
        CHECK(!diag(control,&diagnostics));
        printf("{\"inspect_only\":true,\"connections\":%u,\"in_flight\":%u,\"callback_add\":%llu,\"callback_release\":%llu}\n",
            s.connections,s.in_flight,(unsigned long long)diagnostics.events[OL_EVENT_CALLBACK_ADD],
            (unsigned long long)diagnostics.events[OL_EVENT_CALLBACK_RELEASE]);
        CHECK(CloseHandle(control));return 0;
    }
    CHECK(!command(control,OL_LAB_RESET,0));CHECK(roots(control,0));
    for(unsigned repeat=0;repeat<10;++repeat)
        for(unsigned kind=0;kind<7;++kind)
            for(unsigned before=0;before<2;++before)CHECK(!pending_open_case(control,kind,before));
    for(unsigned i=0;i<25;++i) {
        file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);
        CHECK(!connect_lab(file));CHECK(!write_packet(file));CHECK(!cancel_read(file));
        CHECK(CloseHandle(file));CHECK(roots(control,0));
    }
    CHECK(!command(control,OL_LAB_FAIL_OPEN,1));
    file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);CHECK(connect_lab(file)!=0);
    CHECK(CloseHandle(file));CHECK(roots(control,0));

    CHECK(!command(control,OL_LAB_KEEP_CALLBACKS,1));
    for(unsigned i=0;i<OL_MAX_CHANNELS;++i) {
        file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);CHECK(!connect_lab(file));
        CHECK(!command(control,OL_LAB_FAIL_CLOSE,1));CHECK(CloseHandle(file));
        CHECK(roots(control,i+1));
    }
    file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);CHECK(connect_lab(file)!=0);CHECK(CloseHandle(file));
    CHECK(roots(control,2));CHECK(!command(control,OL_LAB_RELEASE_CALLBACKS,0));CHECK(roots(control,0));
    CHECK(!command(control,OL_LAB_KEEP_CALLBACKS,0));

    for(unsigned reason=0;reason<=3;++reason) {
        file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);CHECK(!connect_lab(file));
        CHECK(!command(control,OL_LAB_REMOTE_DISCONNECT,reason));CHECK(CloseHandle(file));
        CHECK(roots(control,0));CHECK(!command(control,OL_LAB_RESET,0));
    }
    file=open_stream(control);CHECK(file!=INVALID_HANDLE_VALUE);CHECK(!connect_lab(file));
    CHECK(!command(control,OL_LAB_REMOVE,0));CHECK(CloseHandle(file));CHECK(roots(control,1));
    CHECK(!command(control,OL_LAB_RELEASE_CALLBACKS,0));CHECK(roots(control,0));
    CHECK(!command(control,OL_LAB_RESET,0));
    memset(&diagnostics,0,sizeof(diagnostics));
    CHECK(!ioctl(control,OL_IOCTL_DIAGNOSTICS,&request,sizeof(request),&diagnostics,sizeof(diagnostics)));
    CHECK(diagnostics.size==sizeof(diagnostics) && diagnostics.count==OL_EVENT_COUNT);
    CHECK(diagnostics.events[OL_EVENT_CONTRACT_FAULT]==0);
    CHECK(diagnostics.events[OL_EVENT_CALLBACK_ADD]==diagnostics.events[OL_EVENT_CALLBACK_RELEASE]);
    CHECK(CloseHandle(control));
    printf("{\"kind\":\"real_wdf_synthetic_provider_kernel_lab\",\"checks\":%u,\"pending_open_cases\":%u,\"file_retirement_waits\":%u,\"file_retirement_max_ms\":%llu,"
           "\"callback_add\":%llu,\"callback_release\":%llu,\"roots_destroyed\":%llu,"
           "\"real_bluetooth_tested\":false,\"headset_audio_tested\":false,"
           "\"verifier_status_requires_external_evidence\":true}\n",checks,pending_cases,file_retirement_waits,(unsigned long long)file_retirement_max_ms,
           (unsigned long long)diagnostics.events[OL_EVENT_CALLBACK_ADD],
           (unsigned long long)diagnostics.events[OL_EVENT_CALLBACK_RELEASE],
           (unsigned long long)diagnostics.events[OL_EVENT_ROOT_DESTROY]);
    return 0;
}
