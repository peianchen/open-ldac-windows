/* SPDX-License-Identifier: Apache-2.0 */
/* Real Windows named-pipe I/O tests, never a driver or Bluetooth endpoint.
 * Including the actual adapter exposes only its static I/O ownership helpers;
 * no replacement ReadFile/WriteFile/CancelIoEx/wait implementation is used. */
#include "../../src/win_native_io.c"
#include <stdio.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if(!(c)) { \
    fprintf(stderr,"FAIL %s:%d: %s (Windows error %lu)\n",__FILE__,__LINE__,#c,GetLastError());return 1; \
} } while(0)
typedef struct {wl_win_native_io *io;HANDLE server;} pipe_fixture;

static int make_pipe(pipe_fixture *p) {
    static unsigned serial;wchar_t name[128];HANDLE client;
    OVERLAPPED connect={0};DWORD bytes=0,error;
    _snwprintf(name,sizeof(name)/sizeof(name[0]),L"\\\\.\\pipe\\OpenLdacOwnedIoTest-%lu-%u",
               GetCurrentProcessId(),++serial);
    memset(p,0,sizeof(*p));p->server=INVALID_HANDLE_VALUE;
    p->server=CreateNamedPipeW(name,PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
       PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,4096,4096,0,NULL);
    CHECK(p->server!=INVALID_HANDLE_VALUE);
    connect.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(connect.hEvent!=NULL);
    CHECK(!ConnectNamedPipe(p->server,&connect));error=GetLastError();CHECK(error==ERROR_IO_PENDING);
    client=CreateFileW(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    CHECK(client!=INVALID_HANDLE_VALUE);
    CHECK(GetOverlappedResult(p->server,&connect,&bytes,TRUE));CHECK(CloseHandle(connect.hEvent));
    {
        DWORD mode=PIPE_READMODE_MESSAGE;CHECK(SetNamedPipeHandleState(client,&mode,NULL,NULL));
    }
    /* Explicit test-only attach: create() allocates user-mode state and events
     * but never opens this path or sends native-driver IOCTLs. */
    p->io=wl_win_native_create(0,name,NULL,NULL);CHECK(p->io!=NULL);
    p->io->handles[WL_SIGNALING]=client;
    p->io->in_mtu[WL_SIGNALING]=p->io->out_mtu[WL_SIGNALING]=OL_MAX_PACKET;
    return 0;
}
static void close_pipe(pipe_fixture *p) {
    wl_win_native_destroy(p->io);p->io=NULL;
    if(p->server!=INVALID_HANDLE_VALUE)CloseHandle(p->server);
    p->server=INVALID_HANDLE_VALUE;
}
static int peer_write(pipe_fixture *p,const void *data,DWORD size) {
    DWORD bytes;
    CHECK(native_perform(p->io,p->server,&p->io->operations[WL_MEDIA],NATIVE_WRITE,
          0,data,size,NULL,0,&bytes,1000,NULL)==ERROR_SUCCESS);
    CHECK(bytes==size);return 0;
}

static int pending_and_short_tests(void) {
    pipe_fixture p;uint8_t data[16]={0};DWORD bytes,error;
    const uint8_t packet[]={0x12,9,0x55};native_slot *slot;
    CHECK(make_pipe(&p)==0);slot=&p.io->operations[WL_SIGNALING];
    CHECK(slot_begin(p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data))==ERROR_IO_PENDING);
    CHECK(slot->active && !slot->completed);
    CHECK(peer_write(&p,packet,sizeof(packet))==0);
    CHECK(wait_slot(p.io,p.io->handles[0],slot,1000,NULL)==0);
    CHECK(slot->completed && !slot->active && slot->bytes==sizeof(packet));
    CHECK(!memcmp(data,packet,sizeof(packet)));
    /* An actual short successful OS read supplies the exact byte count. The
     * same count validator used by native_send rejects any partial send; this
     * fixture does not claim that Windows produced a short successful write. */
    CHECK(exact_write_result(slot->bytes,sizeof(data))==ERROR_WRITE_FAULT);
    CHECK(exact_write_result(slot->bytes,sizeof(packet))==ERROR_SUCCESS);
    CHECK(peer_write(&p,packet,sizeof(packet))==0);
    error=native_perform(p.io,p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data),&bytes,1000,NULL);
    CHECK(error==0 && bytes==sizeof(packet) && !slot->active);
    close_pipe(&p);return 0;
}
static int cancel_timeout_race_tests(void) {
    pipe_fixture p;uint8_t data[8];DWORD bytes,error;native_slot *slot;
    CHECK(make_pipe(&p)==0);slot=&p.io->operations[0];
    CHECK(native_perform(p.io,p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data),&bytes,5,NULL)==ERROR_TIMEOUT);
    CHECK(!slot->active && slot->completed && slot->error==ERROR_OPERATION_ABORTED);
    CHECK(slot_begin(p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data))==ERROR_IO_PENDING);
    CHECK(CancelIoEx(p.io->handles[0],&slot->ov));
    CHECK(wait_slot(p.io,p.io->handles[0],slot,1000,NULL)==ERROR_OPERATION_ABORTED);
    CHECK(!slot->active && slot->completed);
    /* The completed request cannot be found by cancellation. Drain must still
     * obtain its successful result rather than treating NOT_FOUND as retirement. */
    CHECK(slot_begin(p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data))==ERROR_IO_PENDING);
    CHECK(peer_write(&p,"abc",3)==0);
    CHECK(WaitForSingleObject(slot->ov.hEvent,1000)==WAIT_OBJECT_0);
    CHECK(!CancelIoEx(p.io->handles[0],&slot->ov));CHECK(GetLastError()==ERROR_NOT_FOUND);
    slot_cancel_drain(p.io->handles[0],slot);
    CHECK(!slot->active && slot->completed && slot->error==0 && slot->bytes==3);
    /* Both completion and stop are ready. Stop wins, while actual completion
     * is drained and the next operation can reuse the slot safely. */
    CHECK(slot_begin(p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data))==ERROR_IO_PENDING);
    CHECK(peer_write(&p,"def",3)==0);CHECK(SetEvent(p.io->stop));
    error=wait_slot(p.io,p.io->handles[0],slot,1000,NULL);
    CHECK(error==ERROR_OPERATION_ABORTED && !slot->active && slot->completed);
    CHECK(ResetEvent(p.io->stop));
    CHECK(peer_write(&p,"ghi",3)==0);
    CHECK(native_perform(p.io,p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data),&bytes,1000,NULL)==0);
    CHECK(bytes==3 && !memcmp(data,"ghi",3));
    close_pipe(&p);return 0;
}
static int monitor_handoff_tests(void) {
    pipe_fixture p;uint8_t packet[OL_MAX_PACKET];size_t length=0;
    const uint8_t suspend[]={0x10,9},suspended[]={0x12,9};
    const uint8_t abort_command[]={0x20,10},aborted[]={0x22,10};
    CHECK(make_pipe(&p)==0);
    /* Test the pending-read state left by the final source wait directly.
     * This proves ownership/handoff without requiring VM scheduling to meet
     * a positive 10ms pacing deadline. It is not real-time acceptance. */
    CHECK(arm_monitor(p.io)==0 && p.io->monitor_armed && p.io->monitor.active);
    {
        OVERLAPPED original=p.io->monitor.ov;
        CHECK(arm_monitor(p.io)==0); /* idempotent: do not submit a second read */
        CHECK(p.io->monitor_armed && p.io->monitor.active);
        CHECK(!memcmp(&original,&p.io->monitor.ov,sizeof(original)));
    }
    CHECK(native_send(p.io,WL_SIGNALING,suspend,sizeof(suspend),1000)==0);
    CHECK(peer_write(&p,suspended,sizeof(suspended))==0);
    CHECK(native_receive(p.io,WL_SIGNALING,packet,sizeof(packet),&length,1000)==0);
    CHECK(length==sizeof(suspended) && !memcmp(packet,suspended,length));
    CHECK(!p.io->monitor_armed && !p.io->monitor.active && !p.io->operations[0].active);
    CHECK(native_send(p.io,WL_SIGNALING,abort_command,sizeof(abort_command),1000)==0);
    CHECK(peer_write(&p,aborted,sizeof(aborted))==0);
    CHECK(native_receive(p.io,WL_SIGNALING,packet,sizeof(packet),&length,1000)==0);
    CHECK(length==sizeof(aborted) && !memcmp(packet,aborted,length));
    CHECK(!p.io->monitor.active && !p.io->operations[0].active);
    close_pipe(&p);return 0;
}
static int reported_count_bounds_tests(void) {
    pipe_fixture p;uint8_t packet[OL_MAX_PACKET+16];size_t length=123;
    CHECK(make_pipe(&p)==0);
    /* This seam supplies an impossible reported count after a retired monitor.
     * It tests defensive bounds ordering, not an oversized OS write/read. */
    p.io->monitor_armed=1;p.io->monitor.completed=1;
    p.io->monitor.error=0;p.io->monitor.bytes=OL_MAX_PACKET+1;
    CHECK(SetEvent(p.io->monitor.ov.hEvent));
    memset(packet,0xa5,sizeof(packet));
    CHECK(native_receive(p.io,WL_SIGNALING,packet,OL_MAX_PACKET,&length,1000)!=0);
    CHECK(wl_win_native_last_error(p.io)==ERROR_INVALID_DATA && length==0);
    for(size_t i=0;i<sizeof(packet);++i)CHECK(packet[i]==0xa5);
    close_pipe(&p);return 0;
}
static int peer_failure_tests(void) {
    pipe_fixture p;const uint8_t unsolicited[]={0,9},media[]={1,2};HANDLE duplicate;
    CHECK(make_pipe(&p)==0);CHECK(arm_monitor(p.io)==0 && p.io->monitor.active);
    CHECK(DuplicateHandle(GetCurrentProcess(),p.io->handles[0],GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS));
    /* A separate test handle lets native_send execute its media validation;
     * this is a pipe seam, not evidence of independent Bluetooth channels. */
    p.io->handles[WL_MEDIA]=duplicate;p.io->out_mtu[WL_MEDIA]=OL_MAX_PACKET;p.io->in_mtu[WL_MEDIA]=OL_MAX_PACKET;
    CHECK(peer_write(&p,unsolicited,sizeof(unsolicited))==0);
    CHECK(WaitForSingleObject(p.io->monitor.ov.hEvent,1000)==WAIT_OBJECT_0);
    CHECK(native_send(p.io,WL_MEDIA,media,sizeof(media),1000)!=0);
    CHECK(wl_win_native_last_error(p.io)==ERROR_CONNECTION_ABORTED);
    close_pipe(&p);
    CHECK(make_pipe(&p)==0);CHECK(arm_monitor(p.io)==0 && p.io->monitor.active);
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->clock_started=1;p.io->clock_rate=48000;p.io->last_offset=0;
    CHECK(DisconnectNamedPipe(p.server));
    CHECK(WaitForSingleObject(p.io->monitor.ov.hEvent,1000)==WAIT_OBJECT_0);
    CHECK(native_wait_samples(p.io,480,48000)!=0);
    CHECK(wl_win_native_last_error(p.io)==ERROR_BROKEN_PIPE ||
          wl_win_native_last_error(p.io)==ERROR_PIPE_NOT_CONNECTED ||
          wl_win_native_last_error(p.io)==ERROR_NO_DATA);
    close_pipe(&p);return 0;
}
static DWORD WINAPI delayed_stop(void *context) {
    Sleep(10);SetEvent((HANDLE)context);return 0;
}
static int timer_and_validation_tests(void) {
    pipe_fixture p;HANDLE thread;unsigned out=0,in=0;volatile LONG request=990;
    CHECK(make_pipe(&p)==0);
    CHECK(native_open(p.io,2,&out,&in)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    CHECK(native_open(p.io,WL_SIGNALING,&out,&in)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_STATE);
    p.io->requested=&request;
    CHECK(native_requested(p.io)==990 && request==0 && native_requested(p.io)==0);
    /* Seed only the test clock. The timer target is 60 seconds away so this
     * is a stop/wait ownership test, not a positive VM scheduling benchmark. */
    CHECK(arm_monitor(p.io)==0 && p.io->monitor.active);
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->clock_started=1;p.io->clock_rate=48000;p.io->last_offset=0;
    CHECK(native_wait_samples(p.io,0,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    thread=CreateThread(NULL,0,delayed_stop,p.io->stop,0,NULL);CHECK(thread!=NULL);
    CHECK(native_wait_samples(p.io,UINT64_C(48000)*60,48000)!=0 &&
          wl_win_native_last_error(p.io)==ERROR_OPERATION_ABORTED);
    CHECK(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));
    CHECK(native_stopped(p.io));
    close_pipe(&p);
    CHECK(make_pipe(&p)==0);CHECK(arm_monitor(p.io)==0 && p.io->monitor.active);
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->clock_started=1;p.io->clock_rate=48000;p.io->last_offset=0;
    p.io->anchor.QuadPart-=p.io->frequency.QuadPart/10;
    CHECK(native_wait_samples(p.io,1,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_TIMEOUT);
    /* Close cancels and drains the still-pending monitor before destroying its
     * event, buffer and context. Repeated channel closes are harmless. */
    native_close(p.io,WL_MEDIA);native_close(p.io,WL_SIGNALING);native_close(p.io,WL_SIGNALING);
    CHECK(!p.io->monitor.active && !p.io->monitor_armed);
    close_pipe(&p);
    CHECK(wl_win_native_create(16,L"unused-owned-test-path",NULL,NULL)==NULL);
    CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
    return 0;
}
typedef struct {HANDLE server;LARGE_INTEGER before_read;DWORD error,bytes;} delayed_reader;
static DWORD WINAPI delayed_peer_read(void *context) {
    delayed_reader *reader=(delayed_reader *)context;OVERLAPPED ov={0};uint8_t packet[OL_MAX_PACKET];
    Sleep(50);
    QueryPerformanceCounter(&reader->before_read);
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!ov.hEvent){reader->error=GetLastError();return 1;}
    if(!ReadFile(reader->server,packet,sizeof(packet),&reader->bytes,&ov)) {
        reader->error=GetLastError();
        if(reader->error==ERROR_IO_PENDING)
            reader->error=GetOverlappedResult(reader->server,&ov,&reader->bytes,TRUE)?0:GetLastError();
    }
    CloseHandle(ov.hEvent);return reader->error?1:0;
}
static int attach_media_alias(pipe_fixture *p) {
    HANDLE duplicate;
    CHECK(DuplicateHandle(GetCurrentProcess(),p->io->handles[0],GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS));
    p->io->handles[WL_MEDIA]=duplicate;
    p->io->out_mtu[WL_MEDIA]=p->io->in_mtu[WL_MEDIA]=OL_MAX_PACKET;
    return 0;
}
static int terminal_drain_tests(void) {
    pipe_fixture p;LONGLONG target;LARGE_INTEGER ended;HANDLE thread;
    delayed_reader reader={0};uint8_t packet[OL_MAX_PACKET]={0};wl_playback_io bound;
    CHECK(terminal_target(1,1,44100,10000000,&target)==0 && target==228);
    CHECK(terminal_target(0,48000,48000,10000000,&target)==0 && target==10000000);
    CHECK(terminal_target(LLONG_MAX,1,48000,10000000,&target)==ERROR_ARITHMETIC_OVERFLOW);
    CHECK(terminal_target(0,UINT64_MAX,48000,10000000,&target)==ERROR_ARITHMETIC_OVERFLOW);
    CHECK(terminal_target(-1,0,48000,10000000,&target)==ERROR_INVALID_PARAMETER);
    CHECK(terminal_target(0,0,0,10000000,&target)==ERROR_INVALID_PARAMETER);
    CHECK(make_pipe(&p)==0);
    wl_win_native_bind(p.io,&bound);CHECK(bound.finish_media==native_finish_media);
    CHECK(native_finish_media(p.io,0,0,123)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    CHECK(native_finish_media(p.io,1,0,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_STATE);
    CHECK(native_finish_media(p.io,0,0,48000)==0);
    CHECK(!p.io->clock_started && !p.io->monitor_armed && p.io->media_finished);
    CHECK(native_finish_media(p.io,0,0,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_STATE);
    close_pipe(&p);

    CHECK(make_pipe(&p)==0);CHECK(attach_media_alias(&p)==0);
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->anchor.QuadPart-=p.io->frequency.QuadPart; /* nominal stream end already past */
    p.io->clock_started=1;p.io->clock_rate=48000;p.io->last_offset=0;
    /* Fill the owned pipe's 4096-byte outbound quota. The next 1008-byte
     * WriteFile completes after a delayed peer read frees quota. This is real
     * OS pipe backpressure, not a claim about Bluetooth transmit timing. */
    for(unsigned i=0;i<4;++i)CHECK(native_send(p.io,WL_MEDIA,packet,sizeof(packet),1000)==0);
    reader.server=p.server;
    thread=CreateThread(NULL,0,delayed_peer_read,&reader,0,NULL);CHECK(thread!=NULL);
    CHECK(native_send(p.io,WL_MEDIA,packet,sizeof(packet),2000)==0);
    CHECK(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));
    CHECK(reader.error==0 && reader.bytes==sizeof(packet));
    CHECK(p.io->media_written && p.io->last_media_completion.QuadPart>=reader.before_read.QuadPart);
    CHECK(terminal_target(p.io->last_media_completion.QuadPart,1920,48000,p.io->frequency.QuadPart,&target)==0);
    CHECK(native_finish_media(p.io,1920,1920,48000)==0);
    CHECK(QueryPerformanceCounter(&ended));CHECK(ended.QuadPart>=target);
    CHECK(p.io->monitor_armed && p.io->monitor.active && p.io->media_finished);
    CHECK(native_send(p.io,WL_MEDIA,packet,1,1000)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_STATE);
    {
        const uint8_t closed[]={0x12,8};uint8_t response[OL_MAX_PACKET];size_t bytes=0;
        CHECK(peer_write(&p,closed,sizeof(closed))==0);
        CHECK(native_receive(p.io,WL_SIGNALING,response,sizeof(response),&bytes,1000)==0);
        CHECK(bytes==sizeof(closed) && !memcmp(response,closed,bytes));
        CHECK(!p.io->monitor.active && !p.io->monitor_armed);
    }
    close_pipe(&p);

    /* Exercise the other max() branch: a fixture-seeded future absolute
     * sample timeline dominates the actual write-completion duration. Check
     * only the lower bound, never a VM-sensitive upper scheduling bound. */
    CHECK(make_pipe(&p)==0);CHECK(attach_media_alias(&p)==0);
    CHECK(native_send(p.io,WL_MEDIA,packet,1,1000)==0);
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->anchor.QuadPart+=p.io->frequency.QuadPart/10;
    p.io->clock_started=1;p.io->clock_rate=48000;p.io->last_offset=0;
    CHECK(terminal_target(p.io->anchor.QuadPart,1920,48000,p.io->frequency.QuadPart,&target)==0);
    {
        LONGLONG from_completion;
        CHECK(terminal_target(p.io->last_media_completion.QuadPart,1920,48000,p.io->frequency.QuadPart,&from_completion)==0);
        CHECK(target>from_completion);
    }
    CHECK(native_finish_media(p.io,1920,1920,48000)==0);
    CHECK(QueryPerformanceCounter(&ended));CHECK(ended.QuadPart>=target);
    CHECK(p.io->media_finished && p.io->monitor.active);
    close_pipe(&p);

    CHECK(make_pipe(&p)==0);CHECK(attach_media_alias(&p)==0);
    CHECK(QueryPerformanceCounter(&p.io->anchor));p.io->clock_started=1;p.io->clock_rate=48000;
    CHECK(native_send(p.io,WL_MEDIA,packet,1,1000)==0);
    CHECK(native_finish_media(p.io,1920,0,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    CHECK(native_finish_media(p.io,1920,1920,44100)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    CHECK(native_finish_media(p.io,1919,1920,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    /* Future test anchor makes cancellation independent of scheduling speed. */
    p.io->anchor.QuadPart+=p.io->frequency.QuadPart*60;
    thread=CreateThread(NULL,0,delayed_stop,p.io->stop,0,NULL);CHECK(thread!=NULL);
    CHECK(native_finish_media(p.io,1920,1920,48000)!=0 && wl_win_native_last_error(p.io)==ERROR_OPERATION_ABORTED);
    CHECK(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));
    CHECK(!p.io->media_finished);CHECK(ResetEvent(p.io->stop));
    CHECK(DisconnectNamedPipe(p.server));
    CHECK(WaitForSingleObject(p.io->monitor.ov.hEvent,1000)==WAIT_OBJECT_0);
    CHECK(native_finish_media(p.io,1920,1920,48000)!=0);
    CHECK(wl_win_native_last_error(p.io)==ERROR_BROKEN_PIPE ||
          wl_win_native_last_error(p.io)==ERROR_PIPE_NOT_CONNECTED || wl_win_native_last_error(p.io)==ERROR_NO_DATA);
    close_pipe(&p);return 0;
}
int main(void) {
    if(pending_and_short_tests() || cancel_timeout_race_tests() || monitor_handoff_tests() || reported_count_bounds_tests() ||
       peer_failure_tests() || timer_and_validation_tests() || terminal_drain_tests())return 1;
    printf("{\"kind\":\"real_windows_owned_pipe_native_io_tests\",\"checks\":%u,"
           "\"real_bluetooth_tested\":false,\"headset_audio_tested\":false}\n",checks);
    return 0;
}
