/* Source core runs synchronously at PASSIVE before D0 completion goes upward.
 * This does not establish a cold initialization or WDDM exclusive-access window.
 * Addresses are derived from the bound PDO/resources, never from userspace.
 * One transaction at a time; no user-mode callback, ELF loader or code injection. */
#include <ntstrsafe.h>
#include <stdarg.h>
#include "../../native/rebuilt-core/core.h"
#define CMP_FILTER_CORE
#include "hardware.h"
#undef CMP_FILTER_CORE

typedef struct _CORE_DMA_SLOT { PVOID Cpu; ULONG Bytes; BOOLEAN Live; } CORE_DMA_SLOT;
typedef struct _CORE_HOST_SLOT { PVOID Cpu; } CORE_HOST_SLOT;
typedef struct _RESUME_CORE {
    gpu Gpu;
    PDMA_ADAPTER Adapter;
    PVOID Buffer;
    PHYSICAL_ADDRESS Logical;
    ULONG Used, Slots, Live, Boot0, GpuWidth, BridgeWidth, SbrCount, LastFalconWriteSbr;
    CORE_DMA_SLOT Dma[128];
    CORE_HOST_SLOT Host[32];
    ULONGLONG Deadline;
    ULONGLONG StageBegin;
    ULONG StageNumber;
    BOOLEAN Touched, Fault, Expired;
    PDEVICE_OBJECT Pdo;
} RESUME_CORE;
static RESUME_CORE *ActiveCore, *RetainedCore;
static ULONG StagedBdf;
static LONG StagedPending;
static CMP_FILTER_UNLOCK_RESULT UnlockResult;

static BOOLEAN CoreBudget(void)
{
    if (!ActiveCore || ActiveCore->Fault) return FALSE;
    if (KeQueryInterruptTime() >= ActiveCore->Deadline) {
        ActiveCore->Expired = TRUE; ActiveCore->Fault = TRUE; return FALSE;
    }
    return TRUE;
}
u32 ioread32(const void *address)
{
    ULONGLONG p = (ULONGLONG)address;
    if (!CoreBudget() || p < (ULONGLONG)GpuMapping ||
        p - (ULONGLONG)GpuMapping > CMP_BAR0_BYTES - 4 || (p & 3)) {
        if (ActiveCore) ActiveCore->Fault = TRUE;
        return 0xbadf5620;
    }
    return READ_REGISTER_ULONG((PULONG)address);
}
void iowrite32(u32 value, void *address)
{
    ULONGLONG p = (ULONGLONG)address;
    if (!CoreBudget() || p < (ULONGLONG)GpuMapping ||
        p - (ULONGLONG)GpuMapping > CMP_BAR0_BYTES - 4 || (p & 3)) {
        if (ActiveCore) ActiveCore->Fault = TRUE;
        return;
    }
    ActiveCore->Touched = TRUE;
    if ((p-(ULONGLONG)GpuMapping >= 0x110000 && p-(ULONGLONG)GpuMapping < 0x112000) ||
        (p-(ULONGLONG)GpuMapping >= 0x840000 && p-(ULONGLONG)GpuMapping < 0x842000))
        ActiveCore->LastFalconWriteSbr = ActiveCore->SbrCount;
    WRITE_REGISTER_ULONG((PULONG)address, value);
}
static VOID CoreDelay(u64 milliseconds)
{
    LARGE_INTEGER interval; ULONGLONG now;
    if (!CoreBudget()) return;
    now = KeQueryInterruptTime();
    if (now >= ActiveCore->Deadline || milliseconds > 10000 || milliseconds * 10000 > ActiveCore->Deadline - now) {
        ActiveCore->Fault = TRUE; ActiveCore->Expired = TRUE; return;
    }
    if (!milliseconds) return;
    interval.QuadPart = -(LONGLONG)(milliseconds * 10000);
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}
static VOID CoreLog(gpu *g, const char *format, ...)
{
    CHAR line[512]; va_list args; KIRQL old; ULONG length, room;
    UNREFERENCED_PARAMETER(g);
    va_start(args, format);
    (void)RtlStringCbVPrintfA(line, sizeof(line), format, args);
    va_end(args);
    line[sizeof(line)-1] = 0;
    for (length = 0; length < sizeof(line)-1 && line[length]; ++length) {}
    KeAcquireSpinLock(&TraceLock, &old);
    room = CMP_FILTER_UNLOCK_LOG_BYTES - 1 - UnlockResult.LogBytes;
    if (length > room) { length = room; UnlockResult.Flags |= 8; }
    if (length) {
        RtlCopyMemory(UnlockResult.Log + UnlockResult.LogBytes, line, length);
        UnlockResult.LogBytes += length; UnlockResult.Log[UnlockResult.LogBytes] = 0;
    }
    KeReleaseSpinLock(&TraceLock, old);
}
static VOID CoreStage(ULONG stage)
{
    KIRQL old; ULONGLONG now = KeQueryInterruptTime();
    if (ActiveCore) {
        if (ActiveCore->StageBegin)
            CoreLog(NULL, "FILTER_STAGE_TIMING stage=%u elapsedMs=%llu sbr=%u\n",
                ActiveCore->StageNumber, (now-ActiveCore->StageBegin)/10000, ActiveCore->SbrCount);
        ActiveCore->StageBegin = now; ActiveCore->StageNumber = stage;
    }
    KeAcquireSpinLock(&TraceLock, &old); UnlockResult.Stage = stage; KeReleaseSpinLock(&TraceLock, old);
    CoreLog(NULL, "FILTER_UNLOCK_STAGE=%u\n", stage);
}
/* Read-only evidence. OS-owned MSI/MSI-X fields are never restored here.
 * Probe offsets are derived from the capability chain, not guessed constants. */
static VOID CorePciEvidence(const char *phase)
{
    ULONG cap, ctl, lo, hi, data, mask;
    CoreLog(NULL, "FILTER_PCI phase=%s sbr=%u command=0x%04x gpuLink=0x%04x rootLink=0x%04x\n",
        phase, ActiveCore->SbrCount, PciValue(GpuBdf, 4, 2),
        PciValue(GpuBdf, GpuCap+0x12, 2), PciValue(BridgeBdf, BridgeCap+0x12, 2));
    cap = Capability(GpuBdf, 5);
    if (cap && cap <= 0xe8) {
        ctl = PciValue(GpuBdf, cap+2, 2); lo = PciValue(GpuBdf, cap+4, 4);
        hi = (ctl & 0x80) ? PciValue(GpuBdf, cap+8, 4) : 0;
        data = PciValue(GpuBdf, cap+((ctl & 0x80) ? 12 : 8), 2);
        mask = (ctl & 0x100) ? PciValue(GpuBdf, cap+((ctl & 0x80) ? 16 : 12), 4) : 0;
        CoreLog(NULL, "FILTER_MSI phase=%s cap=0x%02x control=0x%04x enabled=%u address=%08x:%08x data=0x%04x mask=0x%08x\n",
            phase, cap, ctl, ctl&1, hi, lo, data, mask);
    } else CoreLog(NULL, "FILTER_MSI phase=%s cap=0x%02x unavailable\n", phase, cap);
    cap = Capability(GpuBdf, 0x11);
    if (cap && cap <= 0xf4) {
        ctl = PciValue(GpuBdf, cap+2, 2);
        CoreLog(NULL, "FILTER_MSIX phase=%s cap=0x%02x control=0x%04x enabled=%u functionMask=%u table=0x%08x pba=0x%08x\n",
            phase, cap, ctl, !!(ctl&0x8000), !!(ctl&0x4000),
            PciValue(GpuBdf, cap+4, 4), PciValue(GpuBdf, cap+8, 4));
    } else CoreLog(NULL, "FILTER_MSIX phase=%s cap=0x%02x unavailable\n", phase, cap);
}
static void *CoreHostAlloc(gpu *g, u64 bytes)
{
    ULONG i; PVOID result;
    UNREFERENCED_PARAMETER(g);
    if (!CoreBudget() || !bytes || bytes > 2 * 1024 * 1024) return NULL;
    for (i = 0; i < RTL_NUMBER_OF(ActiveCore->Host); ++i) if (!ActiveCore->Host[i].Cpu) break;
    if (i == RTL_NUMBER_OF(ActiveCore->Host)) return NULL;
    result = ExAllocatePool2(POOL_FLAG_NON_PAGED, (SIZE_T)bytes, 'hUMC');
    ActiveCore->Host[i].Cpu = result; return result;
}
static void CoreHostFree(gpu *g, void *cpu)
{
    ULONG i; UNREFERENCED_PARAMETER(g);
    for (i = 0; i < RTL_NUMBER_OF(ActiveCore->Host); ++i) if (ActiveCore->Host[i].Cpu == cpu && cpu) {
        ExFreePoolWithTag(cpu, 'hUMC'); ActiveCore->Host[i].Cpu = NULL; return;
    }
    if (cpu) ActiveCore->Fault = TRUE;
}
static void *CoreDmaAlloc(gpu *g, u64 bytes, u64 *address)
{
    ULONG rounded; CORE_DMA_SLOT *slot;
    UNREFERENCED_PARAMETER(g);
    if (!address || !CoreBudget() || !bytes || bytes > CMP_ARENA_BYTES - PAGE_SIZE ||
        ActiveCore->Slots == RTL_NUMBER_OF(ActiveCore->Dma)) return NULL;
    rounded = ((ULONG)bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (rounded > CMP_ARENA_BYTES - ActiveCore->Used) return NULL;
    slot = &ActiveCore->Dma[ActiveCore->Slots++];
    slot->Cpu = (PUCHAR)ActiveCore->Buffer + ActiveCore->Used;
    slot->Bytes = (ULONG)bytes; slot->Live = TRUE;
    *address = (u64)ActiveCore->Logical.QuadPart + ActiveCore->Used;
    ActiveCore->Used += rounded; ++ActiveCore->Live;
    RtlZeroMemory(slot->Cpu, rounded); KeMemoryBarrier(); return slot->Cpu;
}
static void CoreDmaFree(gpu *g, void *cpu, u64 bytes, u64 address)
{
    ULONG i; UNREFERENCED_PARAMETER(g);
    for (i = 0; i < ActiveCore->Slots; ++i) {
        CORE_DMA_SLOT *slot = &ActiveCore->Dma[i];
        if (slot->Cpu == cpu && slot->Live && bytes == slot->Bytes &&
            address == (u64)ActiveCore->Logical.QuadPart + ((PUCHAR)cpu - (PUCHAR)ActiveCore->Buffer)) {
            slot->Live = FALSE; --ActiveCore->Live; return;
        }
    }
    ActiveCore->Fault = TRUE;
}
static int CorePciRead(gpu *g, u32 offset, u32 *value)
{
    UNREFERENCED_PARAMETER(g);
    if (!CoreBudget() || !value || offset > 252 || (offset & 3)) return -1;
    *value = PciValue(GpuBdf, offset, 4);
    return *value == MAXULONG ? -1 : 0;
}
static BOOLEAN CorePciWrite(ULONG offset, ULONG width, ULONG value)
{
    if (!CoreBudget()) return FALSE;
    /* Only fixed BARs, saved command and PCIe TLS/ASPM controls. */
    ActiveCore->Touched = TRUE;
    return HalSetBusDataByOffset(PCIConfiguration, GpuBdf >> 8, Slot(GpuBdf),
        &value, offset, width) == width && PciValue(GpuBdf, offset, width) == value;
}
static BOOLEAN CoreLinkReady(ULONG bdf, ULONG cap)
{
    ULONG v = PciValue(bdf, cap + 0x12, 2);
    return v != 65535 && (v & 15) && (v & 0x3f0) && !(v & 0x800);
}
static BOOLEAN CoreReady(void)
{
    ULONG i, boot, progress, memory;
    if (!CoreBudget() || PciValue(GpuBdf, 0, 4) != 0x220d10de ||
        !CoreLinkReady(GpuBdf, GpuCap) || !CoreLinkReady(BridgeBdf, BridgeCap)) return FALSE;
    for (i = 0; i < 6; ++i) if (PciValue(GpuBdf, 0x10 + i*4, 4) != SavedBars[i]) return FALSE;
    if (PciValue(GpuBdf, 4, 2) != SavedCommand || ioread32(GpuMapping) != ActiveCore->Boot0) return FALSE;
    boot = ioread32((PUCHAR)GpuMapping + 0x118128);
    progress = ioread32((PUCHAR)GpuMapping + 0x118234);
    memory = ioread32((PUCHAR)GpuMapping + 0x100ce0);
    return !InvalidWord(boot) && (boot & 1) && !InvalidWord(progress) &&
        (progress & 255) == 255 && !InvalidWord(memory) && (memory & 0x3f0);
}
static int CoreSbr(void)
{
    ULONG control, i, stable = 0, observed; LARGE_INTEGER interval;
    ULONGLONG sbrBegin = KeQueryInterruptTime(), readyBegin, readyDeadline;
    const ULONG settleMs = 250, pollMs = 50, stableNeeded = 3;
    if (!CoreBudget()) return -1;
    control = PciValue(BridgeBdf, 0x3e, 2) & ~0x400u;
    if (control > 65535 || (control & 0x40)) return -1;
    ++ActiveCore->SbrCount; CorePciEvidence("before-sbr");
    ActiveCore->Touched = TRUE;
    if (!NT_SUCCESS(BridgeWord(0x3e, control | 0x40))) return -1;
    observed = PciValue(BridgeBdf, 0x3e, 2);
    /* Always deassert even on deadline/fault. Do not leave the subtree reset. */
    interval.QuadPart = -1000000; KeDelayExecutionThread(KernelMode, FALSE, &interval);
    if (!NT_SUCCESS(BridgeWord(0x3e, control & ~0x40u)) ||
        PciValue(BridgeBdf, 0x3e, 2) != (control & ~0x40u)) return -1;
    if (observed != (control | 0x40)) return -1;
    CoreDelay(100);
    for (i = 0; i < 46; ++i) {
        if (!CoreBudget()) return -1;
        if (PciValue(GpuBdf, 0, 4) == 0x220d10de && CoreLinkReady(GpuBdf, GpuCap) &&
            CoreLinkReady(BridgeBdf, BridgeCap)) break;
        CoreDelay(20);
    }
    if (i == 46) return -1;
    for (i = 0; i < 6; ++i) if (!CorePciWrite(0x10 + i*4, 4, SavedBars[i])) return -1;
    if (!CorePciWrite(4, 2, SavedCommand)) return -1;
    readyBegin = KeQueryInterruptTime();
    readyDeadline = readyBegin + 8050ULL * 10000; // Same ~8 second recovery budget.
    CoreDelay(settleMs);
    // Three samples 50ms apart retain the verified minimum 100ms stable window.
    // A slow bootstrap keeps polling; never release DMA based on elapsed time alone.
    for (i = 0; KeQueryInterruptTime() < readyDeadline; ++i) {
        if (!CoreBudget()) return -1;
        stable = CoreReady() ? stable + 1 : 0;
        if (stable == stableNeeded) {
            CorePciEvidence("after-sbr"); CoreLog(NULL, "FILTER_SBR_READY\n");
            CoreLog(NULL, "FILTER_SBR_TIMING sbr=%u elapsedMs=%llu readyMs=%llu polls=%u settleMs=%u pollMs=%u stableSamples=%u\n",
                ActiveCore->SbrCount, (KeQueryInterruptTime()-sbrBegin)/10000,
                (KeQueryInterruptTime()-readyBegin)/10000, i+1, settleMs, pollMs, stableNeeded);
            return 0;
        }
        CoreDelay(pollMs);
    }
    return -1;
}
static int CoreHandoff(gpu *g)
{
    KIRQL old; UNREFERENCED_PARAMETER(g);
    CoreLog(NULL, "FILTER_HANDOFF_BEGIN\n");
    if (CoreSbr() || CoreSbr()) return -1;
    KeAcquireSpinLock(&TraceLock, &old); ++UnlockResult.Handoffs; KeReleaseSpinLock(&TraceLock, old);
    CoreLog(NULL, "FILTER_HANDOFF_COMPLETE\n"); return 0;
}
static NTSTATUS CoreBind(FILTER_EXTENSION *ext)
{
    ULONG bus, slot, bridge = MAXULONG, count = 0, buses, i, probes = 0;
    ULONGLONG scanBegin = KeQueryInterruptTime();
    CMP_TARGET target; NTSTATUS status; BOOLEAN assigned = FALSE;
    /* A direct upstream bridge, and no other function in its entire SBR subtree. */
    for (bus = 0; bus < 256; ++bus) {
        if (!CoreBudget()) return STATUS_IO_TIMEOUT;
        for (slot = 0; slot < 256; slot += 8) {
            ULONG base = (bus << 8) | slot, id, header, fn, functions;
            id = PciValue(base, 0, 4); ++probes;
            if (!(id & 65535) || (id & 65535) == 65535) continue;
            header = PciValue(base, 0x0e, 1);
            if (header >= 255) return STATUS_DEVICE_CONFIGURATION_ERROR;
            functions = (header & 0x80) ? 8 : 1;
            for (fn = 0; fn < functions; ++fn) {
                ULONG bdf = base | fn;
                if (fn) { id = PciValue(bdf, 0, 4); ++probes; }
                if (!(id & 65535) || (id & 65535) == 65535 || PciValue(bdf, 8, 4) >> 16 != 0x0604) continue;
                buses = PciValue(bdf, 0x18, 4);
                if (((buses >> 8) & 255) == (ext->Bdf >> 8)) { bridge = bdf; ++count; }
            }
        }
    }
    CoreLog(NULL, "FILTER_TOPOLOGY_SCAN probes=%u elapsedMs=%llu bridges=%u\n",
        probes, (KeQueryInterruptTime()-scanBegin)/10000, count);
    if (count != 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
    buses = PciValue(bridge, 0x18, 4);
    if (((buses >> 16) & 255) < (ext->Bdf >> 8)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (bus = ext->Bdf >> 8; bus <= ((buses >> 16) & 255); ++bus)
        for (slot = 0; slot < 256; ++slot) {
            ULONG bdf = (bus << 8) | slot, id = PciValue(bdf, 0, 4);
            if (bdf != ext->Bdf && (id & 65535) != 65535 && (id & 65535) != 0)
                return STATUS_DEVICE_BUSY;
        }
    McfgLength = 0; HardwareInitialize(); target.Gpu = ext->Bdf; target.Bridge = bridge;
    status = BindHardware(&target); if (!NT_SUCCESS(status)) return status;
    for (i = 0; i < ext->MemoryCount; ++i)
        if (ext->Memory[i] == GpuPhysical && ext->MemoryLength[i] >= CMP_BAR0_BYTES) assigned = TRUE;
    if (!assigned || (SavedCommand & 6) != 6 || !(PciValue(bridge, 4, 2) & 4))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    ActiveCore->Boot0 = READ_REGISTER_ULONG((PULONG)GpuMapping);
    ActiveCore->GpuWidth = (PciValue(GpuBdf, GpuCap + 0x12, 2) >> 4) & 63;
    ActiveCore->BridgeWidth = (PciValue(BridgeBdf, BridgeCap + 0x12, 2) >> 4) & 63;
    if (!ActiveCore->GpuWidth || !ActiveCore->BridgeWidth) return STATUS_DEVICE_NOT_READY;
    CoreLog(NULL, "FILTER_BIND gpu=0x%04x bridge=0x%04x BAR0=0x%llx\n", GpuBdf, BridgeBdf, GpuPhysical);
    return STATUS_SUCCESS;
}
static NTSTATUS CoreAllocateArena(FILTER_EXTENSION *ext)
{
    DEVICE_DESCRIPTION description; PDMA_ADAPTER adapter; ULONG maps;
    RtlZeroMemory(&description, sizeof(description));
    description.Version = DEVICE_DESCRIPTION_VERSION3; description.Master = TRUE;
    description.Dma32BitAddresses = TRUE; description.InterfaceType = PCIBus;
    description.BusNumber = ext->Bdf >> 8; description.DmaWidth = Width32Bits;
    description.DmaSpeed = Compatible; description.MaximumLength = CMP_ARENA_BYTES;
    description.DmaAddressWidth = 32;
    adapter = IoGetDmaAdapter(ext->Pdo, &description, &maps);
    if (!adapter) return STATUS_NOT_SUPPORTED;
    ActiveCore->Adapter = adapter;
    if (!adapter->DmaOperations || adapter->DmaOperations->Size <
        FIELD_OFFSET(DMA_OPERATIONS, FreeCommonBuffer) + sizeof(PFREE_COMMON_BUFFER) ||
        !adapter->DmaOperations->PutDmaAdapter || !adapter->DmaOperations->AllocateCommonBuffer ||
        !adapter->DmaOperations->FreeCommonBuffer) return STATUS_NOT_SUPPORTED;
    ActiveCore->Buffer = adapter->DmaOperations->AllocateCommonBuffer(adapter, CMP_ARENA_BYTES, &ActiveCore->Logical, TRUE);
    if (!ActiveCore->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
    if ((ULONGLONG)ActiveCore->Logical.QuadPart > 0x100000000ULL - CMP_ARENA_BYTES ||
        (ActiveCore->Logical.QuadPart & (PAGE_SIZE - 1))) return STATUS_CONFLICTING_ADDRESSES;
    CoreLog(NULL, "FILTER_DMA_ARENA device=0x%llx physical=0x%llx bytes=%u real-PDO-common-buffer\n",
        ActiveCore->Logical.QuadPart, MmGetPhysicalAddress(ActiveCore->Buffer).QuadPart, CMP_ARENA_BYTES);
    return STATUS_SUCCESS;
}
static BOOLEAN CoreGen2(void)
{
    ULONG ctl, i, stable = 0, g, r;
    if ((PciValue(GpuBdf, GpuCap + 2, 2) & 15) < 2 ||
        (PciValue(BridgeBdf, BridgeCap + 2, 2) & 15) < 2 ||
        !(PciValue(GpuBdf, GpuCap + 0x2c, 4) & 2)) return FALSE;
    ctl = PciValue(GpuBdf, GpuCap + 0x30, 2);
    if (!CorePciWrite(GpuCap + 0x30, 2, (ctl & ~15u) | 2)) return FALSE;
    ctl = PciValue(BridgeBdf, BridgeCap + 0x30, 2);
    if (!NT_SUCCESS(BridgeWord(BridgeCap + 0x30, (ctl & ~15u) | 2))) return FALSE;
    ctl = PciValue(GpuBdf, GpuCap + 0x10, 2);
    if (!CorePciWrite(GpuCap + 0x10, 2, ctl & ~3u)) return FALSE;
    ctl = PciValue(BridgeBdf, BridgeCap + 0x10, 2);
    if ((ctl & 0x10) || !NT_SUCCESS(BridgeWord(BridgeCap + 0x10, (ctl & ~3u) | 0x20))) return FALSE;
    for (i = 0; i < 50; ++i) {
        CoreDelay(100); if (!CoreBudget()) return FALSE;
        g = PciValue(GpuBdf, GpuCap + 0x12, 2); r = PciValue(BridgeBdf, BridgeCap + 0x12, 2);
        stable = (g & 15) == 2 && (r & 15) == 2 && !((g | r) & 0x800) &&
            ((g >> 4) & 63) >= ActiveCore->GpuWidth && ((r >> 4) & 63) >= ActiveCore->BridgeWidth ? stable+1 : 0;
        if (stable == 3) return TRUE;
    }
    return FALSE;
}
static BOOLEAN CoreCanReleaseForPnp(void)
{
    ULONG cpu, riscv, dma, ctl;
    /* This pinned core submits host DMA only through GSP/SEC2. A verified
     * whole-subtree SBR after their LAST write discards those host references.
     * Require fresh bootstrap/identity, no live slots and stopped/idle GSP;
     * do not infer quiescence from live==0 or inaccessible SEC2 alone. */
    cpu = GspRead(GpuMapping, 0x100); riscv = GspRead(GpuMapping, 0x1388);
    dma = GspRead(GpuMapping, 0x118); ctl = GspRead(GpuMapping, 0x10c);
    CoreLog(NULL, "FILTER_PNP_DMA_RELEASE_GATE sbr=%u lastFalconWriteSbr=%u live=%u cpu=0x%08x riscv=0x%08x dma=0x%08x ctl=0x%08x\n",
        ActiveCore->SbrCount, ActiveCore->LastFalconWriteSbr, ActiveCore->Live, cpu, riscv, dma, ctl);
    return CoreBudget() && CoreReady() && !ActiveCore->Live &&
        ActiveCore->SbrCount > ActiveCore->LastFalconWriteSbr &&
        !InvalidWord(cpu) && (cpu&0x10) && !InvalidWord(riscv) && !(riscv&0x80) &&
        !InvalidWord(dma) && (dma&0x100003)==2 && !InvalidWord(ctl) && !(ctl&6);
}
static NTSTATUS ResumeUnlock(FILTER_EXTENSION *ext, BOOLEAN sbrOnly, BOOLEAN staged)
{
    NTSTATUS status; ULONG i, ctl, gpuLink = 0, bridgeLink = 0; u32 after = 0; int ret = -1, scan; const plm_target *target;
    post_gate gate; gpu *g; KIRQL old; RESUME_CORE *ctx;
    ULONGLONG begin = KeQueryInterruptTime();
    CoreStage(2);
    ctx = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ctx), 'uUMC');
    if (!ctx) {
        KeAcquireSpinLock(&TraceLock, &old);
        UnlockResult.Status = (ULONG)STATUS_INSUFFICIENT_RESOURCES; UnlockResult.CoreReturn = -1;
        KeReleaseSpinLock(&TraceLock, old); return STATUS_INSUFFICIENT_RESOURCES;
    }
    ActiveCore = ctx; ctx->Deadline = begin + 120000ULL * 10000;
    ctx->Pdo = ext->Pdo; ObReferenceObject(ctx->Pdo);
    ctx->StageBegin = begin; ctx->StageNumber = 2;
    CoreLog(NULL, "FILTER_SOURCE_CORE=v5.1 native-Win64 filter=0.6.7 budgetMs=120000 mode=%s\n",
#ifdef CMP_UNLOCK_SESSION
        "session-post-start-ioctl"
#else
        staged ? "pnp-staged-unlock" : sbrOnly ? "single-sbr-no-firmware" : "full-unlock-boot-arena"
#endif
    );
    status = CoreBind(ext); if (!NT_SUCCESS(status)) goto done;
    ctl = GspRead(GpuMapping, 0x100);
    i = GspRead(GpuMapping, 0x1388);
    if (InvalidWord(ctl) || !(ctl & 0x10) || InvalidWord(i) || (i & 0x80) || !GspIdle(GpuMapping)) {
        CoreLog(NULL, "FILTER_PRE_RESET_REFUSED GSP_CPU=0x%08x RISCV=0x%08x DMA=0x%08x\n", ctl, i, GspRead(GpuMapping, 0x118));
        status = STATUS_DEVICE_NOT_READY; goto done;
    }
    if (sbrOnly) {
        CoreStage(4); ret = CoreSbr(); if (ret) goto core_failed;
        CoreStage(12); status = STATUS_SUCCESS;
        CoreLog(NULL, "FILTER_SINGLE_SBR_COMPLETED: no firmware, no CPU start, no DMA adapter/buffer; NVIDIA recovery remains unverified\n");
        goto done;
    }
    CoreStage(3); status = CoreAllocateArena(ext); if (!NT_SUCCESS(status)) goto done;
    g = &ctx->Gpu; g->bar0 = (u64)GpuMapping; g->device_id = 0x220d;
    g->host_alloc = CoreHostAlloc; g->host_free = CoreHostFree;
    g->dma_alloc = CoreDmaAlloc; g->dma_alloc_low = CoreDmaAlloc; g->dma_free = CoreDmaFree;
    g->pci_read = CorePciRead; g->delay_ms = CoreDelay; g->reset = CoreHandoff; g->log = CoreLog;
    CoreStage(4); ret = CoreHandoff(g); if (ret) goto core_failed;
    CoreStage(5); ret = do_permissive_with_handoff(g, 1, CoreHandoff); if (ret) goto core_failed;
    CoreStage(6); ret = do_permissive_with_handoff(g, 2, CoreHandoff); if (ret) goto core_failed;
    CoreStage(7);
    for (i = 0; i <= 9; ++i) {
        target = NULL; scan = pcie_find_first_closed_plm(g, &target);
        if (!scan) break;
        if (scan != 1 || !target || i == 9) { ret = -1; goto core_failed; }
        CoreLog(g, "FILTER_PLM offset=0x%08x name=%s\n", target->offset, target->name);
        ret = ga102_booter_open_plm_with_handoff(g, target->offset, target->name, &after, CoreHandoff);
        if (ret || after != MAXULONG) goto core_failed;
    }
    CoreStage(8); ret = pcie_run_pre_reset_group(g, 2); if (ret) goto core_failed;
    if ((PciValue(GpuBdf, GpuCap + 2, 2) & 15) < 2 ||
        (PciValue(BridgeBdf, BridgeCap + 2, 2) & 15) < 2 ||
        !(PciValue(GpuBdf, GpuCap + 0x2c, 4) & 2)) { ret = -1; goto core_failed; }
    ctl = PciValue(BridgeBdf, BridgeCap + 0x30, 2);
    if (!NT_SUCCESS(BridgeWord(BridgeCap + 0x30, (ctl & ~15u) | 2))) { ret = -1; goto core_failed; }
    ret = CoreHandoff(g); if (ret) goto core_failed;
    CoreStage(9); ret = pcie_check_post_reset_gate(g, 2, &gate); if (ret) goto core_failed;
    ret = pcie_restore_post_reset_group(g, 2, &gate); if (ret) goto core_failed;
    CoreStage(10); if (!CoreGen2()) { ret = -1; goto core_failed; }
    CoreStage(11);
    ret = pcie_find_first_closed_plm(g, &target);
    if (ret || ioread32((PUCHAR)GpuMapping+0x82381c) != 0x88888888 ||
        !quadro_source_valid(ioread32((PUCHAR)GpuMapping+QUADRO_OFFSET)) ||
        ioread32((PUCHAR)GpuMapping+0x823820) != 8 || ioread32((PUCHAR)GpuMapping+0x823830) != 4 ||
        !(ioread32((PUCHAR)GpuMapping+0x8860c) & 1) || ctx->Fault || ctx->Live) {
        ret = -1; goto core_failed;
    }
    if (staged && !CoreCanReleaseForPnp()) { ret = -1; goto core_failed; }
    ret = 0; status = STATUS_SUCCESS; if (!staged) CoreStage(12);
    CoreLog(g, staged ? "FILTER_FULL_UNLOCK_VERIFIED_BEFORE_PNP_HANDOFF\n" : "FILTER_FULL_UNLOCK_VERIFIED_BEFORE_UPPER_D0_COMPLETION\n"); goto done;
core_failed:
    if (!ret) ret = -1;
    status = ctx->Expired ? STATUS_IO_TIMEOUT : STATUS_DEVICE_HARDWARE_ERROR;
    CoreLog(NULL, "FILTER_UNLOCK_FAILED core=%d live=%u fault=%u deadline=%u\n", ret, ctx->Live, ctx->Fault, ctx->Expired);
done:
    if (ctx->StageBegin) CoreLog(NULL, "FILTER_STAGE_TIMING stage=%u elapsedMs=%llu sbr=%u terminal=1\n",
        ctx->StageNumber, (KeQueryInterruptTime()-ctx->StageBegin)/10000, ctx->SbrCount);
    if (Bound) CorePciEvidence("before-upper-completion");
    if (ctx->Gpu.host_free && (rebuilt_vbios_kernel_release_cache(&ctx->Gpu) || ctx->Fault)) {
        ctx->Fault = TRUE;
        if (NT_SUCCESS(status)) { status = STATUS_DEVICE_HARDWARE_ERROR; ret = -1; }
        CoreLog(NULL, "FILTER_VBIOS_CACHE_OWNERSHIP_FAILED\n");
    }
    CoreLog(NULL, "FILTER_UNLOCK_END status=0x%08x core=%d live=%u hardwareWritten=%u\n", (ULONG)status, ret, ctx->Live, ctx->Touched);
    if (Bound) { gpuLink = PciValue(GpuBdf, GpuCap + 0x12, 2); bridgeLink = PciValue(BridgeBdf, BridgeCap + 0x12, 2); }
    KeAcquireSpinLock(&TraceLock, &old);
    UnlockResult.CoreReturn = ret; UnlockResult.Status = (ULONG)status;
    UnlockResult.ElapsedMs = (ULONG)((KeQueryInterruptTime() - begin) / 10000);
    UnlockResult.DeviceAddress = (ULONGLONG)ctx->Logical.QuadPart;
    UnlockResult.Physical = ctx->Buffer ? (ULONGLONG)MmGetPhysicalAddress(ctx->Buffer).QuadPart : 0;
    UnlockResult.LiveAllocations = ctx->Live;
    UnlockResult.LinkGpu = gpuLink;
    UnlockResult.LinkBridge = bridgeLink;
    if (NT_SUCCESS(status)) UnlockResult.Flags |= 1;
    if (ctx->Touched) UnlockResult.Flags |= 2;
    if (ctx->Expired) UnlockResult.Flags |= 16;
    if ((ctx->Touched && !NT_SUCCESS(status)) || ctx->Live) UnlockResult.Flags |= 4;
    if (ctx->Touched && ctx->Buffer) UnlockResult.Flags |= 32;
    if (sbrOnly) UnlockResult.Flags |= 64;
    if (staged) UnlockResult.Flags |= 512;
    KeReleaseSpinLock(&TraceLock, old);
    HardwareCleanup();
    for (i = 0; i < RTL_NUMBER_OF(ctx->Host); ++i)
        if (ctx->Host[i].Cpu) ExFreePoolWithTag(ctx->Host[i].Cpu, 'hUMC');
    if ((ctx->Touched && !NT_SUCCESS(status)) || ctx->Live) {
        RetainedCore = ctx; InterlockedExchange(&GpuQuarantined, 1);
        CoreLog(NULL, "FILTER_UNLOCK_QUARANTINED: fail D0; retain PDO and DMA until reboot\n");
    } else if (staged && NT_SUCCESS(status)) {
        if (ctx->Buffer) ctx->Adapter->DmaOperations->FreeCommonBuffer(ctx->Adapter, CMP_ARENA_BYTES, ctx->Logical, ctx->Buffer, TRUE);
        if (ctx->Adapter) ctx->Adapter->DmaOperations->PutDmaAdapter(ctx->Adapter);
        ObDereferenceObject(ctx->Pdo); ExFreePoolWithTag(ctx, 'uUMC');
        CoreLog(NULL,
#ifdef CMP_UNLOCK_SESSION
            "SESSION_DMA_RELEASED: common-buffer/adapter/extra-PDO released before IOCTL completion; safe to close session handle and rebind NVIDIA\n"
#else
            "FILTER_PNP_DMA_RELEASED: common-buffer/adapter/extra-PDO reference released before START completion; no retained arena\n"
#endif
        );
#ifndef CMP_UNLOCK_SESSION
        StagedBdf = ext->Bdf; InterlockedExchange(&StagedPending, 1);
#endif
        KeAcquireSpinLock(&TraceLock, &old);
        UnlockResult.Flags = (UnlockResult.Flags & ~32u) | 1024;
        UnlockResult.Stage = 12;
        KeReleaseSpinLock(&TraceLock, old);
    } else if (ctx->Touched) {
        /* 0.5.0 reclaimed this arena/adapter as soon as core handoff returned.
         * The core's live==0 proves its suballocation ownership is closed;
         * it does not prove teardown is compatible with NVIDIA's subsequent
         * WDDM resume/domain ownership. For the 0x116 lifetime experiment,
         * retain the unchanged bytes, DMA adapter and PDO for this boot.
         * This is NOT evidence that teardown caused the reported TDR.
         * No reuse or second acquisition is allowed before reboot. */
        RetainedCore = ctx;
        if (ctx->Buffer)
            CoreLog(NULL, "FILTER_DMA_RETAINED_SUCCESS bytes=%u; no FreeCommonBuffer/PutDmaAdapter; no reuse before reboot\n", CMP_ARENA_BYTES);
        else CoreLog(NULL, "FILTER_SINGLE_SBR_ONE_SHOT: retain PDO reference; another destructive experiment requires reboot\n");
    } else {
        if (ctx->Buffer) ctx->Adapter->DmaOperations->FreeCommonBuffer(ctx->Adapter, CMP_ARENA_BYTES, ctx->Logical, ctx->Buffer, TRUE);
        if (ctx->Adapter && ctx->Adapter->DmaOperations && ctx->Adapter->DmaOperations->PutDmaAdapter)
            ctx->Adapter->DmaOperations->PutDmaAdapter(ctx->Adapter);
        ObDereferenceObject(ctx->Pdo); ExFreePoolWithTag(ctx, 'uUMC');
    }
    ActiveCore = NULL; return status;
}
