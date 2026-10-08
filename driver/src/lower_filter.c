#include <ntifs.h>
#include <wdmsec.h>
#include "../include/protocol.h"
#include "caller_auth.h"

/* Default transparent. Explicit one-shot D3->D0 diagnostics run at PASSIVE:
 * mode 0 reads BAR0 and tests an unsubmitted common buffer; mode 1 also
 * verifies stopped-GSP DMA; mode 2 explicitly resets GSP first.
 * mode 3 runs the opt-in source full unlock with firmware and SBR.
 * mode 4 tests one SBR without host firmware submission or DMA allocation.
 * mode 5 reads PCI resources/reset support in the next PnP START window.
 * mode 6 tests one Windows bus function reset in that START window.
 * mode 7 stages full unlock, releases DMA, then fails first START for handoff.
 * No mode waits for user-mode during power completion. */
typedef struct _FILTER_EXTENSION {
    PDEVICE_OBJECT Lower, Pdo, Self;
    IO_REMOVE_LOCK RemoveLock;
    ULONG Bdf;
    BOOLEAN Started;
    DEVICE_POWER_STATE Power;
    ULONGLONG Memory[6];
    ULONG MemoryLength[6], MemoryCount;
    ULONG InterruptCount, InterruptFlags;
} FILTER_EXTENSION;
static PDEVICE_OBJECT ControlDevice;
#ifdef CMP_UNLOCK_SESSION
static PDEVICE_OBJECT SessionDevice;
static ULONG SessionRuns;
#endif
static FAST_MUTEX OwnerLock;
static PFILE_OBJECT Owner;
static PEPROCESS OwnerProcess;
static KSPIN_LOCK TraceLock;
static CMP_FILTER_EVENT Events[CMP_FILTER_EVENTS];
static ULONGLONG Latest;
static ULONG EventCount, DeviceCount, LastBdf = MAXULONG, Ready;
static LONG ProbeArmed, ProbeBusy;
static ULONG ProbeTarget, ProbeMode;
static CMP_FILTER_PROBE ProbeResult;
static const GUID ControlClass = {0xb4922e7b,0xa628,0x4637,{0x9b,0x55,0x51,0x6f,0x68,0xe5,0xec,0x92}};
DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE FilterAddDevice;
DRIVER_DISPATCH FilterDispatch;

static NTSTATUS Complete(PIRP irp, NTSTATUS status, ULONG_PTR bytes)
{
    irp->IoStatus.Status = status; irp->IoStatus.Information = bytes;
    IoCompleteRequest(irp, IO_NO_INCREMENT); return status;
}
static VOID Record(FILTER_EXTENSION *ext, ULONG phase, ULONG major,
    ULONG minor, NTSTATUS status, ULONG value)
{
    KIRQL old; LARGE_INTEGER now; CMP_FILTER_EVENT *event;
    ULONG irql = KeGetCurrentIrql();
    KeQuerySystemTime(&now);
    KeAcquireSpinLock(&TraceLock, &old);
    event = &Events[Latest % CMP_FILTER_EVENTS];
    event->Sequence = ++Latest; event->Time = (ULONGLONG)now.QuadPart;
    event->Phase = phase; event->Major = major; event->Minor = minor;
    event->Status = (ULONG)status; event->Value = value; event->Irql = irql;
    if (EventCount < CMP_FILTER_EVENTS) ++EventCount;
    LastBdf = ext->Bdf;
    Ready = ext->Started; /* PnP started, not a claim about NVIDIA Code 0 or D0. */
    KeReleaseSpinLock(&TraceLock, old);
}
static ULONG PowerValue(PIO_STACK_LOCATION stack)
{
    if (stack->MajorFunction != IRP_MJ_POWER) return 0;
    return ((ULONG)stack->Parameters.Power.Type << 16) |
        stack->Parameters.Power.State.SystemState;
}
static VOID CacheResources(FILTER_EXTENSION *ext, PCM_RESOURCE_LIST resources)
{
    ULONG i, j;
    ext->MemoryCount = 0;
    ext->InterruptCount = 0; ext->InterruptFlags = 0;
    if (!resources) return;
    for (i = 0; i < resources->Count; ++i) {
        PCM_PARTIAL_RESOURCE_LIST list = &resources->List[i].PartialResourceList;
        for (j = 0; j < list->Count; ++j) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &list->PartialDescriptors[j];
            if (r->Type == CmResourceTypeInterrupt) {
                ++ext->InterruptCount; ext->InterruptFlags |= r->Flags;
            }
            if (r->Type == CmResourceTypeMemory && ext->MemoryCount < 6) {
                ext->Memory[ext->MemoryCount] = (ULONGLONG)r->u.Memory.Start.QuadPart;
                ext->MemoryLength[ext->MemoryCount++] = r->u.Memory.Length;
            }
        }
    }
}
static ULONG ReadPci(FILTER_EXTENSION *ext, ULONG offset)
{
    PCI_SLOT_NUMBER slot; ULONG value = MAXULONG;
    slot.u.AsULONG = 0; slot.u.bits.DeviceNumber = (ext->Bdf >> 3) & 31;
    slot.u.bits.FunctionNumber = ext->Bdf & 7;
    if (HalGetBusDataByOffset(PCIConfiguration, ext->Bdf >> 8, slot.u.AsULONG,
        &value, offset, sizeof(value)) != sizeof(value)) return MAXULONG;
    return value;
}
#include "resume_gpu_probe.h"
#include "resume_unlock.h"
#include "start_probe.h"
typedef struct _RESUME_WORK {
    FILTER_EXTENSION *Ext;
    PIO_WORKITEM Item;
    PIRP Irp;
    ULONG Mode;
} RESUME_WORK;
static NTSTATUS ProbeD0(FILTER_EXTENSION *ext, CMP_FILTER_PROBE *result)
{
    ULONG low, i, maps; BOOLEAN assigned = FALSE;
    PHYSICAL_ADDRESS bar, logical; PVOID mapping, buffer; PDMA_ADAPTER adapter;
    DEVICE_DESCRIPTION description; NTSTATUS status;
    result->Stage = 3;
    if (ext->Bdf > 0xffff || !ext->Started || ReadPci(ext, 0) != 0x220d10de)
        return STATUS_NO_SUCH_DEVICE;
    low = ReadPci(ext, 0x10);
    result->Command = ReadPci(ext, 4) & 65535;
    if (!low || low == MAXULONG || (low & 1) || ((low & 6) != 0 && (low & 6) != 4) ||
        result->Command == 65535 || !(result->Command & 2)) return STATUS_DEVICE_NOT_READY;
    result->Bar0 = low & ~15u;
    if ((low & 6) == 4) result->Bar0 |= (ULONGLONG)ReadPci(ext, 0x14) << 32;
    for (i = 0; i < ext->MemoryCount; ++i)
        if (ext->Memory[i] == result->Bar0 && ext->MemoryLength[i] >= 0x1000000) assigned = TRUE;
    if (!assigned || !result->Bar0 || (result->Bar0 & 0xffffff) ||
        result->Bar0 > 0x000fffffffffffffULL - 0x1000000) return STATUS_DEVICE_CONFIGURATION_ERROR;
    bar.QuadPart = (LONGLONG)result->Bar0;
    mapping = MmMapIoSpace(bar, 0x1000000, MmNonCached);
    if (!mapping) return STATUS_INSUFFICIENT_RESOURCES;
    result->Boot0 = READ_REGISTER_ULONG((PULONG)mapping);
    result->GfwBoot = READ_REGISTER_ULONG((PULONG)((PUCHAR)mapping + 0x118128));
    result->GfwProgress = READ_REGISTER_ULONG((PULONG)((PUCHAR)mapping + 0x118234));
    result->GspCpu = READ_REGISTER_ULONG((PULONG)((PUCHAR)mapping + 0x110100));
    result->Sec2Cpu = READ_REGISTER_ULONG((PULONG)((PUCHAR)mapping + 0x840100));
    MmUnmapIoSpace(mapping, 0x1000000);
    if (result->Boot0 == MAXULONG || result->Boot0 == 0 || result->Boot0 == 0xbadf5620)
        return STATUS_DEVICE_NOT_READY;
    result->Flags |= 1;
    result->Stage = 4;
    RtlZeroMemory(&description, sizeof(description));
    description.Version = DEVICE_DESCRIPTION_VERSION3; description.Master = TRUE;
    description.Dma32BitAddresses = TRUE; description.InterfaceType = PCIBus;
    description.BusNumber = ext->Bdf >> 8; description.DmaWidth = Width32Bits;
    description.DmaSpeed = Compatible; description.MaximumLength = PAGE_SIZE;
    description.DmaAddressWidth = 32;
    adapter = IoGetDmaAdapter(ext->Pdo, &description, &maps);
    if (!adapter) return STATUS_NOT_SUPPORTED;
    result->Flags |= 2;
    if (!adapter->DmaOperations || adapter->DmaOperations->Size <
        FIELD_OFFSET(DMA_OPERATIONS, FreeCommonBuffer) + sizeof(PFREE_COMMON_BUFFER) ||
        !adapter->DmaOperations->PutDmaAdapter) return STATUS_NOT_SUPPORTED;
    if (!adapter->DmaOperations->AllocateCommonBuffer || !adapter->DmaOperations->FreeCommonBuffer) {
        adapter->DmaOperations->PutDmaAdapter(adapter); return STATUS_NOT_SUPPORTED;
    }
    result->Stage = 5;
    buffer = adapter->DmaOperations->AllocateCommonBuffer(adapter, PAGE_SIZE, &logical, TRUE);
    status = STATUS_INSUFFICIENT_RESOURCES;
    if (buffer) {
        result->DeviceAddress = (ULONGLONG)logical.QuadPart;
        result->Physical = (ULONGLONG)MmGetPhysicalAddress(buffer).QuadPart;
        result->Bytes = PAGE_SIZE; result->Flags |= 4;
        /* Never expose this address or program a DMA command. Free is valid:
         * neither the GPU nor the user process ever sees the allocation. */
        adapter->DmaOperations->FreeCommonBuffer(adapter, PAGE_SIZE, logical, buffer, TRUE);
        result->Flags |= 8;
        status = result->DeviceAddress <= 0x100000000ULL - PAGE_SIZE ? STATUS_SUCCESS : STATUS_CONFLICTING_ADDRESSES;
    }
    adapter->DmaOperations->PutDmaAdapter(adapter);
    if (NT_SUCCESS(status)) result->Stage = 6;
    return status;
}
static VOID ResumeWorker(PDEVICE_OBJECT device, PVOID context)
{
    RESUME_WORK *work = (RESUME_WORK *)context;
    FILTER_EXTENSION *ext = work->Ext; PIRP irp = work->Irp;
    CMP_FILTER_PROBE result; LARGE_INTEGER now; ULONGLONG begin; KIRQL old;
    UNREFERENCED_PARAMETER(device);
    RtlZeroMemory(&result, sizeof(result)); result.Version = 5; result.Bdf = ext->Bdf;
    KeQuerySystemTime(&now); result.Time = (ULONGLONG)now.QuadPart;
    begin = KeQueryInterruptTime();
    result.Irql = KeGetCurrentIrql();
    Record(ext, 5, work->Mode == 8 ? IRP_MJ_DEVICE_CONTROL : work->Mode >= 5 ? IRP_MJ_PNP : IRP_MJ_POWER,
        work->Mode >= 5 ? IRP_MN_START_DEVICE : IRP_MN_SET_POWER, STATUS_PENDING, work->Mode >= 5 ? 0 : 0x10001);
    if (work->Mode == 8) {
        /* Control IOCTL only: START has already completed. */
        result.Stage = 14; result.Status = (ULONG)ResumeUnlock(ext, FALSE, TRUE);
        irp->IoStatus.Status = (NTSTATUS)result.Status;
        irp->IoStatus.Information = 0;
        if (GpuQuarantined) result.Flags |= 0x400;
    } else if (work->Mode == 7) {
        result.Stage = 14; result.Status = (ULONG)ResumeUnlock(ext, FALSE, TRUE);
        if (GpuQuarantined) result.Flags |= 0x400;
        /* Expected first Code 10: never resume NVIDIA on SBR-cleared MSI.
         * User-mode waits for this IRP to finish, then cycles only this device. */
        if (NT_SUCCESS((NTSTATUS)result.Status) && StagedPending) {
            irp->IoStatus.Status = STATUS_DEVICE_NOT_READY; ext->Started = FALSE;
            CoreLog(NULL, "FILTER_PNP_HANDOFF_PENDING: first START intentionally stopped; next unarmed START is transparent\n");
        }
    } else if (work->Mode >= 5) {
        result.Stage = 14; result.Status = (ULONG)ProbeStart(ext, work->Mode == 6);
        if (GpuQuarantined) result.Flags |= 0x400;
    } else if (work->Mode == 3 || work->Mode == 4) {
        result.Stage = 14; result.Status = (ULONG)ResumeUnlock(ext, work->Mode == 4, FALSE);
        if (GpuQuarantined) result.Flags |= 0x400;
    } else result.Status = (ULONG)ProbeD0(ext, &result);
    if (work->Mode >= 1 && work->Mode <= 2) {
        result.Flags |= 0x200;
        if (work->Mode == 2 && NT_SUCCESS((NTSTATUS)result.Status)) {
            result.ResetStatus = (ULONG)InitializeStoppedGsp(ext, &result);
            result.Status = result.ResetStatus;
        }
        if (NT_SUCCESS((NTSTATUS)result.Status)) result.Status = (ULONG)GpuProbeD0(ext, &result);
        if ((result.Flags & 0x800) && !NT_SUCCESS((NTSTATUS)result.Status)) {
            InterlockedExchange(&GpuQuarantined, 1); result.Flags |= 0x400;
        }
    }
    if (result.Flags & 0x400) {
        irp->IoStatus.Status = STATUS_DEVICE_HARDWARE_ERROR;
        if (work->Mode >= 5) ext->Started = FALSE;
    }
    result.ElapsedMs = (ULONG)((KeQueryInterruptTime() - begin) / 10000);
    KeAcquireSpinLock(&TraceLock, &old); ProbeResult = result; KeReleaseSpinLock(&TraceLock, old);
    Record(ext, 6, work->Mode == 8 ? IRP_MJ_DEVICE_CONTROL : work->Mode >= 5 ? IRP_MJ_PNP : IRP_MJ_POWER,
        work->Mode >= 5 ? IRP_MN_START_DEVICE : IRP_MN_SET_POWER, (NTSTATUS)result.Status, work->Mode >= 5 ? 0 : 0x10001);
    InterlockedExchange(&ProbeBusy, 0);
    IoFreeWorkItem(work->Item); ExFreePoolWithTag(work, 'pPMC');
    IoReleaseRemoveLock(&ext->RemoveLock, irp);
    /* Ordinary probe failure preserves PCI's status. Unrestored GPU state
     * fails D0 upward and retains potentially DMA-visible allocations. */
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}
static NTSTATUS ResumeComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    RESUME_WORK *work = (RESUME_WORK *)context;
    FILTER_EXTENSION *ext = work->Ext;
    UNREFERENCED_PARAMETER(device);
    if (NT_SUCCESS(irp->IoStatus.Status)) {
        if (work->Mode >= 5) ext->Started = TRUE;
        else ext->Power = PowerDeviceD0;
    }
    Record(ext, 3, work->Mode >= 5 ? IRP_MJ_PNP : IRP_MJ_POWER,
        work->Mode >= 5 ? IRP_MN_START_DEVICE : IRP_MN_SET_POWER, irp->IoStatus.Status, work->Mode >= 5 ? 0 : 0x10001);
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    if (NT_SUCCESS(irp->IoStatus.Status) && InterlockedCompareExchange(&ProbeArmed, 0, 1) == 1) {
        IoQueueWorkItem(work->Item, ResumeWorker, DelayedWorkQueue, work);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }
    InterlockedExchange(&ProbeBusy, 0);
    IoFreeWorkItem(work->Item); ExFreePoolWithTag(work, 'pPMC');
    IoReleaseRemoveLock(&ext->RemoveLock, irp);
    return STATUS_CONTINUE_COMPLETION;
}
static NTSTATUS ForwardComplete(PDEVICE_OBJECT device, PIRP irp, PVOID context)
{
    FILTER_EXTENSION *ext = (FILTER_EXTENSION *)context;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    UNREFERENCED_PARAMETER(device);
    if (NT_SUCCESS(irp->IoStatus.Status)) {
        if (stack->MajorFunction == IRP_MJ_PNP) {
            if (stack->MinorFunction == IRP_MN_START_DEVICE) {
                ext->Started = TRUE;
#ifdef CMP_UNLOCK_SESSION
                ext->Power = PowerDeviceD0;
                { POWER_STATE state; state.DeviceState = PowerDeviceD0; PoSetPowerState(ext->Self, DevicePowerState, state); }
#endif
                if (ext->Bdf == StagedBdf && StagedPending) {
                    InterlockedExchange(&StagedPending, 0);
                    CoreLog(NULL, "FILTER_PNP_HANDOFF_LOWER_START: transparent second START, NVIDIA initialization continues above\n");
                }
            }
            if (stack->MinorFunction == IRP_MN_STOP_DEVICE ||
                stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL) ext->Started = FALSE;
        }
        if (stack->MajorFunction == IRP_MJ_POWER && stack->MinorFunction == IRP_MN_SET_POWER &&
            stack->Parameters.Power.Type == DevicePowerState) {
            ext->Power = stack->Parameters.Power.State.DeviceState;
#ifdef CMP_UNLOCK_SESSION
            PoSetPowerState(ext->Self, DevicePowerState, stack->Parameters.Power.State);
#endif
        }
    }
    if (stack->MajorFunction == IRP_MJ_POWER || stack->MajorFunction == IRP_MJ_PNP)
        Record(ext, 3, stack->MajorFunction, stack->MinorFunction, irp->IoStatus.Status, PowerValue(stack));
    if (irp->PendingReturned) IoMarkIrpPending(irp);
    IoReleaseRemoveLock(&ext->RemoveLock, irp);
    return STATUS_CONTINUE_COMPLETION;
}
static NTSTATUS DispatchStack(PDEVICE_OBJECT device, PIRP irp)
{
    FILTER_EXTENSION *ext = (FILTER_EXTENSION *)device->DeviceExtension;
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = IoAcquireRemoveLock(&ext->RemoveLock, irp);
    if (!NT_SUCCESS(status)) return Complete(irp, status, 0);
#ifdef CMP_UNLOCK_SESSION
    /* This build owns the function-driver role. It never unlocks in PnP/power. */
    if (stack->MajorFunction == IRP_MJ_PNP) {
        switch (stack->MinorFunction) {
        case IRP_MN_START_DEVICE:
            irp->IoStatus.Status = STATUS_SUCCESS; break;
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
            if (ProbeBusy || GpuQuarantined || RetainedCore) {
                IoReleaseRemoveLock(&ext->RemoveLock, irp);
                return Complete(irp, STATUS_DEVICE_BUSY, 0);
            }
            irp->IoStatus.Status = STATUS_SUCCESS; break;
        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_STOP_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            irp->IoStatus.Status = STATUS_SUCCESS; break;
        case IRP_MN_DEVICE_USAGE_NOTIFICATION:
            if (stack->Parameters.UsageNotification.InPath) {
                IoReleaseRemoveLock(&ext->RemoveLock, irp);
                return Complete(irp, STATUS_NOT_SUPPORTED, 0);
            }
            irp->IoStatus.Status = STATUS_SUCCESS; break;
        default: break;
        }
    }
    if (stack->MajorFunction == IRP_MJ_POWER &&
        (stack->MinorFunction == IRP_MN_SET_POWER || stack->MinorFunction == IRP_MN_QUERY_POWER)) {
        if (ProbeBusy || GpuQuarantined || RetainedCore) {
            PoStartNextPowerIrp(irp); IoReleaseRemoveLock(&ext->RemoveLock, irp);
            return Complete(irp, STATUS_DEVICE_BUSY, 0);
        }
        irp->IoStatus.Status = STATUS_SUCCESS;
    }
#endif
    if (stack->MajorFunction == IRP_MJ_PNP && stack->MinorFunction == IRP_MN_START_DEVICE)
        CacheResources(ext, stack->Parameters.StartDevice.AllocatedResourcesTranslated);
    if (stack->MajorFunction == IRP_MJ_POWER || stack->MajorFunction == IRP_MJ_PNP)
        Record(ext, 2, stack->MajorFunction, stack->MinorFunction, irp->IoStatus.Status, PowerValue(stack));
    if (stack->MajorFunction == IRP_MJ_PNP && stack->MinorFunction == IRP_MN_REMOVE_DEVICE) {
        PDEVICE_OBJECT lower = ext->Lower; KIRQL old;
        ext->Started = FALSE;
        Record(ext, 4, IRP_MJ_PNP, IRP_MN_REMOVE_DEVICE, STATUS_SUCCESS, 0);
        KeAcquireSpinLock(&TraceLock, &old); if (DeviceCount) --DeviceCount;
#ifdef CMP_UNLOCK_SESSION
        if (SessionDevice == device) SessionDevice = NULL;
#endif
        KeReleaseSpinLock(&TraceLock, old);
        IoReleaseRemoveLockAndWait(&ext->RemoveLock, irp);
        IoSkipCurrentIrpStackLocation(irp); status = IoCallDriver(lower, irp);
        IoDetachDevice(lower); ObDereferenceObject(ext->Pdo); IoDeleteDevice(device); return status;
    }
    if (((ProbeMode >= 5 && stack->MajorFunction == IRP_MJ_PNP && stack->MinorFunction == IRP_MN_START_DEVICE) ||
        (ProbeMode < 5 && stack->MajorFunction == IRP_MJ_POWER && stack->MinorFunction == IRP_MN_SET_POWER &&
        stack->Parameters.Power.Type == DevicePowerState && stack->Parameters.Power.State.DeviceState == PowerDeviceD0 &&
        ext->Power == PowerDeviceD3)) && ext->Bdf == ProbeTarget &&
        InterlockedCompareExchange(&ProbeArmed, 1, 1) == 1 && InterlockedCompareExchange(&ProbeBusy, 1, 0) == 0) {
        RESUME_WORK *work = (RESUME_WORK *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*work), 'pPMC');
        if (work) {
            work->Item = IoAllocateWorkItem(device); work->Ext = ext; work->Irp = irp; work->Mode = ProbeMode;
            if (work->Item) {
                /* Always pend this candidate IRP, including synchronous lower
                 * completion. No reads of the IRP after IoCallDriver. */
                IoMarkIrpPending(irp); IoCopyCurrentIrpStackLocationToNext(irp);
                IoSetCompletionRoutine(irp, ResumeComplete, work, TRUE, TRUE, TRUE);
                if (stack->MajorFunction == IRP_MJ_POWER) { PoStartNextPowerIrp(irp); PoCallDriver(ext->Lower, irp); }
                else IoCallDriver(ext->Lower, irp);
                return STATUS_PENDING;
            }
            ExFreePoolWithTag(work, 'pPMC');
        }
        InterlockedExchange(&ProbeBusy, 0); InterlockedExchange(&ProbeArmed, 0);
        Record(ext, 6, stack->MajorFunction, stack->MinorFunction, STATUS_INSUFFICIENT_RESOURCES, PowerValue(stack));
    }
    /* Preserve IoStatus and every unhandled request. A filter must not claim
     * or reject the function driver's ordinary I/O, PnP, or power requests. */
    IoCopyCurrentIrpStackLocationToNext(irp);
    IoSetCompletionRoutine(irp, ForwardComplete, ext, TRUE, TRUE, TRUE);
    if (stack->MajorFunction == IRP_MJ_POWER) {
        PoStartNextPowerIrp(irp); return PoCallDriver(ext->Lower, irp);
    }
    return IoCallDriver(ext->Lower, irp);
}
static VOID Snapshot(CMP_FILTER_DIAGNOSTICS *info)
{
    KIRQL old; ULONG i; ULONGLONG first;
    RtlZeroMemory(info, sizeof(*info));
    KeAcquireSpinLock(&TraceLock, &old);
    info->Version = 1; info->Count = EventCount; info->Latest = Latest;
    info->DeviceCount = DeviceCount; info->Bdf = LastBdf; info->Ready = Ready;
    first = Latest - EventCount;
    for (i = 0; i < EventCount; ++i) info->Events[i] = Events[(first + i) % CMP_FILTER_EVENTS];
    KeReleaseSpinLock(&TraceLock, old);
}
NTSTATUS FilterDispatch(PDEVICE_OBJECT device, PIRP irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST; ULONG_PTR bytes = 0;
    if (device != ControlDevice) return DispatchStack(device, irp);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return Complete(irp, STATUS_INVALID_DEVICE_STATE, 0);
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        if (irp->RequestorMode != UserMode || IoGetRequestorProcess(irp) != PsGetCurrentProcess() || IoIs32bitProcess(irp))
            return Complete(irp, STATUS_ACCESS_DENIED, 0);
        status = CallerAuthVerify(PsGetCurrentProcess());
        if (!NT_SUCCESS(status)) return Complete(irp, STATUS_ACCESS_DENIED, 0);
    }
    ExAcquireFastMutex(&OwnerLock);
    switch (stack->MajorFunction) {
    case IRP_MJ_CREATE:
        if (Owner) { status = STATUS_SHARING_VIOLATION; break; }
        Owner = stack->FileObject; OwnerProcess = PsGetCurrentProcess(); ObReferenceObject(OwnerProcess);
        status = STATUS_SUCCESS; break;
    case IRP_MJ_CLEANUP:
        if (Owner == stack->FileObject) {
            InterlockedExchange(&ProbeArmed, 0); /* Exit before resume cancels unconsumed probe. */
            Owner = NULL; ObDereferenceObject(OwnerProcess); OwnerProcess = NULL;
        }
        status = STATUS_SUCCESS; break;
    case IRP_MJ_CLOSE: status = STATUS_SUCCESS; break;
    case IRP_MJ_DEVICE_CONTROL:
        if (Owner != stack->FileObject || OwnerProcess != PsGetCurrentProcess() ||
            IoGetRequestorProcess(irp) != OwnerProcess || irp->RequestorMode != UserMode) {
            status = STATUS_ACCESS_DENIED; break;
        }
#ifdef CMP_UNLOCK_SESSION
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_SESSION_STATUS) {
            CMP_SESSION_INFO *info; KIRQL old;
            if (stack->Parameters.DeviceIoControl.InputBufferLength ||
                stack->Parameters.DeviceIoControl.OutputBufferLength != sizeof(*info)) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            info = (CMP_SESSION_INFO *)irp->AssociatedIrp.SystemBuffer;
            KeAcquireSpinLock(&TraceLock, &old);
            info->Version = 1; info->Bdf = LastBdf; info->Started = Ready;
            info->Busy = (ULONG)ProbeBusy; info->Quarantined = (ULONG)GpuQuarantined;
            info->Retained = RetainedCore != NULL; info->Runs = SessionRuns;
            info->Status = UnlockResult.Status;
            KeReleaseSpinLock(&TraceLock, old);
            status = STATUS_SUCCESS; bytes = sizeof(*info); break;
        }
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_SESSION_RUN) {
            ULONG target; KIRQL old; FILTER_EXTENSION *ext; RESUME_WORK *work; PDEVICE_OBJECT sessionDevice;
            if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(ULONG) ||
                stack->Parameters.DeviceIoControl.OutputBufferLength) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            target = *(ULONG *)irp->AssociatedIrp.SystemBuffer;
            KeAcquireSpinLock(&TraceLock, &old);
            if (!SessionDevice || DeviceCount != 1 || target > 0xffff || target != LastBdf || !Ready || ProbeBusy ||
                GpuQuarantined || RetainedCore) {
                KeReleaseSpinLock(&TraceLock, old); status = STATUS_INVALID_DEVICE_STATE; break;
            }
            sessionDevice = SessionDevice;
            ext = (FILTER_EXTENSION *)sessionDevice->DeviceExtension;
            if (ext->Power != PowerDeviceD0) { KeReleaseSpinLock(&TraceLock, old); status = STATUS_DEVICE_NOT_READY; break; }
            status = IoAcquireRemoveLock(&ext->RemoveLock, irp);
            if (!NT_SUCCESS(status)) { KeReleaseSpinLock(&TraceLock, old); break; }
            ProbeBusy = 1;
            KeReleaseSpinLock(&TraceLock, old);
            work = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*work), 'pPMC');
            if (work) work->Item = IoAllocateWorkItem(sessionDevice);
            if (!work || !work->Item) {
                if (work) ExFreePoolWithTag(work, 'pPMC');
                InterlockedExchange(&ProbeBusy, 0); IoReleaseRemoveLock(&ext->RemoveLock, irp);
                status = STATUS_INSUFFICIENT_RESOURCES; break;
            }
            work->Ext = ext; work->Irp = irp; work->Mode = 8;
            KeAcquireSpinLock(&TraceLock, &old);
            ++SessionRuns; RtlZeroMemory(&UnlockResult, sizeof(UnlockResult));
            UnlockResult.Version = 1; UnlockResult.Stage = 1; UnlockResult.Bdf = target;
            UnlockResult.Status = (ULONG)STATUS_PENDING; UnlockResult.Flags = 2048;
            KeReleaseSpinLock(&TraceLock, old);
            IoMarkIrpPending(irp);
            /* FAST_MUTEX raises to APC_LEVEL. Release it BEFORE queueing PASSIVE work. */
            ExReleaseFastMutex(&OwnerLock);
            IoQueueWorkItem(work->Item, ResumeWorker, DelayedWorkQueue, work);
            return STATUS_PENDING;
        }
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_FILTER_ARM_PROBE) {
            status = STATUS_INVALID_DEVICE_REQUEST; break;
        }
#endif
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_FILTER_ARM_PROBE) {
            KIRQL old; LARGE_INTEGER now; ULONG target, mode = 0;
            if ((stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(ULONG) &&
                stack->Parameters.DeviceIoControl.InputBufferLength != 2 * sizeof(ULONG)) ||
                stack->Parameters.DeviceIoControl.OutputBufferLength != 0) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            target = *(ULONG *)irp->AssociatedIrp.SystemBuffer;
            if (stack->Parameters.DeviceIoControl.InputBufferLength == 2 * sizeof(ULONG))
                mode = ((ULONG *)irp->AssociatedIrp.SystemBuffer)[1];
            if (mode > 7 || GpuQuarantined) { status = STATUS_INVALID_DEVICE_STATE; break; }
            KeAcquireSpinLock(&TraceLock, &old);
            if (DeviceCount != 1 || LastBdf != target || target > 0xffff || !Ready || ProbeBusy || ProbeArmed || RetainedCore || RetainedStartReset || StagedPending)
                status = STATUS_INVALID_DEVICE_STATE;
            else {
                RtlZeroMemory(&ProbeResult, sizeof(ProbeResult));
                KeQuerySystemTime(&now); ProbeResult.Time = (ULONGLONG)now.QuadPart;
                ProbeResult.Version = 5; ProbeResult.Stage = 1; ProbeResult.Bdf = target;
                ProbeResult.Status = (ULONG)STATUS_PENDING; ProbeTarget = target; ProbeMode = mode;
                if (mode >= 3) {
                    RtlZeroMemory(&UnlockResult, sizeof(UnlockResult));
                    UnlockResult.Version = 1; UnlockResult.Stage = 1;
                    UnlockResult.Bdf = target; UnlockResult.Status = (ULONG)STATUS_PENDING;
                }
                if (mode >= 1) ProbeResult.Flags = 0x200;
                InterlockedExchange(&ProbeArmed, 1); status = STATUS_SUCCESS;
            }
            KeReleaseSpinLock(&TraceLock, old); break;
        }
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_FILTER_PCI_STATUS) {
            CMP_FILTER_PCI_SNAPSHOT *snapshot; ULONG target, i; KIRQL old;
            if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(ULONG) ||
                stack->Parameters.DeviceIoControl.OutputBufferLength != sizeof(*snapshot)) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            target = *(ULONG *)irp->AssociatedIrp.SystemBuffer;
            KeAcquireSpinLock(&TraceLock, &old);
            status = DeviceCount == 1 && Ready && target <= 0xffff && LastBdf == target ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
            KeReleaseSpinLock(&TraceLock, old);
            if (!NT_SUCCESS(status)) break;
            snapshot = (CMP_FILTER_PCI_SNAPSHOT *)irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(snapshot, sizeof(*snapshot)); snapshot->Version = 1; snapshot->Bdf = target;
            for (i = 0; i < 256; i += 4) *(UNALIGNED ULONG *)(snapshot->Config+i) = PciValue(target, i, 4);
            if (*(UNALIGNED ULONG *)snapshot->Config != 0x220d10de) { status = STATUS_NO_SUCH_DEVICE; break; }
            snapshot->Bytes = 256; bytes = sizeof(*snapshot); status = STATUS_SUCCESS; break;
        }
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_FILTER_UNLOCK_STATUS) {
            KIRQL old;
            if (stack->Parameters.DeviceIoControl.InputBufferLength != 0 ||
                stack->Parameters.DeviceIoControl.OutputBufferLength != sizeof(UnlockResult)) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            KeAcquireSpinLock(&TraceLock, &old);
            *(CMP_FILTER_UNLOCK_RESULT *)irp->AssociatedIrp.SystemBuffer = UnlockResult;
            ((CMP_FILTER_UNLOCK_RESULT *)irp->AssociatedIrp.SystemBuffer)->Version = 1;
            KeReleaseSpinLock(&TraceLock, old);
            bytes = sizeof(UnlockResult); status = STATUS_SUCCESS; break;
        }
        if (stack->Parameters.DeviceIoControl.IoControlCode == CMP_FILTER_PROBE_STATUS) {
            KIRQL old;
            if (stack->Parameters.DeviceIoControl.InputBufferLength != 0 ||
                stack->Parameters.DeviceIoControl.OutputBufferLength != sizeof(ProbeResult)) { status = STATUS_INFO_LENGTH_MISMATCH; break; }
            KeAcquireSpinLock(&TraceLock, &old);
            *(CMP_FILTER_PROBE *)irp->AssociatedIrp.SystemBuffer = ProbeResult;
            ((CMP_FILTER_PROBE *)irp->AssociatedIrp.SystemBuffer)->Version = 5;
            KeReleaseSpinLock(&TraceLock, old);
            bytes = sizeof(ProbeResult); status = STATUS_SUCCESS; break;
        }
        if (stack->Parameters.DeviceIoControl.IoControlCode != CMP_FILTER_STATUS) break;
        if (stack->Parameters.DeviceIoControl.InputBufferLength != 0 ||
            stack->Parameters.DeviceIoControl.OutputBufferLength != sizeof(CMP_FILTER_DIAGNOSTICS)) {
            status = STATUS_INFO_LENGTH_MISMATCH; break;
        }
        Snapshot((CMP_FILTER_DIAGNOSTICS *)irp->AssociatedIrp.SystemBuffer);
        bytes = sizeof(CMP_FILTER_DIAGNOSTICS); status = STATUS_SUCCESS; break;
    }
    ExReleaseFastMutex(&OwnerLock); return Complete(irp, status, bytes);
}
NTSTATUS FilterAddDevice(PDRIVER_OBJECT driver, PDEVICE_OBJECT pdo)
{
    PDEVICE_OBJECT device; FILTER_EXTENSION *ext; NTSTATUS status;
    ULONG bus, slot, size; KIRQL old;
    status = IoCreateDevice(driver, sizeof(FILTER_EXTENSION), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &device);
    if (!NT_SUCCESS(status)) return status;
    ext = (FILTER_EXTENSION *)device->DeviceExtension;
    ext->Self = device; ext->Pdo = pdo; ObReferenceObject(pdo);
    ext->Bdf = MAXULONG; ext->Power = PowerDeviceUnspecified;
    IoInitializeRemoveLock(&ext->RemoveLock, 'fPMC', 0, 0);
    if (NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyBusNumber, sizeof(bus), &bus, &size)) &&
        size == sizeof(bus) && bus <= 255 &&
        NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyAddress, sizeof(slot), &slot, &size)) &&
        size == sizeof(slot) && (slot >> 16) <= 31 && (slot & 65535) <= 7)
        ext->Bdf = (bus << 8) | ((slot >> 16) << 3) | (slot & 7);
    ext->Lower = IoAttachDeviceToDeviceStack(device, pdo);
    if (!ext->Lower) { ObDereferenceObject(pdo); IoDeleteDevice(device); return STATUS_NO_SUCH_DEVICE; }
    device->Flags |= ext->Lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE | DO_POWER_INRUSH);
    KeAcquireSpinLock(&TraceLock, &old); ++DeviceCount; KeReleaseSpinLock(&TraceLock, old);
#ifdef CMP_UNLOCK_SESSION
    KeAcquireSpinLock(&TraceLock, &old); SessionDevice = device; KeReleaseSpinLock(&TraceLock, old);
#endif
    Record(ext, 1, IRP_MJ_PNP, 0, STATUS_SUCCESS, ext->Lower->Flags);
    device->Flags &= ~DO_DEVICE_INITIALIZING; return STATUS_SUCCESS;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry)
{
#ifdef CMP_UNLOCK_SESSION
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\CMP90HXUnlockSession");
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\CMP90HXUnlockSession");
#else
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\CMP90HXLowerFilter");
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\CMP90HXLowerFilter");
#endif
    UNICODE_STRING acl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    PDEVICE_OBJECT device; ULONG i; NTSTATUS status;
    UNREFERENCED_PARAMETER(registry);
    ExInitializeFastMutex(&OwnerLock); KeInitializeSpinLock(&TraceLock);
    status = CallerAuthInitialize(); if (!NT_SUCCESS(status)) return status;
    status = IoCreateDeviceSecure(driver, 0, &name, CMP_DEVICE_TYPE, FILE_DEVICE_SECURE_OPEN,
        TRUE, &acl, &ControlClass, &device);
    if (!NT_SUCCESS(status)) { CallerAuthShutdown(); return status; }
    status = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(status)) { IoDeleteDevice(device); CallerAuthShutdown(); return status; }
    ControlDevice = device;
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) driver->MajorFunction[i] = FilterDispatch;
    driver->DriverExtension->AddDevice = FilterAddDevice;
    driver->DriverUnload = NULL; /* test updates/removal finish on reboot */
    device->Flags &= ~DO_DEVICE_INITIALIZING; return STATUS_SUCCESS;
}
