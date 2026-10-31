/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OL_NATIVE_TEST_PLATFORM_H
#define OL_NATIVE_TEST_PLATFORM_H
/* Host-only WDF boundary shim. This is NOT a Windows kernel or Verifier.
 * Production context definitions and transport function bodies remain shared. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define _In_
#define _Inout_
#define _Out_
#define _In_opt_
#define _Use_decl_annotations_
#define _IRQL_requires_max_(x)
#define _IRQL_requires_same_
#define FORCEINLINE inline
#define VOID void
#define TRUE 1
#define FALSE 0
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define NT_ASSERT(x) mock_assert(!!(x), #x, __LINE__)
#define C_ASSERT(x) _Static_assert(x, #x)
#define RtlZeroMemory(p,n) memset((p),0,(n))
typedef int32_t NTSTATUS, LONG;
typedef int64_t LONG64;
typedef uint32_t ULONG;
typedef uint16_t USHORT;
typedef uint64_t BTH_ADDR;
typedef unsigned char BOOLEAN;
typedef void *PVOID;
typedef struct mock_object *WDFOBJECT;
typedef WDFOBJECT WDFDEVICE, WDFIOTARGET, WDFREQUEST, WDFSPINLOCK, WDFMEMORY;
typedef WDFOBJECT WDFFILEOBJECT, WDFQUEUE, WDFDRIVER, PDEVICE_OBJECT;
typedef void *WDFCONTEXT, *PWDFDEVICE_INIT;
typedef uintptr_t L2CAP_CHANNEL_HANDLE;
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)-1)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)-2)
#define STATUS_DEVICE_BUSY ((NTSTATUS)-3)
#define STATUS_CANCELLED ((NTSTATUS)-4)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)-5)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)-6)
#define STATUS_TOO_MANY_OPENED_FILES ((NTSTATUS)-7)
#define STATUS_REVISION_MISMATCH ((NTSTATUS)-8)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)-9)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)-10)
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)-11)
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)-12)
#define STATUS_CONNECTION_DISCONNECTED ((NTSTATUS)-13)
#define STATUS_DEVICE_NOT_CONNECTED ((NTSTATUS)-14)
#define NT_SUCCESS(x) ((x)>=0)
typedef struct { struct { NTSTATUS Status; } IoStatus; } WDF_REQUEST_COMPLETION_PARAMS;
typedef WDF_REQUEST_COMPLETION_PARAMS *PWDF_REQUEST_COMPLETION_PARAMS;
typedef void EVT_WDF_REQUEST_COMPLETION_ROUTINE(WDFREQUEST,WDFIOTARGET,PWDF_REQUEST_COMPLETION_PARAMS,WDFCONTEXT);
typedef EVT_WDF_REQUEST_COMPLETION_ROUTINE *PFN_WDF_REQUEST_COMPLETION_ROUTINE;
typedef void EVT_WDF_OBJECT_CONTEXT_CLEANUP(WDFOBJECT);
typedef void EVT_WDF_OBJECT_CONTEXT_DESTROY(WDFOBJECT);
typedef NTSTATUS EVT_WDF_DRIVER_DEVICE_ADD(WDFDRIVER,PWDFDEVICE_INIT);
typedef NTSTATUS EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT(WDFDEVICE);
typedef void EVT_WDF_DEVICE_FILE_CREATE(WDFDEVICE,WDFREQUEST,WDFFILEOBJECT);
typedef void EVT_WDF_FILE_CLOSE(WDFFILEOBJECT);
typedef void EVT_WDF_FILE_CLEANUP(WDFFILEOBJECT);
typedef void EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL(WDFQUEUE,WDFREQUEST,size_t,size_t,ULONG);
typedef void EVT_WDF_IO_QUEUE_IO_READ(WDFQUEUE,WDFREQUEST,size_t);
typedef EVT_WDF_IO_QUEUE_IO_READ EVT_WDF_IO_QUEUE_IO_WRITE;
typedef void EVT_WDF_IO_QUEUE_IO_STOP(WDFQUEUE,WDFREQUEST,ULONG);
typedef void EVT_WDF_DEVICE_SURPRISE_REMOVAL(WDFDEVICE);
typedef enum { IndicationAddReference,IndicationReleaseReference,IndicationRemoteConnect,IndicationRemoteDisconnect } INDICATION_CODE;
typedef enum { HciDisconnect,L2capDisconnectRequest,RadioPoweredDown,HardwareRemoval } L2CAP_DISCONNECT_REASON;
typedef struct { union { struct { L2CAP_DISCONNECT_REASON Reason; BOOLEAN CloseNow; } Disconnect; } Parameters; } INDICATION_PARAMETERS, *PINDICATION_PARAMETERS;
typedef void (*PFNBTHPORT_INDICATION_CALLBACK)(PVOID,INDICATION_CODE,PINDICATION_PARAMETERS);
typedef struct { void *ClientContext[4]; unsigned type; } BRB_HEADER;
typedef struct { USHORT Min,Max,Preferred; } MOCK_MTU;
typedef struct { ULONG Flags; MOCK_MTU Mtu; } MOCK_CONFIG;
typedef struct { struct { USHORT Mtu; } Params; } MOCK_RESULTS;
struct _BRB_L2CA_OPEN_CHANNEL {
    BRB_HEADER Hdr; BTH_ADDR BtAddress; USHORT Psm; ULONG ChannelFlags;
    MOCK_CONFIG ConfigOut,ConfigIn; unsigned IncomingQueueDepth,CallbackFlags;
    PFNBTHPORT_INDICATION_CALLBACK Callback; void *CallbackContext,*ReferenceObject;
    MOCK_RESULTS OutResults,InResults; L2CAP_CHANNEL_HANDLE ChannelHandle;
};
struct _BRB_L2CA_CLOSE_CHANNEL { BRB_HEADER Hdr; BTH_ADDR BtAddress; L2CAP_CHANNEL_HANDLE ChannelHandle; };
struct _BRB_L2CA_ACL_TRANSFER {
    BRB_HEADER Hdr; BTH_ADDR BtAddress; L2CAP_CHANNEL_HANDLE ChannelHandle;
    void *Buffer,*BufferMDL; ULONG BufferSize,TransferFlags;
};
typedef union { BRB_HEADER Hdr; struct _BRB_L2CA_OPEN_CHANNEL open; struct _BRB_L2CA_CLOSE_CHANNEL close; struct _BRB_L2CA_ACL_TRANSFER transfer; } BRB,*PBRB;
typedef struct { void *Context; void (*InterfaceDereference)(void*); } INTERFACE,*PINTERFACE;
typedef struct { INTERFACE Interface; void (*BthReuseBrb)(PBRB,unsigned); } BTH_PROFILE_DRIVER_INTERFACE;
typedef struct { uint64_t Mask; } BTH_HOST_FEATURE_MASK;
typedef struct { BTH_ADDR address; } BTH_DEVICE_INFO;
typedef struct { void *buffer; size_t size; } WDF_MEMORY_DESCRIPTOR;
typedef struct { WDFOBJECT ParentObject; size_t ContextSize; unsigned ExecutionLevel;
    EVT_WDF_OBJECT_CONTEXT_CLEANUP *EvtCleanupCallback;
    EVT_WDF_OBJECT_CONTEXT_DESTROY *EvtDestroyCallback; } WDF_OBJECT_ATTRIBUTES;
typedef struct { int unused; } WDF_REQUEST_REUSE_PARAMS;
#define WDF_OBJECT_ATTRIBUTES_INIT(a) memset((a),0,sizeof(*(a)))
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a,T) do { WDF_OBJECT_ATTRIBUTES_INIT(a); (a)->ContextSize=sizeof(T); } while(0)
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(T,F) static inline T *F(WDFOBJECT o) { return (T*)mock_context(o,sizeof(T)); }
#define WDF_REQUEST_REUSE_PARAMS_INIT(p,f,s) ((void)(p),(void)(f),(void)(s))
#define WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(m,b,n) do { (m)->buffer=(b); (m)->size=(n); } while(0)
#define WDF_REQUEST_REUSE_NO_FLAGS 0
#define WdfExecutionLevelPassive 1
#define WdfIoTargetLeaveSentIoPending 1
#define BRB_L2CA_OPEN_CHANNEL 1
#define BRB_L2CA_CLOSE_CHANNEL 2
#define BRB_L2CA_ACL_TRANSFER 3
#define CF_ROLE_EITHER 1
#define CF_LINK_ENCRYPTED 2
#define CFG_MTU 1
#define L2CAP_MIN_MTU 48
#define CALLBACK_DISCONNECT 1
#define ACL_TRANSFER_DIRECTION_OUT 0
#define ACL_TRANSFER_DIRECTION_IN 1
#define ACL_SHORT_TRANSFER_OK 2
#define IOCTL_INTERNAL_BTH_SUBMIT_BRB 1
#define IOCTL_INTERNAL_BTHENUM_GET_DEVINFO 2
#define BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI 1
static const int GUID_BTHDDI_PROFILE_DRIVER_INTERFACE=0;
void mock_assert(int,const char*,int);
void *mock_context(WDFOBJECT,size_t);
WDFDRIVER WdfGetDriver(void);
void WdfObjectReference(WDFOBJECT);
void WdfObjectDereference(WDFOBJECT);
void WdfObjectDelete(WDFOBJECT);
void WdfIoTargetStop(WDFIOTARGET,unsigned);
NTSTATUS WdfObjectCreate(WDF_OBJECT_ATTRIBUTES*,WDFOBJECT*);
NTSTATUS WdfSpinLockCreate(WDF_OBJECT_ATTRIBUTES*,WDFSPINLOCK*);
void WdfSpinLockAcquire(WDFSPINLOCK);
void WdfSpinLockRelease(WDFSPINLOCK);
NTSTATUS WdfRequestCreate(WDF_OBJECT_ATTRIBUTES*,WDFIOTARGET,WDFREQUEST*);
NTSTATUS WdfMemoryCreatePreallocated(WDF_OBJECT_ATTRIBUTES*,void*,size_t,WDFMEMORY*);
NTSTATUS WdfRequestReuse(WDFREQUEST,WDF_REQUEST_REUSE_PARAMS*);
NTSTATUS WdfIoTargetFormatRequestForInternalIoctlOthers(WDFIOTARGET,WDFREQUEST,ULONG,WDFMEMORY,void*,void*,void*,void*,void*);
void WdfRequestSetCompletionRoutine(WDFREQUEST,PFN_WDF_REQUEST_COMPLETION_ROUTINE,void*);
BOOLEAN WdfRequestSend(WDFREQUEST,WDFIOTARGET,void*);
BOOLEAN WdfRequestCancelSentRequest(WDFREQUEST);
BOOLEAN WdfRequestIsCanceled(WDFREQUEST);
NTSTATUS WdfRequestGetStatus(WDFREQUEST);
void WdfRequestComplete(WDFREQUEST,NTSTATUS);
void WdfRequestCompleteWithInformation(WDFREQUEST,NTSTATUS,size_t);
WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE);
WDFDEVICE WdfFileObjectGetDevice(WDFFILEOBJECT);
WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST);
PDEVICE_OBJECT WdfDeviceWdmGetDeviceObject(WDFDEVICE);
void ObReferenceObject(PDEVICE_OBJECT);
void ObDereferenceObject(PDEVICE_OBJECT);
void *WdfMemoryGetBuffer(WDFMEMORY,size_t*);
NTSTATUS WdfRequestRetrieveInputMemory(WDFREQUEST,WDFMEMORY*);
NTSTATUS WdfRequestRetrieveOutputMemory(WDFREQUEST,WDFMEMORY*);
NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST,size_t,void**,size_t*);
NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST,size_t,void**,size_t*);
NTSTATUS WdfFdoQueryForInterface(WDFDEVICE,const int*,PINTERFACE,size_t,unsigned,void*);
NTSTATUS WdfIoTargetSendInternalIoctlSynchronously(WDFIOTARGET,WDFREQUEST,ULONG,void*,WDF_MEMORY_DESCRIPTOR*,void*,void*);
static inline LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static inline LONG InterlockedDecrement(volatile LONG *p) { return --*p; }
static inline LONG InterlockedExchange(volatile LONG *p,LONG v) { LONG o=*p;*p=v;return o; }
static inline LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG c) { LONG o=*p;if(o==c)*p=v;return o; }
static inline LONG64 InterlockedCompareExchange64(volatile LONG64*p,LONG64 v,LONG64 c) { LONG64 o=*p;if(o==c)*p=v;return o; }
static inline LONG64 InterlockedExchangeAdd64(volatile LONG64*p,LONG64 v) { LONG64 o=*p;*p+=v;return o; }
static inline LONG64 InterlockedIncrement64(volatile LONG64*p) { return ++*p; }
#endif
