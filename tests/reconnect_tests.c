/* SPDX-License-Identifier: Apache-2.0 */
#include "reconnect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while(0)
/* Deliberate attempt stub: this suite tests only coordinator policy/ordering.
 * The separately linked playback_tests exercises real codec/session behavior. */
typedef struct {
    int outcomes[4], stop, stop_after_attempt, stop_in_wait, stop_in_rewind;
    int wait_error, rewind_error, allow, active;
    unsigned calls, waits, rewinds, classifications, pending;
    unsigned requested[4], delays[3];
    wl_playback_options seen[4];
} fixture;
int wl_playback_run(const wl_playback_io *io, const wl_pcm_source *source,
    const wl_playback_options *options, wl_playback_report *report) {
    fixture *f=io->context; unsigned i=f->calls++;
    (void)source;
    CHECK(i<4 && !f->active); f->active=1; f->seen[i]=*options;
    memset(report,0,sizeof(*report));
    report->requested_kbps=f->requested[i]?f->requested[i]:options->bitrate_kbps;
    if(f->pending){report->requested_kbps=f->pending;f->pending=0;}
    report->media_packets=i+1; report->result=f->outcomes[i];
    f->active=0; /* Models run's mandatory media/signaling retirement. */
    if(f->stop_after_attempt)f->stop=1;
    return report->result;
}
static int stopped(void *ctx){return ((fixture *)ctx)->stop;}
static int rewind_source(void *ctx){fixture *f=ctx; CHECK(!f->active); ++f->rewinds;
    if(f->stop_in_rewind)f->stop=1;return f->rewind_error;}
static int wait_retry(void *ctx,unsigned ms){fixture *f=ctx;CHECK(!f->active && f->waits<3);
    CHECK(f->rewinds==f->waits); f->delays[f->waits++]=ms;
    if(f->stop_in_wait)f->stop=1;return f->wait_error;}
static int retryable(void *ctx,int result,const wl_playback_report *report){fixture *f=ctx;
    CHECK(!f->active && result==WL_PLAY_TRANSPORT && report->result==result);
    ++f->classifications;return f->allow;}
static void defaults(fixture *f){memset(f,0,sizeof(*f));f->allow=1;}
static int run(fixture *f,unsigned retries,uint64_t generation,wl_playback_report *p,wl_reconnect_report *r){
    wl_playback_io io={0};wl_pcm_source source={0};wl_playback_options options={990,generation,44,1};
    wl_reconnect_policy policy={f,retries,rewind_source,wait_retry,retryable};
    io.context=f;io.stopped=stopped;
    return wl_playback_reconnect(&io,&source,&options,&policy,p,r);
}
static void success_and_recovery(void){
    fixture f;wl_playback_report p;wl_reconnect_report r;
    defaults(&f);CHECK(run(&f,3,7,&p,&r)==WL_PLAY_OK);
    CHECK(r.attempts==1 && r.restarts==0 && f.waits==0 && r.last_generation==7);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.requested[0]=660;
    CHECK(run(&f,3,7,&p,&r)==WL_PLAY_OK);
    CHECK(r.attempts==2 && r.restarts==1 && !r.exhausted && r.last_generation==8);
    CHECK(f.waits==1 && f.rewinds==1 && f.delays[0]==1000 && f.classifications==1);
    CHECK(f.seen[1].generation==8 && f.seen[1].ssrc==45 && f.seen[1].bitrate_kbps==660);
    CHECK(p.requested_kbps==660 && p.media_packets==2); /* Last attempt only. */
    defaults(&f);f.outcomes[0]=f.outcomes[1]=WL_PLAY_TRANSPORT;
    f.requested[0]=660;f.requested[1]=330;
    CHECK(run(&f,3,7,&p,&r)==WL_PLAY_OK);
    CHECK(r.attempts==3 && r.restarts==2 && f.delays[1]==2000);
    CHECK(f.seen[2].generation==9 && f.seen[2].ssrc==46 && f.seen[2].bitrate_kbps==330);
    CHECK(p.requested_kbps==330);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.pending=330;
    CHECK(run(&f,3,7,&p,&r)==WL_PLAY_OK);
    CHECK(f.seen[1].bitrate_kbps==330 && p.requested_kbps==330 && !f.pending);
}
static void bounds_and_failures(void){
    fixture f;wl_playback_report p;wl_reconnect_report r;
    for(unsigned retries=0;retries<=3;++retries){
        defaults(&f);for(unsigned i=0;i<4;++i)f.outcomes[i]=WL_PLAY_TRANSPORT;
        CHECK(run(&f,retries,9,&p,&r)==WL_PLAY_TRANSPORT);
        CHECK(r.attempts==retries+1 && r.restarts==retries && f.waits==retries && f.rewinds==retries);
        CHECK(r.exhausted==(retries!=0));
        for(unsigned i=0;i<retries;++i)CHECK(f.delays[i]==(1000u<<i));
    }
    const int terminal[]={WL_PLAY_ARGUMENT,WL_PLAY_PROTOCOL,WL_PLAY_SOURCE,WL_PLAY_CODEC,
        WL_PLAY_STOPPED,WL_PLAY_BITRATE,WL_PLAY_MTU,WL_PLAY_PEER_CLOSED,WL_PLAY_PEER_ABORTED};
    for(unsigned i=0;i<sizeof(terminal)/sizeof(*terminal);++i){
        defaults(&f);f.outcomes[0]=terminal[i];CHECK(run(&f,3,9,&p,&r)==terminal[i]);
        CHECK(f.calls==1 && !f.waits && !f.classifications);
    }
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.allow=0;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_TRANSPORT);CHECK(f.calls==1 && !f.waits);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;
    CHECK(run(&f,3,UINT64_MAX,&p,&r)==WL_PLAY_TRANSPORT);
    CHECK(r.generation_exhausted && r.attempts==1 && !f.waits && !f.rewinds);
    defaults(&f);f.outcomes[0]=f.outcomes[1]=WL_PLAY_TRANSPORT;
    CHECK(run(&f,3,UINT64_MAX-1,&p,&r)==WL_PLAY_TRANSPORT);
    CHECK(r.generation_exhausted && r.attempts==2 && r.last_generation==UINT64_MAX);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.wait_error=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_TRANSPORT);CHECK(f.calls==1 && !f.rewinds);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.rewind_error=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_SOURCE);CHECK(f.calls==1 && f.rewinds==1 && r.restarts==0);
}
static void cancellation(void){
    fixture f;wl_playback_report p;wl_reconnect_report r;
    defaults(&f);f.stop=1;CHECK(run(&f,3,9,&p,&r)==WL_PLAY_STOPPED);CHECK(!f.calls && !f.waits);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.stop_after_attempt=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_STOPPED);CHECK(f.calls==1 && !f.classifications && !f.waits);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.stop_in_wait=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_STOPPED);CHECK(f.calls==1 && !f.rewinds);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.stop_in_wait=1;f.wait_error=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_STOPPED);CHECK(f.calls==1 && !f.rewinds);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.stop_in_rewind=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_STOPPED);CHECK(f.calls==1 && f.rewinds==1 && !r.restarts);
    defaults(&f);f.outcomes[0]=WL_PLAY_TRANSPORT;f.stop_in_rewind=1;f.rewind_error=1;
    CHECK(run(&f,3,9,&p,&r)==WL_PLAY_STOPPED);CHECK(f.calls==1 && f.rewinds==1 && !r.restarts);
}
static void arguments(void){
    fixture f;wl_playback_report p;wl_reconnect_report r;wl_playback_io io={0};
    wl_pcm_source source={0};wl_playback_options options={0};wl_reconnect_policy policy={0};
    defaults(&f);io.context=&f;io.stopped=stopped;policy.context=&f;
    CHECK(run(&f,4,1,&p,&r)==WL_PLAY_ARGUMENT && !f.calls);
    CHECK(wl_playback_reconnect(NULL,&source,&options,&policy,&p,&r)==WL_PLAY_ARGUMENT);
    CHECK(wl_playback_reconnect(&io,NULL,&options,&policy,&p,&r)==WL_PLAY_ARGUMENT);
    CHECK(wl_playback_reconnect(&io,&source,NULL,&policy,&p,&r)==WL_PLAY_ARGUMENT);
    CHECK(wl_playback_reconnect(&io,&source,&options,NULL,&p,&r)==WL_PLAY_ARGUMENT);
    CHECK(wl_playback_reconnect(&io,&source,&options,&policy,NULL,&r)==WL_PLAY_ARGUMENT);
    CHECK(wl_playback_reconnect(&io,&source,&options,&policy,&p,NULL)==WL_PLAY_ARGUMENT);
    policy.max_retries=1;
    CHECK(wl_playback_reconnect(&io,&source,&options,&policy,&p,&r)==WL_PLAY_ARGUMENT && !f.calls);
    policy.max_retries=0;
    CHECK(wl_playback_reconnect(&io,&source,&options,&policy,&p,&r)==WL_PLAY_OK); /* No callbacks needed. */
}
int main(void){success_and_recovery();bounds_and_failures();cancellation();arguments();
    printf("reconnect_tests: %u checks passed (stubbed attempts; no driver/RF).\n",checks);return 0;}
