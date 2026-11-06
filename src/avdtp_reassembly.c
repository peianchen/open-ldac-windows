/* SPDX-License-Identifier: Apache-2.0 */
#include "avdtp_reassembly.h"
#include <string.h>

void wl_avdtp_reassembly_reset(wl_avdtp_reassembly *s) {
    if(s)memset(s,0,sizeof(*s));
}
static int poison(wl_avdtp_reassembly *s,int error) {
    if(s){s->active=0;s->remaining=0;s->size=0;s->poisoned=1;}
    return error;
}
int wl_avdtp_reassembly_push(wl_avdtp_reassembly *s,const uint8_t *packet,
                            size_t size,const uint8_t **message,size_t *message_size) {
    unsigned type;uint8_t tuple,signal;size_t header,payload;
    if(message)*message=NULL;
    if(message_size)*message_size=0;
    if(!s || !packet || !message || !message_size)
        return poison(s,WL_AVDTP_REASSEMBLY_INVALID_ARGUMENT);
    if(s->poisoned)return WL_AVDTP_REASSEMBLY_POISONED;
    if(!size)return poison(s,WL_AVDTP_REASSEMBLY_MALFORMED);
    if(size>WL_AVDTP_REASSEMBLY_MAX_PACKET)return poison(s,WL_AVDTP_REASSEMBLY_LIMIT);
    type=(packet[0]>>2)&3u;tuple=packet[0]&0xf3u;
    header=type==0?2u:type==1?3u:1u;
    if(size<header)return poison(s,WL_AVDTP_REASSEMBLY_MALFORMED);
    payload=size-header;
    if(type==0 || type==1) {
        if(s->active)return poison(s,WL_AVDTP_REASSEMBLY_SEQUENCE);
        signal=packet[header-1]&0x3fu;
        /* Signal zero is left to the session, including version-dependent
         * GENERAL_REJECT handling. Framing does not interpret signal IDs. */
        if(type==1 && (packet[1]<2 || packet[1]>WL_AVDTP_REASSEMBLY_MAX_FRAGMENTS))
            return poison(s,packet[1]<2?WL_AVDTP_REASSEMBLY_MALFORMED:WL_AVDTP_REASSEMBLY_LIMIT);
        s->remaining=type==1?(uint8_t)(packet[1]-1u):0;
        s->tuple=tuple;s->size=2;
        /* memmove also supports a caller passing the previous completed
         * output as the next SINGLE input; save header values before copy. */
        memmove(s->packet+2,packet+header,payload);
        s->packet[0]=tuple;s->packet[1]=signal;s->size+=payload;
        s->active=(uint8_t)(type==1);
        if(s->active)return WL_AVDTP_REASSEMBLY_MORE;
    } else {
        if(!s->active || tuple!=s->tuple || !s->remaining ||
           (type==2 && s->remaining<=1) || (type==3 && s->remaining!=1))
            return poison(s,WL_AVDTP_REASSEMBLY_SEQUENCE);
        if(s->size>WL_AVDTP_REASSEMBLY_MAX_PACKET ||
           payload>WL_AVDTP_REASSEMBLY_MAX_PACKET-s->size)
            return poison(s,WL_AVDTP_REASSEMBLY_LIMIT);
        memmove(s->packet+s->size,packet+header,payload);s->size+=payload;
        --s->remaining;
        if(type==2)return WL_AVDTP_REASSEMBLY_MORE;
        s->active=0;
    }
    *message=s->packet;*message_size=s->size;
    return WL_AVDTP_REASSEMBLY_COMPLETE;
}
