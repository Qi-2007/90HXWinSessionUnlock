/* One-shot PnP START evidence; mode 6 explicitly invokes one bus function reset.
 * Query/read through the attached bus stack at PASSIVE, before START goes up.
 * Interface references live only within this worker's remove-lock lifetime. */
static const GUID StartBusGuid = {0x496b8280,0x6f25,0x11d0,{0xbe,0xaf,0x08,0x00,0x2b,0xe2,0x09,0x2f}};
static const GUID StartResetGuid = {0x649fdf26,0x3bc0,0x4813,{0xad,0x24,0x7e,0x0c,0x1e,0xda,0x3f,0xa3}};
typedef struct _START_RESET_CONTEXT {
    DEVICE_RESET_INTERFACE_STANDARD Interface;
    FUNCTION_LEVEL_DEVICE_RESET_PARAMETERS Parameters;
    volatile LONG Completed, Status;
} START_RESET_CONTEXT;
/* Exactly one submitted reset per boot. Callback parameters and queried bus
 * reference remain valid even after a timeout/late completion. No stack/IRP/ext
 * pointer is exposed to the callback; DriverUnload is already disabled. */
static START_RESET_CONTEXT *RetainedStartReset;
static VOID StartResetCompletion(NTSTATUS status, PVOID context)
{
    START_RESET_CONTEXT *reset = (START_RESET_CONTEXT *)context;
    InterlockedExchange(&reset->Status, (LONG)status);
    InterlockedExchange(&reset->Completed, 1);
}
static NTSTATUS StartQueryInterface(FILTER_EXTENSION *ext, const GUID *id,
    PINTERFACE interface, USHORT size, USHORT version)
{
    KEVENT event; IO_STATUS_BLOCK iosb; PIRP request; PIO_STACK_LOCATION stack;
    NTSTATUS status;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    request = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, ext->Lower, NULL, 0, NULL, &event, &iosb);
    if (!request) return STATUS_INSUFFICIENT_RESOURCES;
    request->IoStatus.Status = STATUS_NOT_SUPPORTED;
    request->IoStatus.Information = 0;
    stack = IoGetNextIrpStackLocation(request);
    stack->MinorFunction = IRP_MN_QUERY_INTERFACE;
    stack->Parameters.QueryInterface.InterfaceType = id;
    stack->Parameters.QueryInterface.Size = size;
    stack->Parameters.QueryInterface.Version = version;
    stack->Parameters.QueryInterface.Interface = interface;
    stack->Parameters.QueryInterface.InterfaceSpecificData = NULL;
    status = IoCallDriver(ext->Lower, request);
    if (status == STATUS_PENDING) {
        (void)KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}
static NTSTATUS StartReadConfig(FILTER_EXTENSION *ext, UCHAR config[256], const char *phase)
{
    BUS_INTERFACE_STANDARD bus;
    ULONG bytes = 0, cap, count; BOOLEAN seen[256]; NTSTATUS status;
    RtlZeroMemory(&bus, sizeof(bus)); RtlZeroMemory(config, 256);
    status = StartQueryInterface(ext, &StartBusGuid, (PINTERFACE)&bus, sizeof(bus), 1);
    if (NT_SUCCESS(status)) {
        if (bus.Size < sizeof(bus) || bus.Version != 1 || !bus.GetBusData || !bus.InterfaceDereference)
            status = STATUS_INVALID_DEVICE_STATE;
        else {
            bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, config, 0, 256);
            if (bytes != 256 || *(UNALIGNED ULONG *)config != 0x220d10de)
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if (bus.InterfaceDereference) bus.InterfaceDereference(bus.Context);
    }
    CoreLog(NULL, "FILTER_START_BUS_INTERFACE status=0x%08x bytes=%u phase=%s\n", (ULONG)status, bytes, phase);
    if (NT_SUCCESS(status)) {
        CoreLog(NULL, "FILTER_START_PCI phase=%s command=0x%04x bar0=0x%08x bar1=0x%08x; before-upper-start-completion\n",
            phase, *(UNALIGNED USHORT *)(config+4), *(UNALIGNED ULONG *)(config+0x10), *(UNALIGNED ULONG *)(config+0x14));
        RtlZeroMemory(seen, sizeof(seen)); cap = config[0x34] & ~3u;
        for (count = 0; cap && count < 48; ++count) {
            ULONG ctl;
            if (cap < 0x40 || cap > 0xfc || (cap & 3) || seen[cap]) {
                status = STATUS_DEVICE_CONFIGURATION_ERROR; break;
            }
            seen[cap] = TRUE; ctl = *(UNALIGNED USHORT *)(config+cap+2);
            if (config[cap] == 5 && cap <= 0xe8)
                CoreLog(NULL, "FILTER_START_MSI phase=%s cap=0x%x control=0x%04x enabled=%u address=%08x:%08x data=0x%04x\n",
                    phase, cap, ctl, ctl&1, (ctl&0x80)?*(UNALIGNED ULONG *)(config+cap+8):0,
                    *(UNALIGNED ULONG *)(config+cap+4), *(UNALIGNED USHORT *)(config+cap+((ctl&0x80)?12:8)));
            if (config[cap] == 0x11)
                CoreLog(NULL, "FILTER_START_MSIX phase=%s cap=0x%x control=0x%04x\n", phase, cap, ctl);
            cap = config[cap+1];
        }
        if (cap && count == 48) status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return status;
}
static NTSTATUS StartVerifyResetConfig(const UCHAR before[256], const UCHAR after[256])
{
    ULONG cap, count, length; USHORT ctl;
    if (RtlCompareMemory(before, after, 4) != 4 ||
        RtlCompareMemory(before+4, after+4, 2) != 2 ||
        RtlCompareMemory(before+0x10, after+0x10, 24) != 24)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    cap = before[0x34] & ~3u;
    for (count = 0; cap && count < 48; ++count) {
        if (cap < 0x40 || cap > 0xfc || (cap & 3)) return STATUS_DEVICE_CONFIGURATION_ERROR;
        ctl = *(UNALIGNED USHORT *)(before+cap+2);
        if (before[cap] == 5) {
            length = (ctl&0x80)?14:10;
            if (ctl&0x100) length = (ctl&0x80)?24:20;
            if (cap+length > 256 || RtlCompareMemory(before+cap, after+cap, length) != length)
                return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        /* MSI-X table contents live in BAR space; this experiment doesn't map
         * them, so refuse enabled MSI-X rather than claim delivery is preserved. */
        if (before[cap] == 0x11 && (ctl&0x8000)) return STATUS_NOT_SUPPORTED;
        cap = before[cap+1];
    }
    return cap ? STATUS_DEVICE_CONFIGURATION_ERROR : STATUS_SUCCESS;
}
static NTSTATUS ProbeStart(FILTER_EXTENSION *ext, BOOLEAN requestReset)
{
    DEVICE_RESET_INTERFACE_STANDARD reset;
    UCHAR config[256], after[256]; ULONG i; NTSTATUS status, resetStatus; KIRQL old;
    BOOLEAN submitted = FALSE; ULONGLONG begin = KeQueryInterruptTime();
    CoreStage(2);
    CoreLog(NULL, "FILTER_START_WINDOW filter=0.5.4 lower-start-success=1 bdf=0x%04x irql=%u memoryResources=%u interrupts=%u interruptFlags=0x%x mode=%s; no MMIO/DMA/firmware/manual-SBR\n",
        ext->Bdf, KeGetCurrentIrql(), ext->MemoryCount, ext->InterruptCount, ext->InterruptFlags,
        requestReset?"bus-function-reset":"read-only");
    for (i = 0; i < ext->MemoryCount; ++i)
        CoreLog(NULL, "FILTER_START_MEMORY index=%u address=0x%llx length=0x%x\n", i, ext->Memory[i], ext->MemoryLength[i]);
    status = StartReadConfig(ext, config, "before-reset");
    RtlZeroMemory(&reset, sizeof(reset));
    resetStatus = StartQueryInterface(ext, &StartResetGuid, (PINTERFACE)&reset,
        FIELD_OFFSET(DEVICE_RESET_INTERFACE_STANDARD, QueryBusSpecificResetInfo), DEVICE_RESET_INTERFACE_VERSION_1);
    CoreLog(NULL, "FILTER_START_RESET_INTERFACE status=0x%08x size=%u version=%u supported=0x%x function=%u platform=%u callback=%u; query result, invocation logged separately\n",
        (ULONG)resetStatus, reset.Size, reset.Version, NT_SUCCESS(resetStatus)?reset.SupportedResetTypes:0,
        NT_SUCCESS(resetStatus)?(reset.SupportedResetTypes&1)!=0:0,
        NT_SUCCESS(resetStatus)?(reset.SupportedResetTypes&2)!=0:0,
        NT_SUCCESS(resetStatus)&&reset.DeviceReset!=NULL);
    if (requestReset && NT_SUCCESS(status)) {
        START_RESET_CONTEXT *ctx;
        if (!NT_SUCCESS(resetStatus) || reset.Size < FIELD_OFFSET(DEVICE_RESET_INTERFACE_STANDARD, QueryBusSpecificResetInfo) ||
            reset.Version != 1 || !(reset.SupportedResetTypes&1) || !reset.DeviceReset || !reset.InterfaceDereference)
            status = STATUS_NOT_SUPPORTED;
        /* Gate readable identity/configuration and a fully verifiable interrupt
         * configuration before invoking any destructive operation. */
        else if (!NT_SUCCESS(StartVerifyResetConfig(config, config))) status = STATUS_NOT_SUPPORTED;
        else if (RetainedStartReset) status = STATUS_INVALID_DEVICE_STATE;
        else {
            ctx = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), 'rSMC');
            if (!ctx) status = STATUS_INSUFFICIENT_RESOURCES;
            else {
                ctx->Interface = reset; ctx->Status = (LONG)STATUS_PENDING;
                ctx->Parameters.Size = sizeof(ctx->Parameters);
                ctx->Parameters.DeviceResetCompletion = StartResetCompletion;
                ctx->Parameters.CompletionContext = ctx;
                RetainedStartReset = ctx; reset.InterfaceDereference = NULL; submitted = TRUE;
                CoreStage(3);
                CoreLog(NULL, "FILTER_START_FUNCTION_RESET_BEGIN type=FunctionLevelDeviceReset flags=0; one submitted reset per boot\n");
                resetStatus = ctx->Interface.DeviceReset(ctx->Interface.Context, FunctionLevelDeviceReset, 0, &ctx->Parameters);
                CoreLog(NULL, "FILTER_START_FUNCTION_RESET_RETURN status=0x%08x callback=%u\n", (ULONG)resetStatus,
                    (ULONG)InterlockedCompareExchange(&ctx->Completed, 0, 0));
                status = resetStatus;
                if (NT_SUCCESS(resetStatus)) {
                    ULONGLONG deadline = KeQueryInterruptTime()+30000ULL*10000;
                    LARGE_INTEGER delay; delay.QuadPart = -10LL*10000;
                    while (!InterlockedCompareExchange(&ctx->Completed, 0, 0) && KeQueryInterruptTime() < deadline)
                        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
                    status = InterlockedCompareExchange(&ctx->Completed, 0, 0) ?
                        (NTSTATUS)InterlockedCompareExchange(&ctx->Status, 0, 0) : STATUS_IO_TIMEOUT;
                    if (NT_SUCCESS(status) && status != STATUS_SUCCESS) status = STATUS_INVALID_DEVICE_STATE;
                }
                CoreLog(NULL, "FILTER_START_FUNCTION_RESET_COMPLETE status=0x%08x callback=%u elapsedMs=%u; context/interface retained until reboot\n",
                    (ULONG)status, (ULONG)InterlockedCompareExchange(&ctx->Completed, 0, 0),
                    (ULONG)((KeQueryInterruptTime()-begin)/10000));
                if (NT_SUCCESS(status)) {
                    status = StartReadConfig(ext, after, "after-reset");
                    if (NT_SUCCESS(status)) status = StartVerifyResetConfig(config, after);
                    CoreLog(NULL, "FILTER_START_RESET_CONFIG_VERIFY status=0x%08x preserved=%u; identity/BARs/Command/MSI comparison, no manual restoration\n", (ULONG)status, NT_SUCCESS(status));
                }
            }
        }
    }
    if (NT_SUCCESS(resetStatus) && reset.InterfaceDereference) reset.InterfaceDereference(reset.Context);
    if (submitted && !NT_SUCCESS(status)) InterlockedExchange(&GpuQuarantined, 1);
    CoreLog(NULL, "FILTER_START_PROBE_END status=0x%08x submitted=%u; NVIDIA initialization is not yet verified\n", (ULONG)status, submitted);
    KeAcquireSpinLock(&TraceLock, &old);
    UnlockResult.Stage = 12; UnlockResult.Status = (ULONG)status;
    UnlockResult.Flags |= 128 | (NT_SUCCESS(status)?1:0);
    if (requestReset) UnlockResult.Flags |= 256;
    if (submitted) UnlockResult.Flags |= 2 | (NT_SUCCESS(status)?0:4);
    UnlockResult.ElapsedMs = (ULONG)((KeQueryInterruptTime()-begin)/10000);
    KeReleaseSpinLock(&TraceLock, old);
    return status;
}
