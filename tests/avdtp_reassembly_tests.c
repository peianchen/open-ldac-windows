/* SPDX-License-Identifier: Apache-2.0 */
/* Offline framing fixtures; not Bluetooth interoperability or qualification. */
#include "avdtp_reassembly.h"
#include <stdio.h>
#include <string.h>

static unsigned checks;
#define CHECK(c) do {++checks;if(!(c)){fprintf(stderr,"FAIL reassembly:%d: %s\n",__LINE__,#c);return 1;}}while(0)
typedef struct {uint8_t before[16];wl_avdtp_reassembly state;uint8_t after[16];} guarded;
static void init(guarded *g) {
    memset(g,0xa5,sizeof(*g));wl_avdtp_reassembly_reset(&g->state);
}
static int intact(const guarded *g) {
    for(unsigned i=0;i<16;++i)CHECK(g->before[i]==0xa5&&g->after[i]==0xa5);
    CHECK(g->state.size<=WL_AVDTP_REASSEMBLY_MAX_PACKET);
    CHECK(g->state.remaining<WL_AVDTP_REASSEMBLY_MAX_FRAGMENTS);
    return 0;
}
static int push(guarded *g,const uint8_t *p,size_t size,int expected,
                const uint8_t *whole,size_t length) {
    const uint8_t *out=(const uint8_t *)"old";size_t n=12345;
    CHECK(wl_avdtp_reassembly_push(&g->state,p,size,&out,&n)==expected);
    CHECK(intact(g)==0);
    if(expected==WL_AVDTP_REASSEMBLY_COMPLETE) {
        CHECK(out==g->state.packet&&n==length&&!g->state.active&&!g->state.remaining);
        CHECK(!memcmp(out,whole,n));
    } else {
        CHECK(out==NULL&&n==0);
        if(expected<0)CHECK(g->state.poisoned&&!g->state.active&&!g->state.size&&!g->state.remaining);
    }
    return 0;
}
static int single_tests(void) {
    guarded g;uint8_t p[1025],wanted[1024];const uint8_t *out;size_t n;
    init(&g);
    for(unsigned label=0;label<16;++label)for(unsigned type=0;type<4;++type)
    for(unsigned signal=0;signal<64;++signal){
        p[0]=(uint8_t)((label<<4)|type);p[1]=(uint8_t)(signal|0xc0);
        wanted[0]=p[0];wanted[1]=(uint8_t)signal;
        CHECK(push(&g,p,2,0,wanted,2)==0);
    }
    for(unsigned i=0;i<sizeof(p);++i)p[i]=(uint8_t)i;
    p[0]=0xe2;p[1]=2;memcpy(wanted,p,1024);
    CHECK(push(&g,p,1024,0,wanted,1024)==0);
    CHECK(wl_avdtp_reassembly_push(&g.state,g.state.packet,g.state.size,&out,&n)==0);
    CHECK(n==1024&&!memcmp(out,wanted,n)&&intact(&g)==0);
    CHECK(push(&g,p,1025,WL_AVDTP_REASSEMBLY_LIMIT,NULL,0)==0);
    CHECK(push(&g,p,2,WL_AVDTP_REASSEMBLY_POISONED,NULL,0)==0);
    wl_avdtp_reassembly_reset(&g.state);
    CHECK(push(&g,p,2,0,p,2)==0);
    return 0;
}
static int all_splits(void) {
    guarded g;uint8_t whole[34],start[35],cont[34],end[34];
    for(unsigned i=2;i<sizeof(whole);++i)whole[i]=(uint8_t)(i*7);
    for(unsigned label=0;label<16;++label)for(unsigned message=0;message<4;++message){
        whole[0]=(uint8_t)((label<<4)|message);whole[1]=2;
        for(size_t a=0;a<=32;++a){
            init(&g);start[0]=whole[0]|4;start[1]=2;start[2]=whole[1]|0x80;
            memcpy(start+3,whole+2,a);end[0]=whole[0]|12;memcpy(end+1,whole+2+a,32-a);
            CHECK(push(&g,start,a+3,1,NULL,0)==0);
            CHECK(push(&g,end,33-a,0,whole,sizeof(whole))==0);
        }
    }
    whole[0]=0x52;whole[1]=2;
    /* Every three-way split of a capability-shaped payload, including zero
     * payload fragments. Framing neither assumes TLV boundaries nor parses it. */
    for(size_t a=0;a<=32;++a)for(size_t b=a;b<=32;++b){
        init(&g);start[0]=0x56;start[1]=3;start[2]=2;memcpy(start+3,whole+2,a);
        cont[0]=0x5a;memcpy(cont+1,whole+2+a,b-a);
        end[0]=0x5e;memcpy(end+1,whole+2+b,32-b);
        CHECK(push(&g,start,a+3,1,NULL,0)==0);
        CHECK(push(&g,cont,b-a+1,1,NULL,0)==0);
        CHECK(push(&g,end,33-b,0,whole,sizeof(whole))==0);
    }
    return 0;
}
static int boundary_tests(void) {
    guarded g;uint8_t start[1024]={0x96,2,0xc2},end[4]={0x9e,7,8,9};
    uint8_t expected[1024]={0x92,2};
    memset(start+3,0x51,sizeof(start)-3);memset(expected+2,0x51,sizeof(expected)-2);expected[1023]=7;
    init(&g);CHECK(push(&g,start,1024,1,NULL,0)==0);
    CHECK(push(&g,end,2,0,expected,1024)==0);
    init(&g);CHECK(push(&g,start,1024,1,NULL,0)==0);
    CHECK(push(&g,end,3,WL_AVDTP_REASSEMBLY_LIMIT,NULL,0)==0);
    init(&g);start[1]=16;CHECK(push(&g,start,3,1,NULL,0)==0);
    for(unsigned i=0;i<14;++i){uint8_t cont=0x9a;CHECK(push(&g,&cont,1,1,NULL,0)==0);}
    CHECK(push(&g,end,1,0,expected,2)==0);
    CHECK(push(&g,end,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    return 0;
}
static int invalid_tests(void) {
    guarded g;const uint8_t *out;size_t size;
    const uint8_t single[]={0x22,2},start[]={0x26,3,2},cont[]={0x2a},end[]={0x2e};
    init(&g);out=single;size=99;
    CHECK(wl_avdtp_reassembly_push(NULL,single,2,&out,&size)==WL_AVDTP_REASSEMBLY_INVALID_ARGUMENT);
    CHECK(!out&&!size);
    CHECK(push(&g,NULL,0,WL_AVDTP_REASSEMBLY_INVALID_ARGUMENT,NULL,0)==0);
    wl_avdtp_reassembly_reset(NULL);
    init(&g);CHECK(push(&g,single,0,WL_AVDTP_REASSEMBLY_MALFORMED,NULL,0)==0);
    init(&g);CHECK(push(&g,single,1,WL_AVDTP_REASSEMBLY_MALFORMED,NULL,0)==0);
    init(&g);CHECK(push(&g,start,2,WL_AVDTP_REASSEMBLY_MALFORMED,NULL,0)==0);
    for(unsigned count=0;count<256;++count)if(count<2||count>16){
        uint8_t p[]={0x26,(uint8_t)count,2};init(&g);
        CHECK(push(&g,p,3,count<2?WL_AVDTP_REASSEMBLY_MALFORMED:WL_AVDTP_REASSEMBLY_LIMIT,NULL,0)==0);
    }
    init(&g);CHECK(push(&g,cont,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    init(&g);CHECK(push(&g,end,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    init(&g);CHECK(push(&g,start,3,1,NULL,0)==0);
    CHECK(push(&g,single,2,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    init(&g);CHECK(push(&g,start,3,1,NULL,0)==0);
    CHECK(push(&g,start,3,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    init(&g);CHECK(push(&g,start,3,1,NULL,0)==0);
    CHECK(push(&g,end,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    init(&g);CHECK(push(&g,start,3,1,NULL,0)==0);
    CHECK(push(&g,cont,1,1,NULL,0)==0);
    CHECK(push(&g,cont,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    for(unsigned tuple=0;tuple<256;++tuple)if(!(tuple&12)&&tuple!=0x22){
        uint8_t p=(uint8_t)(tuple|8);init(&g);CHECK(push(&g,start,3,1,NULL,0)==0);
        CHECK(push(&g,&p,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    }
    init(&g);size=99;
    CHECK(wl_avdtp_reassembly_push(&g.state,single,2,NULL,&size)==WL_AVDTP_REASSEMBLY_INVALID_ARGUMENT);
    CHECK(!size&&g.state.poisoned);
    init(&g);out=single;
    CHECK(wl_avdtp_reassembly_push(&g.state,single,2,&out,NULL)==WL_AVDTP_REASSEMBLY_INVALID_ARGUMENT);
    CHECK(!out&&g.state.poisoned);
    /* A timeout/new epoch discards an incomplete prefix, never joins epochs. */
    init(&g);CHECK(push(&g,start,3,1,NULL,0)==0);wl_avdtp_reassembly_reset(&g.state);
    CHECK(push(&g,end,1,WL_AVDTP_REASSEMBLY_SEQUENCE,NULL,0)==0);
    wl_avdtp_reassembly_reset(&g.state);CHECK(push(&g,single,2,0,single,2)==0);
    return 0;
}
static uint32_t random_next(uint32_t *s){*s=*s*UINT32_C(1664525)+UINT32_C(1013904223);return *s;}
static int random_bounds(void) {
    guarded g;uint8_t packet[1025];uint32_t seed=0x6a09e667;
    for(unsigned round=0;round<2000;++round){
        init(&g);
        for(unsigned fragment=0;fragment<20;++fragment){
            const uint8_t *out=(const uint8_t *)"old";size_t n=99;
            size_t bytes=random_next(&seed)%sizeof(packet);
            for(size_t i=0;i<bytes;++i)packet[i]=(uint8_t)(random_next(&seed)>>24);
            int rc=wl_avdtp_reassembly_push(&g.state,packet,bytes,&out,&n);
            CHECK(intact(&g)==0);
            if(rc==0){CHECK(out==g.state.packet&&n>=2&&n<=1024&&!g.state.active);}
            else {CHECK(!out&&!n);if(rc<0){CHECK(g.state.poisoned);break;}}
        }
    }
    return 0;
}
int main(void) {
    if(single_tests()||all_splits()||boundary_tests()||invalid_tests()||random_bounds())return 1;
    printf("{\"kind\":\"offline_avdtp_reassembly_tests\",\"checks\":%u,"
           "\"real_bluetooth_tested\":false,\"headset_audio_tested\":false}\n",checks);
    return 0;
}
