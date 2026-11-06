/* SPDX-License-Identifier: Apache-2.0 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#include "win_native_io.h"
#include "native_transport.h"

/* This adapter has one serialized owner. Stop and bitrate requests are the
 * only cross-thread inputs; their caller-owned storage must outlive destroy.
 * No locks, worker callbacks, global timer-resolution change or streaming heap. */
typedef struct {
    OVERLAPPED ov;
    int active, completed;
    DWORD bytes, error;
} native_slot;
enum native_operation { NATIVE_IOCTL, NATIVE_READ, NATIVE_WRITE };
struct wl_win_native_io {
    wchar_t *path;
    HANDLE handles[2], stop, timer;
    int own_stop;
    native_slot operations[2], monitor;
    uint8_t monitor_buffer[OL_MAX_PACKET];
    unsigned out_mtu[2], in_mtu[2];
    int monitor_armed, clock_started, media_written, media_finished;
    LARGE_INTEGER frequency, anchor, last_media_completion;
    uint64_t last_offset;
    unsigned clock_rate;
    volatile LONG *requested;
    DWORD last_error;
};
static const GUID native_interface = OL_NATIVE_INTERFACE_GUID;
_Static_assert(sizeof(ol_native_status)==64,"native status ABI");

static DWORD slot_finish(HANDLE file,native_slot *slot,int wait) {
    if(slot->completed)return slot->error;
    if(!GetOverlappedResult(file,&slot->ov,&slot->bytes,wait?TRUE:FALSE)) {
        DWORD error=GetLastError();
        if(error==ERROR_IO_INCOMPLETE)return error;
        slot->error=error;
    } else slot->error=ERROR_SUCCESS;
    slot->active=0;slot->completed=1;return slot->error;
}
static void slot_cancel_drain(HANDLE file,native_slot *slot) {
    if(!slot->active)return;
    /* ERROR_NOT_FOUND is a normal completion/cancel race. Even other cancel
     * errors do not license freeing memory. TRUE waits for actual retirement;
     * an uncooperative lower driver can therefore stall this owner safely.
     * https://learn.microsoft.com/windows/win32/api/ioapiset/nf-ioapiset-cancelioex */
    (void)CancelIoEx(file,&slot->ov);
    (void)slot_finish(file,slot,1);
}
static DWORD slot_begin(HANDLE file,native_slot *slot,enum native_operation op,
                        DWORD code,const void *input,DWORD input_size,
                        void *output,DWORD output_size) {
    BOOL ok;HANDLE event=slot->ov.hEvent;
    if(slot->active)return ERROR_BUSY;
    memset(slot,0,sizeof(*slot));slot->ov.hEvent=event;
    if(!ResetEvent(event))return GetLastError();
    if(op==NATIVE_IOCTL)
        ok=DeviceIoControl(file,code,(void *)input,input_size,output,output_size,&slot->bytes,&slot->ov);
    else if(op==NATIVE_READ)ok=ReadFile(file,output,output_size,&slot->bytes,&slot->ov);
    else ok=WriteFile(file,input,input_size,&slot->bytes,&slot->ov);
    if(!ok) {
        slot->error=GetLastError();
        if(slot->error==ERROR_IO_PENDING){slot->active=1;return ERROR_IO_PENDING;}
    }
    slot->completed=1;
    /* A synchronous completion is already retired, including short/error
     * completions. The explicit signal also supports the persistent monitor. */
    (void)SetEvent(event);return slot->error;
}
static DWORD stopped_error(wl_win_native_io *io) {
    DWORD wait=WaitForSingleObject(io->stop,0);
    if(wait==WAIT_OBJECT_0)return ERROR_OPERATION_ABORTED;
    if(wait==WAIT_FAILED)return GetLastError();
    return ERROR_SUCCESS;
}
static DWORD wait_slot(wl_win_native_io *io,HANDLE file,native_slot *slot,
                       DWORD timeout,HANDLE peer_event) {
    HANDLE events[3];DWORD count=0,wait,error;
    events[count++]=io->stop;
    if(peer_event)events[count++]=peer_event;
    events[count++]=slot->ov.hEvent;
    wait=WaitForMultipleObjects(count,events,FALSE,timeout);
    if(wait==WAIT_OBJECT_0)error=ERROR_OPERATION_ABORTED;
    else if(peer_event && wait==WAIT_OBJECT_0+1)error=ERROR_CONNECTION_ABORTED;
    else if(wait==WAIT_OBJECT_0+count-1) {
        error=slot_finish(file,slot,0);
        if(error!=ERROR_IO_INCOMPLETE)return error;
    } else error=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:wait==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE;
    slot_cancel_drain(file,slot);return error;
}
/* The same implementation is exercised by owned named pipes in the OS tests. */
static DWORD native_perform(wl_win_native_io *io,HANDLE file,native_slot *slot,
                            enum native_operation op,DWORD code,const void *input,
                            DWORD input_size,void *output,DWORD output_size,
                            DWORD *bytes,DWORD timeout,HANDLE peer_event) {
    DWORD error;*bytes=0;
    if((error=stopped_error(io))!=0)return error;
    if(peer_event && WaitForSingleObject(peer_event,0)==WAIT_OBJECT_0)return ERROR_CONNECTION_ABORTED;
    error=slot_begin(file,slot,op,code,input,input_size,output,output_size);
    if(error==ERROR_IO_PENDING)error=wait_slot(io,file,slot,timeout,peer_event);
    *bytes=slot->bytes;
    if(!error)error=stopped_error(io);
    if(!error && peer_event && WaitForSingleObject(peer_event,0)==WAIT_OBJECT_0)error=ERROR_CONNECTION_ABORTED;
    return error;
}
static DWORD exact_write_result(DWORD bytes,size_t expected) {
    return bytes==expected?ERROR_SUCCESS:ERROR_WRITE_FAULT;
}
static int result(wl_win_native_io *io,DWORD error) {
    io->last_error=error;return error?-1:0;
}
static DWORD select_path(unsigned index,wchar_t **chosen) {
    HDEVINFO devices;SP_DEVICE_INTERFACE_DATA item;DWORD needed=0,error=ERROR_SUCCESS;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail=NULL;
    *chosen=NULL;
    devices=SetupDiGetClassDevsW(&native_interface,NULL,NULL,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
    if(devices==INVALID_HANDLE_VALUE)return GetLastError();
    memset(&item,0,sizeof(item));item.cbSize=sizeof(item);
    if(!SetupDiEnumDeviceInterfaces(devices,NULL,&native_interface,index,&item)){error=GetLastError();goto done;}
    if(SetupDiGetDeviceInterfaceDetailW(devices,&item,NULL,0,&needed,NULL) ||
       GetLastError()!=ERROR_INSUFFICIENT_BUFFER || needed<sizeof(*detail) || needed>65536) {
        error=ERROR_INVALID_DATA;goto done;
    }
    detail=(SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(needed);
    if(!detail){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
    memset(detail,0,needed);detail->cbSize=sizeof(*detail);
    if(!SetupDiGetDeviceInterfaceDetailW(devices,&item,detail,needed,NULL,NULL)){error=GetLastError();goto done;}
    {
        size_t offset=(size_t)((uint8_t *)detail->DevicePath-(uint8_t *)detail);
        size_t capacity=(needed-offset)/sizeof(wchar_t),length=0;
        while(length<capacity && detail->DevicePath[length])++length;
        if(length==capacity || !length){error=ERROR_INVALID_DATA;goto done;}
        *chosen=(wchar_t *)malloc((length+1)*sizeof(wchar_t));
        if(!*chosen){error=ERROR_NOT_ENOUGH_MEMORY;goto done;}
        memcpy(*chosen,detail->DevicePath,(length+1)*sizeof(wchar_t));
    }
done:
    free(detail);SetupDiDestroyDeviceInfoList(devices);return error;
}
static DWORD arm_monitor(wl_win_native_io *io) {
    DWORD error;
    if(io->monitor_armed)return ERROR_SUCCESS;
    if(io->handles[WL_SIGNALING]==INVALID_HANDLE_VALUE)return ERROR_INVALID_HANDLE;
    error=slot_begin(io->handles[WL_SIGNALING],&io->monitor,NATIVE_READ,0,NULL,0,
                     io->monitor_buffer,io->in_mtu[WL_SIGNALING]);
    io->monitor_armed=1;
    return error==ERROR_IO_PENDING?ERROR_SUCCESS:error;
}
static DWORD monitor_error(wl_win_native_io *io) {
    DWORD wait,error;
    if(!io->monitor_armed)return ERROR_SUCCESS;
    wait=WaitForSingleObject(io->monitor.ov.hEvent,0);
    if(wait==WAIT_TIMEOUT)return ERROR_SUCCESS;
    if(wait==WAIT_FAILED)return GetLastError();
    error=slot_finish(io->handles[WL_SIGNALING],&io->monitor,0);
    if(error==ERROR_IO_INCOMPLETE)return ERROR_SUCCESS;
    /* This subset does not dispatch unsolicited signaling commands/responses
     * while streaming. Any packet, zero-byte EOF or transport error is fatal;
     * a future owner-thread remote-command policy may route these packets. */
    return error?error:ERROR_CONNECTION_ABORTED;
}
static int native_open(void *context,unsigned channel,unsigned *out_mtu,unsigned *in_mtu) {
    wl_win_native_io *io=(wl_win_native_io *)context;DWORD error,bytes;
    ol_native_request request={sizeof(request),OL_NATIVE_ABI};ol_native_status status;
    if(!io || channel>WL_MEDIA || !out_mtu || !in_mtu)return io?result(io,ERROR_INVALID_PARAMETER):-1;
    if(io->handles[channel]!=INVALID_HANDLE_VALUE ||
       (channel==WL_MEDIA && io->handles[WL_SIGNALING]==INVALID_HANDLE_VALUE))return result(io,ERROR_INVALID_STATE);
    if((error=stopped_error(io))!=0)return result(io,error);
    io->handles[channel]=CreateFileW(io->path,GENERIC_READ|GENERIC_WRITE,
         FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    if(io->handles[channel]==INVALID_HANDLE_VALUE)return result(io,GetLastError());
    error=native_perform(io,io->handles[channel],&io->operations[channel],NATIVE_IOCTL,
          OL_IOCTL_CONNECT,&request,sizeof(request),NULL,0,&bytes,15000,NULL);
    if(!error && bytes)error=ERROR_INVALID_DATA;
    memset(&status,0,sizeof(status));
    if(!error)error=native_perform(io,io->handles[channel],&io->operations[channel],NATIVE_IOCTL,
          OL_IOCTL_QUERY,&request,sizeof(request),&status,sizeof(status),&bytes,5000,NULL);
    if(!error && (bytes!=sizeof(status) || status.size!=sizeof(status) || status.version!=OL_NATIVE_ABI ||
         status.connection_state!=OL_STATE_OPEN || status.max_packet!=OL_MAX_PACKET ||
         status.max_channels!=OL_MAX_CHANNELS || status.outbound_mtu<48 || status.outbound_mtu>OL_MAX_PACKET ||
         status.inbound_mtu<48 || status.inbound_mtu>OL_MAX_PACKET))error=ERROR_INVALID_DATA;
    if(error) {
        slot_cancel_drain(io->handles[channel],&io->operations[channel]);
        CloseHandle(io->handles[channel]);io->handles[channel]=INVALID_HANDLE_VALUE;
        return result(io,error);
    }
    io->out_mtu[channel]=status.outbound_mtu;io->in_mtu[channel]=status.inbound_mtu;
    *out_mtu=status.outbound_mtu;*in_mtu=status.inbound_mtu;
    return result(io,ERROR_SUCCESS);
}
static int native_send(void *context,unsigned channel,const uint8_t *packet,size_t length,unsigned timeout) {
    wl_win_native_io *io=(wl_win_native_io *)context;DWORD bytes,error;HANDLE peer=NULL;
    if(!io || channel>WL_MEDIA || !packet || !length || length>OL_MAX_PACKET)return io?result(io,ERROR_INVALID_PARAMETER):-1;
    if(io->handles[channel]==INVALID_HANDLE_VALUE)return result(io,ERROR_INVALID_HANDLE);
    if(length>io->out_mtu[channel])return result(io,ERROR_BAD_LENGTH);
    if(channel==WL_MEDIA) {
        if(io->media_finished)return result(io,ERROR_INVALID_STATE);
        if((error=arm_monitor(io))!=0 || (error=monitor_error(io))!=0)return result(io,error);
        peer=io->monitor.ov.hEvent;
    }
    error=native_perform(io,io->handles[channel],&io->operations[channel],NATIVE_WRITE,
          0,packet,(DWORD)length,NULL,0,&bytes,timeout,peer);
    if(!error)error=exact_write_result(bytes,length);
    if(!error && channel==WL_MEDIA) {
        if(!QueryPerformanceCounter(&io->last_media_completion))error=ERROR_GEN_FAILURE;
        else io->media_written=1;
    }
    return result(io,error);
}
static int native_receive(void *context,unsigned channel,uint8_t *packet,size_t capacity,size_t *length,unsigned timeout) {
    wl_win_native_io *io=(wl_win_native_io *)context;DWORD bytes=0,error;
    if(length)*length=0;
    if(!io || channel>WL_MEDIA || !packet || !length)return io?result(io,ERROR_INVALID_PARAMETER):-1;
    if(io->handles[channel]==INVALID_HANDLE_VALUE)return result(io,ERROR_INVALID_HANDLE);
    /* Refuse a too-small buffer rather than silently truncate an L2CAP SDU. */
    if(capacity<io->in_mtu[channel])return result(io,ERROR_INSUFFICIENT_BUFFER);
    if(channel==WL_SIGNALING && io->monitor_armed) {
        error=stopped_error(io);
        if(!error)error=wait_slot(io,io->handles[channel],&io->monitor,timeout,NULL);
        else slot_cancel_drain(io->handles[channel],&io->monitor);
        bytes=io->monitor.bytes;io->monitor_armed=0;
        if(!error)error=stopped_error(io);
        if(!error && bytes && bytes<=io->in_mtu[channel] && bytes<=OL_MAX_PACKET)
            memcpy(packet,io->monitor_buffer,bytes);
    } else error=native_perform(io,io->handles[channel],&io->operations[channel],NATIVE_READ,
                 0,NULL,0,packet,io->in_mtu[channel],&bytes,timeout,NULL);
    if(!error && (!bytes || bytes>io->in_mtu[channel] || bytes>OL_MAX_PACKET))error=ERROR_INVALID_DATA;
    if(!error)*length=bytes;
    return result(io,error);
}
static DWORD clock_target(wl_win_native_io *io,uint64_t offset,unsigned rate,LONGLONG *target) {
    uint64_t seconds=offset/rate,remainder=offset%rate;
    uint64_t frequency=(uint64_t)io->frequency.QuadPart,whole,fraction;
    if(seconds>(uint64_t)LLONG_MAX/frequency || remainder>UINT64_MAX/frequency)return ERROR_ARITHMETIC_OVERFLOW;
    whole=seconds*frequency;fraction=remainder*frequency/rate;
    if(whole>(uint64_t)LLONG_MAX-fraction || whole+fraction>(uint64_t)LLONG_MAX-(uint64_t)io->anchor.QuadPart)
        return ERROR_ARITHMETIC_OVERFLOW;
    *target=io->anchor.QuadPart+(LONGLONG)(whole+fraction);return ERROR_SUCCESS;
}
static int native_wait_samples(void *context,uint64_t offset,unsigned rate) {
    wl_win_native_io *io=(wl_win_native_io *)context;LARGE_INTEGER now,due;LONGLONG target;DWORD error,wait;
    HANDLE events[3];
    if(!io || (rate!=44100 && rate!=48000 && rate!=88200 && rate!=96000))return io?result(io,ERROR_INVALID_PARAMETER):-1;
    if((error=stopped_error(io))!=0)return result(io,error);
    if(!io->clock_started) {
        if(offset!=0)return result(io,ERROR_INVALID_PARAMETER);
        if(!QueryPerformanceCounter(&io->anchor))return result(io,ERROR_GEN_FAILURE);
        io->clock_started=1;io->clock_rate=rate;io->last_offset=0;
    } else if(rate!=io->clock_rate || offset<=io->last_offset)return result(io,ERROR_INVALID_PARAMETER);
    if((error=arm_monitor(io))!=0 || (error=monitor_error(io))!=0)return result(io,error);
    if((error=clock_target(io,offset,rate,&target))!=0)return result(io,error);
    events[0]=io->stop;events[1]=io->monitor.ov.hEvent;events[2]=io->timer;
    for(;;) {
        long double remaining;
        if((error=stopped_error(io))!=0 || (error=monitor_error(io))!=0)return result(io,error);
        if(!QueryPerformanceCounter(&now))return result(io,ERROR_GEN_FAILURE);
        if(now.QuadPart>=target) {
            if((long double)(now.QuadPart-target)*1000.0L>(long double)io->frequency.QuadPart*40.0L)
                return result(io,ERROR_TIMEOUT);
            io->last_offset=offset;return result(io,ERROR_SUCCESS);
        }
        remaining=(long double)(target-now.QuadPart)*10000000.0L/(long double)io->frequency.QuadPart;
        if(remaining>=(long double)LLONG_MAX-1)return result(io,ERROR_ARITHMETIC_OVERFLOW);
        due.QuadPart=-(LONGLONG)(remaining+1.0L);
        if(!SetWaitableTimer(io->timer,&due,0,NULL,NULL,FALSE))return result(io,GetLastError());
        wait=WaitForMultipleObjects(3,events,FALSE,INFINITE);
        if(wait==WAIT_OBJECT_0)return result(io,ERROR_OPERATION_ABORTED);
        if(wait==WAIT_OBJECT_0+1) {
            error=monitor_error(io);return result(io,error?error:ERROR_CONNECTION_ABORTED);
        }
        if(wait!=WAIT_OBJECT_0+2)return result(io,wait==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE);
        /* Recheck the absolute QPC deadline after every timer wake; rounding
         * never turns into an active spin or changes the source sample clock. */
    }
}
/* Ceiling conversion is used only for the terminal sender drain. It never
 * shortens either the absolute encoded duration or the duration measured from
 * the final successful write completion. This is not a receiver playout ACK. */
static DWORD terminal_target(LONGLONG anchor,uint64_t samples,unsigned rate,
                             LONGLONG frequency,LONGLONG *target) {
    uint64_t seconds,remainder,whole,product,fraction;
    if(anchor<0 || frequency<=0 || !rate)return ERROR_INVALID_PARAMETER;
    seconds=samples/rate;remainder=samples%rate;
    if(seconds>(uint64_t)LLONG_MAX/(uint64_t)frequency ||
       remainder>UINT64_MAX/(uint64_t)frequency)return ERROR_ARITHMETIC_OVERFLOW;
    whole=seconds*(uint64_t)frequency;product=remainder*(uint64_t)frequency;
    fraction=product/rate+(product%rate!=0);
    if(fraction>(uint64_t)LLONG_MAX-whole ||
       whole+fraction>(uint64_t)(LLONG_MAX-anchor))return ERROR_ARITHMETIC_OVERFLOW;
    *target=anchor+(LONGLONG)(whole+fraction);return ERROR_SUCCESS;
}
static int native_finish_media(void *context,uint64_t end_offset,
                               unsigned final_packet_samples,unsigned rate) {
    wl_win_native_io *io=(wl_win_native_io *)context;
    LARGE_INTEGER now,due;LONGLONG target,completion_target;DWORD error,wait;
    HANDLE events[3];
    if(!io || (rate!=44100 && rate!=48000 && rate!=88200 && rate!=96000))
        return io?result(io,ERROR_INVALID_PARAMETER):-1;
    if((error=stopped_error(io))!=0)return result(io,error);
    if(io->media_finished)return result(io,ERROR_INVALID_STATE);
    if(!io->media_written) {
        if(end_offset || final_packet_samples || io->clock_started)return result(io,ERROR_INVALID_STATE);
        io->media_finished=1;return result(io,ERROR_SUCCESS);
    }
    if(!io->clock_started || rate!=io->clock_rate || !final_packet_samples ||
       final_packet_samples>15u*256u || end_offset<=io->last_offset ||
       end_offset-io->last_offset!=final_packet_samples ||
       io->handles[WL_MEDIA]==INVALID_HANDLE_VALUE)return result(io,ERROR_INVALID_PARAMETER);
    if((error=terminal_target(io->anchor.QuadPart,end_offset,rate,io->frequency.QuadPart,&target))!=0 ||
       (error=terminal_target(io->last_media_completion.QuadPart,final_packet_samples,rate,
                              io->frequency.QuadPart,&completion_target))!=0)return result(io,error);
    if(completion_target>target)target=completion_target;
    if((error=arm_monitor(io))!=0 || (error=monitor_error(io))!=0)return result(io,error);
    events[0]=io->stop;events[1]=io->monitor.ov.hEvent;events[2]=io->timer;
    for(;;) {
        long double remaining;
        if((error=stopped_error(io))!=0 || (error=monitor_error(io))!=0)return result(io,error);
        if(!QueryPerformanceCounter(&now))return result(io,ERROR_GEN_FAILURE);
        if(now.QuadPart>=target){io->media_finished=1;return result(io,ERROR_SUCCESS);}
        remaining=(long double)(target-now.QuadPart)*10000000.0L/(long double)io->frequency.QuadPart;
        if(remaining>=(long double)LLONG_MAX-1)return result(io,ERROR_ARITHMETIC_OVERFLOW);
        due.QuadPart=-(LONGLONG)(remaining+1.0L);
        if(!SetWaitableTimer(io->timer,&due,0,NULL,NULL,FALSE))return result(io,GetLastError());
        wait=WaitForMultipleObjects(3,events,FALSE,INFINITE);
        if(wait==WAIT_OBJECT_0)return result(io,ERROR_OPERATION_ABORTED);
        if(wait==WAIT_OBJECT_0+1) {
            error=monitor_error(io);return result(io,error?error:ERROR_CONNECTION_ABORTED);
        }
        if(wait!=WAIT_OBJECT_0+2)return result(io,wait==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE);
    }
}
static int native_stopped(void *context) {
    wl_win_native_io *io=(wl_win_native_io *)context;DWORD error;
    if(!io)return 1;
    error=stopped_error(io);if(error)io->last_error=error;return error!=0;
}
static uint64_t native_now_ms(void *context) {
    (void)context;return (uint64_t)GetTickCount64();
}
static unsigned native_requested(void *context) {
    wl_win_native_io *io=(wl_win_native_io *)context;
    return io && io->requested?(unsigned)InterlockedExchange(io->requested,0):0;
}
static void native_close(void *context,unsigned channel) {
    wl_win_native_io *io=(wl_win_native_io *)context;
    if(!io || channel>WL_MEDIA)return;
    /* Idempotent and defensive even if a caller forgets portable close order. */
    if(channel==WL_SIGNALING && io->handles[WL_MEDIA]!=INVALID_HANDLE_VALUE)native_close(io,WL_MEDIA);
    if(channel==WL_SIGNALING && io->monitor_armed) {
        slot_cancel_drain(io->handles[channel],&io->monitor);io->monitor_armed=0;
    }
    if(io->handles[channel]!=INVALID_HANDLE_VALUE) {
        slot_cancel_drain(io->handles[channel],&io->operations[channel]);
        CloseHandle(io->handles[channel]);io->handles[channel]=INVALID_HANDLE_VALUE;
    }
    if(channel==WL_MEDIA){(void)CancelWaitableTimer(io->timer);io->clock_started=0;io->last_offset=0;io->media_written=0;io->media_finished=0;}
    io->out_mtu[channel]=io->in_mtu[channel]=0;
}
wl_win_native_io *wl_win_native_create(unsigned index,const wchar_t *path,HANDLE stop,volatile LONG *requested) {
    wl_win_native_io *io;DWORD error=ERROR_SUCCESS;
    if(index>15){SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
    io=(wl_win_native_io *)calloc(1,sizeof(*io));
    if(!io){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
    io->handles[0]=io->handles[1]=INVALID_HANDLE_VALUE;io->stop=stop;io->requested=requested;
    if(path) {
        size_t length=0;while(length<32768 && path[length])++length;
        if(!length || length==32768){error=ERROR_INVALID_PARAMETER;goto failed;}
        io->path=(wchar_t *)malloc((length+1)*sizeof(wchar_t));
        if(!io->path){error=ERROR_NOT_ENOUGH_MEMORY;goto failed;}
        memcpy(io->path,path,(length+1)*sizeof(wchar_t));
    } else if((error=select_path(index,&io->path))!=0)goto failed;
    if(!io->stop){io->stop=CreateEventW(NULL,TRUE,FALSE,NULL);io->own_stop=1;}
    if(!io->stop){error=GetLastError();goto failed;}
    for(unsigned i=0;i<2;++i) {
        io->operations[i].ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
        if(!io->operations[i].ov.hEvent){error=GetLastError();goto failed;}
    }
    io->monitor.ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!io->monitor.ov.hEvent){error=GetLastError();goto failed;}
    io->timer=CreateWaitableTimerW(NULL,FALSE,NULL);
    if(!io->timer){error=GetLastError();goto failed;}
    if(!QueryPerformanceFrequency(&io->frequency) || io->frequency.QuadPart<=0){error=ERROR_GEN_FAILURE;goto failed;}
    return io;
failed:
    wl_win_native_destroy(io);SetLastError(error);return NULL;
}
void wl_win_native_bind(wl_win_native_io *io,wl_playback_io *out) {
    if(!out)return;
    memset(out,0,sizeof(*out));if(!io)return;
    out->context=io;out->open=native_open;out->send=native_send;out->receive=native_receive;
    out->wait_samples=native_wait_samples;out->finish_media=native_finish_media;out->stopped=native_stopped;
    out->requested_bitrate=native_requested;out->close=native_close;out->now_ms=native_now_ms;
}
void wl_win_native_destroy(wl_win_native_io *io) {
    if(!io)return;
    native_close(io,WL_MEDIA);native_close(io,WL_SIGNALING);
    for(unsigned i=0;i<2;++i)if(io->operations[i].ov.hEvent)CloseHandle(io->operations[i].ov.hEvent);
    if(io->monitor.ov.hEvent)CloseHandle(io->monitor.ov.hEvent);
    if(io->timer)CloseHandle(io->timer);
    if(io->own_stop && io->stop)CloseHandle(io->stop);
    free(io->path);free(io);
}
DWORD wl_win_native_last_error(const wl_win_native_io *io) {return io?io->last_error:ERROR_INVALID_PARAMETER;}
#endif
