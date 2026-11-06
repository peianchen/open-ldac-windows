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
    pipe_fixture p;uint8_t data[8];DWORD bytes,error;native_slot *slot;wl_win_native_timing timing;
    CHECK(make_pipe(&p)==0);slot=&p.io->operations[0];
    CHECK(native_perform(p.io,p.io->handles[0],slot,NATIVE_READ,0,NULL,0,data,sizeof(data),&bytes,5,NULL)==ERROR_TIMEOUT);
    CHECK(!slot->active && slot->completed && slot->error==ERROR_OPERATION_ABORTED);
    CHECK(wl_win_native_get_timing(p.io,&timing)==0);
    CHECK(timing.io_wait_timeouts==1 && timing.io_completion_timeout_errors==0);
    CHECK(timing.pacing_deadline_misses==0 && timing.peak_pacing_lateness_us==0);
    CHECK(timing.last_timeout_source==WL_WIN_TIMEOUT_IO_WAIT);
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
    CHECK(wl_win_native_get_timing(p.io,&timing)==0);
    CHECK(timing.io_wait_timeouts==1 && timing.io_completion_timeout_errors==0);
    CHECK(timing.last_timeout_source==WL_WIN_TIMEOUT_IO_WAIT);
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
    CHECK(native_poll_control(p.io,0)==WL_PLAY_IO_CONTROL);
    CHECK(native_send(p.io,WL_MEDIA,media,sizeof(media),1000)==0);
    CHECK(native_poll_control(p.io,0)==WL_PLAY_IO_CONTROL);
    CHECK(wl_win_native_last_error(p.io)==ERROR_SUCCESS);
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
    pipe_fixture p;HANDLE thread;unsigned out=0,in=0;volatile LONG request=990;wl_win_native_timing timing;
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
    CHECK(wl_win_native_get_timing(p.io,&timing)==0);
    CHECK(timing.pacing_deadline_misses==1 && timing.last_pacing_miss_offset==1);
    CHECK(timing.peak_pacing_lateness_us>WL_WIN_NATIVE_MAX_LATENESS_MS*1000u);
    CHECK(timing.io_wait_timeouts==0 && timing.io_completion_timeout_errors==0);
    CHECK(timing.last_timeout_source==WL_WIN_TIMEOUT_PACING);
    CHECK(timing.pacing_misses_before_timer_wait==1 && timing.pacing_misses_after_timer_wake==0);
    CHECK(timing.pacing_timer_wake_samples==0 && timing.peak_timer_wake_lateness_us==0);
    CHECK(timing.peak_before_wait_lateness_us==timing.peak_pacing_lateness_us);
    /* Close cancels and drains the still-pending monitor before destroying its
     * event, buffer and context. Repeated channel closes are harmless. */
    native_close(p.io,WL_MEDIA);native_close(p.io,WL_SIGNALING);native_close(p.io,WL_SIGNALING);
    CHECK(!p.io->monitor.active && !p.io->monitor_armed);
    /* Close/reopen clears the stream clock, never lifetime timing evidence. */
    CHECK(wl_win_native_get_timing(p.io,&timing)==0);
    CHECK(timing.pacing_deadline_misses==1 && timing.last_pacing_miss_offset==1);
    CHECK(timing.io_wait_timeouts==0 && timing.last_timeout_source==WL_WIN_TIMEOUT_PACING);
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
    CHECK(bound.now_ms!=NULL);
    { uint64_t before=GetTickCount64(),actual=bound.now_ms(p.io),after=GetTickCount64();
      CHECK(actual>=before&&actual<=after); }
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
    {
        wl_win_native_timing timing;
        CHECK(wl_win_native_get_timing(p.io,&timing)==0);
        CHECK(timing.media_write_calls==5 && timing.media_write_timing_samples==5);
        CHECK(timing.max_media_write_us>0);
        CHECK(timing.io_wait_timeouts==0 && timing.pacing_deadline_misses==0);
    }
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
static int control_readiness_pause_tests(void) {
    pipe_fixture p;uint8_t packet[OL_MAX_PACKET];size_t bytes=0;HANDLE thread;
    const uint8_t suspend[]={0x10,9,4};LARGE_INTEGER before_anchor,before_completion;
    LONGLONG shift;wl_playback_io bound;
    CHECK(make_pipe(&p)==0);
    wl_win_native_bind(p.io,&bound);
    CHECK(bound.poll_control==native_poll_control && bound.pause_clock==native_pause_clock);
    CHECK(native_poll_control(p.io,0)==0 && p.io->monitor.active);
    CHECK(native_poll_control(p.io,5)==0 && p.io->monitor.active);
    CHECK(native_pause_clock(p.io,2)<0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    CHECK(peer_write(&p,suspend,sizeof(suspend))==0);
    CHECK(native_poll_control(p.io,1000)==WL_PLAY_IO_CONTROL);
    CHECK(native_wait_samples(p.io,0,48000)==WL_PLAY_IO_CONTROL);
    CHECK(p.io->wait_pending && p.io->pending_offset==0);
    CHECK(native_wait_samples(p.io,0,48000)==WL_PLAY_IO_CONTROL);
    CHECK(native_wait_samples(p.io,128,48000)<0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    CHECK(native_receive(p.io,WL_SIGNALING,packet,sizeof(packet),&bytes,1000)==0);
    CHECK(bytes==sizeof(suspend) && !memcmp(packet,suspend,bytes));
    CHECK(native_poll_control(p.io,0)==0 && p.io->monitor.active);
    /* Seed a long future target: retry validation and stop ownership do not
     * depend on a VM meeting a short real-time media scheduling deadline. */
    CHECK(QueryPerformanceCounter(&p.io->anchor));p.io->anchor.QuadPart+=p.io->frequency.QuadPart*60;
    thread=CreateThread(NULL,0,delayed_stop,p.io->stop,0,NULL);CHECK(thread!=NULL);
    CHECK(native_wait_samples(p.io,0,48000)<0 && wl_win_native_last_error(p.io)==ERROR_OPERATION_ABORTED);
    CHECK(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));
    CHECK(ResetEvent(p.io->stop));
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->last_media_completion=p.io->anchor;p.io->media_written=1;
    before_anchor=p.io->anchor;before_completion=p.io->last_media_completion;
    CHECK(native_pause_clock(p.io,1)==0 && p.io->clock_paused);
    CHECK(native_pause_clock(p.io,1)==0); /* idempotent, does not reset pause start */
    Sleep(60);
    CHECK(native_wait_samples(p.io,0,48000)<0 && wl_win_native_last_error(p.io)==ERROR_INVALID_STATE);
    CHECK(native_pause_clock(p.io,0)==0 && !p.io->clock_paused);
    shift=p.io->anchor.QuadPart-before_anchor.QuadPart;
    CHECK(shift>=p.io->frequency.QuadPart/20);
    CHECK(p.io->last_media_completion.QuadPart-before_completion.QuadPart==shift);
    CHECK(native_pause_clock(p.io,0)==0 && p.io->anchor.QuadPart==before_anchor.QuadPart+shift);
    CHECK(native_pause_clock(p.io,1)==0);
    p.io->anchor.QuadPart=LLONG_MAX;
    Sleep(1);
    CHECK(native_pause_clock(p.io,0)<0 && wl_win_native_last_error(p.io)==ERROR_ARITHMETIC_OVERFLOW);
    CHECK(p.io->clock_paused && p.io->anchor.QuadPart==LLONG_MAX);
    close_pipe(&p);

    CHECK(make_pipe(&p)==0);
    CHECK(native_poll_control(p.io,0)==0);
    CHECK(peer_write(&p,suspend,sizeof(suspend))==0);
    CHECK(WaitForSingleObject(p.io->monitor.ov.hEvent,1000)==WAIT_OBJECT_0);
    CHECK(SetEvent(p.io->stop));
    CHECK(native_poll_control(p.io,0)<0 && wl_win_native_last_error(p.io)==ERROR_OPERATION_ABORTED);
    CHECK(ResetEvent(p.io->stop));
    CHECK(native_poll_control(p.io,0)==WL_PLAY_IO_CONTROL);
    CHECK(native_receive(p.io,WL_SIGNALING,packet,sizeof(packet),&bytes,1000)==0);
    thread=CreateThread(NULL,0,delayed_stop,p.io->stop,0,NULL);CHECK(thread!=NULL);
    CHECK(native_poll_control(p.io,60000)<0 && wl_win_native_last_error(p.io)==ERROR_OPERATION_ABORTED);
    CHECK(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));
    CHECK(p.io->monitor.active); /* owner retains monitor until close drains it */
    close_pipe(&p);

    CHECK(make_pipe(&p)==0);
    /* Defensive completion seam for zero-byte EOF, distinct from an actual
     * disconnect (covered above). It must never be reported as control data. */
    p.io->monitor_armed=1;p.io->monitor.completed=1;p.io->monitor.bytes=0;
    CHECK(SetEvent(p.io->monitor.ov.hEvent));
    CHECK(native_poll_control(p.io,0)<0 && wl_win_native_last_error(p.io)==ERROR_CONNECTION_ABORTED);
    close_pipe(&p);
    return 0;
}
static int interrupted_initial_wait_resume_success_test(void) {
    pipe_fixture p;uint8_t packet[OL_MAX_PACKET];size_t bytes=0;
    const uint8_t suspend[]={0x10,9,4};LARGE_INTEGER before_pause,after_wait;LONGLONG target;
    CHECK(make_pipe(&p)==0);
    CHECK(native_poll_control(p.io,0)==0);
    CHECK(peer_write(&p,suspend,sizeof(suspend))==0);
    CHECK(native_poll_control(p.io,1000)==WL_PLAY_IO_CONTROL);
    CHECK(native_wait_samples(p.io,0,48000)==WL_PLAY_IO_CONTROL);
    CHECK(p.io->wait_pending && p.io->pending_offset==0);
    CHECK(native_receive(p.io,WL_SIGNALING,packet,sizeof(packet),&bytes,1000)==0);
    CHECK(bytes==sizeof(suspend) && !memcmp(packet,suspend,bytes));
    /* A fixture-seeded 100ms future anchor avoids making pre-wait assertion
     * scheduling part of the test. The production wait and unchanged 40ms
     * lateness guard still execute after a real pause longer than 40ms. */
    CHECK(QueryPerformanceCounter(&p.io->anchor));
    p.io->anchor.QuadPart+=p.io->frequency.QuadPart/10;
    before_pause=p.io->anchor;
    CHECK(native_pause_clock(p.io,1)==0);
    Sleep(60);
    CHECK(native_pause_clock(p.io,0)==0);
    CHECK(p.io->anchor.QuadPart-before_pause.QuadPart>=p.io->frequency.QuadPart/20);
    CHECK(clock_target(p.io,0,48000,&target)==0);
    CHECK(native_wait_samples(p.io,0,48000)==0);
    CHECK(QueryPerformanceCounter(&after_wait));CHECK(after_wait.QuadPart>=target);
    CHECK(!p.io->wait_pending && p.io->last_offset==0);
    CHECK(native_wait_samples(p.io,0,48000)<0 && wl_win_native_last_error(p.io)==ERROR_INVALID_PARAMETER);
    close_pipe(&p);return 0;
}
typedef struct {HANDLE server;DWORD error,read_bytes;LARGE_INTEGER before_read;} control_reader;
static DWORD WINAPI signal_then_read(void *context) {
    control_reader *r=(control_reader *)context;OVERLAPPED ov={0};DWORD bytes=0;
    const uint8_t control[]={0x10,9,4};uint8_t packet[OL_MAX_PACKET];
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!ov.hEvent){r->error=GetLastError();return 1;}
    Sleep(10);
    if(!WriteFile(r->server,control,sizeof(control),&bytes,&ov)) {
        r->error=GetLastError();
        if(r->error==ERROR_IO_PENDING)r->error=GetOverlappedResult(r->server,&ov,&bytes,TRUE)?0:GetLastError();
    }
    if(r->error){CloseHandle(ov.hEvent);return 1;}
    Sleep(50);
    QueryPerformanceCounter(&r->before_read);
    {HANDLE event=ov.hEvent;memset(&ov,0,sizeof(ov));ov.hEvent=event;ResetEvent(event);}
    if(!ReadFile(r->server,packet,sizeof(packet),&r->read_bytes,&ov)) {
        r->error=GetLastError();
        if(r->error==ERROR_IO_PENDING)r->error=GetOverlappedResult(r->server,&ov,&r->read_bytes,TRUE)?0:GetLastError();
    }
    CloseHandle(ov.hEvent);return r->error?1:0;
}
static int control_during_write_and_finish_tests(void) {
    pipe_fixture p;uint8_t packet[OL_MAX_PACKET]={0};size_t bytes;control_reader reader={0};HANDLE thread;
    CHECK(make_pipe(&p)==0);CHECK(attach_media_alias(&p)==0);
    /* Real pipe quota makes the fifth write pending. A peer control arrives
     * before a later read releases quota; it must not cancel/retry that write. */
    for(unsigned i=0;i<4;++i)CHECK(native_send(p.io,WL_MEDIA,packet,sizeof(packet),1000)==0);
    reader.server=p.server;thread=CreateThread(NULL,0,signal_then_read,&reader,0,NULL);CHECK(thread!=NULL);
    CHECK(native_send(p.io,WL_MEDIA,packet,sizeof(packet),2000)==0);
    CHECK(WaitForSingleObject(thread,2000)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));
    CHECK(reader.error==0 && reader.read_bytes==sizeof(packet));
    CHECK(!p.io->operations[WL_MEDIA].active && p.io->operations[WL_MEDIA].bytes==sizeof(packet));
    CHECK(p.io->last_media_completion.QuadPart>=reader.before_read.QuadPart);
    CHECK(native_poll_control(p.io,0)==WL_PLAY_IO_CONTROL);
    CHECK(QueryPerformanceCounter(&p.io->anchor));p.io->anchor.QuadPart-=p.io->frequency.QuadPart;
    p.io->clock_started=1;p.io->clock_rate=48000;p.io->last_offset=0;
    CHECK(native_finish_media(p.io,128,128,48000)==WL_PLAY_IO_CONTROL);
    CHECK(!p.io->media_finished && p.io->monitor_armed);
    CHECK(native_receive(p.io,WL_SIGNALING,packet,sizeof(packet),&bytes,1000)==0);
    CHECK(bytes==3 && packet[0]==0x10 && packet[1]==9 && packet[2]==4);
    CHECK(native_finish_media(p.io,128,128,48000)==0 && p.io->media_finished);
    close_pipe(&p);return 0;
}
static int retryable_error_tests(void) {
    const DWORD positive[]={ERROR_NETNAME_DELETED,ERROR_CONNECTION_ABORTED,
        ERROR_CONNECTION_INVALID,ERROR_DEVICE_NOT_CONNECTED,ERROR_BROKEN_PIPE,
        ERROR_NO_DATA,ERROR_PIPE_NOT_CONNECTED};
    const DWORD negative[]={ERROR_SUCCESS,ERROR_OPERATION_ABORTED,ERROR_TIMEOUT,
        ERROR_SEM_TIMEOUT,WAIT_TIMEOUT,ERROR_NOT_ENOUGH_MEMORY,ERROR_OUTOFMEMORY,
        ERROR_NO_SYSTEM_RESOURCES,ERROR_ACCESS_DENIED,ERROR_INVALID_PARAMETER,
        ERROR_INVALID_DATA,ERROR_INVALID_HANDLE,ERROR_NOT_READY,ERROR_NO_MORE_ITEMS,
        ERROR_BUSY,ERROR_IO_PENDING,ERROR_IO_INCOMPLETE,ERROR_FILE_NOT_FOUND,0xffffffffu};
    for(unsigned i=0;i<sizeof(positive)/sizeof(positive[0]);++i)
        CHECK(wl_win_native_retryable_error(positive[i])==1);
    for(unsigned i=0;i<sizeof(negative)/sizeof(negative[0]);++i)
        CHECK(wl_win_native_retryable_error(negative[i])==0);
    CHECK(ERROR_NETNAME_DELETED==64);
    CHECK(ERROR_OPERATION_ABORTED==995);
    return 0;
}
static unsigned timer_factory_calls,timer_factory_contract_errors;
static DWORD timer_factory_first_error,timer_factory_second_error;
static int real_timer_high_resolution,real_timer_fallback;
static DWORD real_timer_high_resolution_error;
static HANDLE WINAPI timer_factory_seam(LPSECURITY_ATTRIBUTES attributes,LPCWSTR name,DWORD flags,DWORD access) {
    DWORD failure=timer_factory_calls?timer_factory_second_error:timer_factory_first_error;
    DWORD expected_flags=timer_factory_calls?0:CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
    if(attributes || name || flags!=expected_flags || access!=(SYNCHRONIZE|TIMER_MODIFY_STATE))
        ++timer_factory_contract_errors;
    ++timer_factory_calls;
    if(failure){SetLastError(failure);return NULL;}
    /* A real ordinary handle avoids fake HANDLE lifetime assumptions. This
     * seam tests selection/failure policy; actual high-resolution creation is
     * checked separately with the unmodified API below. */
    return CreateWaitableTimerExW(NULL,NULL,0,access);
}
static void reset_timer_factory(DWORD first,DWORD second) {
    timer_factory_calls=timer_factory_contract_errors=0;
    timer_factory_first_error=first;timer_factory_second_error=second;
}
static int high_resolution_timer_tests(void) {
    wl_win_native_timing timing={0};HANDLE timer;LARGE_INTEGER due;DWORD handle_flags;
    reset_timer_factory(0,0);
    timer=create_media_timer(&timing,timer_factory_seam);CHECK(timer!=NULL);
    CHECK(timer_factory_calls==1 && timer_factory_contract_errors==0);
    CHECK(timing.high_resolution_timer && !timing.timer_fallback_used && !timing.high_resolution_timer_error);
    CHECK(CloseHandle(timer));
    memset(&timing,0,sizeof(timing));reset_timer_factory(ERROR_INVALID_PARAMETER,0);
    timer=create_media_timer(&timing,timer_factory_seam);CHECK(timer!=NULL);
    CHECK(timer_factory_calls==2 && timer_factory_contract_errors==0);
    CHECK(!timing.high_resolution_timer && timing.timer_fallback_used);
    CHECK(timing.high_resolution_timer_error==ERROR_INVALID_PARAMETER);CHECK(CloseHandle(timer));
    memset(&timing,0,sizeof(timing));reset_timer_factory(ERROR_ACCESS_DENIED,0);
    CHECK(create_media_timer(&timing,timer_factory_seam)==NULL && GetLastError()==ERROR_ACCESS_DENIED);
    CHECK(timer_factory_calls==1 && !timer_factory_contract_errors && !timing.timer_fallback_used);
    CHECK(timing.high_resolution_timer_error==ERROR_ACCESS_DENIED);
    memset(&timing,0,sizeof(timing));reset_timer_factory(ERROR_NOT_ENOUGH_MEMORY,0);
    CHECK(create_media_timer(&timing,timer_factory_seam)==NULL && GetLastError()==ERROR_NOT_ENOUGH_MEMORY);
    CHECK(timer_factory_calls==1 && !timer_factory_contract_errors && !timing.timer_fallback_used);
    memset(&timing,0,sizeof(timing));reset_timer_factory(ERROR_INVALID_PARAMETER,ERROR_NOT_ENOUGH_MEMORY);
    CHECK(create_media_timer(&timing,timer_factory_seam)==NULL && GetLastError()==ERROR_NOT_ENOUGH_MEMORY);
    CHECK(timer_factory_calls==2 && !timer_factory_contract_errors && timing.timer_fallback_used);
    CHECK(timing.high_resolution_timer_error==ERROR_INVALID_PARAMETER);
    memset(&timing,0,sizeof(timing));
    timer=create_media_timer(&timing,CreateWaitableTimerExW);CHECK(timer!=NULL);
    CHECK(timing.high_resolution_timer ||
          (timing.timer_fallback_used && timing.high_resolution_timer_error==ERROR_INVALID_PARAMETER));
    real_timer_high_resolution=timing.high_resolution_timer;real_timer_fallback=timing.timer_fallback_used;
    real_timer_high_resolution_error=timing.high_resolution_timer_error;
    CHECK(GetHandleInformation(timer,&handle_flags));CHECK(!(handle_flags&HANDLE_FLAG_INHERIT));
    due.QuadPart=-10000; /* one millisecond, one shot; no system-wide timer request */
    CHECK(SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE));
    CHECK(WaitForSingleObject(timer,1000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(timer,0)==WAIT_TIMEOUT); /* auto-reset, not manual-reset */
    CHECK(CancelWaitableTimer(timer));CHECK(CloseHandle(timer));
    return 0;
}
static int timing_diagnostic_tests(void) {
    pipe_fixture p;wl_win_native_timing timing;uint64_t counter=UINT64_MAX;
    timing_increment(&counter);CHECK(counter==UINT64_MAX);
    counter=UINT64_MAX-1;timing_increment(&counter);CHECK(counter==UINT64_MAX);
    CHECK(timing_microseconds(1,10000000)==0);
    CHECK(timing_microseconds(10,10000000)==1);
    CHECK(timing_microseconds(400000,10000000)==40000);
    CHECK(timing_microseconds(LLONG_MAX,1)==UINT64_MAX);
    CHECK(timing_microseconds(-1,10000000)==0);
    CHECK(timing_microseconds(1,0)==0);
    memset(&timing,0,sizeof(timing));
    timing_pacing_observation(&timing,500,0);
    timing_pacing_observation(&timing,250,1);
    timing_pacing_observation(&timing,750,1);
    CHECK(timing.peak_pacing_lateness_us==750 && timing.peak_before_wait_lateness_us==500);
    CHECK(timing.peak_timer_wake_lateness_us==750 && timing.pacing_timer_wake_samples==2);
    timing.pacing_timer_wake_samples=UINT64_MAX;
    timing_pacing_observation(&timing,0,1);CHECK(timing.pacing_timer_wake_samples==UINT64_MAX);
    memset(&timing,0xa5,sizeof(timing));
    CHECK(wl_win_native_get_timing(NULL,&timing)<0);
    CHECK(timing.pacing_deadline_misses==0 && timing.io_wait_timeouts==0 && timing.media_write_calls==0);
    CHECK(wl_win_native_get_timing(NULL,NULL)<0);
    CHECK(make_pipe(&p)==0);
    CHECK(wl_win_native_get_timing(p.io,NULL)<0);
    CHECK(wl_win_native_get_timing(p.io,&timing)==0);
    CHECK(timing.last_timeout_source==WL_WIN_TIMEOUT_NONE && timing.media_write_timing_samples==0);
    /* Classification seam: injected returned error codes are not actual OS
     * completion-timeout evidence. Real wait timeout is exercised above. */
    CHECK(timing_io_completion(p.io,ERROR_OPERATION_ABORTED)==ERROR_OPERATION_ABORTED);
    CHECK(wl_win_native_get_timing(p.io,&timing)==0 && timing.io_completion_timeout_errors==0);
    CHECK(timing_io_completion(p.io,ERROR_TIMEOUT)==ERROR_TIMEOUT);
    CHECK(timing_io_completion(p.io,ERROR_SEM_TIMEOUT)==ERROR_SEM_TIMEOUT);
    CHECK(wl_win_native_get_timing(p.io,&timing)==0);
    CHECK(timing.io_completion_timeout_errors==2 && timing.io_wait_timeouts==0);
    CHECK(timing.pacing_deadline_misses==0 && timing.last_timeout_source==WL_WIN_TIMEOUT_IO_COMPLETION);
    native_close(p.io,WL_MEDIA);native_close(p.io,WL_SIGNALING);
    CHECK(wl_win_native_get_timing(p.io,&timing)==0 && timing.io_completion_timeout_errors==2);
    close_pipe(&p);return 0;
}
int main(void) {
    if(high_resolution_timer_tests() || timing_diagnostic_tests() || retryable_error_tests() || pending_and_short_tests() || cancel_timeout_race_tests() || monitor_handoff_tests() || reported_count_bounds_tests() ||
       peer_failure_tests() || timer_and_validation_tests() || terminal_drain_tests() ||
       control_readiness_pause_tests() || interrupted_initial_wait_resume_success_test() ||
       control_during_write_and_finish_tests())return 1;
    printf("{\"kind\":\"real_windows_owned_pipe_native_io_tests\",\"checks\":%u,"
           "\"real_bluetooth_tested\":false,\"headset_audio_tested\":false,"
           "\"high_resolution_timer_observed\":%s,\"timer_fallback_observed\":%s,"
           "\"high_resolution_timer_error\":%lu}\n",checks,
           real_timer_high_resolution?"true":"false",real_timer_fallback?"true":"false",
           (unsigned long)real_timer_high_resolution_error);
    return 0;
}
