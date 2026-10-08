/* One stopped-Falcon DMEM block. The ordinary probe never resets the engine;
 * InitializeStoppedGsp is a separate explicit destructive experiment.
 * No CPU start, SBR or firmware execution.
 * Anything possibly referenced by unquiescent DMA is retained until reboot. */
static LONG GpuQuarantined;
static PDMA_ADAPTER RetainedAdapter;
static PVOID RetainedBuffer;
static ULONG GspRead(PVOID mmio, ULONG offset)
{ return READ_REGISTER_ULONG((PULONG)((PUCHAR)mmio + 0x110000 + offset)); }
static VOID GspWrite(PVOID mmio, ULONG offset, ULONG value)
{ WRITE_REGISTER_ULONG((PULONG)((PUCHAR)mmio + 0x110000 + offset), value); }
static BOOLEAN InvalidWord(ULONG v)
{ return v == MAXULONG || (v & 0xffff0000) == 0xbadf0000 || v == 0xdead5ec1 || v == 0xdead5ec2 || v == 0xdeaddead; }
static BOOLEAN GspIdle(PVOID mmio)
{ ULONG v = GspRead(mmio, 0x118); return !InvalidWord(v) && (v & 3) == 2; }
static VOID ReadDmem(PVOID mmio, ULONG *data)
{ ULONG i; GspWrite(mmio, 0x1c0, 1u << 25); for (i = 0; i < 64; ++i) data[i] = GspRead(mmio, 0x1c4); }
static VOID WriteDmem(PVOID mmio, const ULONG *data)
{ ULONG i; GspWrite(mmio, 0x1c0, 1u << 24); for (i = 0; i < 64; ++i) GspWrite(mmio, 0x1c4, data[i]); }
/* Capture the same readback used for verification; no extra DMEM transaction.
 * Keep the primary failure separate from cleanup evidence. */
static BOOLEAN MatchDmem(PVOID mmio, const ULONG *expected, CMP_FILTER_PROBE *result,
    ULONG reason, BOOLEAN cleanup)
{
    ULONG actual[64], i, first = MAXULONG, matches = 0;
    ReadDmem(mmio, actual); result->DmemPortAfter = GspRead(mmio, 0x1c0);
    RtlCopyMemory(cleanup ? result->CleanupObserved : result->Observed, actual, 8 * sizeof(ULONG));
    for (i = 0; i < 64; ++i) {
        if (actual[i] == expected[i]) ++matches;
        else if (first == MAXULONG) first = i;
    }
    if (!cleanup) result->PioMatches = matches;
    if (first != MAXULONG) {
        if (cleanup) {
            result->CleanupReason = reason; result->CleanupIndex = first;
            result->CleanupExpected = expected[first]; result->CleanupActual = actual[first];
        } else {
            result->Reason = reason; result->FailureIndex = first;
            result->Expected = expected[first]; result->Actual = actual[first];
        }
    }
    return matches == 64;
}
static BOOLEAN CheckedGsp(PVOID mmio, ULONG offset, ULONG value)
{ GspWrite(mmio, offset, value); return GspRead(mmio, offset) == value; }
/* Explicit experiment only. Engine reset destroys the old firmware/DMEM
 * state; a successful later probe can restore only the post-reset state.
 * No SBR, SEC2, CPU start, firmware, privilege-mask or PCI writes. */
static NTSTATUS InitializeStoppedGsp(FILTER_EXTENSION *ext, CMP_FILTER_PROBE *result)
{
    PHYSICAL_ADDRESS bar; PVOID mmio; ULONG cpu, bcr, engine, cfg, ctl, riscv;
    NTSTATUS status = STATUS_DEVICE_NOT_READY; LARGE_INTEGER delay;
    ULONGLONG begin;
    result->Stage = 14; result->Flags |= 0x1000; result->ResetStage = 1;
    if (GpuQuarantined || ReadPci(ext, 0) != 0x220d10de || (ReadPci(ext, 4) & 6) != 6)
        return status;
    bar.QuadPart = (LONGLONG)result->Bar0;
    mmio = MmMapIoSpace(bar, 0x1000000, MmNonCached);
    if (!mmio) return STATUS_INSUFFICIENT_RESOURCES;
    engine = GspRead(mmio, 0x3c0); cpu = GspRead(mmio, 0x100);
    bcr = GspRead(mmio, 0x1668); cfg = GspRead(mmio, 0xf4);
    riscv = GspRead(mmio, 0x1388); ctl = GspRead(mmio, 0x10c);
    result->ResetEngineBefore = engine;
    if (READ_REGISTER_ULONG((PULONG)mmio) != result->Boot0 || InvalidWord(engine) ||
        InvalidWord(cpu) || !(cpu & 0x10) || (engine & 1) || InvalidWord(bcr) ||
        (bcr & 0x11) != 1 || InvalidWord(riscv) || (riscv & 0x80) ||
        InvalidWord(cfg) || (cfg & 0x1000) || InvalidWord(ctl) || (ctl & 6) || !GspIdle(mmio)) goto done;
    /* NVIDIA's pre-reset helper bounds RESET_READY waiting and allows its
     * timeout because hardware does not always set this bit. */
    delay.QuadPart = -10000; begin = KeQueryInterruptTime();
    while (!(cfg & 0x80000000) && KeQueryInterruptTime() - begin < 10000) {
        KeDelayExecutionThread(KernelMode, FALSE, &delay); cfg = GspRead(mmio, 0xf4);
        if (InvalidWord(cfg)) goto done;
    }
    if (cfg & 0x80000000) result->ResetFlags |= 1;
    /* Revalidate immediately before the first destructive write. */
    if (ReadPci(ext, 0) != 0x220d10de || READ_REGISTER_ULONG((PULONG)mmio) != result->Boot0 ||
        GspRead(mmio, 0x3c0) != engine || GspRead(mmio, 0x100) != cpu ||
        GspRead(mmio, 0x1668) != bcr || GspRead(mmio, 0x1388) != riscv || !GspIdle(mmio)) goto done;
    result->ResetStage = 2; result->Flags |= 0x800; result->ResetFlags |= 2;
    status = STATUS_DEVICE_HARDWARE_ERROR;
    if (!CheckedGsp(mmio, 0x3c0, engine | 1)) goto done;
    delay.QuadPart = -100000; KeDelayExecutionThread(KernelMode, FALSE, &delay);
    result->ResetStage = 3;
    if (!CheckedGsp(mmio, 0x3c0, engine & ~1u)) goto done;
    result->ResetFlags |= 4;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    result->ResetStage = 4; begin = KeQueryInterruptTime(); delay.QuadPart = -10000;
    do {
        /* Reset can select RISC-V and lock Falcon DMACTL/CPUCTL reads.
         * NVIDIA waits on HWCFG2 alone before selecting Falcon. */
        cfg = GspRead(mmio, 0xf4);
        if (InvalidWord(cfg)) goto done;
        if (!(cfg & 0x1000)) break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    } while (KeQueryInterruptTime() - begin < 1000000);
    if (cfg & 0x1000) { status = STATUS_IO_TIMEOUT; goto done; }
    result->ResetStage = 5; bcr = GspRead(mmio, 0x1668);
    riscv = GspRead(mmio, 0x1388);
    if (InvalidWord(bcr) || InvalidWord(riscv) || (riscv & 0x80)) goto done;
    result->ResetFlags |= 0x10;
    /* Request Falcon without asserting VALID in software. Preserve BRFETCH
     * and unrelated bits; hardware must report VALID within the deadline. */
    GspWrite(mmio, 0x1668, bcr & ~0x11u);
    result->ResetFlags |= 0x20;
    begin = KeQueryInterruptTime();
    do {
        bcr = GspRead(mmio, 0x1668);
        if (InvalidWord(bcr)) goto done;
        if ((bcr & 0x11) == 1) break;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    } while (KeQueryInterruptTime() - begin < 1000000);
    if ((bcr & 0x11) != 1) { status = STATUS_IO_TIMEOUT; goto done; }
    result->ResetFlags |= 0x40;
    ctl = GspRead(mmio, 0x10c);
    if (InvalidWord(ctl) || (ctl & 6)) goto done;
    result->ResetFlags |= 0x80;
    result->ResetStage = 6;
    if (!CheckedGsp(mmio, 0x84, result->Boot0)) goto done;
    cpu = GspRead(mmio, 0x100); riscv = GspRead(mmio, 0x1388);
    if (InvalidWord(cpu) || !(cpu & 0x30) || InvalidWord(riscv) || (riscv & 0x80) || !GspIdle(mmio)) goto done;
    result->ResetStage = 7; result->ResetFlags |= 8; status = STATUS_SUCCESS;
done:
    /* Always attempt release after assertion, but only with stable identity
     * and a readable engine register whose other bits still match. */
    if ((result->ResetFlags & 2) && !(result->ResetFlags & 4)) {
        ULONG current = GspRead(mmio, 0x3c0);
        if (ReadPci(ext, 0) == 0x220d10de && READ_REGISTER_ULONG((PULONG)mmio) == result->Boot0 &&
            !InvalidWord(current) && !((current ^ engine) & ~1u) &&
            CheckedGsp(mmio, 0x3c0, current & ~1u)) result->ResetFlags |= 4;
    }
    result->ResetEngineAfter = GspRead(mmio, 0x3c0);
    result->ResetBcrAfter = GspRead(mmio, 0x1668);
    result->ResetHwcfgAfter = GspRead(mmio, 0xf4);
    result->ResetCpuAfter = GspRead(mmio, 0x100);
    result->ResetDmaCtlAfter = GspRead(mmio, 0x10c);
    result->ResetRiscvAfter = GspRead(mmio, 0x1388);
    MmUnmapIoSpace(mmio, 0x1000000); return status;
}
static NTSTATUS GspTransfer(FILTER_EXTENSION *ext, PVOID mmio, ULONGLONG address,
    BOOLEAN outbound, BOOLEAN *pending, CMP_FILTER_PROBE *result)
{
    ULONG cpu = GspRead(mmio, 0x100), bcr = GspRead(mmio, 0x1668), cmd;
    ULONGLONG begin; LARGE_INTEGER delay;
    if (InvalidWord(cpu) || InvalidWord(bcr) || !(cpu & 0x30) || (bcr & 0x11) != 1 ||
        (ReadPci(ext, 4) & 6) != 6 || (address & 255) || address > 0x100000000ULL - 256)
        return STATUS_DEVICE_NOT_READY;
    if (!CheckedGsp(mmio, 0x110, (ULONG)(address >> 8)) || !CheckedGsp(mmio, 0x128, 0) ||
        !CheckedGsp(mmio, 0x114, 0) || !CheckedGsp(mmio, 0x11c, 0)) return STATUS_DEVICE_HARDWARE_ERROR;
    *pending = TRUE; result->Flags |= 0x100;
    GspWrite(mmio, 0x118, outbound ? 0x620 : 0x600);
    begin = KeQueryInterruptTime(); delay.QuadPart = -10000;
    do {
        cmd = GspRead(mmio, 0x118); result->DmaCommand = cmd;
        if (InvalidWord(cmd) || (cmd & 0x100000)) return STATUS_DEVICE_HARDWARE_ERROR;
        if ((cmd & 3) == 2) { *pending = FALSE; return STATUS_SUCCESS; }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    } while (KeQueryInterruptTime() - begin < 1000000); /* 100 ms; no retry */
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS GpuProbeD0(FILTER_EXTENSION *ext, CMP_FILTER_PROBE *result)
{
    const ULONG offsets[8] = {0x10c,0x110,0x114,0x11c,0x128,0x600,0x624,0x1c0};
    ULONG saved[8], original[64], pattern[64], actual[64], i, maps;
    ULONG bus, dev, fn, matches = 0; BOOLEAN upstream = FALSE;
    ULONGLONG scanBegin = KeQueryInterruptTime();
    PHYSICAL_ADDRESS bar, logical = {0}; PVOID mmio, buffer = NULL; PDMA_ADAPTER adapter = NULL;
    DEVICE_DESCRIPTION description; NTSTATUS status = STATUS_DEVICE_NOT_READY;
    BOOLEAN selected = FALSE, modified = FALSE, pending = FALSE, restored = TRUE;
    result->Stage = 7; result->Flags |= 0x200;
    if (GpuQuarantined) return STATUS_DEVICE_HARDWARE_ERROR;
    /* Read only: require the direct bridge's upstream BME, never enable it. */
    for (bus = 0; bus <= (ext->Bdf >> 8); ++bus) for (dev = 0; dev < 32; ++dev)
        for (fn = 0; fn < 8; ++fn) {
            FILTER_EXTENSION candidate = {0}; ULONG busses;
            candidate.Bdf = (bus << 8) | (dev << 3) | fn;
            if (KeQueryInterruptTime() - scanBegin > 1000000) return STATUS_IO_TIMEOUT;
            if (fn == 0 && (ReadPci(&candidate, 0) & 65535) == 65535) break;
            if ((ReadPci(&candidate, 8) >> 16) != 0x0604) continue;
            busses = ReadPci(&candidate, 0x18);
            if (((busses >> 8) & 255) != (ext->Bdf >> 8)) continue;
            if (++matches != 1 || !(ReadPci(&candidate, 4) & 4)) return STATUS_DEVICE_NOT_READY;
            upstream = TRUE;
        }
    if (!upstream || (ReadPci(ext, 4) & 6) != 6) return status;
    bar.QuadPart = (LONGLONG)result->Bar0; mmio = MmMapIoSpace(bar, 0x1000000, MmNonCached);
    if (!mmio) return STATUS_INSUFFICIENT_RESOURCES;
    result->GspCpu = GspRead(mmio, 0x100);
    result->Bcr = GspRead(mmio, 0x1668); result->DmaCommand = GspRead(mmio, 0x118);
    if (InvalidWord(result->GspCpu) || !(result->GspCpu & 0x30) || InvalidWord(result->Bcr) ||
        (result->Bcr & 0x11) != 1 || !GspIdle(mmio) || InvalidWord(GspRead(mmio, 0x10c)) ||
        (GspRead(mmio, 0x10c) & 6)) goto cleanup;
    for (i = 0; i < 8; ++i) { saved[i] = GspRead(mmio, offsets[i]); if (InvalidWord(saved[i])) goto cleanup; }
    selected = TRUE; result->Stage = 8; result->DmemPortBefore = saved[7];
    ReadDmem(mmio, original); result->DmemPortAfter = GspRead(mmio, 0x1c0);
    RtlCopyMemory(result->Original, original, sizeof(result->Original));
    for (i = 0; i < 64; ++i) {
        if (InvalidWord(original[i])) {
            result->Reason = 1; result->FailureIndex = i; result->Actual = original[i];
            RtlCopyMemory(result->Observed, original, sizeof(result->Observed));
            goto cleanup;
        }
        pattern[i] = original[i] ^ 0x9058a5a5 ^ (i * 0x10203);
    }
    modified = TRUE; WriteDmem(mmio, pattern);
    if (!MatchDmem(mmio, pattern, result, 2, FALSE)) goto cleanup;
    WriteDmem(mmio, original); if (!MatchDmem(mmio, original, result, 3, FALSE)) goto cleanup;
    result->Flags |= 0x10; result->Stage = 9;
    RtlZeroMemory(&description, sizeof(description)); description.Version = DEVICE_DESCRIPTION_VERSION3;
    description.Master = TRUE; description.Dma32BitAddresses = TRUE; description.DmaAddressWidth = 32;
    description.InterfaceType = PCIBus; description.BusNumber = ext->Bdf >> 8;
    description.DmaWidth = Width32Bits; description.DmaSpeed = Compatible; description.MaximumLength = 8192;
    adapter = IoGetDmaAdapter(ext->Pdo, &description, &maps);
    if (!adapter) { status = STATUS_NOT_SUPPORTED; goto cleanup; }
    if (!adapter->DmaOperations || adapter->DmaOperations->Size < FIELD_OFFSET(DMA_OPERATIONS, FreeCommonBuffer) + sizeof(PFREE_COMMON_BUFFER) ||
        !adapter->DmaOperations->AllocateCommonBuffer || !adapter->DmaOperations->FreeCommonBuffer || !adapter->DmaOperations->PutDmaAdapter) {
        status = STATUS_NOT_SUPPORTED; goto cleanup;
    }
    buffer = adapter->DmaOperations->AllocateCommonBuffer(adapter, 8192, &logical, TRUE);
    if (!buffer) { status = STATUS_INSUFFICIENT_RESOURCES; goto cleanup; }
    result->DeviceAddress = (ULONGLONG)logical.QuadPart; result->Physical = (ULONGLONG)MmGetPhysicalAddress(buffer).QuadPart;
    result->Bytes = 8192; result->Flags &= ~8u;
    if ((result->DeviceAddress & 255) || result->DeviceAddress > 0x100000000ULL - 8192) { status = STATUS_CONFLICTING_ADDRESSES; goto cleanup; }
    for (i = 0; i < 64; ++i) { ((ULONG *)buffer)[i] = pattern[i]; ((ULONG *)((PUCHAR)buffer + 4096))[i] = ~pattern[i]; }
    KeMemoryBarrier();
    if (!CheckedGsp(mmio, 0x10c, saved[0] & ~1u) || !CheckedGsp(mmio, 0x624, saved[6] | 0x80) ||
        !CheckedGsp(mmio, 0x600, (saved[5] & ~0x10007u) | 5)) { status = STATUS_DEVICE_HARDWARE_ERROR; goto cleanup; }
    result->Stage = 10; status = GspTransfer(ext, mmio, result->DeviceAddress, FALSE, &pending, result);
    if (!NT_SUCCESS(status)) goto cleanup;
    ReadDmem(mmio, actual);
    for (i = 0; i < 64; ++i) if (actual[i] == pattern[i]) ++result->InMatches;
    if (result->InMatches != 64) { status = STATUS_DATA_ERROR; goto cleanup; }
    result->Flags |= 0x20; result->Stage = 11;
    status = GspTransfer(ext, mmio, result->DeviceAddress + 4096, TRUE, &pending, result);
    if (!NT_SUCCESS(status)) goto cleanup;
    KeMemoryBarrier();
    for (i = 0; i < 64; ++i) if (((volatile ULONG *)((PUCHAR)buffer + 4096))[i] == pattern[i]) ++result->OutMatches;
    if (result->OutMatches != 64) { status = STATUS_DATA_ERROR; goto cleanup; }
    result->Flags |= 0x40;
cleanup:
    if (pending && !GspIdle(mmio)) { restored = FALSE; result->CleanupReason = 1; }
    else {
        pending = FALSE;
        if (modified) { WriteDmem(mmio, original); if (!MatchDmem(mmio, original, result, 2, TRUE)) restored = FALSE; }
        if (selected) for (i = 8; i > 0; --i) {
            ULONG readback;
            GspWrite(mmio, offsets[i-1], saved[i-1]); readback = GspRead(mmio, offsets[i-1]);
            if (readback != saved[i-1]) {
                restored = FALSE;
                if (!result->CleanupReason) {
                    result->CleanupReason = 3; result->CleanupIndex = offsets[i-1];
                    result->CleanupExpected = saved[i-1]; result->CleanupActual = readback;
                }
            }
        }
    }
    if (!restored) {
        /* Do not recycle or remap a potentially DMA-visible page. Fail D0
         * upward rather than declare NVIDIA restoration successful. */
        InterlockedExchange(&GpuQuarantined, 1); result->Flags |= 0x400;
        RetainedAdapter = adapter; RetainedBuffer = buffer;
        status = STATUS_DEVICE_HARDWARE_ERROR;
    } else {
        if (selected) result->Flags |= 0x80;
        if (buffer) { adapter->DmaOperations->FreeCommonBuffer(adapter, 8192, logical, buffer, TRUE); result->Flags |= 8; }
        if (adapter && adapter->DmaOperations && adapter->DmaOperations->PutDmaAdapter) adapter->DmaOperations->PutDmaAdapter(adapter);
        if (NT_SUCCESS(status)) result->Stage = 13;
    }
    MmUnmapIoSpace(mmio, 0x1000000); return status;
}
