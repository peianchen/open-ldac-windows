/* SPDX-License-Identifier: Apache-2.0 */
#include "playback.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL playback:%d: %s\n",__LINE__,#x);return 1;}}while(0)
enum { EVENT_OPEN_SIGNAL=100,EVENT_OPEN_MEDIA,EVENT_WAIT=200,EVENT_MEDIA=201,
       EVENT_SOURCE=202,EVENT_FINISH=203,EVENT_CLOSE_ACCEPT=204,EVENT_CLOSE_MEDIA=300,EVENT_CLOSE_SIGNAL=301 };
typedef struct {
    unsigned calls,fail_at,stop_at,stop_on_failure,stop;
    unsigned out[2],in[2],opened[2],closed[2],peer_open,peer_started;
    unsigned selected_seid,source_rate,format_bytes,total_frames,remaining,read_calls;
    unsigned bad_short_tail,overrun_frames,no_exact_rate,reject_signal,malformed_signal;
    unsigned first_sink_sbc,cap_requests,unmatched_remaining,unmatched_forever,receive_calls;
    unsigned request_calls,request_at,request_kbps,signal_sends;
    unsigned media_packets,media_bytes,media_frames,wait_calls,ready_wait,violation;
    unsigned finish_calls,final_packet_samples,finished,fail_finish;
    uint64_t last_wait,media_hash,now,transaction_start;
    unsigned send_delay,receive_delay,backward_on_receive;
    unsigned receive_timeouts[256],timeout_count;
    unsigned fragment_mask,fragment_count,fragment_index,fragment_active,fragment_corrupt;
    unsigned missing_end,large_discover,fragment_messages,fragment_packets,peer_command_signal;
    uint8_t fragment_message[160];size_t fragment_size,fragment_offset;
    uint8_t last_command[18];size_t last_command_size;
    unsigned events[1024],event_count;
} fake;
static void event(fake *f,unsigned kind) {
    if(f->event_count<sizeof(f->events)/sizeof(f->events[0]))f->events[f->event_count++]=kind;
    else f->violation=1;
}
static int call(fake *f) {
    ++f->calls;
    if(f->stop_at&&f->calls>=f->stop_at)f->stop=1;
    if(f->fail_at&&f->calls==f->fail_at){if(f->stop_on_failure)f->stop=1;return 1;}
    return 0;
}
static int f_open(void *context,unsigned channel,unsigned *out,unsigned *in) {
    fake *f=context;event(f,channel==WL_SIGNALING?EVENT_OPEN_SIGNAL:EVENT_OPEN_MEDIA);
    if(call(f))return -1;
    if(channel>1 || (channel==WL_MEDIA&&!f->peer_open)){f->violation=1;return -1;}
    f->opened[channel]=1;*out=f->out[channel];*in=f->in[channel];return 0;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
static int f_send(void *context,unsigned channel,const uint8_t *packet,size_t size,unsigned timeout) {
    fake *f=context;
    if(channel==WL_SIGNALING){
        event(f,size>=2?packet[1]&0x3fu:0);++f->signal_sends;
    }else event(f,EVENT_MEDIA);
    if(call(f))return -1;
    if(channel>1||!f->opened[channel]||!timeout||size>f->out[channel]){f->violation=1;return -1;}
    if(channel==WL_SIGNALING){
        if(size<2||size>sizeof(f->last_command)){f->violation=1;return -1;}
        f->transaction_start=f->now;f->now+=f->send_delay;
        memcpy(f->last_command,packet,size);f->last_command_size=size;
        if((packet[1]&0x3fu)==WL_AVDTP_START&&!f->opened[WL_MEDIA]){f->violation=1;return -1;}
        if((packet[1]&0x3fu)==WL_AVDTP_CLOSE&&!f->finished){f->violation=1;return -1;}
        return 0;
    }
    if(!f->peer_started||!f->ready_wait||size<=13||packet[0]!=0x80||packet[1]!=96||
       (((unsigned)packet[2]<<8)|packet[3])!=(f->media_packets&65535u)||
       be32(packet+4)!=(uint32_t)((uint64_t)f->media_frames*(f->source_rate>48000?256u:128u))||
       be32(packet+8)!=0x11223344u||!(packet[12]&15u)||(packet[12]&0xf0u)||packet[13]!=0xaa){f->violation=1;return -1;}
    for(size_t i=0;i<size;++i){f->media_hash^=packet[i];f->media_hash*=UINT64_C(1099511628211);}
    f->final_packet_samples=(packet[12]&15u)*(f->source_rate>48000?256u:128u);
    f->media_frames+=packet[12]&15u;++f->media_packets;f->media_bytes+=(unsigned)size;
    f->ready_wait=0;return 0;
}
/* Wire fixture: split a complete peer response into AVDTP signaling packets.
 * It owns the original response, so callbacks do not rebuild or reapply peer
 * state transitions for each fragment. Header-only fragments exercise the
 * bounded packet-count contract even for an empty ACCEPT payload. */
static int f_fragment(fake *f,uint8_t *packet,size_t capacity,size_t *size) {
    unsigned index=f->fragment_index,count=f->fragment_count?f->fragment_count:3;
    unsigned kind=index==0?1:index+1==count?3:2;
    size_t header=index==0?3:1,remaining=f->fragment_size-f->fragment_offset;
    size_t take=(remaining+(count-index)-1)/(count-index);
    if(f->missing_end&&index+1==count)return -1;
    if(capacity<header){f->violation=1;return -1;}
    if(take>capacity-header)take=capacity-header;
    packet[0]=(uint8_t)((f->fragment_message[0]&0xf3u)|(kind<<2));
    if(index==0){packet[1]=(uint8_t)count;packet[2]=f->fragment_message[1];}
    memcpy(packet+header,f->fragment_message+f->fragment_offset,take);
    if(index==1){
        if(f->fragment_corrupt==1)packet[0]=(uint8_t)((packet[0]&0xf3u)|12u); /* early END */
        if(f->fragment_corrupt==2)packet[0]^=0x10; /* wrong transaction */
        if(f->fragment_corrupt==3){packet[0]&=0xf3u;packet[1]=f->fragment_message[1];header=2;} /* interleaved SINGLE */
    }
    if(index+1==count&&f->fragment_corrupt==4)packet[0]=(uint8_t)((packet[0]&0xf3u)|8u); /* late CONTINUE */
    f->fragment_offset+=take;++f->fragment_index;++f->fragment_packets;
    *size=header+take;
    if(f->fragment_index==count){
        if(f->fragment_offset!=f->fragment_size){f->violation=1;return -1;}
        f->fragment_active=0;
    }
    return 0;
}
static int f_receive(void *context,unsigned channel,uint8_t *packet,size_t capacity,size_t *size,unsigned timeout) {
    fake *f=context;uint8_t out[160]={0};size_t n=2;unsigned signal;
    ++f->receive_calls;
    if(call(f))return -1;
    if(channel!=WL_SIGNALING||!f->opened[WL_SIGNALING]||!timeout||f->last_command_size<2){f->violation=1;return -1;}
    if(f->timeout_count<sizeof(f->receive_timeouts)/sizeof(f->receive_timeouts[0]))
        f->receive_timeouts[f->timeout_count++]=timeout;
    else {f->violation=1;return -1;}
    if(f->now<f->transaction_start || f->now-f->transaction_start>=5000 ||
       timeout!=5000-(unsigned)(f->now-f->transaction_start)){f->violation=1;return -1;}
    f->now+=f->receive_delay;
    if(f->backward_on_receive){f->now-=f->backward_on_receive;f->backward_on_receive=0;}
    if(f->fragment_active)return f_fragment(f,packet,capacity,size);
    signal=f->last_command[1]&0x3fu;
    out[0]=(f->last_command[0]&0xf0u)|WL_AVDTP_ACCEPT;out[1]=(uint8_t)signal;
    if(f->unmatched_forever||f->unmatched_remaining){
        out[0]=(uint8_t)((((f->last_command[0]>>4)+1u)&15u)<<4)|WL_AVDTP_ACCEPT;
        if(f->unmatched_remaining)--f->unmatched_remaining;
    }else if(f->reject_signal==signal){
        out[0]=(f->last_command[0]&0xf0u)|WL_AVDTP_REJECT;
        if(signal==WL_AVDTP_SET_CONFIGURATION){out[2]=7;out[3]=0x29;n=4;}
        else if(signal==WL_AVDTP_START||signal==WL_AVDTP_SUSPEND){out[2]=(uint8_t)(f->selected_seid<<2);out[3]=0x31;n=4;}
        else {out[2]=0x31;n=3;}
    }else if(f->malformed_signal==signal){out[2]=0;n=3;}
    else if(signal==WL_AVDTP_DISCOVER){
        if(f->large_discover){
            /* 62 distinct, unused audio-sink SEPs: valid maximum discovery
             * payload, rather than inventing a large optional capability. */
            for(unsigned i=0;i<62;++i){out[2+2*i]=(uint8_t)((i+1)<<2);out[3+2*i]=8;}
            n=126;
        }else {out[2]=4;out[3]=8;out[4]=8;out[5]=8;n=6;}
    }
    else if(signal==WL_AVDTP_GET_CAPABILITIES){
        unsigned seid=f->last_command[2]>>2;++f->cap_requests;
        out[2]=1;out[3]=0;out[4]=7;
        if(f->first_sink_sbc&&seid==1){out[5]=2;out[6]=0;out[7]=0;n=8;}
        else {
            static const uint8_t vendor[6]={0x2d,1,0,0,0xaa,0};
            out[5]=10;out[6]=0;out[7]=0xff;memcpy(out+8,vendor,6);
            out[14]=(uint8_t)(f->no_exact_rate?0x20:0x3c);out[15]=7;n=16;f->selected_seid=seid;
        }
    }else if(signal==WL_AVDTP_OPEN)f->peer_open=1;
    else if(signal==WL_AVDTP_START)f->peer_started=1;
    else if(signal==WL_AVDTP_SUSPEND)f->peer_started=0;
    else if(signal==WL_AVDTP_CLOSE)event(f,EVENT_CLOSE_ACCEPT);
    if(f->peer_command_signal==signal)out[0]&=0xf0u;
    if(f->fragment_mask&(1u<<signal)){
        memcpy(f->fragment_message,out,n);f->fragment_size=n;f->fragment_offset=2;
        f->fragment_index=0;f->fragment_active=1;++f->fragment_messages;
        return f_fragment(f,packet,capacity,size);
    }
    if(n>capacity){f->violation=1;return -1;}
    memcpy(packet,out,n);*size=n;return 0;
}
static int f_wait(void *context,uint64_t offset,unsigned rate) {
    fake *f=context;event(f,EVENT_WAIT);
    if(call(f))return -1;
    if(rate!=f->source_rate||offset!=(uint64_t)f->media_frames*(rate>48000?256u:128u)){f->violation=1;return -1;}
    f->last_wait=offset;++f->wait_calls;f->ready_wait=1;return 0;
}
static int f_finish(void *context,uint64_t end,unsigned final_samples,unsigned rate) {
    fake *f=context;event(f,EVENT_FINISH);++f->finish_calls;
    if(call(f)||f->fail_finish)return -1;
    if(f->finished||rate!=f->source_rate||
       end!=(uint64_t)f->media_frames*(rate>48000?256u:128u)||
       final_samples!=f->final_packet_samples){f->violation=1;return -1;}
    f->last_wait=end;f->finished=1;return 0;
}
static uint64_t f_now(void *context){return ((fake *)context)->now;}
static int f_stopped(void *context){return ((fake*)context)->stop!=0;}
static unsigned f_requested(void *context){fake *f=context;++f->request_calls;return f->request_calls==f->request_at?f->request_kbps:0;}
static void f_close(void *context,unsigned channel) {
    fake *f=context;event(f,channel==WL_MEDIA?EVENT_CLOSE_MEDIA:EVENT_CLOSE_SIGNAL);
    if(channel>1){f->violation=1;return;}++f->closed[channel];f->opened[channel]=0;
}
static int f_read(void *context,void *pcm,unsigned capacity,unsigned *frames) {
    fake *f=context;unsigned take;event(f,EVENT_SOURCE);++f->read_calls;
    if(call(f))return -1;
    if(capacity!=128){f->violation=1;return -1;}
    memset(pcm,0x7f,(size_t)capacity*2*f->format_bytes);
    take=f->remaining<capacity?f->remaining:capacity;
    if(!take&&f->bad_short_tail){take=5;f->bad_short_tail=0;}
    else f->remaining-=take;
    memset(pcm,0,(size_t)take*2*f->format_bytes);
    *frames=f->overrun_frames?129:take;return 0;
}
static void init(fake *f,unsigned frames) {
    memset(f,0,sizeof(*f));f->out[0]=f->out[1]=1008;f->in[0]=f->in[1]=1008;
    f->source_rate=48000;f->format_bytes=4;f->remaining=f->total_frames=frames;
    f->media_hash=UINT64_C(14695981039346656037);f->now=100000;
}
static int run(fake *f,unsigned rate,LDACBT_SMPL_FMT_T format,unsigned bitrate,wl_playback_report *r) {
    wl_playback_io io={f,f_open,f_send,f_receive,f_wait,f_finish,f_stopped,f_requested,f_close,f_now};
    wl_pcm_source source={f,f_read,rate,format};
    wl_playback_options options={bitrate,1,0x11223344u,1};
    f->source_rate=rate;f->format_bytes=format==LDACBT_SMPL_FMT_S16?2:format==LDACBT_SMPL_FMT_S24?3:4;
    return wl_playback_run(&io,&source,&options,r);
}
static int closed_in_order(const fake *f) {
    return f->closed[0]==1&&f->closed[1]==1&&f->event_count>=2&&
        f->events[f->event_count-2]==EVENT_CLOSE_MEDIA&&f->events[f->event_count-1]==EVENT_CLOSE_SIGNAL;
}
static int success_tests(void) {
    const unsigned rates[]={44100,48000,88200,96000};
    const LDACBT_SMPL_FMT_T formats[]={LDACBT_SMPL_FMT_S16,LDACBT_SMPL_FMT_S24,LDACBT_SMPL_FMT_S32,LDACBT_SMPL_FMT_F32};
    fake f;wl_playback_report r;
    for(unsigned rate=0;rate<4;++rate)for(unsigned format=0;format<4;++format)for(int quality=0;quality<3;++quality){
        unsigned kbps=wl_bitrate_kbps(rates[rate],quality);
        init(&f,300);f.first_sink_sbc=1;
        CHECK(run(&f,rates[rate],formats[format],quality?kbps:0,&r)==WL_PLAY_OK);
        CHECK(!f.violation&&closed_in_order(&f));CHECK(r.result==WL_PLAY_OK&&r.stage==WL_AVDTP_SESSION_CLOSED);
        CHECK(r.source_frames==300&&r.encoded_frames==f.media_frames&&r.media_packets==f.media_packets);
        CHECK(r.media_bytes==f.media_bytes&&f.cap_requests==2&&r.requested_kbps==kbps&&r.actual_kbps==kbps);
        CHECK(f.wait_calls==f.media_packets&&f.finish_calls==1&&f.finished&&f.last_wait==r.encoded_frames*(rates[rate]>48000?256u:128u));
        CHECK(r.negotiated_mtu==1008&&r.bitrate_changes==0&&f.media_packets>0);
    }
    init(&f,0);CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);
    CHECK(!f.violation&&closed_in_order(&f)&&r.source_frames==0);
    init(&f,256);f.out[0]=48;f.in[0]=48;f.out[1]=679;f.in[1]=48;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);
    CHECK(!f.violation&&r.negotiated_mtu==679&&closed_in_order(&f));
    init(&f,256);f.unmatched_remaining=2;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK&&closed_in_order(&f));
    return 0;
}
static int graceful_eof_tests(void) {
    fake f;wl_playback_report r;
    const unsigned lengths[]={0,1,127,128,129,4096};
    for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i){
        init(&f,lengths[i]);CHECK(run(&f,48000,LDACBT_SMPL_FMT_S16,0,&r)==WL_PLAY_OK);
        CHECK(f.finish_calls==1&&closed_in_order(&f)&&!f.violation);
        CHECK(f.event_count>=5);
        CHECK(f.events[f.event_count-5]==EVENT_FINISH);
        CHECK(f.events[f.event_count-4]==WL_AVDTP_CLOSE);
        CHECK(f.events[f.event_count-3]==EVENT_CLOSE_ACCEPT);
        for(unsigned j=0;j<f.event_count;++j)
            CHECK(f.events[j]!=WL_AVDTP_ABORT&&f.events[j]!=WL_AVDTP_SUSPEND);
    }
    init(&f,128);f.fail_finish=1;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_S16,0,&r)==WL_PLAY_TRANSPORT);
    CHECK(f.finish_calls==1&&!f.finished&&closed_in_order(&f));
    for(unsigned j=0;j<f.event_count;++j)CHECK(f.events[j]!=WL_AVDTP_CLOSE);
    init(&f,128);f.malformed_signal=WL_AVDTP_CLOSE;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_S16,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_MALFORMED&&closed_in_order(&f));
    init(&f,128);
    {
        wl_playback_io io={&f,f_open,f_send,f_receive,f_wait,NULL,f_stopped,f_requested,f_close,f_now};
        wl_pcm_source source={&f,f_read,48000,LDACBT_SMPL_FMT_S16};
        wl_playback_options options={0,1,0x11223344u,1};
        CHECK(wl_playback_run(&io,&source,&options,&r)==WL_PLAY_ARGUMENT);
        CHECK(f.calls==0&&f.closed[0]==0&&f.closed[1]==0);
    }
    return 0;
}
static void fragment_setup(fake *f,unsigned frames) {
    init(f,frames);f->fragment_mask=(1u<<WL_AVDTP_DISCOVER)|(1u<<WL_AVDTP_GET_CAPABILITIES);
}
static int fragmentation_tests(void) {
    fake plain,f;wl_playback_report a,r;unsigned calls;
    init(&plain,300);CHECK(run(&plain,48000,LDACBT_SMPL_FMT_F32,0,&a)==WL_PLAY_OK);
    fragment_setup(&f,300);f.in[0]=48;f.large_discover=1;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);
    CHECK(!f.violation&&closed_in_order(&f)&&f.fragment_messages==2&&f.fragment_packets==6);
    CHECK(f.media_hash==plain.media_hash&&r.media_bytes==a.media_bytes&&r.requested_kbps==990);
    fragment_setup(&f,300);f.unmatched_remaining=2;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);
    CHECK(f.fragment_messages==4&&f.fragment_packets==12&&!f.violation&&closed_in_order(&f));
    for(unsigned corruption=1;corruption<=4;++corruption){
        fragment_setup(&f,300);f.fragment_corrupt=corruption;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
        CHECK(r.protocol_error==WL_AVDTP_SESSION_MALFORMED&&!f.violation&&closed_in_order(&f));
        CHECK(f.media_packets==0&&f.read_calls==0);
    }
    fragment_setup(&f,300);f.peer_command_signal=WL_AVDTP_DISCOVER;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_REMOTE_COMMAND&&f.read_calls==0&&closed_in_order(&f));
    fragment_setup(&f,300);f.missing_end=1;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_TRANSPORT);
    CHECK(f.read_calls==0&&closed_in_order(&f)&&!f.violation);
    fragment_setup(&f,300);f.unmatched_forever=1;f.fragment_count=16;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_TIMEOUT&&f.receive_calls==64);
    CHECK(f.fragment_messages==4&&f.fragment_packets==64&&closed_in_order(&f)&&!f.violation);
    fragment_setup(&f,300);CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);calls=f.calls;
    for(unsigned n=1;n<=calls;++n){
        fragment_setup(&f,300);f.fail_at=n;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)!=WL_PLAY_OK);
        CHECK(closed_in_order(&f)&&!f.violation);
        fragment_setup(&f,300);f.stop_at=n;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_STOPPED);
        CHECK(closed_in_order(&f)&&!f.violation);
    }
    return 0;
}
static int deadline_tests(void) {
    fake f;wl_playback_report r;
    fragment_setup(&f,300);f.receive_delay=1700;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_TIMEOUT&&f.receive_calls==3);
    CHECK(f.receive_timeouts[0]==5000&&f.receive_timeouts[1]==3300&&f.receive_timeouts[2]==1600);
    CHECK(f.read_calls==0&&closed_in_order(&f)&&!f.violation);
    fragment_setup(&f,300);f.send_delay=250;f.receive_delay=100;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);
    CHECK(f.receive_timeouts[0]==4750&&f.receive_timeouts[1]==4650&&f.receive_timeouts[2]==4550);
    CHECK(!f.violation&&closed_in_order(&f));
    init(&f,300);f.send_delay=5000;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_TIMEOUT&&f.receive_calls==0&&closed_in_order(&f));
    init(&f,300);f.receive_delay=5000;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_TIMEOUT&&f.receive_calls==1&&f.read_calls==0&&closed_in_order(&f));
    init(&f,300);f.backward_on_receive=1;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL);
    CHECK(r.protocol_error==WL_AVDTP_SESSION_TIMEOUT&&f.receive_calls==1&&closed_in_order(&f));
    init(&f,300);f.receive_delay=5000;f.stop_at=3;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_STOPPED);
    CHECK(closed_in_order(&f));
    init(&f,300);
    {
        wl_playback_io io={&f,f_open,f_send,f_receive,f_wait,f_finish,f_stopped,f_requested,f_close,NULL};
        wl_pcm_source source={&f,f_read,48000,LDACBT_SMPL_FMT_F32};
        wl_playback_options options={0,1,0x11223344u,1};
        CHECK(wl_playback_run(&io,&source,&options,&r)==WL_PLAY_ARGUMENT);
        CHECK(f.calls==0&&f.closed[0]==0&&f.closed[1]==0);
    }
    return 0;
}
static int padding_and_bitrate_tests(void) {
    fake short_file,padded_file;wl_playback_report a,b;
    init(&short_file,300);init(&padded_file,384);
    CHECK(run(&short_file,48000,LDACBT_SMPL_FMT_F32,0,&a)==WL_PLAY_OK);
    CHECK(run(&padded_file,48000,LDACBT_SMPL_FMT_F32,0,&b)==WL_PLAY_OK);
    CHECK(short_file.media_hash==padded_file.media_hash&&a.media_bytes==b.media_bytes&&a.encoded_frames==b.encoded_frames);
    CHECK(a.source_frames==300&&b.source_frames==384);
    init(&short_file,300);short_file.bad_short_tail=1;
    CHECK(run(&short_file,48000,LDACBT_SMPL_FMT_F32,0,&a)==WL_PLAY_SOURCE&&closed_in_order(&short_file));
    init(&short_file,128);short_file.overrun_frames=1;
    CHECK(run(&short_file,48000,LDACBT_SMPL_FMT_F32,0,&a)==WL_PLAY_SOURCE&&closed_in_order(&short_file));
    init(&short_file,4096);short_file.request_at=2;short_file.request_kbps=660;
    CHECK(run(&short_file,48000,LDACBT_SMPL_FMT_F32,0,&a)==WL_PLAY_OK);
    CHECK(a.requested_kbps==660&&a.actual_kbps==660&&a.bitrate_changes==1&&closed_in_order(&short_file));
    init(&short_file,512);short_file.request_at=2;short_file.request_kbps=909;
    CHECK(run(&short_file,48000,LDACBT_SMPL_FMT_F32,0,&a)==WL_PLAY_BITRATE&&closed_in_order(&short_file));
    return 0;
}
static int failure_tests(void) {
    fake f;wl_playback_report r;unsigned calls;
    init(&f,300);CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_OK);calls=f.calls;
    for(unsigned n=1;n<=calls;++n){
        init(&f,300);f.fail_at=n;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)!=WL_PLAY_OK);
        CHECK(closed_in_order(&f)&&!f.violation);
        init(&f,300);f.stop_at=n;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_STOPPED);
        CHECK(closed_in_order(&f)&&!f.violation);
        init(&f,300);f.fail_at=n;f.stop_on_failure=1;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_STOPPED&&closed_in_order(&f));
    }
    init(&f,300);f.stop=1;CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_STOPPED);
    CHECK(f.calls==0&&closed_in_order(&f));
    init(&f,300);f.no_exact_rate=1;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL&&r.protocol_error==WL_AVDTP_SESSION_CONVERSION_REQUIRED);
    CHECK(f.opened[1]==0&&f.media_packets==0&&f.read_calls==0&&closed_in_order(&f));
    const unsigned rejects[]={WL_AVDTP_DISCOVER,WL_AVDTP_GET_CAPABILITIES,WL_AVDTP_SET_CONFIGURATION,WL_AVDTP_OPEN,WL_AVDTP_START,WL_AVDTP_CLOSE};
    for(unsigned i=0;i<sizeof(rejects)/sizeof(rejects[0]);++i){
        init(&f,300);f.reject_signal=rejects[i];
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL&&closed_in_order(&f));
    }
    init(&f,300);f.malformed_signal=WL_AVDTP_OPEN;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL&&closed_in_order(&f)&&f.media_packets==0);
    init(&f,300);f.unmatched_forever=1;
    CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_PROTOCOL&&r.protocol_error==WL_AVDTP_SESSION_TIMEOUT);
    CHECK(f.receive_calls==16&&closed_in_order(&f));
    for(unsigned channel=0;channel<2;++channel)for(unsigned kind=0;kind<4;++kind){
        init(&f,300);
        if(kind==0)f.out[channel]=channel?678:47;
        if(kind==1)f.out[channel]=1009;
        if(kind==2)f.in[channel]=47;
        if(kind==3)f.in[channel]=1009;
        CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_MTU&&closed_in_order(&f));
        CHECK(f.media_packets==0);
    }
    init(&f,300);CHECK(run(&f,48000,LDACBT_SMPL_FMT_F32,909,&r)==WL_PLAY_BITRATE&&f.calls==0&&!f.closed[0]);
    init(&f,300);CHECK(run(&f,192000,LDACBT_SMPL_FMT_F32,0,&r)==WL_PLAY_ARGUMENT&&f.calls==0);
    init(&f,300);CHECK(run(&f,48000,(LDACBT_SMPL_FMT_T)99,0,&r)==WL_PLAY_ARGUMENT&&f.calls==0);
    CHECK(wl_playback_run(NULL,NULL,NULL,NULL)==WL_PLAY_ARGUMENT);
    return 0;
}
int main(void) {
    if(success_tests()||graceful_eof_tests()||fragmentation_tests()||deadline_tests()||padding_and_bitrate_tests()||failure_tests())return 1;
    printf("PASS: %u portable playback fake-peer checks; no Bluetooth, driver load or audio-endpoint acceptance.\n",checks);
    return 0;
}
