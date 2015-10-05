/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms,
 * with or without modification, are permitted provided
 * that the following conditions are met:
 *
 * *   Redistributions of source code must retain the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the
 *     following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "driver.h"
#include "ioctls.h"
#include "..\..\include\xeniface_ioctls.h"
#include "log.h"
#include "irp_queue.h"

#define XENSTORE_ABS_PATH_MAX 3072
#define XENSTORE_REL_PATH_MAX 2048

static
NTSTATUS
__CaptureUserBuffer(
    __in  PVOID Buffer,
    __in  ULONG Length,
    __out PVOID *CapturedBuffer
    )
{
    NTSTATUS Status;
    PVOID TempBuffer = NULL;

    if (Length == 0) {
        *CapturedBuffer = NULL;
        return STATUS_SUCCESS;
    }

    Status = STATUS_NO_MEMORY;
    TempBuffer = ExAllocatePoolWithTag(NonPagedPool, Length, XENIFACE_POOL_TAG);
    if (TempBuffer == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = STATUS_SUCCESS;

#pragma prefast(suppress: 6320) // we want to catch all exceptions
    try {
        ProbeForRead(Buffer, Length, 1);
        RtlCopyMemory(TempBuffer, Buffer, Length);
    } except(EXCEPTION_EXECUTE_HANDLER) {
        XenIfaceDebugPrint(ERROR, "Exception while probing/reading buffer at %p, size 0x%lx\n", Buffer, Length);
        ExFreePoolWithTag(TempBuffer, XENIFACE_POOL_TAG);
        TempBuffer = NULL;
        Status = GetExceptionCode();
    }

    *CapturedBuffer = TempBuffer;

    return Status;
 }

static
VOID
__FreeCapturedBuffer(
    __in  PVOID CapturedBuffer
    )
{
    if (CapturedBuffer != NULL) {
        ExFreePoolWithTag(CapturedBuffer, XENIFACE_POOL_TAG);
    }
}

static FORCEINLINE
BOOLEAN
__IsValidStr(
    __in  PCHAR             Str,
    __in  ULONG             Len
    )
{
    for ( ; Len--; ++Str) {
        if (*Str == '\0')
            return TRUE;
        if (!isprint((unsigned char)*Str))
            break;
    }
    return FALSE;
}

static FORCEINLINE
ULONG
__MultiSzLen(
    __in  PCHAR             Str,
    __out PULONG            Count
    )
{
    ULONG Length = 0;
    if (Count)  *Count = 0;
    do {
        for ( ; *Str; ++Str, ++Length) ;
        ++Str; ++Length;
        if (*Count) ++(*Count);
    } while (*Str);
    return Length;
}

static FORCEINLINE
VOID
__DisplayMultiSz(
    __in PCHAR              Caller,
    __in PCHAR              Str
    )
{
    PCHAR   Ptr;
    ULONG   Idx;
    ULONG   Len;

    for (Ptr = Str, Idx = 0; *Ptr; ++Idx) {
        Len = (ULONG)strlen(Ptr);
        XenIfaceDebugPrint(TRACE, "|%s: [%d]=(%d)->\"%s\"\n", Caller, Idx, Len, Ptr);
        Ptr += (Len + 1);
    }
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreRead(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS    status;
    PCHAR       Value;
    ULONG       Length;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen == 0)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if (!__IsValidStr(Buffer, InLen))
        goto fail2;

    status = XENBUS_STORE(Read, &Fdo->StoreInterface, NULL, NULL, Buffer, &Value);
    if (!NT_SUCCESS(status))
        goto fail3;

    Length = (ULONG)strlen(Value) + 1;

    status = STATUS_BUFFER_OVERFLOW;
    if (OutLen == 0) {
        XenIfaceDebugPrint(TRACE, "(\"%s\")=(%d)\n", Buffer, Length);
        goto done;
    }

    status = STATUS_INVALID_PARAMETER;
    if (OutLen < Length)
        goto fail4;

    XenIfaceDebugPrint(TRACE, "(\"%s\")=(%d)->\"%s\"\n", Buffer, Length, Value);

    RtlCopyMemory(Buffer, Value, Length);
    Buffer[Length - 1] = 0;
    status = STATUS_SUCCESS;

done:
    *Info = (ULONG_PTR)Length;
    XENBUS_STORE(Free, &Fdo->StoreInterface, Value);
    return status;

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4 (\"%s\")=(%d < %d)\n", Buffer, OutLen, Length);
    XENBUS_STORE(Free, &Fdo->StoreInterface, Value);
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3 (\"%s\")\n", Buffer);
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreWrite(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS    status;
    PCHAR       Value;
    ULONG       Length;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen == 0 || OutLen != 0)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if (!__IsValidStr(Buffer, InLen))
        goto fail2;

    Length = (ULONG)strlen(Buffer) + 1;
    Value = Buffer + Length;

    if (!__IsValidStr(Value, InLen - Length))
        goto fail3;

    status = XENBUS_STORE(Printf, &Fdo->StoreInterface, NULL, NULL, Buffer, Value);
    if (!NT_SUCCESS(status))
        goto fail4;

    XenIfaceDebugPrint(TRACE, "(\"%s\"=\"%s\")\n", Buffer, Value);
    return status;

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4 (\"%s\")\n", Value);
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3 (\"%s\")\n", Buffer);
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreDirectory(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS    status;
    PCHAR       Value;
    ULONG       Length;
    ULONG       Count;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen == 0)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if (!__IsValidStr(Buffer, InLen))
        goto fail2;

    status = XENBUS_STORE(Directory, &Fdo->StoreInterface, NULL, NULL, Buffer, &Value);
    if (!NT_SUCCESS(status))
        goto fail3;

    Length = __MultiSzLen(Value, &Count) + 1;

    status = STATUS_BUFFER_OVERFLOW;
    if (OutLen == 0) {
        XenIfaceDebugPrint(TRACE, "(\"%s\")=(%d)(%d)\n", Buffer, Length, Count);
        goto done;
    }

    status = STATUS_INVALID_PARAMETER;
    if (OutLen < Length)
        goto fail4;

    XenIfaceDebugPrint(INFO, "(\"%s\")=(%d)(%d)\n", Buffer, Length, Count);
#if DBG
    __DisplayMultiSz(__FUNCTION__, Value);
#endif

    RtlCopyMemory(Buffer, Value, Length);
    Buffer[Length - 2] = 0;
    Buffer[Length - 1] = 0;
    status = STATUS_SUCCESS;

done:
    *Info = (ULONG_PTR)Length;
    XENBUS_STORE(Free, &Fdo->StoreInterface, Value);
    return status;

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4 (\"%s\")=(%d < %d)\n", Buffer, OutLen, Length);
    XENBUS_STORE(Free, &Fdo->StoreInterface, Value);
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3 (\"%s\")\n", Buffer);
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreRemove(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS    status;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen == 0 || OutLen != 0)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if (!__IsValidStr(Buffer, InLen))
        goto fail2;

    status = XENBUS_STORE(Remove, &Fdo->StoreInterface, NULL, NULL, Buffer);
    if (!NT_SUCCESS(status))
        goto fail3;

    XenIfaceDebugPrint(TRACE, "(\"%s\")\n", Buffer);
    return status;

fail3:
    XenIfaceDebugPrint(ERROR, "Fail3 (\"%s\")\n", Buffer);
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreSetPermissions(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS status;
    PXENIFACE_STORE_SET_PERMISSIONS_IN In = (PXENIFACE_STORE_SET_PERMISSIONS_IN)Buffer;
    ULONG Index;
    PCHAR Path;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen < sizeof(XENIFACE_STORE_SET_PERMISSIONS_IN) || OutLen != 0)
        goto fail1;

    if (InLen < sizeof(XENIFACE_STORE_SET_PERMISSIONS_IN) + In->NumberPermissions * sizeof(XENBUS_STORE_PERMISSION))
        goto fail2;

    status = STATUS_INVALID_PARAMETER;
    if (In->PathLength == 0 || In->PathLength > XENSTORE_ABS_PATH_MAX)
        goto fail3;

    status = __CaptureUserBuffer(In->Path, In->PathLength, &Path);
    if (!NT_SUCCESS(status))
        goto fail4;

    Path[In->PathLength - 1] = 0;
    XenIfaceDebugPrint(TRACE, "> Path '%s', NumberPermissions %lu\n", Path, In->NumberPermissions);

    for (Index = 0; Index < In->NumberPermissions; Index++) {
        XenIfaceDebugPrint(TRACE, "> %lu: Domain %d, Mask 0x%x\n", Index, In->Permissions[Index].Domain, In->Permissions[Index].Mask);
        if ((In->Permissions[Index].Mask & ~XENIFACE_STORE_ALLOWED_PERMISSIONS) != 0)
            goto fail5;
    }

    status = XENBUS_STORE(PermissionsSet,
                          &Fdo->StoreInterface,
                          NULL, // transaction
                          NULL, // prefix
                          Path,
                          In->Permissions,
                          In->NumberPermissions);

    if (!NT_SUCCESS(status))
        goto fail6;

    __FreeCapturedBuffer(Path);
    return status;

fail6:
    XenIfaceDebugPrint(ERROR, "Fail6\n");
fail5:
    XenIfaceDebugPrint(ERROR, "Fail5\n");
    __FreeCapturedBuffer(Path);
fail4:
    XenIfaceDebugPrint(ERROR, "Fail4\n");
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreAddWatch(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_STORE_ADD_WATCH_IN In = (PXENIFACE_STORE_ADD_WATCH_IN)Buffer;
    PXENIFACE_STORE_ADD_WATCH_OUT Out = (PXENIFACE_STORE_ADD_WATCH_OUT)Buffer;
    PCHAR Path;
    PXENIFACE_STORE_CONTEXT Context;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_STORE_ADD_WATCH_IN) || OutLen != sizeof(XENIFACE_STORE_ADD_WATCH_OUT))
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if (In->PathLength == 0 || In->PathLength > XENSTORE_ABS_PATH_MAX)
        goto fail2;

    status = __CaptureUserBuffer(In->Path, In->PathLength, &Path);
    if (!NT_SUCCESS(status))
        goto fail3;

    Path[In->PathLength - 1] = 0;

    status = STATUS_NO_MEMORY;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(XENIFACE_STORE_CONTEXT), XENIFACE_POOL_TAG);
    if (Context == NULL)
        goto fail4;

    RtlZeroMemory(Context, sizeof(XENIFACE_STORE_CONTEXT));

    Context->FileObject = FileObject;

    status = ObReferenceObjectByHandle(In->Event, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode, &Context->Event, NULL);
    if (!NT_SUCCESS(status))
        goto fail5;

    XenIfaceDebugPrint(TRACE, "> Path '%s', Event %p, FO %p\n", Path, In->Event, FileObject);

    status = XENBUS_STORE(WatchAdd,
                          &Fdo->StoreInterface,
                          NULL, // prefix
                          Path,
                          Context->Event,
                          &Context->Watch);

    if (!NT_SUCCESS(status))
        goto fail6;

    __FreeCapturedBuffer(Path);

    ExInterlockedInsertTailList(&Fdo->StoreWatchList, &Context->Entry, &Fdo->StoreWatchLock);

    XenIfaceDebugPrint(TRACE, "< Context %p, Watch %p\n", Context, Context->Watch);

    Out->Context = Context;
    *Info = sizeof(XENIFACE_STORE_ADD_WATCH_OUT);

    return status;

fail6:
    XenIfaceDebugPrint(ERROR, "Fail6\n");
    ObDereferenceObject(Context->Event);
fail5:
    XenIfaceDebugPrint(ERROR, "Fail5\n");
    RtlZeroMemory(Context, sizeof(XENIFACE_STORE_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
fail4:
    XenIfaceDebugPrint(ERROR, "Fail4\n");
    __FreeCapturedBuffer(Path);
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static
VOID
StoreFreeWatch(
    __in  PXENIFACE_FDO Fdo,
    __in  PXENIFACE_STORE_CONTEXT Context
    )
{
    NTSTATUS status;

    XenIfaceDebugPrint(TRACE, "Context %p, Watch %p, FO %p\n",
                       Context, Context->Watch, Context->FileObject);

    status = XENBUS_STORE(WatchRemove,
                          &Fdo->StoreInterface,
                          Context->Watch);

    ASSERT(NT_SUCCESS(status)); // this is fatal since we'd leave an active watch without cleaning it up

    ObDereferenceObject(Context->Event);
    RtlZeroMemory(Context, sizeof(XENIFACE_STORE_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlStoreRemoveWatch(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject
    )
{
    NTSTATUS status;
    PXENIFACE_STORE_REMOVE_WATCH_IN In = (PXENIFACE_STORE_REMOVE_WATCH_IN)Buffer;
    PXENIFACE_STORE_CONTEXT Context = NULL;
    KIRQL Irql;
    PLIST_ENTRY Node;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_STORE_REMOVE_WATCH_IN) || OutLen != 0)
        goto fail1;

    XenIfaceDebugPrint(TRACE, "> Context %p, FO %p\n", In->Context, FileObject);

    KeAcquireSpinLock(&Fdo->StoreWatchLock, &Irql);
    Node = Fdo->StoreWatchList.Flink;
    while (Node->Flink != Fdo->StoreWatchList.Flink) {
        Context = CONTAINING_RECORD(Node, XENIFACE_STORE_CONTEXT, Entry);

        Node = Node->Flink;
        if (Context != In->Context || Context->FileObject != FileObject)
            continue;

        RemoveEntryList(&Context->Entry);
        break;
    }
    KeReleaseSpinLock(&Fdo->StoreWatchLock, Irql);

    status = STATUS_NOT_FOUND;
    if (Context == NULL || Context != In->Context)
        goto fail2;

    StoreFreeWatch(Fdo, Context);

    return STATUS_SUCCESS;

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

_Function_class_(KDEFERRED_ROUTINE)
_IRQL_requires_(DISPATCH_LEVEL)
_IRQL_requires_same_
VOID
EvtchnNotificationDpc(
    __in     PKDPC Dpc,
    __in_opt PVOID _Context,
    __in_opt PVOID Argument1,
    __in_opt PVOID Argument2
    )
{
    PXENIFACE_EVTCHN_CONTEXT Context = (PXENIFACE_EVTCHN_CONTEXT)Argument1;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(_Context);
    UNREFERENCED_PARAMETER(Argument2);

    ASSERT(Context);

#if DBG
    XenIfaceDebugPrint(INFO, "Channel %p, LocalPort %d, Active %d, Cpu %lu\n",
                       Context->Channel, Context->LocalPort, Context->Active, KeGetCurrentProcessorNumber());
#endif
    if (Context->Active) {
        KeSetEvent(Context->Event, 0, FALSE);

        XENBUS_EVTCHN(Unmask,
                      &Context->Fdo->EvtchnInterface,
                      Context->Channel,
                      FALSE);
    }
}

_Function_class_(KSERVICE_ROUTINE)
_IRQL_requires_(HIGH_LEVEL)
_IRQL_requires_same_
static DECLSPEC_NOINLINE
BOOLEAN
EvtchnInterruptHandler(
    __in     PKINTERRUPT Interrupt,
    __in_opt PVOID Argument
    )
{
    PXENIFACE_EVTCHN_CONTEXT Context = (PXENIFACE_EVTCHN_CONTEXT)Argument;
    PROCESSOR_NUMBER ProcNumber;
    ULONG ProcIndex;

    UNREFERENCED_PARAMETER(Interrupt);
    ASSERT(Context);

    KeGetCurrentProcessorNumberEx(&ProcNumber);
    ProcIndex = KeGetProcessorIndexFromNumber(&ProcNumber);
    // we're running at high irql, queue a dpc to signal the event
    if (Context->Active)
        KeInsertQueueDpc(&Context->Fdo->EvtchnDpc[ProcIndex], Context, NULL);

    return TRUE;
}

_IRQL_requires_(PASSIVE_LEVEL) // needed for KeFlushQueuedDpcs
static
VOID
EvtchnFree(
    __in PXENIFACE_FDO Fdo,
    __in PXENIFACE_EVTCHN_CONTEXT Context
    )
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    XenIfaceDebugPrint(TRACE, "Context %p, LocalPort %d, FO %p\n",
                       Context, Context->LocalPort, Context->FileObject);

    InterlockedExchange8(&Context->Active, 0);

    XENBUS_EVTCHN(Close,
                  &Fdo->EvtchnInterface,
                  Context->Channel);

    // There may still be a pending event at this time.
    // Wait for our DPCs to complete.
    KeFlushQueuedDpcs();

    ObDereferenceObject(Context->Event);
    RtlZeroMemory(Context, sizeof(XENIFACE_EVTCHN_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
}

// Cleanup store watches and event channels, called on file object close.
_IRQL_requires_(PASSIVE_LEVEL) // EvtchnFree calls KeFlushQueuedDpcs
VOID
XenIfaceCleanup(
    PXENIFACE_FDO Fdo,
    PFILE_OBJECT  FileObject
    )
{
    PLIST_ENTRY Node;
    PXENIFACE_STORE_CONTEXT StoreContext;
    PXENIFACE_EVTCHN_CONTEXT EvtchnContext;
    KIRQL Irql;
    LIST_ENTRY ToFree;

    XenIfaceDebugPrint(TRACE, "FO %p, IRQL %d, Cpu %lu\n", FileObject, KeGetCurrentIrql(), KeGetCurrentProcessorNumber());

    // store watches
    KeAcquireSpinLock(&Fdo->StoreWatchLock, &Irql);
    Node = Fdo->StoreWatchList.Flink;
    while (Node->Flink != Fdo->StoreWatchList.Flink) {
        StoreContext = CONTAINING_RECORD(Node, XENIFACE_STORE_CONTEXT, Entry);

        Node = Node->Flink;
        if (StoreContext->FileObject != FileObject)
            continue;

        XenIfaceDebugPrint(TRACE, "Store context %p\n", StoreContext);
        RemoveEntryList(&StoreContext->Entry);
        StoreFreeWatch(Fdo, StoreContext);
    }
    KeReleaseSpinLock(&Fdo->StoreWatchLock, Irql);

    // event channels
    InitializeListHead(&ToFree);
    KeAcquireSpinLock(&Fdo->EvtchnLock, &Irql);
    Node = Fdo->EvtchnList.Flink;
    while (Node->Flink != Fdo->EvtchnList.Flink) {
        EvtchnContext = CONTAINING_RECORD(Node, XENIFACE_EVTCHN_CONTEXT, Entry);

        Node = Node->Flink;
        if (EvtchnContext->FileObject != FileObject)
            continue;

        XenIfaceDebugPrint(TRACE, "Evtchn context %p\n", EvtchnContext);
        RemoveEntryList(&EvtchnContext->Entry);
        // EvtchnFree requires PASSIVE_LEVEL and we're inside a lock
        InsertTailList(&ToFree, &EvtchnContext->Entry);
    }
    KeReleaseSpinLock(&Fdo->EvtchnLock, Irql);

    Node = ToFree.Flink;
    while (Node->Flink != ToFree.Flink) {
        EvtchnContext = CONTAINING_RECORD(Node, XENIFACE_EVTCHN_CONTEXT, Entry);
        Node = Node->Flink;

        RemoveEntryList(&EvtchnContext->Entry);
        EvtchnFree(Fdo, EvtchnContext);
    }
}

// Complete a canceled gnttab IRP, cleanup associated grant/map.
_Function_class_(IO_WORKITEM_ROUTINE)
VOID
CompleteGnttabIrp(
    __in      PDEVICE_OBJECT DeviceObject,
    __in_opt  PVOID          Context
    )
{
    PXENIFACE_DX Dx = (PXENIFACE_DX)DeviceObject->DeviceExtension;
    PXENIFACE_FDO Fdo = Dx->Fdo;
    PIRP Irp = Context;
    PXENIFACE_CONTEXT_ID Id;
    PIO_WORKITEM WorkItem;
    KAPC_STATE ApcState;
    BOOLEAN ChangeProcess;

    ASSERT(Context != NULL);

    Id = Irp->Tail.Overlay.DriverContext[0];
    WorkItem = Irp->Tail.Overlay.DriverContext[1];
    
    // We are not guaranteed to be in the context of the process that initiated the IRP,
    // but we need to be there to unmap memory.
    ChangeProcess = PsGetCurrentProcess() != Id->Process;
    if (ChangeProcess) {
        XenIfaceDebugPrint(TRACE, "Changing process from %p to %p\n", PsGetCurrentProcess(), Id->Process);
        KeStackAttachProcess(Id->Process, &ApcState);
    }

    XenIfaceDebugPrint(TRACE, "Irp %p, Process %p, Id %lu, Type %d, IRQL %d\n",
                       Irp, Id->Process, Id->RequestId, Id->Type, KeGetCurrentIrql());

    switch (Id->Type) {

    case XENIFACE_CONTEXT_GRANT:
        GnttabFreeGrant(Fdo, CONTAINING_RECORD(Id, XENIFACE_GRANT_CONTEXT, Id));
        break;

    case XENIFACE_CONTEXT_MAP:
        GnttabFreeMap(Fdo, CONTAINING_RECORD(Id, XENIFACE_MAP_CONTEXT, Id));
        break;

    default:
        ASSERT(FALSE);
    }

    if (ChangeProcess)
        KeUnstackDetachProcess(&ApcState);

    IoFreeWorkItem(WorkItem);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

_Requires_exclusive_lock_held_(Fdo->EvtchnLock)
static
PXENIFACE_EVTCHN_CONTEXT
EvtchnFindChannel(
    __in      PXENIFACE_FDO Fdo,
    __in      ULONG         LocalPort,
    __in_opt  PFILE_OBJECT  FileObject
    )
{
    PXENIFACE_EVTCHN_CONTEXT Context, Found = NULL;
    PLIST_ENTRY Node;

    Node = Fdo->EvtchnList.Flink;
    while (Node->Flink != Fdo->EvtchnList.Flink) {
        Context = CONTAINING_RECORD(Node, XENIFACE_EVTCHN_CONTEXT, Entry);

        Node = Node->Flink;
        if (Context->LocalPort != LocalPort)
            continue;

        if (FileObject != NULL && Context->FileObject != FileObject)
            continue;

        Found = Context;
        break;
    }

    return Found;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlEvtchnBindUnbound(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_EVTCHN_BIND_UNBOUND_IN In = (PXENIFACE_EVTCHN_BIND_UNBOUND_IN)Buffer;
    PXENIFACE_EVTCHN_BIND_UNBOUND_OUT Out = (PXENIFACE_EVTCHN_BIND_UNBOUND_OUT)Buffer;
    PXENIFACE_EVTCHN_CONTEXT Context;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_EVTCHN_BIND_UNBOUND_IN) || OutLen != sizeof(XENIFACE_EVTCHN_BIND_UNBOUND_OUT))
        goto fail1;

    status = STATUS_NO_MEMORY;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(XENIFACE_EVTCHN_CONTEXT), XENIFACE_POOL_TAG);
    if (Context == NULL)
        goto fail2;

    RtlZeroMemory(Context, sizeof(XENIFACE_EVTCHN_CONTEXT));
    Context->FileObject = FileObject;

    XenIfaceDebugPrint(TRACE, "> RemoteDomain %d, Mask %d, FO %p\n",
                       In->RemoteDomain, In->Mask, FileObject);

    status = ObReferenceObjectByHandle(In->Event, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode, &Context->Event, NULL);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = STATUS_UNSUCCESSFUL;
    Context->Channel = XENBUS_EVTCHN(Open,
                                     &Fdo->EvtchnInterface,
                                     XENBUS_EVTCHN_TYPE_UNBOUND,
                                     EvtchnInterruptHandler,
                                     Context,
                                     In->RemoteDomain,
                                     TRUE);
    if (Context->Channel == NULL)
        goto fail4;

    Context->LocalPort = XENBUS_EVTCHN(GetPort,
                                       &Fdo->EvtchnInterface,
                                       Context->Channel);

    Context->Fdo = Fdo;

    ExInterlockedInsertTailList(&Fdo->EvtchnList, &Context->Entry, &Fdo->EvtchnLock);

    InterlockedExchange8(&Context->Active, 1);
    Out->LocalPort = Context->LocalPort;
    *Info = sizeof(XENIFACE_EVTCHN_BIND_UNBOUND_OUT);

    if (!In->Mask) {
        XENBUS_EVTCHN(Unmask,
                      &Fdo->EvtchnInterface,
                      Context->Channel,
                      FALSE);
    }

    XenIfaceDebugPrint(TRACE, "< LocalPort %lu, Context %p\n", Context->LocalPort, Context);
    return STATUS_SUCCESS;

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4\n");
    ObDereferenceObject(Context->Event);
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");
    RtlZeroMemory(Context, sizeof(XENIFACE_EVTCHN_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlEvtchnBindInterdomain(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_EVTCHN_BIND_INTERDOMAIN_IN In = (PXENIFACE_EVTCHN_BIND_INTERDOMAIN_IN)Buffer;
    PXENIFACE_EVTCHN_BIND_INTERDOMAIN_OUT Out = (PXENIFACE_EVTCHN_BIND_INTERDOMAIN_OUT)Buffer;
    PXENIFACE_EVTCHN_CONTEXT Context;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_EVTCHN_BIND_INTERDOMAIN_IN) || OutLen != sizeof(XENIFACE_EVTCHN_BIND_INTERDOMAIN_OUT))
        goto fail1;

    status = STATUS_NO_MEMORY;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(XENIFACE_EVTCHN_CONTEXT), XENIFACE_POOL_TAG);
    if (Context == NULL)
        goto fail2;

    RtlZeroMemory(Context, sizeof(XENIFACE_EVTCHN_CONTEXT));
    Context->FileObject = FileObject;

    XenIfaceDebugPrint(TRACE, "> RemoteDomain %d, RemotePort %lu, Mask %d, FO %p\n",
                       In->RemoteDomain, In->RemotePort, In->Mask, FileObject);

    status = ObReferenceObjectByHandle(In->Event, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode, &Context->Event, NULL);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = STATUS_UNSUCCESSFUL;
    Context->Channel = XENBUS_EVTCHN(Open,
                                     &Fdo->EvtchnInterface,
                                     XENBUS_EVTCHN_TYPE_INTER_DOMAIN,
                                     EvtchnInterruptHandler,
                                     Context,
                                     In->RemoteDomain,
                                     In->RemotePort,
                                     TRUE);
    if (Context->Channel == NULL)
        goto fail4;

    Context->LocalPort = XENBUS_EVTCHN(GetPort,
                                       &Fdo->EvtchnInterface,
                                       Context->Channel);

    Context->Fdo = Fdo;

    ExInterlockedInsertTailList(&Fdo->EvtchnList, &Context->Entry, &Fdo->EvtchnLock);

    InterlockedExchange8(&Context->Active, 1);
    Out->LocalPort = Context->LocalPort;
    *Info = sizeof(XENIFACE_EVTCHN_BIND_INTERDOMAIN_OUT);

    if (!In->Mask) {
        XENBUS_EVTCHN(Unmask,
                      &Fdo->EvtchnInterface,
                      Context->Channel,
                      FALSE);
    }

    XenIfaceDebugPrint(TRACE, "< LocalPort %lu, Context %p\n", Context->LocalPort, Context);

    return STATUS_SUCCESS;

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4\n");
    ObDereferenceObject(Context->Event);
fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");
    RtlZeroMemory(Context, sizeof(XENIFACE_EVTCHN_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlEvtchnClose(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject
    )
{
    NTSTATUS status;
    PXENIFACE_EVTCHN_CLOSE_IN In = (PXENIFACE_EVTCHN_CLOSE_IN)Buffer;
    PXENIFACE_EVTCHN_CONTEXT Context = NULL;
    KIRQL Irql;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_EVTCHN_CLOSE_IN) || OutLen != 0)
        goto fail1;

    XenIfaceDebugPrint(TRACE, "> LocalPort %lu, FO %p\n", In->LocalPort, FileObject);

    KeAcquireSpinLock(&Fdo->EvtchnLock, &Irql);
    Context = EvtchnFindChannel(Fdo, In->LocalPort, FileObject);
    if (Context != NULL)
        RemoveEntryList(&Context->Entry);
    KeReleaseSpinLock(&Fdo->EvtchnLock, Irql);
    if (Context != NULL)
        EvtchnFree(Fdo, Context);

    status = STATUS_NOT_FOUND;
    if (Context == NULL)
        goto fail2;

    return STATUS_SUCCESS;

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

_Requires_lock_not_held_(Fdo->EvtchnLock)
static DECLSPEC_NOINLINE
NTSTATUS
EvtchnNotify(
    __in      PXENIFACE_FDO Fdo,
    __in      ULONG         LocalPort,
    __in_opt  PFILE_OBJECT  FileObject
    )
{
    NTSTATUS status;
    PXENIFACE_EVTCHN_CONTEXT Context = NULL;
    KIRQL Irql;

    KeAcquireSpinLock(&Fdo->EvtchnLock, &Irql);

    Context = EvtchnFindChannel(Fdo, LocalPort, FileObject);

    status = STATUS_NOT_FOUND;
    if (Context == NULL)
        goto fail1;

    XENBUS_EVTCHN(Send,
                  &Fdo->EvtchnInterface,
                  Context->Channel);

    KeReleaseSpinLock(&Fdo->EvtchnLock, Irql);

    return STATUS_SUCCESS;

fail1:
    KeReleaseSpinLock(&Fdo->EvtchnLock, Irql);
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlEvtchnNotify(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject
    )
{
    NTSTATUS status;
    PXENIFACE_EVTCHN_NOTIFY_IN In = (PXENIFACE_EVTCHN_NOTIFY_IN)Buffer;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_EVTCHN_NOTIFY_IN) || OutLen != 0)
        goto fail1;
#if DBG
    XenIfaceDebugPrint(INFO, "> LocalPort %d, FO %p\n", In->LocalPort, FileObject);
#endif

    return EvtchnNotify(Fdo, In->LocalPort, FileObject);

fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlEvtchnUnmask(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __in  PFILE_OBJECT      FileObject
    )
{
    NTSTATUS status;
    PXENIFACE_EVTCHN_UNMASK_IN In = (PXENIFACE_EVTCHN_UNMASK_IN)Buffer;
    PXENIFACE_EVTCHN_CONTEXT Context = NULL;
    KIRQL Irql;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_EVTCHN_UNMASK_IN) || OutLen != 0)
        goto fail1;

    XenIfaceDebugPrint(TRACE, "> LocalPort %d, FO %p\n", In->LocalPort, FileObject);

    KeAcquireSpinLock(&Fdo->EvtchnLock, &Irql);

    Context = EvtchnFindChannel(Fdo, In->LocalPort, FileObject);

    status = STATUS_INVALID_PARAMETER;
    if (Context == NULL)
        goto fail2;

    XENBUS_EVTCHN(Unmask,
                  &Fdo->EvtchnInterface,
                  Context->Channel,
                  FALSE);

    KeReleaseSpinLock(&Fdo->EvtchnLock, Irql);

    return STATUS_SUCCESS;

fail2:
    KeReleaseSpinLock(&Fdo->EvtchnLock, Irql);
    XenIfaceDebugPrint(ERROR, "Fail2\n");

fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

_Acquires_exclusive_lock_(((PXENIFACE_FDO)Argument)->GnttabCacheLock)
_IRQL_requires_(DISPATCH_LEVEL)
VOID
GnttabAcquireLock(
    __in  PVOID Argument
    )
{
    PXENIFACE_FDO Fdo = Argument;

    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);

    KeAcquireSpinLockAtDpcLevel(&Fdo->GnttabCacheLock);
}

_Releases_exclusive_lock_(((PXENIFACE_FDO)Argument)->GnttabCacheLock)
_IRQL_requires_(DISPATCH_LEVEL)
VOID
GnttabReleaseLock(
    __in  PVOID Argument
    )
{
    PXENIFACE_FDO Fdo = Argument;

    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);

    KeReleaseSpinLockFromDpcLevel(&Fdo->GnttabCacheLock);
}

_Requires_lock_not_held_(Fdo->IrpQueueLock)
static
PIRP
FindGnttabIrp(
    __in  PXENIFACE_FDO Fdo,
    __in  PXENIFACE_CONTEXT_ID Id
    )
{
    KIRQL Irql;
    PIRP Irp;

    CsqAcquireLock(&Fdo->IrpQueue, &Irql);
    Irp = CsqPeekNextIrp(&Fdo->IrpQueue, NULL, Id);
    CsqReleaseLock(&Fdo->IrpQueue, Irql);
    return Irp;
}

_IRQL_requires_max_(APC_LEVEL)
VOID
GnttabFreeGrant(
    __in PXENIFACE_FDO Fdo,
    __in PXENIFACE_GRANT_CONTEXT Context
    )
{
    NTSTATUS status;
    ULONG Page;

    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);

    XenIfaceDebugPrint(TRACE, "Context %p\n", Context);

    if (Context->Flags & XENIFACE_GNTTAB_USE_NOTIFY_OFFSET) {
        ((PCHAR)Context->KernelVa)[Context->NotifyOffset] = 0;
    }

    if (Context->Flags & XENIFACE_GNTTAB_USE_NOTIFY_PORT) {
        status = EvtchnNotify(Fdo, Context->NotifyPort, NULL);

        if (!NT_SUCCESS(status)) // non-fatal, we must free memory
            XenIfaceDebugPrint(ERROR, "failed to notify port %lu: 0x%x\n", Context->NotifyPort, status);
    }

    // unmap from user address space
    MmUnmapLockedPages(Context->UserVa, Context->Mdl);

    // stop sharing
    for (Page = 0; Page < Context->NumberPages; Page++) {
        status = XENBUS_GNTTAB(RevokeForeignAccess,
                               &Fdo->GnttabInterface,
                               Fdo->GnttabCache,
                               FALSE,
                               Context->Grants[Page]);

        ASSERT(NT_SUCCESS(status)); // failure here is fatal, something must've gone catastrophically wrong
    }

    IoFreeMdl(Context->Mdl);

    RtlZeroMemory(Context->KernelVa, Context->NumberPages * PAGE_SIZE);
    ExFreePoolWithTag(Context->KernelVa, XENIFACE_POOL_TAG);

    RtlZeroMemory(Context->Grants, Context->NumberPages * sizeof(PXENBUS_GNTTAB_ENTRY));
    ExFreePoolWithTag(Context->Grants, XENIFACE_POOL_TAG);

    RtlZeroMemory(Context, sizeof(XENIFACE_GRANT_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabPermitForeignAccess(
    __in     PXENIFACE_FDO  Fdo,
    __in     PCHAR          Buffer,
    __in     ULONG          InLen,
    __in     ULONG          OutLen,
    __inout  PIRP           Irp
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS_IN In = (PXENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS_IN)Buffer;
    PXENIFACE_GRANT_CONTEXT Context;
    ULONG Page;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS_IN) || OutLen != 0)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if ((In->NumberPages == 0) || (In->NumberPages > 1024 * 1024) ||
        ((In->Flags & XENIFACE_GNTTAB_USE_NOTIFY_OFFSET) && (In->NotifyOffset >= In->NumberPages * PAGE_SIZE))
        )
        goto fail2;

    status = STATUS_NO_MEMORY;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(XENIFACE_GRANT_CONTEXT), XENIFACE_POOL_TAG);
    if (Context == NULL)
        goto fail3;

    RtlZeroMemory(Context, sizeof(XENIFACE_GRANT_CONTEXT));
    Context->Id.Type = XENIFACE_CONTEXT_GRANT;
    Context->Id.Process = PsGetCurrentProcess();
    Context->Id.RequestId = In->RequestId;
    Context->RemoteDomain = In->RemoteDomain;
    Context->NumberPages = In->NumberPages;
    Context->Flags = In->Flags;
    Context->NotifyOffset = In->NotifyOffset;
    Context->NotifyPort = In->NotifyPort;

    XenIfaceDebugPrint(TRACE, "> RemoteDomain %d, NumberPages %lu, Flags 0x%x, Offset 0x%x, Port %d, Process %p, Id %lu\n",
                       Context->RemoteDomain, Context->NumberPages, Context->Flags, Context->NotifyOffset, Context->NotifyPort,
                       Context->Id.Process, Context->Id.RequestId);

    // Check if the request ID is unique.
    // This doesn't protect us from simultaneous requests with the same ID arriving here
    // but another check for duplicate ID is performed when the context/IRP is queued at the end.
    // Ideally we would lock the whole section but that's not really an option since we touch user memory.
    status = STATUS_INVALID_PARAMETER;
    if (FindGnttabIrp(Fdo, &Context->Id) != NULL)
        goto fail4;

    status = STATUS_NO_MEMORY;
    Context->Grants = ExAllocatePoolWithTag(NonPagedPool, Context->NumberPages * sizeof(PXENBUS_GNTTAB_ENTRY), XENIFACE_POOL_TAG);
    if (Context->Grants == NULL)
        goto fail5;

    RtlZeroMemory(Context->Grants, Context->NumberPages * sizeof(PXENBUS_GNTTAB_ENTRY));

    // allocate memory to share
    status = STATUS_NO_MEMORY;
    Context->KernelVa = ExAllocatePoolWithTag(NonPagedPool, Context->NumberPages * PAGE_SIZE, XENIFACE_POOL_TAG);
    if (Context->KernelVa == NULL)
        goto fail6;

    RtlZeroMemory(Context->KernelVa, Context->NumberPages * PAGE_SIZE);
    Context->Mdl = IoAllocateMdl(Context->KernelVa, Context->NumberPages * PAGE_SIZE, FALSE, FALSE, NULL);
    if (Context->Mdl == NULL)
        goto fail7;

    MmBuildMdlForNonPagedPool(Context->Mdl);
    ASSERT(MmGetMdlByteCount(Context->Mdl) == Context->NumberPages * PAGE_SIZE);

    // perform sharing
    for (Page = 0; Page < Context->NumberPages; Page++) {
        status = XENBUS_GNTTAB(PermitForeignAccess,
                               &Fdo->GnttabInterface,
                               Fdo->GnttabCache,
                               FALSE,
                               Context->RemoteDomain,
                               MmGetMdlPfnArray(Context->Mdl)[Page],
                               (Context->Flags & XENIFACE_GNTTAB_READONLY) != 0,
                               &(Context->Grants[Page]));

// prefast somehow thinks that this call can modify Page...
#pragma prefast(suppress:6385)
        XenIfaceDebugPrint(INFO, "Grants[%lu] = %p\n", Page, Context->Grants[Page]);
        if (!NT_SUCCESS(status))
            goto fail8;
    }

    // map into user mode
#pragma prefast(suppress:6320) // we want to catch all exceptions
    __try {
        Context->UserVa = MmMapLockedPagesSpecifyCache(Context->Mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        goto fail9;
    }

    status = STATUS_UNSUCCESSFUL;
    if (Context->UserVa == NULL)
        goto fail10;

    XenIfaceDebugPrint(TRACE, "< Context %p, Irp %p, KernelVa %p, UserVa %p\n", Context, Irp, Context->KernelVa, Context->UserVa);
    
    // Insert the IRP/context into the pending queue.
    // This also checks (again) if the request ID is unique.
    Irp->Tail.Overlay.DriverContext[0] = &Context->Id;
    status = IoCsqInsertIrpEx(&Fdo->IrpQueue, Irp, NULL, &Context->Id);
    if (!NT_SUCCESS(status))
        goto fail11;

    return STATUS_PENDING;

fail11:
    XenIfaceDebugPrint(ERROR, "Fail11\n");
    MmUnmapLockedPages(Context->UserVa, Context->Mdl);

fail10:
    XenIfaceDebugPrint(ERROR, "Fail10\n");

fail9:
    XenIfaceDebugPrint(ERROR, "Fail9\n");

fail8:
    XenIfaceDebugPrint(ERROR, "Fail8: Page = %lu\n", Page);

    while (Page > 0) {
        ASSERT(NT_SUCCESS(XENBUS_GNTTAB(RevokeForeignAccess,
                                        &Fdo->GnttabInterface,
                                        Fdo->GnttabCache,
                                        FALSE,
                                        Context->Grants[Page - 1])));

        --Page;
    }
    IoFreeMdl(Context->Mdl);

fail7:
    XenIfaceDebugPrint(ERROR, "Fail7\n");
    ExFreePoolWithTag(Context->KernelVa, XENIFACE_POOL_TAG);

fail6:
    XenIfaceDebugPrint(ERROR, "Fail6\n");
    ExFreePoolWithTag(Context->Grants, XENIFACE_POOL_TAG);

fail5:
    XenIfaceDebugPrint(ERROR, "Fail5\n");

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4\n");
    RtlZeroMemory(Context, sizeof(XENIFACE_GRANT_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);

fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");

fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabGetGrantResult(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_GET_GRANT_RESULT_IN In = (PXENIFACE_GNTTAB_GET_GRANT_RESULT_IN)Buffer;
    PXENIFACE_GNTTAB_GET_GRANT_RESULT_OUT Out = (PXENIFACE_GNTTAB_GET_GRANT_RESULT_OUT)Buffer;
    XENIFACE_CONTEXT_ID Id;
    KIRQL Irql;
    PIRP Irp;
    PXENIFACE_CONTEXT_ID ContextId;
    PXENIFACE_GRANT_CONTEXT Context;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_GET_GRANT_RESULT_IN))
        goto fail1;

    Id.Process = PsGetCurrentProcess();
    Id.RequestId = In->RequestId;
    Id.Type = XENIFACE_CONTEXT_GRANT;

    XenIfaceDebugPrint(TRACE, "> Process %p, Id %lu\n", Id.Process, Id.RequestId);

    CsqAcquireLock(&Fdo->IrpQueue, &Irql);
    Irp = CsqPeekNextIrp(&Fdo->IrpQueue, NULL, &Id);

    status = STATUS_NOT_FOUND;
    if (Irp == NULL)
        goto fail2;

    ContextId = Irp->Tail.Overlay.DriverContext[0];
    Context = CONTAINING_RECORD(ContextId, XENIFACE_GRANT_CONTEXT, Id);

    status = STATUS_INVALID_BUFFER_SIZE;
    if (OutLen != (sizeof(XENIFACE_GNTTAB_GET_GRANT_RESULT_OUT) + sizeof(ULONG) * Context->NumberPages))
        goto fail3;

    Out->Address = Context->UserVa;
    XenIfaceDebugPrint(TRACE, "< Address %p, Irp %p\n", Context->UserVa, Irp);

    for (ULONG Page = 0; Page < Context->NumberPages; Page++) {
        Out->References[Page] = XENBUS_GNTTAB(GetReference,
                                              &Fdo->GnttabInterface,
                                              Context->Grants[Page]);
        XenIfaceDebugPrint(INFO, "Ref[%lu] = %lu\n", Page, Out->References[Page]);
    }

    CsqReleaseLock(&Fdo->IrpQueue, Irql);
    *Info = OutLen;

    return STATUS_SUCCESS;

fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");
fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
    CsqReleaseLock(&Fdo->IrpQueue, Irql);
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabRevokeForeignAccess(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_REVOKE_FOREIGN_ACCESS_IN In = (PXENIFACE_GNTTAB_REVOKE_FOREIGN_ACCESS_IN)Buffer;
    PXENIFACE_GRANT_CONTEXT Context = NULL;
    XENIFACE_CONTEXT_ID Id;
    PIRP PendingIrp;
    PXENIFACE_CONTEXT_ID ContextId;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_REVOKE_FOREIGN_ACCESS_IN))
        goto fail1;

    Id.Type = XENIFACE_CONTEXT_GRANT;
    Id.Process = PsGetCurrentProcess();
    Id.RequestId = In->RequestId;

    XenIfaceDebugPrint(TRACE, "> Process %p, Id %lu\n", Id.Process, Id.RequestId);

    status = STATUS_NOT_FOUND;
    PendingIrp = IoCsqRemoveNextIrp(&Fdo->IrpQueue, &Id);
    if (PendingIrp == NULL)
        goto fail2;

    ContextId = PendingIrp->Tail.Overlay.DriverContext[0];
    Context = CONTAINING_RECORD(ContextId, XENIFACE_GRANT_CONTEXT, Id);
    GnttabFreeGrant(Fdo, Context);

    PendingIrp->IoStatus.Status = STATUS_SUCCESS;
    PendingIrp->IoStatus.Information = 0;
    IoCompleteRequest(PendingIrp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabMapForeignPages(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __inout  PIRP           Irp
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_MAP_FOREIGN_PAGES_IN In = (PXENIFACE_GNTTAB_MAP_FOREIGN_PAGES_IN)Buffer;
    PXENIFACE_MAP_CONTEXT Context;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen < sizeof(XENIFACE_GNTTAB_MAP_FOREIGN_PAGES_IN) || OutLen != 0)
        goto fail1;

    status = STATUS_INVALID_PARAMETER;
    if ((In->NumberPages == 0) || (In->NumberPages > 1024 * 1024) ||
        ((In->Flags & XENIFACE_GNTTAB_USE_NOTIFY_OFFSET) && (In->NotifyOffset >= In->NumberPages * PAGE_SIZE))
        )
        goto fail2;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_MAP_FOREIGN_PAGES_IN) + sizeof(ULONG) * In->NumberPages)
        goto fail3;

    status = STATUS_NO_MEMORY;
    Context = ExAllocatePoolWithTag(NonPagedPool, sizeof(XENIFACE_MAP_CONTEXT), XENIFACE_POOL_TAG);
    if (Context == NULL)
        goto fail4;

    RtlZeroMemory(Context, sizeof(XENIFACE_MAP_CONTEXT));
    Context->Id.Type = XENIFACE_CONTEXT_MAP;
    Context->Id.Process = PsGetCurrentProcess();
    Context->Id.RequestId = In->RequestId;
    Context->RemoteDomain = In->RemoteDomain;
    Context->NumberPages = In->NumberPages;
    Context->Flags = In->Flags;
    Context->NotifyOffset = In->NotifyOffset;
    Context->NotifyPort = In->NotifyPort;

    XenIfaceDebugPrint(TRACE, "> RemoteDomain %d, NumberPages %lu, Flags 0x%x, Offset 0x%x, Port %d, Process %p, Id %lu\n",
                       Context->RemoteDomain, Context->NumberPages, Context->Flags, Context->NotifyOffset, Context->NotifyPort,
                       Context->Id.Process, Context->Id.RequestId);

    for (ULONG i = 0; i < In->NumberPages; i++)
        XenIfaceDebugPrint(INFO, "> Ref %d\n", In->References[i]);

    status = STATUS_INVALID_PARAMETER;
    if (FindGnttabIrp(Fdo, &Context->Id) != NULL)
        goto fail5;

    status = XENBUS_GNTTAB(MapForeignPages,
                           &Fdo->GnttabInterface,
                           Context->RemoteDomain,
                           Context->NumberPages,
                           In->References,
                           Context->Flags & XENIFACE_GNTTAB_READONLY,
                           &Context->Address);

    if (!NT_SUCCESS(status))
        goto fail6;

    status = STATUS_NO_MEMORY;
    Context->KernelVa = MmMapIoSpace(Context->Address, Context->NumberPages * PAGE_SIZE, MmCached);
    if (Context->KernelVa == NULL)
        goto fail7;

    status = STATUS_NO_MEMORY;
    Context->Mdl = IoAllocateMdl(Context->KernelVa, Context->NumberPages * PAGE_SIZE, FALSE, FALSE, NULL);
    if (Context->Mdl == NULL)
        goto fail8;

    MmBuildMdlForNonPagedPool(Context->Mdl);

    // map into user mode
#pragma prefast(suppress: 6320) // we want to catch all exceptions
    __try {
        Context->UserVa = MmMapLockedPagesSpecifyCache(Context->Mdl, UserMode, MmCached, NULL, FALSE, NormalPagePriority);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        goto fail9;
    }

    status = STATUS_UNSUCCESSFUL;
    if (Context->UserVa == NULL)
        goto fail10;

    XenIfaceDebugPrint(TRACE, "< Context %p, Irp %p, Address %p, KernelVa %p, UserVa %p\n",
                       Context, Irp, Context->Address, Context->KernelVa, Context->UserVa);

    // Insert the IRP/context into the pending queue.
    // This also checks (again) if the request ID is unique.
    Irp->Tail.Overlay.DriverContext[0] = &Context->Id;
    status = IoCsqInsertIrpEx(&Fdo->IrpQueue, Irp, NULL, &Context->Id);
    if (!NT_SUCCESS(status))
        goto fail11;

    return STATUS_PENDING;

fail11:
    XenIfaceDebugPrint(ERROR, "Fail11\n");
    MmUnmapLockedPages(Context->UserVa, Context->Mdl);

fail10:
    XenIfaceDebugPrint(ERROR, "Fail10\n");

fail9:
    XenIfaceDebugPrint(ERROR, "Fail9\n");
    IoFreeMdl(Context->Mdl);

fail8:
    XenIfaceDebugPrint(ERROR, "Fail8\n");
    MmUnmapIoSpace(Context->KernelVa, Context->NumberPages * PAGE_SIZE);

fail7:
    XenIfaceDebugPrint(ERROR, "Fail7\n");
    ASSERT(NT_SUCCESS(XENBUS_GNTTAB(UnmapForeignPages,
                                    &Fdo->GnttabInterface,
                                    Context->Address
                                    )));

fail6:
    XenIfaceDebugPrint(ERROR, "Fail6\n");

fail5:
    XenIfaceDebugPrint(ERROR, "Fail5\n");
    RtlZeroMemory(Context, sizeof(XENIFACE_MAP_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);

fail4:
    XenIfaceDebugPrint(ERROR, "Fail4\n");

fail3:
    XenIfaceDebugPrint(ERROR, "Fail3\n");

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");

fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabGetMapResult(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_GET_MAP_RESULT_IN In = (PXENIFACE_GNTTAB_GET_MAP_RESULT_IN)Buffer;
    PXENIFACE_GNTTAB_GET_MAP_RESULT_OUT Out = (PXENIFACE_GNTTAB_GET_MAP_RESULT_OUT)Buffer;
    XENIFACE_CONTEXT_ID Id;
    KIRQL Irql;
    PIRP Irp;
    PXENIFACE_MAP_CONTEXT Context;
    PXENIFACE_CONTEXT_ID ContextId;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_GET_MAP_RESULT_IN) || OutLen != sizeof(XENIFACE_GNTTAB_GET_MAP_RESULT_OUT))
        goto fail1;

    Id.Type = XENIFACE_CONTEXT_MAP;
    Id.Process = PsGetCurrentProcess();
    Id.RequestId = In->RequestId;

    XenIfaceDebugPrint(TRACE, "> Process %p, Id %lu\n", Id.Process, Id.RequestId);

    CsqAcquireLock(&Fdo->IrpQueue, &Irql);
    Irp = CsqPeekNextIrp(&Fdo->IrpQueue, NULL, &Id);

    status = STATUS_NOT_FOUND;
    if (Irp == NULL)
        goto fail2;

    ContextId = Irp->Tail.Overlay.DriverContext[0];
    Context = CONTAINING_RECORD(ContextId, XENIFACE_MAP_CONTEXT, Id);

    Out->Address = Context->UserVa;
    XenIfaceDebugPrint(TRACE, "< Address %p, Irp %p\n", Context->UserVa, Irp);

    CsqReleaseLock(&Fdo->IrpQueue, Irql);
    *Info = OutLen;

    return STATUS_SUCCESS;

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
    CsqReleaseLock(&Fdo->IrpQueue, Irql);
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

_IRQL_requires_max_(APC_LEVEL)
DECLSPEC_NOINLINE
VOID
GnttabFreeMap(
    __in  PXENIFACE_FDO Fdo,
    __in  PXENIFACE_MAP_CONTEXT Context
    )
{
    NTSTATUS status;

    ASSERT(KeGetCurrentIrql() <= APC_LEVEL);

    XenIfaceDebugPrint(TRACE, "Context %p\n", Context);

    if (Context->Flags & XENIFACE_GNTTAB_USE_NOTIFY_OFFSET) {
        ((PCHAR)Context->KernelVa)[Context->NotifyOffset] = 0;
    }

    if (Context->Flags & XENIFACE_GNTTAB_USE_NOTIFY_PORT) {
        status = EvtchnNotify(Fdo, Context->NotifyPort, NULL);

        if (!NT_SUCCESS(status)) // non-fatal, we must free memory
            XenIfaceDebugPrint(ERROR, "failed to notify port %lu: 0x%x\n", Context->NotifyPort, status);
    }

    // unmap from user address space
    MmUnmapLockedPages(Context->UserVa, Context->Mdl);

    IoFreeMdl(Context->Mdl);

    // unmap from system space
    MmUnmapIoSpace(Context->KernelVa, Context->NumberPages * PAGE_SIZE);

    // undo mapping
    status = XENBUS_GNTTAB(UnmapForeignPages,
                           &Fdo->GnttabInterface,
                           Context->Address);

    ASSERT(NT_SUCCESS(status));

    RtlZeroMemory(Context, sizeof(XENIFACE_MAP_CONTEXT));
    ExFreePoolWithTag(Context, XENIFACE_POOL_TAG);
}

static DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabUnmapForeignPages(
    __in  PXENIFACE_FDO     Fdo,
    __in  PCHAR             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_UNMAP_FOREIGN_PAGES_IN In = (PXENIFACE_GNTTAB_UNMAP_FOREIGN_PAGES_IN)Buffer;
    PXENIFACE_MAP_CONTEXT Context = NULL;
    XENIFACE_CONTEXT_ID Id;
    PIRP PendingIrp;
    PXENIFACE_CONTEXT_ID ContextId;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_UNMAP_FOREIGN_PAGES_IN) && OutLen != 0)
        goto fail1;

    Id.Type = XENIFACE_CONTEXT_MAP;
    Id.Process = PsGetCurrentProcess();
    Id.RequestId = In->RequestId;

    XenIfaceDebugPrint(TRACE, "> Process %p, Id %lu\n", Id.Process, Id.RequestId);

    status = STATUS_NOT_FOUND;
    PendingIrp = IoCsqRemoveNextIrp(&Fdo->IrpQueue, &Id);
    if (PendingIrp == NULL)
        goto fail2;

    ContextId = PendingIrp->Tail.Overlay.DriverContext[0];
    Context = CONTAINING_RECORD(ContextId, XENIFACE_MAP_CONTEXT, Id);
    GnttabFreeMap(Fdo, Context);

    PendingIrp->IoStatus.Status = STATUS_SUCCESS;
    PendingIrp->IoStatus.Information = 0;
    IoCompleteRequest(PendingIrp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail2:
    XenIfaceDebugPrint(ERROR, "Fail2\n");
fail1:
    XenIfaceDebugPrint(ERROR, "Fail1 (%08x)\n", status);
    return status;
}

NTSTATUS
XenIfaceIoctl(
    __in  PXENIFACE_FDO     Fdo,
    __in  PIRP              Irp
    )
{
    NTSTATUS            status;
    PIO_STACK_LOCATION  Stack = IoGetCurrentIrpStackLocation(Irp);
    PVOID               Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG               InLen = Stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG               OutLen = Stack->Parameters.DeviceIoControl.OutputBufferLength;

    status = STATUS_DEVICE_NOT_READY;
    if (Fdo->InterfacesAcquired == FALSE)
        goto done;

    switch (Stack->Parameters.DeviceIoControl.IoControlCode) {
        // store
    case IOCTL_XENIFACE_STORE_READ:
        status = IoctlStoreRead(Fdo, (PCHAR)Buffer, InLen, OutLen, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_STORE_WRITE:
        status = IoctlStoreWrite(Fdo, (PCHAR)Buffer, InLen, OutLen);
        break;

    case IOCTL_XENIFACE_STORE_DIRECTORY:
        status = IoctlStoreDirectory(Fdo, (PCHAR)Buffer, InLen, OutLen, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_STORE_REMOVE:
        status = IoctlStoreRemove(Fdo, (PCHAR)Buffer, InLen, OutLen);
        break;

    case IOCTL_XENIFACE_STORE_SET_PERMISSIONS:
        status = IoctlStoreSetPermissions(Fdo, (PCHAR)Buffer, InLen, OutLen);
        break;

    case IOCTL_XENIFACE_STORE_ADD_WATCH:
        status = IoctlStoreAddWatch(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_STORE_REMOVE_WATCH:
        status = IoctlStoreRemoveWatch(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject);
        break;

        // evtchn
    case IOCTL_XENIFACE_EVTCHN_BIND_UNBOUND:
        status = IoctlEvtchnBindUnbound(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_EVTCHN_BIND_INTERDOMAIN:
        status = IoctlEvtchnBindInterdomain(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_EVTCHN_CLOSE:
        status = IoctlEvtchnClose(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject);
        break;

    case IOCTL_XENIFACE_EVTCHN_NOTIFY:
        status = IoctlEvtchnNotify(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject);
        break;

    case IOCTL_XENIFACE_EVTCHN_UNMASK:
        status = IoctlEvtchnUnmask(Fdo, (PCHAR)Buffer, InLen, OutLen, Stack->FileObject);
        break;

        // gnttab
    case IOCTL_XENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS:
        status = IoctlGnttabPermitForeignAccess(Fdo, (PCHAR)Buffer, InLen, OutLen, Irp);
        break;

    case IOCTL_XENIFACE_GNTTAB_GET_GRANT_RESULT:
        status = IoctlGnttabGetGrantResult(Fdo, (PCHAR)Buffer, InLen, OutLen, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_GNTTAB_REVOKE_FOREIGN_ACCESS:
        status = IoctlGnttabRevokeForeignAccess(Fdo, (PCHAR)Buffer, InLen, OutLen);
        break;

    case IOCTL_XENIFACE_GNTTAB_MAP_FOREIGN_PAGES:
        status = IoctlGnttabMapForeignPages(Fdo, (PCHAR)Buffer, InLen, OutLen, Irp);
        break;

    case IOCTL_XENIFACE_GNTTAB_GET_MAP_RESULT:
        status = IoctlGnttabGetMapResult(Fdo, (PCHAR)Buffer, InLen, OutLen, &Irp->IoStatus.Information);
        break;

    case IOCTL_XENIFACE_GNTTAB_UNMAP_FOREIGN_PAGES:
        status = IoctlGnttabUnmapForeignPages(Fdo, (PCHAR)Buffer, InLen, OutLen);
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

done:

    Irp->IoStatus.Status = status;

    if (status != STATUS_PENDING)
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}
