/* SPDX-License-Identifier: Apache-2.0 */
#include "clisrv.h"
#include "device.h"
#include "queue.h"

/* Deterministic API boundary model. These actual transport wrappers run as host
 * C, not in a Windows kernel. No scheduler, IRQL or Bluetooth contract is proven. */
struct mock_object {
    unsigned refs,deleted,locked,completed,sent,canceled,stopped;
    void *context,*buffer; size_t context_size,size;
    struct mock_object *parent,*children,*next,*device,*file,*memory;
    EVT_WDF_OBJECT_CONTEXT_CLEANUP *cleanup;
    EVT_WDF_OBJECT_CONTEXT_DESTROY *destroy;
    PFN_WDF_REQUEST_COMPLETION_ROUTINE completion;
    void *completion_context;
    NTSTATUS status; size_t information;
};
static WDFOBJECT driver;
static unsigned checks,live,allocations,fail_alloc,fail_reuse,fail_format,fail_send;
static unsigned close_sends,data_sends,open_sends,cancel_calls,inline_complete;
static unsigned callback_registration;
static PFNBTHPORT_INDICATION_CALLBACK provider_callback;
static void *provider_context;
static WDFREQUEST last_open,last_close,last_data;
static void (*send_hook)(WDFREQUEST);
static NTSTATUS inline_status;

#define CHECK(x) do { ++checks; mock_assert(!!(x),#x,__LINE__); } while(0)
void mock_assert(int ok,const char *what,int line) {
    if (!ok) { fprintf(stderr,"FAIL adapter:%d: %s\n",line,what); exit(1); }
}
void *mock_context(WDFOBJECT o,size_t n) { CHECK(o && o->context_size==n); return o->context; }
WDFDRIVER WdfGetDriver(void) { return driver; }
void WdfIoTargetStop(WDFIOTARGET t,unsigned action) { CHECK(action==WdfIoTargetLeaveSentIoPending);CHECK(!t->stopped);t->stopped=1; }
void WdfObjectReference(WDFOBJECT o) { CHECK(o && o->refs); ++o->refs; }
void WdfObjectDereference(WDFOBJECT o) {
    WDFOBJECT parent;
    CHECK(o && o->refs);
    if (--o->refs) return;
    CHECK(o->deleted && !o->children && !o->locked);
    if (o->destroy) o->destroy(o);
    parent=o->parent;
    if (parent) {
        WDFOBJECT *p=&parent->children;
        while (*p!=o) { CHECK(*p!=NULL); p=&(*p)->next; }
        *p=o->next;
    }
    free(o->context); free(o); --live;
    if (parent) WdfObjectDereference(parent);
}
void WdfObjectDelete(WDFOBJECT o) {
    WDFOBJECT child,next;
    CHECK(o && !o->deleted); o->deleted=1;
    if (o->cleanup) o->cleanup(o);
    for (child=o->children;child;child=next) {
        next=child->next;
        if (!child->deleted) WdfObjectDelete(child);
    }
    WdfObjectDereference(o);
}
static WDFOBJECT new_object(WDF_OBJECT_ATTRIBUTES *a) {
    WDFOBJECT o;
    if (++allocations==fail_alloc) return NULL;
    o=calloc(1,sizeof(*o)); CHECK(o!=NULL);
    o->refs=1; ++live;
    if (a) {
        o->context_size=a->ContextSize;
        if (a->ContextSize) { o->context=calloc(1,a->ContextSize); CHECK(o->context!=NULL); }
        o->cleanup=a->EvtCleanupCallback; o->destroy=a->EvtDestroyCallback;
        o->parent=a->ParentObject;
        if (o->parent) {
            CHECK(!o->parent->deleted);
            WdfObjectReference(o->parent);
            o->next=o->parent->children; o->parent->children=o;
        }
    }
    return o;
}
NTSTATUS WdfObjectCreate(WDF_OBJECT_ATTRIBUTES *a,WDFOBJECT *o) { *o=new_object(a);return *o?0:STATUS_INSUFFICIENT_RESOURCES; }
NTSTATUS WdfSpinLockCreate(WDF_OBJECT_ATTRIBUTES *a,WDFSPINLOCK *o) { return WdfObjectCreate(a,o); }
void WdfSpinLockAcquire(WDFSPINLOCK o) { CHECK(o && !o->deleted && !o->locked);o->locked=1; }
void WdfSpinLockRelease(WDFSPINLOCK o) { CHECK(o && o->locked);o->locked=0; }
NTSTATUS WdfRequestCreate(WDF_OBJECT_ATTRIBUTES *a,WDFIOTARGET t,WDFREQUEST *r) { (void)t;return WdfObjectCreate(a,r); }
NTSTATUS WdfMemoryCreatePreallocated(WDF_OBJECT_ATTRIBUTES *a,void *b,size_t n,WDFMEMORY *m) {
    NTSTATUS s=WdfObjectCreate(a,m);if(NT_SUCCESS(s)) { (*m)->buffer=b;(*m)->size=n; } return s;
}
NTSTATUS WdfRequestReuse(WDFREQUEST r,WDF_REQUEST_REUSE_PARAMS *p) {
    (void)p; CHECK(!r->sent);
    if(fail_reuse) { --fail_reuse;return STATUS_UNSUCCESSFUL; }
    r->completed=0;r->status=0;return 0;
}
NTSTATUS WdfIoTargetFormatRequestForInternalIoctlOthers(WDFIOTARGET t,WDFREQUEST r,ULONG code,
    WDFMEMORY m,void*a,void*b,void*c,void*d,void*e) {
    (void)t;(void)code;(void)a;(void)b;(void)c;(void)d;(void)e;
    if(fail_format) { --fail_format;return STATUS_UNSUCCESSFUL; }
    r->memory=m;return 0;
}
void WdfRequestSetCompletionRoutine(WDFREQUEST r,PFN_WDF_REQUEST_COMPLETION_ROUTINE fn,void *ctx) { r->completion=fn;r->completion_context=ctx; }
static void complete(WDFREQUEST r,NTSTATUS status) {
    WDF_REQUEST_COMPLETION_PARAMS p={{0}};
    CHECK(r && r->sent);r->sent=0;r->status=status;
    /* Real BthPort returned the internal OPEN BRB size through the reused IRP.
     * This lower-stack byte count is not the public CONNECT IOCTL payload. */
    if (((BRB_HEADER *)r->memory->buffer)->type==BRB_L2CA_OPEN_CHANNEL)
        r->information=368;
    p.IoStatus.Status=status;p.IoStatus.Information=r->information;
    r->completion(r,NULL,&p,r->completion_context);
    WdfObjectDereference(r); /* Target-held reference until completion returned. */
}
BOOLEAN WdfRequestSend(WDFREQUEST r,WDFIOTARGET t,void *options) {
    BRB_HEADER *brb;
    (void)t;(void)options;
    if(send_hook) { void(*hook)(WDFREQUEST)=send_hook;send_hook=NULL;hook(r); }
    if(fail_send) { --fail_send;r->status=STATUS_UNSUCCESSFUL;return FALSE; }
    CHECK(!r->completed && !r->sent && r->memory);
    brb=r->memory->buffer;r->sent=1;WdfObjectReference(r);
    if(t->stopped) return TRUE; /* WDF accepts a queued request but does not deliver it. */
    if(brb->type==BRB_L2CA_OPEN_CHANNEL) {
        struct _BRB_L2CA_OPEN_CHANNEL *open=(void*)brb;
        ++open_sends;last_open=r;
        CHECK(open->ReferenceObject!=NULL);
        provider_callback=open->Callback;provider_context=open->CallbackContext;
        if(callback_registration) provider_callback(provider_context,IndicationAddReference,NULL);
        open->OutResults.Params.Mtu=1008;open->InResults.Params.Mtu=1008;open->ChannelHandle=42;
    } else if(brb->type==BRB_L2CA_CLOSE_CHANNEL) { ++close_sends;last_close=r; }
    else { CHECK(brb->type==BRB_L2CA_ACL_TRANSFER);++data_sends;last_data=r; }
    if(inline_complete) complete(r,inline_status);
    return TRUE;
}
BOOLEAN WdfRequestCancelSentRequest(WDFREQUEST r) { CHECK(r && r->refs);++cancel_calls;r->canceled=1;return r->sent?TRUE:FALSE; }
BOOLEAN WdfRequestIsCanceled(WDFREQUEST r) { return (BOOLEAN)r->canceled; }
NTSTATUS WdfRequestGetStatus(WDFREQUEST r) { return r->status; }
void WdfRequestCompleteWithInformation(WDFREQUEST r,NTSTATUS s,size_t n) {
    CHECK(!r->completed);r->completed=1;r->status=s;r->information=n;
    WdfObjectDelete(r);
}
/* Plain Complete supplies status only; it must not silently zero Information. */
void WdfRequestComplete(WDFREQUEST r,NTSTATUS s) { WdfRequestCompleteWithInformation(r,s,r->information); }
WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE q) { return q->device; }
WDFDEVICE WdfFileObjectGetDevice(WDFFILEOBJECT f) { return f->device; }
WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST r) { return r->file; }
PDEVICE_OBJECT WdfDeviceWdmGetDeviceObject(WDFDEVICE d) { return d; }
void ObReferenceObject(PDEVICE_OBJECT d) { WdfObjectReference(d); }
void ObDereferenceObject(PDEVICE_OBJECT d) { WdfObjectDereference(d); }
void *WdfMemoryGetBuffer(WDFMEMORY m,size_t *n) { *n=m->size;return m->buffer; }
NTSTATUS WdfRequestRetrieveInputMemory(WDFREQUEST r,WDFMEMORY *m) { *m=r->memory;return *m?0:STATUS_INVALID_PARAMETER; }
NTSTATUS WdfRequestRetrieveOutputMemory(WDFREQUEST r,WDFMEMORY *m) { return WdfRequestRetrieveInputMemory(r,m); }
NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST r,size_t n,void **b,size_t *s) {
    if(!r->memory||r->memory->size<n)return STATUS_BUFFER_TOO_SMALL;
    *b=r->memory->buffer;if(s)*s=r->memory->size;return 0;
}
NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r,size_t n,void **b,size_t *s) { return WdfRequestRetrieveInputBuffer(r,n,b,s); }
NTSTATUS WdfFdoQueryForInterface(WDFDEVICE d,const int*g,PINTERFACE i,size_t n,unsigned v,void*c) { (void)d;(void)g;(void)i;(void)n;(void)v;(void)c;return 0; }
NTSTATUS WdfIoTargetSendInternalIoctlSynchronously(WDFIOTARGET t,WDFREQUEST r,ULONG c,void*i,WDF_MEMORY_DESCRIPTOR*m,void*a,void*b) { (void)t;(void)r;(void)c;(void)i;(void)a;(void)b;memset(m->buffer,0,m->size);return 0; }
NTSTATUS BthEchoSharedDeviceContextHeaderInit(PBTHECHOSAMPLE_DEVICE_CONTEXT_HEADER h,WDFDEVICE d) { h->Device=d;return 0; }
static void reuse_brb(PBRB b,unsigned type) {
    size_t n=type==BRB_L2CA_OPEN_CHANNEL?sizeof(b->open):type==BRB_L2CA_CLOSE_CHANNEL?sizeof(b->close):sizeof(b->transfer);
    memset(b,0,n);b->Hdr.type=type;
}
typedef struct { WDFDEVICE device;WDFIOTARGET target;WDFFILEOBJECT file;WDFQUEUE queue; } fixture;
static WDFOBJECT typed_object(size_t size) {
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=driver;a.ContextSize=size;
    return new_object(&a);
}
static fixture setup(void) {
    fixture f;
    PBTHECHOSAMPLE_CLIENT_CONTEXT d;
    WDF_OBJECT_ATTRIBUTES a;
    CHECK(live==0);allocations=fail_alloc=fail_reuse=fail_format=fail_send=0;
    close_sends=data_sends=open_sends=cancel_calls=inline_complete=0;
    inline_status=0;callback_registration=1;send_hook=NULL;
    provider_callback=NULL;provider_context=NULL;last_open=last_close=last_data=NULL;
    driver=new_object(NULL);
    f.device=typed_object(sizeof(BTHECHOSAMPLE_CLIENT_CONTEXT));
    f.target=typed_object(0);f.file=typed_object(sizeof(BTHECHOSAMPLE_CLIENT_FILE_CONTEXT));f.queue=typed_object(0);
    f.file->device=f.device;f.queue->device=f.device;
    d=GetClientDeviceContext(f.device);d->Header.Device=f.device;d->Header.IoTarget=f.target;d->Header.ProfileDrvInterface.BthReuseBrb=reuse_brb;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=f.file;
    CHECK(NT_SUCCESS(WdfSpinLockCreate(&a,&GetFileContext(f.file)->Lock)));
    return f;
}
static WDFREQUEST request(fixture f) {
    WDFREQUEST r=typed_object(sizeof(OL_REQUEST_CONTEXT));r->file=f.file;
    WdfObjectReference(r); /* Test observer can inspect status after completion. */
    return r;
}
static WDFREQUEST connect(fixture f) {
    WDFREQUEST r=request(f);
    OlConnectChannel(f.device,r,f.file);
    return r;
}
static void release_provider(void) {
    CHECK(provider_callback!=NULL);
    provider_callback(provider_context,IndicationReleaseReference,NULL);
    provider_callback=NULL;provider_context=NULL;
}
static void finish(fixture f) {
    CHECK(GetClientDeviceContext(f.device)->Connections==0);
    CHECK(GetClientDeviceContext(f.device)->InFlight==0);
    WdfObjectDelete(f.file);WdfObjectDelete(f.queue);WdfObjectDelete(f.target);WdfObjectDelete(f.device);WdfObjectDelete(driver);
    CHECK(live==0);
}
static void removal_regression(void) {
    fixture f=setup();WDFREQUEST r=connect(f);
    INDICATION_PARAMETERS p={0};p.Parameters.Disconnect.Reason=HardwareRemoval;
    complete(last_open,0);CHECK(r->completed && r->status==0);WdfObjectDereference(r);
    provider_callback(provider_context,IndicationRemoteDisconnect,&p);
    CHECK(close_sends==0); /* Red on cd033ca: CLOSE sent after hardware removal. */
    CHECK(p.Parameters.Disconnect.CloseNow==TRUE);
    CHECK(GetClientDeviceContext(f.device)->Removed==1);
    CHECK(GetClientDeviceContext(f.device)->Events[OL_EVENT_CALLBACK_ADD]==1);
    CHECK(GetClientDeviceContext(f.device)->Events[OL_EVENT_REMOTE_DISCONNECT]==1);
    CHECK(GetClientDeviceContext(f.device)->Events[OL_EVENT_REMOVAL]==1);
    OlFileDetach(f.file);
    CHECK(close_sends==0);
    release_provider();finish(f);
}

static void connect_information(void) {
    static const NTSTATUS statuses[]={STATUS_SUCCESS,STATUS_UNSUCCESSFUL,STATUS_CANCELLED};
    unsigned inlined,result;
    for(inlined=0;inlined<2;++inlined) for(result=0;result<3;++result) {
        fixture f=setup();WDFREQUEST r;
        inline_complete=inlined;inline_status=statuses[result];
        r=connect(f);
        if(!inlined)complete(last_open,statuses[result]);
        CHECK(r->completed && r->status==statuses[result]);
        CHECK(r->information==0); /* Red with plain Complete: lower value is 368. */
        inline_complete=0; /* Only OPEN completion timing is varied here. */
        WdfObjectDereference(r);
        OlFileDetach(f.file);
        if(last_close && last_close->sent)complete(last_close,0);
        release_provider();finish(f);
    }
    {
        fixture f=setup();WDFREQUEST r=request(f);
        r->information=368;r->canceled=1;
        OlConnectChannel(f.device,r,f.file);
        CHECK(r->completed && r->status==STATUS_CANCELLED && !open_sends);
        CHECK(r->information==0);
        WdfObjectDereference(r);OlFileDetach(f.file);finish(f);
    }
}

static void close_faults(void) {
    unsigned which,result;
    for(which=0;which<4;++which) for(result=0;result<2;++result) {
        fixture f=setup();WDFREQUEST r=connect(f);
        complete(last_open,0);WdfObjectDereference(r);
        if(which==0)fail_reuse=2;
        if(which==1)fail_format=2;
        if(which==2)fail_send=2;
        OlFileDetach(f.file);
        CHECK(GetClientDeviceContext(f.device)->Connections==1);
        CHECK(GetClientDeviceContext(f.device)->RetainedConnections==1);
        if(which==3) {
            CHECK(close_sends==1);
            if(result) { /* Provider release BEFORE close completion must not delete request. */
                release_provider();
                CHECK(GetClientDeviceContext(f.device)->Connections==1);
                complete(last_close,STATUS_UNSUCCESSFUL);
            } else {
                complete(last_close,0);
                CHECK(GetClientDeviceContext(f.device)->Connections==1);
                release_provider();
            }
        } else {
            PBTHECHO_CONNECTION c=provider_context;
            CHECK(close_sends==0 && c->Life.attempts==2 && !c->Life.closing);
            OlConnectionClose(c);CHECK(c->Life.attempts==2);
            release_provider();
        }
        finish(f);
    }
}
static void allocation_faults(void) {
    unsigned at;
    for(at=1;at<=5;++at) {
        fixture f=setup();WDFREQUEST r=request(f);
        r->information=368;
        fail_alloc=allocations+at;
        OlConnectChannel(f.device,r,f.file);
        CHECK(r->completed && !NT_SUCCESS(r->status));
        CHECK(r->information==0);
        CHECK(open_sends==0 && provider_callback==NULL);
        WdfObjectDereference(r);
        OlFileDetach(f.file);finish(f);
    }
    for(at=0;at<2;++at) {
        fixture f=setup();WDFREQUEST r=request(f);
        r->information=368;
        if(at)fail_format=1;else fail_send=1;
        OlConnectChannel(f.device,r,f.file);
        CHECK(r->completed && !NT_SUCCESS(r->status));
        CHECK(r->information==0);
        CHECK(open_sends==0);WdfObjectDereference(r);
        OlFileDetach(f.file);finish(f);
    }
}
static fixture hook_fixture;
static void cleanup_in_send(WDFREQUEST r) { (void)r;OlFileDetach(hook_fixture.file); }
static void removal_in_send(WDFREQUEST r) { (void)r;OlEvtSurpriseRemoval(hook_fixture.device); }
static void open_races(void) {
    unsigned before_release,success;
    for(before_release=0;before_release<2;++before_release)
    for(success=0;success<2;++success) {
        fixture f=setup();WDFREQUEST r;
        hook_fixture=f;send_hook=cleanup_in_send;r=connect(f);
        CHECK(!r->completed && cancel_calls>=2); /* Cancel(false), then post-Send cancel(true). */
        if(before_release)release_provider();
        complete(last_open,success?0:STATUS_CANCELLED);
        CHECK(r->completed && r->status==STATUS_CANCELLED);
        CHECK(r->information==0); /* Also when successful OPEN races file cleanup. */
        WdfObjectDereference(r);
        if(last_close)complete(last_close,0);
        if(!before_release)release_provider();
        finish(f);
    }
    {
        fixture f=setup();WDFREQUEST r;
        inline_complete=1;r=connect(f);
        CHECK(r->completed && r->status==0);WdfObjectDereference(r);
        OlFileDetach(f.file);
        CHECK(close_sends==1);release_provider();finish(f);
    }
    {
        fixture f=setup();WDFREQUEST r=request(f);
        r->information=368;
        OlEvtSurpriseRemoval(f.device);OlConnectChannel(f.device,r,f.file);
        CHECK(r->completed && r->status==STATUS_DEVICE_NOT_CONNECTED && !open_sends);
        CHECK(r->information==0);
        WdfObjectDereference(r);OlFileDetach(f.file);finish(f);
    }
}
static WDFREQUEST transfer(fixture f,unsigned char *buffer) {
    WDFREQUEST r=request(f);WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=r;
    CHECK(NT_SUCCESS(WdfMemoryCreatePreallocated(&a,buffer,1008,&r->memory)));
    return r;
}
static void data_races(void) {
    unsigned mode;
    for(mode=0;mode<7;++mode) {
        fixture f=setup();WDFREQUEST r=connect(f),io;unsigned char buffer[1008]={0};
        complete(last_open,0);WdfObjectDereference(r);
        io=transfer(f,buffer);
        if(mode==0)inline_complete=1;
        if(mode==1) { hook_fixture=f;send_hook=cleanup_in_send; }
        if(mode==2)fail_format=1;
        if(mode==3)fail_send=1;
        if(mode==4)io->canceled=1;
        if(mode==5) { hook_fixture=f;send_hook=removal_in_send; }
        BthEchoCliEvtQueueIoWrite(f.queue,io,1008);
        if(mode==5) CHECK(data_sends==0 && f.target->stopped && cancel_calls>0);
        if(io->sent) {
            if(mode==6) BthEchoCliEvtQueueIoStop(f.queue,io,0);
            if(mode==1)CHECK(cancel_calls>=2);
            CHECK(GetClientDeviceContext(f.device)->InFlight==1);
            CHECK(!io->completed); /* Asking to cancel never completes/releases it. */
            OlFileDetach(f.file);
            release_provider(); /* Late I/O still protects root and buffer. */
            CHECK(GetClientDeviceContext(f.device)->Connections==1);
            complete(io,STATUS_CANCELLED);
            if(last_close && last_close->sent)complete(last_close,0);
        } else {
            CHECK(io->completed && GetClientDeviceContext(f.device)->InFlight==0);
            OlFileDetach(f.file);
            if(last_close && last_close->sent)complete(last_close,0);
            release_provider();
        }
        CHECK(io->completed && GetOlRequestContext(io)->TransferActive==0);
        WdfObjectDereference(io);finish(f);
    }
}
static void disconnect_reasons(void) {
    unsigned reason;
    for(reason=HciDisconnect;reason<=HardwareRemoval;++reason) {
        fixture f=setup();WDFREQUEST r=connect(f);INDICATION_PARAMETERS p={0};
        p.Parameters.Disconnect.Reason=(L2CAP_DISCONNECT_REASON)reason;
        complete(last_open,0);WdfObjectDereference(r);
        provider_callback(provider_context,IndicationRemoteDisconnect,&p);
        CHECK(p.Parameters.Disconnect.CloseNow && !close_sends);
        CHECK(!ol_life_can_io(&((PBTHECHO_CONNECTION)provider_context)->Life));
        OlFileDetach(f.file);CHECK(!close_sends);release_provider();finish(f);
    }
}
static void diagnostics(void) {
    fixture f=setup();ol_native_request input={sizeof(input),OL_NATIVE_ABI};
    ol_native_diagnostics output;
    WDFREQUEST r=request(f);WDF_OBJECT_ATTRIBUTES a;WDFMEMORY m;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=r;
    /* METHOD_BUFFERED input and output share the same system buffer. */
    memset(&output,0,sizeof(output));memcpy(&output,&input,sizeof(input));
    CHECK(NT_SUCCESS(WdfMemoryCreatePreallocated(&a,&output,sizeof(output),&m)));r->memory=m;
    OlEvtDeviceIoControl(f.queue,r,sizeof(output),sizeof(input),OL_IOCTL_DIAGNOSTICS);
    CHECK(r->completed && r->status==0 && r->information==128);
    CHECK(output.size==128 && output.version==OL_DIAGNOSTICS_ABI && output.count==OL_EVENT_COUNT);
    CHECK(!open_sends && !close_sends && !data_sends);WdfObjectDereference(r);
    OlFileDetach(f.file);finish(f);
}
static void retained_capacity(void) {
    fixture f=setup();PVOID contexts[OL_MAX_CHANNELS];
    PFNBTHPORT_INDICATION_CALLBACK callback=NULL;unsigned i;
    for(i=0;i<OL_MAX_CHANNELS;++i) {
        WDFREQUEST r=connect(f);WDF_OBJECT_ATTRIBUTES a;
        complete(last_open,0);WdfObjectDereference(r);
        contexts[i]=provider_context;callback=provider_callback;
        fail_send=2;OlFileDetach(f.file);
        CHECK(GetClientDeviceContext(f.device)->Connections==(LONG)i+1);
        WdfObjectDelete(f.file); /* Root must outlive its actual file object. */
        f.file=typed_object(sizeof(BTHECHOSAMPLE_CLIENT_FILE_CONTEXT));f.file->device=f.device;
        WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=f.file;
        CHECK(NT_SUCCESS(WdfSpinLockCreate(&a,&GetFileContext(f.file)->Lock)));
    }
    {
        WDFREQUEST r=connect(f);
        CHECK(r->completed && r->status==STATUS_DEVICE_BUSY);
        CHECK(open_sends==OL_MAX_CHANNELS);WdfObjectDereference(r);
    }
    for(i=0;i<OL_MAX_CHANNELS;++i)callback(contexts[i],IndicationReleaseReference,NULL);
    provider_callback=NULL;provider_context=NULL;OlFileDetach(f.file);finish(f);
}
static void transfer_capacity(void) {
    fixture f=setup();WDFREQUEST r=connect(f),io[OL_MAX_INFLIGHT+1];unsigned i;
    unsigned char buffer[1008]={0};
    complete(last_open,0);WdfObjectDereference(r);
    for(i=0;i<=OL_MAX_INFLIGHT;++i) {
        io[i]=transfer(f,buffer);BthEchoCliEvtQueueIoWrite(f.queue,io[i],1008);
    }
    CHECK(GetClientDeviceContext(f.device)->InFlight==OL_MAX_INFLIGHT);
    CHECK(io[OL_MAX_INFLIGHT]->completed && io[OL_MAX_INFLIGHT]->status==STATUS_DEVICE_BUSY);
    WdfObjectDereference(io[OL_MAX_INFLIGHT]);
    fail_format=2;OlFileDetach(f.file);CHECK(cancel_calls>=OL_MAX_INFLIGHT);
    release_provider();
    for(i=0;i<OL_MAX_INFLIGHT;++i) {
        CHECK(GetClientDeviceContext(f.device)->Connections==1);
        complete(io[i],STATUS_CANCELLED);WdfObjectDereference(io[i]);
    }
    finish(f);
}
int main(void) {
    connect_information();
    removal_regression();close_faults();allocation_faults();open_races();data_races();disconnect_reasons();diagnostics();
    retained_capacity();transfer_capacity();
    printf("PASS: %u actual-adapter host checks; NOT kernel/WDF/Verifier acceptance.\n",checks);
    return 0;
}
