/* SPDX-License-Identifier: Apache-2.0 */
/* Offline protocol fixtures. No Bluetooth, driver, VM or audio claims. */
#include "avdtp_session.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; \
} } while (0)

enum { DISCOVER=1, GET_CAPS=2, SET_CONFIG=3, OPEN=6, START=7, SUSPEND=9, ABORT=10 };
static const uint8_t sink5[] = {20, 8};
static const uint8_t ldac[] = {1,0,7,10,0,0xff,0x2d,1,0,0,0xaa,0,0x3c,7};
static const uint8_t sbc[] = {1,0,7,6,0,0,0x3f,0xff,2,53};

typedef struct {
    uint8_t packet[WL_AVDTP_SESSION_MAX_PACKET];
    size_t length;
    wl_avdtp_session_token token;
} command;

static int unchanged_output(wl_avdtp_session *s,size_t needed);
static int emit(wl_avdtp_session *s, command *c, uint8_t signal, size_t length) {
    CHECK(unchanged_output(s,length)==0);
    memset(c, 0xcc, sizeof(*c));
    c->length = wl_avdtp_session_next_command(s,c->packet,sizeof(c->packet),&c->token);
    CHECK(c->length==length);
    CHECK(c->packet[0]==(uint8_t)(c->token.transaction<<4));
    CHECK(c->packet[1]==signal && c->token.signal==signal);
    CHECK(c->token.generation==s->generation && c->token.serial==s->serial);
    CHECK(c->token.transaction<16 && s->pending);
    CHECK(c->packet[length]==0xcc);
    return 0;
}

static int response(wl_avdtp_session *s,const command *c,unsigned type,
                    const uint8_t *payload,size_t size) {
    uint8_t packet[WL_AVDTP_SESSION_MAX_PACKET];
    if(size>sizeof(packet)-2)return WL_AVDTP_SESSION_INVALID_ARGUMENT;
    packet[0]=(uint8_t)((c->token.transaction<<4)|type);
    packet[1]=c->token.signal;
    if(size)memcpy(packet+2,payload,size);
    return wl_avdtp_session_receive(s,c->token.generation,packet,size+2);
}

static int unchanged_output(wl_avdtp_session *s,size_t needed) {
    wl_avdtp_session before=*s;
    uint8_t out[64],saved[64];
    wl_avdtp_session_token t,original;
    memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
    memset(&t,0x5a,sizeof(t));original=t;
    CHECK(wl_avdtp_session_next_command(s,out,needed-1,&t)==0);
    CHECK(!memcmp(s,&before,sizeof(*s)) && !memcmp(&t,&original,sizeof(t)));
    CHECK(!memcmp(out,saved,sizeof(out)));
    CHECK(wl_avdtp_session_next_command(s,NULL,sizeof(out),&t)==0);
    CHECK(!memcmp(s,&before,sizeof(*s)) && !memcmp(&t,&original,sizeof(t)));
    CHECK(wl_avdtp_session_next_command(s,out,sizeof(out),NULL)==0);
    CHECK(!memcmp(s,&before,sizeof(*s)) && !memcmp(out,saved,sizeof(out)));
    CHECK(wl_avdtp_session_next_command(NULL,out,sizeof(out),&t)==0);
    CHECK(!memcmp(out,saved,sizeof(out)) && !memcmp(&t,&original,sizeof(t)));
    return 0;
}

static int caps_pending(wl_avdtp_session *s,unsigned rate,command *c) {
    memset(s,0,sizeof(*s));
    CHECK(wl_avdtp_session_init(s,100,7,rate)==WL_AVDTP_SESSION_OK);
    CHECK(emit(s,c,DISCOVER,2)==0);
    CHECK(response(s,c,2,sink5,sizeof(sink5))==WL_AVDTP_SESSION_OK);
    CHECK(s->state==WL_AVDTP_SESSION_CAPABILITIES);
    CHECK(emit(s,c,GET_CAPS,3)==0 && c->packet[2]==20);
    return 0;
}

static int configure_ready(wl_avdtp_session *s,unsigned rate) {
    command c;
    CHECK(caps_pending(s,rate,&c)==0);
    CHECK(response(s,&c,2,ldac,sizeof(ldac))==WL_AVDTP_SESSION_OK);
    CHECK(s->state==WL_AVDTP_SESSION_CONFIGURE && s->remote_seid==5);
    return 0;
}

static int streaming(wl_avdtp_session *s) {
    command c;
    CHECK(configure_ready(s,48000)==0);
    CHECK(emit(s,&c,SET_CONFIG,18)==0);
    CHECK(response(s,&c,2,NULL,0)==WL_AVDTP_SESSION_OK);
    CHECK(emit(s,&c,OPEN,3)==0);
    CHECK(response(s,&c,2,NULL,0)==WL_AVDTP_SESSION_OK);
    CHECK(wl_avdtp_session_media_ready(s,100)==WL_AVDTP_SESSION_OK);
    CHECK(emit(s,&c,START,3)==0);
    CHECK(response(s,&c,2,NULL,0)==WL_AVDTP_SESSION_OK);
    CHECK(s->state==WL_AVDTP_SESSION_STREAMING);
    return 0;
}

static int lifecycle_tests(void) {
    wl_avdtp_session s={0},before;
    command c;uint8_t out[32];wl_avdtp_session_token token;
    /* SEP2 is a valid SBC-only sink; SEP5 has exact compatible stereo LDAC. */
    const uint8_t endpoints[]={8,8,20,8};
    const uint8_t expected[]={0,SET_CONFIG,20,28,1,0,7,10,0,0xff,
                              0x2d,1,0,0,0xaa,0,0x10,1};
    CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
    CHECK(unchanged_output(&s,2)==0);
    CHECK(emit(&s,&c,DISCOVER,2)==0);
    before=s;
    memset(out,0xa5,sizeof(out));memset(&token,0x5a,sizeof(token));
    CHECK(wl_avdtp_session_next_command(&s,out,sizeof(out),&token)==0);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    for(size_t i=0;i<sizeof(out);++i)CHECK(out[i]==0xa5);
    for(size_t i=0;i<sizeof(token);++i)CHECK(((const uint8_t *)&token)[i]==0x5a);
    CHECK(wl_avdtp_session_request_abort(&s)==WL_AVDTP_SESSION_BUSY);
    CHECK(response(&s,&c,2,endpoints,sizeof(endpoints))==0);
    CHECK(unchanged_output(&s,3)==0);
    CHECK(emit(&s,&c,GET_CAPS,3)==0 && c.packet[2]==8);
    CHECK(response(&s,&c,2,sbc,sizeof(sbc))==0);
    CHECK(s.state==WL_AVDTP_SESSION_CAPABILITIES);
    CHECK(emit(&s,&c,GET_CAPS,3)==0 && c.packet[2]==20);
    CHECK(response(&s,&c,2,ldac,sizeof(ldac))==0);
    CHECK(s.remote_seid==5 && s.codec[6]==0x10 && s.codec[7]==1);
    CHECK(unchanged_output(&s,18)==0);
    CHECK(emit(&s,&c,SET_CONFIG,18)==0);
    CHECK(!memcmp(c.packet+1,expected+1,sizeof(expected)-1));
    CHECK(response(&s,&c,2,NULL,0)==0 && s.state==WL_AVDTP_SESSION_OPEN);
    CHECK(unchanged_output(&s,3)==0);
    CHECK(emit(&s,&c,OPEN,3)==0 && c.packet[2]==20);
    CHECK(response(&s,&c,2,NULL,0)==0 && s.state==WL_AVDTP_SESSION_WAIT_MEDIA);
    before=s;
    CHECK(wl_avdtp_session_next_command(&s,out,sizeof(out),&token)==0);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    CHECK(wl_avdtp_session_media_ready(&s,99)==WL_AVDTP_SESSION_WRONG_GENERATION);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    CHECK(wl_avdtp_session_media_ready(&s,100)==0);
    CHECK(emit(&s,&c,START,3)==0 && c.packet[2]==20);
    CHECK(response(&s,&c,2,NULL,0)==0 && s.state==WL_AVDTP_SESSION_STREAMING);
    CHECK(wl_avdtp_session_request_suspend(&s)==0);
    CHECK(emit(&s,&c,SUSPEND,3)==0 && c.packet[2]==20);
    CHECK(response(&s,&c,2,NULL,0)==0 && s.state==WL_AVDTP_SESSION_SUSPENDED);
    CHECK(wl_avdtp_session_request_resume(&s)==0);
    CHECK(emit(&s,&c,START,3)==0);
    CHECK(response(&s,&c,2,NULL,0)==0 && s.state==WL_AVDTP_SESSION_STREAMING);
    CHECK(wl_avdtp_session_request_abort(&s)==0);
    CHECK(emit(&s,&c,ABORT,3)==0 && c.packet[2]==20);
    CHECK(response(&s,&c,2,NULL,0)==0 && s.state==WL_AVDTP_SESSION_ABORTED);
    CHECK(wl_avdtp_session_next_command(&s,out,sizeof(out),&token)==0);
    return 0;
}

static int exact_rate_tests(void) {
    static const unsigned rates[]={44100,48000,88200,96000};
    static const uint8_t bits[]={0x20,0x10,0x08,0x04};
    for(unsigned i=0;i<4;++i) {
        wl_avdtp_session s;command c;uint8_t caps[sizeof(ldac)];
        CHECK(configure_ready(&s,rates[i])==0);
        CHECK(s.source_rate==rates[i] && s.codec[6]==bits[i] && s.codec[7]==1);
        CHECK(emit(&s,&c,SET_CONFIG,18)==0);
        CHECK(c.packet[16]==bits[i] && c.packet[17]==1);
        CHECK(caps_pending(&s,rates[i],&c)==0);
        memcpy(caps,ldac,sizeof(caps));caps[12]=bits[(i+1)%4];
        CHECK(response(&s,&c,2,caps,sizeof(caps))==WL_AVDTP_SESSION_CONVERSION_REQUIRED);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.configured);
    }
    {
        wl_avdtp_session s={0};command c;uint8_t caps[sizeof(ldac)];
        const uint8_t sinks[]={8,8,20,8};
        CHECK(wl_avdtp_session_init(&s,100,7,96000)==0);CHECK(emit(&s,&c,DISCOVER,2)==0);
        CHECK(response(&s,&c,2,sinks,sizeof(sinks))==0);CHECK(emit(&s,&c,GET_CAPS,3)==0);
        memcpy(caps,ldac,sizeof(caps));caps[12]=0x10;
        CHECK(response(&s,&c,2,caps,sizeof(caps))==0);
        CHECK(s.state==WL_AVDTP_SESSION_CAPABILITIES && !s.configured);
        CHECK(emit(&s,&c,GET_CAPS,3)==0 && c.packet[2]==20);
        CHECK(response(&s,&c,2,ldac,sizeof(ldac))==0);
        CHECK(s.state==WL_AVDTP_SESSION_CONFIGURE && s.remote_seid==5 && s.codec[6]==4);
    }
    return 0;
}

static int matching_tests(void) {
    wl_avdtp_session s={0},before;command c;
    uint8_t packet[4];
    CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
    CHECK(emit(&s,&c,DISCOVER,2)==0);
    packet[0]=(uint8_t)((c.token.transaction<<4)|2);packet[1]=DISCOVER;
    memcpy(packet+2,sink5,2);before=s;
    CHECK(wl_avdtp_session_receive(&s,99,packet,4)==WL_AVDTP_SESSION_WRONG_GENERATION);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    packet[0]^=0x10;
    CHECK(wl_avdtp_session_receive(&s,100,packet,4)==WL_AVDTP_SESSION_UNMATCHED_RESPONSE);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    packet[0]^=0x10;packet[1]=OPEN;
    CHECK(wl_avdtp_session_receive(&s,100,packet,4)==WL_AVDTP_SESSION_UNMATCHED_RESPONSE);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    packet[1]=DISCOVER;
    CHECK(wl_avdtp_session_receive(&s,100,packet,4)==0);
    before=s;
    CHECK(wl_avdtp_session_receive(&s,100,packet,4)==WL_AVDTP_SESSION_UNMATCHED_RESPONSE);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    /* RFA bits in signal, SEP byte and SEP attributes must be ignored. */
    memset(&s,0,sizeof(s));CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
    CHECK(emit(&s,&c,DISCOVER,2)==0);
    packet[0]=(uint8_t)((c.token.transaction<<4)|2);packet[1]=DISCOVER|0xc0;
    packet[2]=21;packet[3]=15;
    CHECK(wl_avdtp_session_receive(&s,100,packet,4)==0);
    CHECK(emit(&s,&c,GET_CAPS,3)==0 && c.packet[2]==20);
    return 0;
}

static int unmatched_malformed_priority_tests(void) {
    wl_avdtp_session s={0},before;command c;
    const uint8_t wrong_reserved_signal[]={0x12,0x00};
    uint8_t oversized[WL_AVDTP_SESSION_MAX_PACKET+1];
    CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
    CHECK(emit(&s,&c,DISCOVER,2)==0 && c.token.transaction==0);
    before=s;
    CHECK(wl_avdtp_session_receive(&s,100,wrong_reserved_signal,
          sizeof(wrong_reserved_signal))==WL_AVDTP_SESSION_UNMATCHED_RESPONSE);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    memset(oversized,0,sizeof(oversized));
    oversized[0]=0x12;oversized[1]=DISCOVER;
    CHECK(wl_avdtp_session_receive(&s,100,oversized,sizeof(oversized))==
          WL_AVDTP_SESSION_UNMATCHED_RESPONSE);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    /* Matching envelopes must still fail validation; unmatched traffic cannot
     * poison the active transaction by supplying an invalid payload/size. */
    oversized[0]=0x02;
    CHECK(wl_avdtp_session_receive(&s,100,oversized,sizeof(oversized))==
          WL_AVDTP_SESSION_MALFORMED);
    CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
    return 0;
}

static int discover_failure_tests(void) {
    const uint8_t odd[]={20};
    const uint8_t duplicate[]={20,8,20,8};
    const uint8_t bad_seid[]={0,8};
    const uint8_t seid63[]={252,8};
    const uint8_t unavailable[]={22,8,24,0,28,0x18};
    const uint8_t *bad[]={odd,duplicate,bad_seid,seid63};
    const size_t lengths[]={sizeof(odd),sizeof(duplicate),sizeof(bad_seid),sizeof(seid63)};
    for(unsigned i=0;i<4;++i) {
        wl_avdtp_session s={0};command c;
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
        CHECK(emit(&s,&c,DISCOVER,2)==0);
        CHECK(response(&s,&c,2,bad[i],lengths[i])==WL_AVDTP_SESSION_MALFORMED);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
    }
    for(unsigned empty=0;empty<2;++empty) {
        wl_avdtp_session s={0};command c;
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
        CHECK(emit(&s,&c,DISCOVER,2)==0);
        CHECK(response(&s,&c,2,empty?NULL:unavailable,empty?0:sizeof(unavailable))==WL_AVDTP_SESSION_NO_ENDPOINT);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED);
    }
    {
        wl_avdtp_session s={0};command c;uint8_t maximum[124];
        for(unsigned i=0;i<62;++i){maximum[2*i]=(uint8_t)((i+1)<<2);maximum[2*i+1]=8;}
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
        CHECK(emit(&s,&c,DISCOVER,2)==0);
        CHECK(response(&s,&c,2,maximum,sizeof(maximum))==0);
        CHECK(s.candidate_count==62 && s.candidates[61]==62);
    }
    {
        wl_avdtp_session s={0};command c;uint8_t overflow[126];
        for(unsigned i=0;i<63;++i){overflow[2*i]=(uint8_t)(((i%62)+1)<<2);overflow[2*i+1]=8;}
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
        CHECK(emit(&s,&c,DISCOVER,2)==0);
        CHECK(response(&s,&c,2,overflow,sizeof(overflow))==WL_AVDTP_SESSION_MALFORMED);
        CHECK(!s.transport_valid);
    }
    return 0;
}

static int capability_failure_tests(void) {
    /* A structurally valid peer can be unsuitable without being malformed. */
    for(unsigned variant=0;variant<4;++variant) {
        wl_avdtp_session s;command c;uint8_t caps[sizeof(ldac)];
        CHECK(caps_pending(&s,48000,&c)==0);
        memcpy(caps,ldac,sizeof(caps));
        if(variant==0)CHECK(response(&s,&c,2,sbc,sizeof(sbc))==WL_AVDTP_SESSION_NO_LDAC);
        if(variant==1){caps[13]=4;CHECK(response(&s,&c,2,caps,sizeof(caps))==WL_AVDTP_SESSION_NO_LDAC);}
        if(variant==2)CHECK(response(&s,&c,2,caps+2,sizeof(caps)-2)==WL_AVDTP_SESSION_MALFORMED);
        if(variant==3){caps[6]=0;CHECK(response(&s,&c,2,caps,sizeof(caps))==WL_AVDTP_SESSION_NO_LDAC);}
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.configured);
    }
    for(unsigned variant=0;variant<5;++variant) {
        wl_avdtp_session s;command c;uint8_t caps[sizeof(ldac)*2];size_t n=sizeof(ldac);
        CHECK(caps_pending(&s,48000,&c)==0);memcpy(caps,ldac,sizeof(ldac));
        if(variant==0)n=sizeof(ldac)-1; /* declared length exceeds packet */
        if(variant==1){caps[n++]=7;} /* trailing truncated TLV header */
        if(variant==2){memcpy(caps+n,ldac,sizeof(ldac));n+=sizeof(ldac);} /* duplicate categories */
        if(variant==3){caps[1]=1;} /* MediaTransport must have zero length */
        if(variant==4)n=0; /* accept without capabilities */
        CHECK(response(&s,&c,2,caps,n)==WL_AVDTP_SESSION_MALFORMED);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid);
    }
    return 0;
}

static int reject_and_packet_tests(void) {
    static const uint8_t discover_reject[]={0x31};
    static const uint8_t config_reject[]={7,0x29};
    for(unsigned config=0;config<2;++config) {
        wl_avdtp_session s={0};command c;
        if(config){CHECK(configure_ready(&s,48000)==0);CHECK(emit(&s,&c,SET_CONFIG,18)==0);}
        else{CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);CHECK(emit(&s,&c,DISCOVER,2)==0);}
        CHECK(response(&s,&c,3,config?config_reject:discover_reject,config?2:1)==WL_AVDTP_SESSION_REJECTED);
        CHECK(s.last_reject_error==(config?0x29:0x31));
        if(config)CHECK(s.last_reject_category==7);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.pending);
    }
    for(unsigned variant=0;variant<7;++variant) {
        wl_avdtp_session s={0},before;command c;uint8_t packet[8];size_t n=2;
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);CHECK(emit(&s,&c,DISCOVER,2)==0);
        packet[0]=(uint8_t)((c.token.transaction<<4)|3);packet[1]=DISCOVER;
        if(variant==0)n=2; /* reject with missing error */
        if(variant==1){packet[2]=0x31;packet[3]=0;n=4;} /* oversized reject */
        if(variant==2){packet[0]=(uint8_t)((c.token.transaction<<4)|1);n=2;} /* GeneralReject */
        if(variant==3){packet[0]=(uint8_t)((c.token.transaction<<4)|2);n=1;} /* short header */
        if(variant==4){packet[0]=(uint8_t)((c.token.transaction<<4)|2|4);n=4;packet[2]=20;packet[3]=8;}
        if(variant==5){packet[0]=(uint8_t)(c.token.transaction<<4);n=2;}
        if(variant==6){packet[0]=(uint8_t)((c.token.transaction<<4)|2);packet[1]=0;n=2;}
        before=s;
        {
            int result=wl_avdtp_session_receive(&s,100,packet,n);
            if(variant==4)CHECK(result==WL_AVDTP_SESSION_UNSUPPORTED_FRAGMENT);
            else if(variant==5)CHECK(result==WL_AVDTP_SESSION_REMOTE_COMMAND);
            else if(variant==2)CHECK(result==WL_AVDTP_SESSION_REJECTED);
            else if(variant==6)CHECK(result==WL_AVDTP_SESSION_UNMATCHED_RESPONSE);
            else CHECK(result==WL_AVDTP_SESSION_MALFORMED);
        }
        if(variant==6)CHECK(!memcmp(&s,&before,sizeof(s)));
        else CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
    }
    /* Every payload-free accept must reject unexpected trailing bytes. */
    for(unsigned stage=0;stage<6;++stage) {
        wl_avdtp_session s;command c;uint8_t junk=0;
        CHECK(configure_ready(&s,48000)==0);CHECK(emit(&s,&c,SET_CONFIG,18)==0);
        if(stage>0){CHECK(response(&s,&c,2,NULL,0)==0);CHECK(emit(&s,&c,OPEN,3)==0);}
        if(stage>1){CHECK(response(&s,&c,2,NULL,0)==0);CHECK(wl_avdtp_session_media_ready(&s,100)==0);CHECK(emit(&s,&c,START,3)==0);}
        if(stage>2){CHECK(response(&s,&c,2,NULL,0)==0);CHECK(wl_avdtp_session_request_suspend(&s)==0);CHECK(emit(&s,&c,SUSPEND,3)==0);}
        if(stage>3){CHECK(response(&s,&c,2,NULL,0)==0);CHECK(wl_avdtp_session_request_resume(&s)==0);CHECK(emit(&s,&c,START,3)==0);}
        if(stage>4){CHECK(response(&s,&c,2,NULL,0)==0);CHECK(wl_avdtp_session_request_abort(&s)==0);CHECK(emit(&s,&c,ABORT,3)==0);}
        CHECK(response(&s,&c,2,&junk,1)==WL_AVDTP_SESSION_MALFORMED);
        CHECK(!s.transport_valid && s.state==WL_AVDTP_SESSION_FAILED);
    }
    return 0;
}

static int fixed_signal_reject_tests(void) {
    for(unsigned start=0;start<2;++start) {
        for(unsigned variant=0;variant<5;++variant) {
            wl_avdtp_session s;command c;uint8_t rejection[3];size_t length=2;
            CHECK(streaming(&s)==0);
            CHECK(wl_avdtp_session_request_suspend(&s)==0);
            CHECK(emit(&s,&c,SUSPEND,3)==0);
            if(start) {
                CHECK(response(&s,&c,2,NULL,0)==0);
                CHECK(wl_avdtp_session_request_resume(&s)==0);
                CHECK(emit(&s,&c,START,3)==0);
            }
            rejection[0]=(uint8_t)(s.remote_seid<<2);rejection[1]=0x31;rejection[2]=0;
            if(variant==1)rejection[0]=(uint8_t)((s.remote_seid+1)<<2);
            if(variant==2)length=1;
            if(variant==3)length=3;
            if(variant==4)rejection[1]=0; /* a reject needs a nonzero error */
            CHECK(response(&s,&c,3,rejection,length)==
                  (variant==0?WL_AVDTP_SESSION_REJECTED:WL_AVDTP_SESSION_MALFORMED));
            CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
            if(variant==0)CHECK(s.last_reject_error==0x31);
        }
    }
    {
        wl_avdtp_session s;command c;const uint8_t error=0x31;
        CHECK(streaming(&s)==0);
        CHECK(wl_avdtp_session_request_abort(&s)==0);CHECK(emit(&s,&c,ABORT,3)==0);
        CHECK(response(&s,&c,3,&error,1)==WL_AVDTP_SESSION_MALFORMED);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
    }
    {
        wl_avdtp_session s={0};command c;const uint8_t junk=0x31;
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);CHECK(emit(&s,&c,DISCOVER,2)==0);
        CHECK(response(&s,&c,1,&junk,1)==WL_AVDTP_SESSION_MALFORMED);
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
    }
    return 0;
}

static int packet_boundary_tests(void) {
    wl_avdtp_session s;command c;
    uint8_t payload[WL_AVDTP_SESSION_MAX_PACKET-2];
    uint8_t oversized[WL_AVDTP_SESSION_MAX_PACKET+1];
    size_t offset=sizeof(ldac);
    memset(payload,0,sizeof(payload));memcpy(payload,ldac,sizeof(ldac));
    /* Pad with well-bounded ignored extension TLVs. Four TLVs fill the 1008
     * remaining bytes; none claims negotiated support or another mandatory
     * MediaTransport/MediaCodec. This is a parser boundary, not certification
     * of an extension category by the Bluetooth specification. */
    while(offset<sizeof(payload)) {
        size_t remaining=sizeof(payload)-offset;
        size_t body=remaining>257?255:remaining-2;
        CHECK(remaining>=2 && body<=255);
        payload[offset]=0x7f;payload[offset+1]=(uint8_t)body;offset+=2+body;
    }
    CHECK(offset==sizeof(payload));
    CHECK(caps_pending(&s,48000,&c)==0);
    CHECK(response(&s,&c,2,payload,sizeof(payload))==0);
    CHECK(s.state==WL_AVDTP_SESSION_CONFIGURE && s.codec[6]==0x10);
    CHECK(caps_pending(&s,48000,&c)==0);
    oversized[0]=(uint8_t)((c.token.transaction<<4)|2);oversized[1]=GET_CAPS;
    memcpy(oversized+2,payload,sizeof(payload));oversized[sizeof(oversized)-1]=0;
    CHECK(wl_avdtp_session_receive(&s,100,oversized,sizeof(oversized))==WL_AVDTP_SESSION_MALFORMED);
    CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
    return 0;
}

static int sequence_exhaustion_tests(void) {
    wl_avdtp_session s={0};uint8_t out[32],saved[32];
    wl_avdtp_session_token t,original;
    CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);
    s.serial=UINT64_MAX;
    /* The capacity/NULL preflight takes precedence over serial exhaustion. */
    CHECK(unchanged_output(&s,2)==0);
    memset(out,0xa5,sizeof(out));memcpy(saved,out,sizeof(out));
    memset(&t,0x5a,sizeof(t));memcpy(&original,&t,sizeof(t));
    CHECK(wl_avdtp_session_next_command(&s,out,sizeof(out),&t)==0);
    CHECK(s.serial==UINT64_MAX && s.state==WL_AVDTP_SESSION_FAILED);
    CHECK(s.last_error==WL_AVDTP_SESSION_SEQUENCE_EXHAUSTED && !s.transport_valid && !s.pending);
    CHECK(!memcmp(out,saved,sizeof(out)) && !memcmp(&t,&original,sizeof(t)));
    CHECK(wl_avdtp_session_init(&s,101,7,48000)==0 && s.serial==0);
    return 0;
}

static int token_and_epoch_tests(void) {
    for(unsigned failed_send=0;failed_send<2;++failed_send) {
        wl_avdtp_session s={0},before;command c,old;wl_avdtp_session_token wrong;
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);CHECK(emit(&s,&old,DISCOVER,2)==0);
        CHECK(response(&s,&old,2,sink5,sizeof(sink5))==0);CHECK(emit(&s,&c,GET_CAPS,3)==0);
        before=s;
        for(unsigned field=0;field<4;++field) {
            wrong=c.token;
            if(field==0)++wrong.generation;if(field==1)++wrong.serial;
            if(field==2)wrong.transaction^=1;if(field==3)wrong.signal^=1;
            CHECK((failed_send?wl_avdtp_session_send_failed(&s,&wrong):wl_avdtp_session_timeout(&s,&wrong))==WL_AVDTP_SESSION_TOKEN_MISMATCH);
            CHECK(!memcmp(&s,&before,sizeof(s)));
        }
        CHECK((failed_send?wl_avdtp_session_send_failed(&s,&old.token):wl_avdtp_session_timeout(&s,&old.token))==WL_AVDTP_SESSION_TOKEN_MISMATCH);
        CHECK(!memcmp(&s,&before,sizeof(s)));
        CHECK((failed_send?wl_avdtp_session_send_failed(&s,&c.token):wl_avdtp_session_timeout(&s,&c.token))==(failed_send?WL_AVDTP_SESSION_SEND_FAILED:WL_AVDTP_SESSION_TIMEOUT));
        CHECK(s.state==WL_AVDTP_SESSION_FAILED && !s.transport_valid && !s.pending);
        before=s;
        CHECK(response(&s,&c,2,ldac,sizeof(ldac))==WL_AVDTP_SESSION_TRANSPORT_INVALID);
        CHECK(!memcmp(&s,&before,sizeof(s)));
        CHECK(wl_avdtp_session_init(&s,100,7,48000)<0);
        CHECK(wl_avdtp_session_init(&s,99,7,48000)<0);
        CHECK(wl_avdtp_session_init(&s,101,7,48000)==0);
        CHECK(emit(&s,&c,DISCOVER,2)==0 && c.token.generation==101);
        before=s;
        CHECK(wl_avdtp_session_timeout(&s,&old.token)==WL_AVDTP_SESSION_TOKEN_MISMATCH);
        CHECK(!memcmp(&s,&before,sizeof(s)));
    }
    {
        wl_avdtp_session s={0},before;command c;
        CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);before=s;
        CHECK(wl_avdtp_session_init(&s,101,7,48000)==WL_AVDTP_SESSION_INVALID_STATE);
        CHECK(!memcmp(&s,&before,sizeof(s)));
        CHECK(emit(&s,&c,DISCOVER,2)==0);before=s;
        CHECK(wl_avdtp_session_transport_failed(&s,99)==WL_AVDTP_SESSION_WRONG_GENERATION);
        CHECK(!memcmp(&s,&before,sizeof(s)));
        CHECK(wl_avdtp_session_transport_failed(&s,100)==WL_AVDTP_SESSION_TRANSPORT_FAILED);
        CHECK(!s.transport_valid && !s.pending && s.state==WL_AVDTP_SESSION_FAILED);
        CHECK(wl_avdtp_session_init(&s,101,7,48000)==0);
    }
    return 0;
}

static int state_and_wrap_tests(void) {
    wl_avdtp_session s={0},before;command c,old;uint8_t out[32];wl_avdtp_session_token t;
    CHECK(wl_avdtp_session_init(NULL,1,7,48000)==WL_AVDTP_SESSION_INVALID_ARGUMENT);
    CHECK(wl_avdtp_session_init(&s,0,7,48000)==WL_AVDTP_SESSION_INVALID_ARGUMENT);
    CHECK(wl_avdtp_session_init(&s,1,0,48000)==WL_AVDTP_SESSION_INVALID_ARGUMENT);
    CHECK(wl_avdtp_session_init(&s,1,63,48000)==WL_AVDTP_SESSION_INVALID_ARGUMENT);
    CHECK(wl_avdtp_session_init(&s,1,7,192000)==WL_AVDTP_SESSION_INVALID_ARGUMENT);
    memset(&s,0,sizeof(s));CHECK(wl_avdtp_session_init(&s,100,7,48000)==0);before=s;
    CHECK(wl_avdtp_session_request_suspend(&s)==WL_AVDTP_SESSION_INVALID_STATE);
    CHECK(wl_avdtp_session_request_resume(&s)==WL_AVDTP_SESSION_INVALID_STATE);
    CHECK(wl_avdtp_session_media_ready(&s,100)==WL_AVDTP_SESSION_INVALID_STATE);
    CHECK(!memcmp(&s,&before,sizeof(s)));
    CHECK(wl_avdtp_session_request_abort(&s)==0 && s.state==WL_AVDTP_SESSION_ABORTED);
    CHECK(wl_avdtp_session_next_command(&s,out,sizeof(out),&t)==0);
    CHECK(streaming(&s)==0);old.token=s.outstanding;
    for(unsigned i=0;i<20;++i) {
        CHECK(wl_avdtp_session_request_suspend(&s)==0);
        CHECK(emit(&s,&c,SUSPEND,3)==0);
        CHECK(c.token.serial>old.token.serial);
        before=s;
        CHECK(wl_avdtp_session_timeout(&s,&old.token)==WL_AVDTP_SESSION_TOKEN_MISMATCH);
        CHECK(!memcmp(&s,&before,sizeof(s)));
        CHECK(response(&s,&c,2,NULL,0)==0);old=c;
        CHECK(wl_avdtp_session_request_resume(&s)==0);CHECK(emit(&s,&c,START,3)==0);
        CHECK(c.token.transaction==(uint8_t)((old.token.transaction+1)&15));
        CHECK(response(&s,&c,2,NULL,0)==0);old=c;
    }
    /* Deliberately no impossible guarantee against a wire duplicate with the
     * same signal/4-bit label after wrap. Only local serial tokens are unique. */
    CHECK(s.state==WL_AVDTP_SESSION_STREAMING && s.serial>32);
    return 0;
}

int main(void) {
    if(lifecycle_tests() || exact_rate_tests() || matching_tests() ||
       unmatched_malformed_priority_tests() ||
       discover_failure_tests() || capability_failure_tests() ||
       reject_and_packet_tests() || fixed_signal_reject_tests() || packet_boundary_tests() ||
       sequence_exhaustion_tests() || token_and_epoch_tests() || state_and_wrap_tests())return 1;
    printf("{\"kind\":\"offline_avdtp_session_tests\",\"checks\":%u,"
           "\"real_bluetooth_tested\":false,\"headset_audio_tested\":false}\n",checks);
    return 0;
}
