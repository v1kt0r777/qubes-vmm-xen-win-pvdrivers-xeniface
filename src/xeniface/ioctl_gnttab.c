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
#include "xeniface_ioctls.h"
#include "log.h"
#include "irp_queue.h"

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

DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabPermitForeignAccess(
    __in     PXENIFACE_FDO  Fdo,
    __in     PVOID          Buffer,
    __in     ULONG          InLen,
    __in     ULONG          OutLen,
    __inout  PIRP           Irp
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS_IN In = Buffer;
    PXENIFACE_GRANT_CONTEXT Context;
    ULONG Page;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_PERMIT_FOREIGN_ACCESS_IN) ||
        OutLen != 0) {
        goto fail1;
    }

    status = STATUS_INVALID_PARAMETER;
    if (In->NumberPages == 0 ||
        In->NumberPages > 1024 * 1024) {
        goto fail2;
    }

    if ((In->Flags & XENIFACE_GNTTAB_USE_NOTIFY_OFFSET) &&
        (In->NotifyOffset >= In->NumberPages * PAGE_SIZE)) {
        goto fail2;
    }

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

    // Check if the request ID is unique for this process.
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
        Context->UserVa = MmMapLockedPagesSpecifyCache(Context->Mdl,
                                                       UserMode,
                                                       MmCached,
                                                       NULL,
                                                       FALSE,
                                                       NormalPagePriority);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
        goto fail9;
    }

    status = STATUS_UNSUCCESSFUL;
    if (Context->UserVa == NULL)
        goto fail10;

    XenIfaceDebugPrint(TRACE, "< Context %p, Irp %p, KernelVa %p, UserVa %p\n",
                       Context, Irp, Context->KernelVa, Context->UserVa);
    
    // Insert the IRP/context into the pending queue.
    // This also checks (again) if the request ID is unique for the calling process.
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

DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabGetGrantResult(
    __in  PXENIFACE_FDO     Fdo,
    __in  PVOID             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_GET_GRANT_RESULT_IN In = Buffer;
    PXENIFACE_GNTTAB_GET_GRANT_RESULT_OUT Out = Buffer;
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

_IRQL_requires_max_(APC_LEVEL)
VOID
GnttabFreeGrant(
    __in     PXENIFACE_FDO            Fdo,
    __inout  PXENIFACE_GRANT_CONTEXT  Context
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

DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabRevokeForeignAccess(
    __in  PXENIFACE_FDO     Fdo,
    __in  PVOID             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_REVOKE_FOREIGN_ACCESS_IN In = Buffer;
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

DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabMapForeignPages(
    __in     PXENIFACE_FDO     Fdo,
    __in     PVOID             Buffer,
    __in     ULONG             InLen,
    __in     ULONG             OutLen,
    __inout  PIRP              Irp
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_MAP_FOREIGN_PAGES_IN In = Buffer;
    ULONG PageIndex;
    PXENIFACE_MAP_CONTEXT Context;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen < sizeof(XENIFACE_GNTTAB_MAP_FOREIGN_PAGES_IN) ||
        OutLen != 0) {
        goto fail1;
    }

    status = STATUS_INVALID_PARAMETER;
    if (In->NumberPages == 0 ||
        In->NumberPages > 1024 * 1024) {
        goto fail2;
    }

    if ((In->Flags & XENIFACE_GNTTAB_USE_NOTIFY_OFFSET) &&
        (In->NotifyOffset >= In->NumberPages * PAGE_SIZE)) {
        goto fail2;
    }

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

    for (PageIndex = 0; PageIndex < In->NumberPages; PageIndex++)
        XenIfaceDebugPrint(INFO, "> Ref %d\n", In->References[PageIndex]);

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
        Context->UserVa = MmMapLockedPagesSpecifyCache(Context->Mdl,
                                                       UserMode,
                                                       MmCached,
                                                       NULL,
                                                       FALSE,
                                                       NormalPagePriority);
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
    // This also checks (again) if the request ID is unique for the calling process.
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

DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabGetMapResult(
    __in  PXENIFACE_FDO     Fdo,
    __in  PVOID             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen,
    __out PULONG_PTR        Info
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_GET_MAP_RESULT_IN In = Buffer;
    PXENIFACE_GNTTAB_GET_MAP_RESULT_OUT Out = Buffer;
    XENIFACE_CONTEXT_ID Id;
    KIRQL Irql;
    PIRP Irp;
    PXENIFACE_MAP_CONTEXT Context;
    PXENIFACE_CONTEXT_ID ContextId;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_GET_MAP_RESULT_IN) ||
        OutLen != sizeof(XENIFACE_GNTTAB_GET_MAP_RESULT_OUT)) {
        goto fail1;
    }

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
    __in     PXENIFACE_FDO            Fdo,
    __inout  PXENIFACE_MAP_CONTEXT    Context
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

DECLSPEC_NOINLINE
NTSTATUS
IoctlGnttabUnmapForeignPages(
    __in  PXENIFACE_FDO     Fdo,
    __in  PVOID             Buffer,
    __in  ULONG             InLen,
    __in  ULONG             OutLen
    )
{
    NTSTATUS status;
    PXENIFACE_GNTTAB_UNMAP_FOREIGN_PAGES_IN In = Buffer;
    PXENIFACE_MAP_CONTEXT Context = NULL;
    XENIFACE_CONTEXT_ID Id;
    PIRP PendingIrp;
    PXENIFACE_CONTEXT_ID ContextId;

    status = STATUS_INVALID_BUFFER_SIZE;
    if (InLen != sizeof(XENIFACE_GNTTAB_UNMAP_FOREIGN_PAGES_IN) &&
        OutLen != 0) {
        goto fail1;
    }

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