#ifndef CMP_HOST_TEST
#include <aux_klib.h>
#endif
#include "../include/hardware_scope.h"

/* Legacy segment-0 transport, matching the previous HAL/ECAM implementation.
 * No caller-supplied physical mappings: derive BAR0 and ECAM from PCI/ACPI. */
static UCHAR Mcfg[44 + 16 * 256];
static ULONG McfgLength;
static BOOLEAN Bound;
static ULONG GpuBdf, BridgeBdf, GpuCap, BridgeCap, PmCap, SavedBars[6], SavedCommand;
static ULONGLONG GpuPhysical, BridgePhysical;
static PVOID GpuMapping, BridgeMapping;

static ULONG Slot(ULONG bdf)
{
    PCI_SLOT_NUMBER slot;
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = (bdf >> 3) & 31;
    slot.u.bits.FunctionNumber = bdf & 7;
    return slot.u.AsULONG;
}
static ULONG PciValue(ULONG bdf, ULONG offset, ULONG width)
{
    ULONG value = MAXULONG;
    if (HalGetBusDataByOffset(PCIConfiguration, bdf >> 8, Slot(bdf), &value, offset, width) != width)
        return MAXULONG;
    return width == 4 ? value : value & ((1u << (width * 8)) - 1);
}
static ULONG Capability(ULONG bdf, ULONG id)
{
    ULONG p = PciValue(bdf, 0x34, 1) & ~3u, i;
    for (i = 0; i < 48 && p >= 0x40 && p <= 0xfc; ++i) {
        ULONG v = PciValue(bdf, p, 2);
        if ((v & 255) == id) return p;
        p = (v >> 8) & ~3u;
    }
    return 0;
}
static VOID HardwareCleanup(void)
{
    Bound = FALSE;
    if (GpuMapping) MmUnmapIoSpace(GpuMapping, CMP_BAR0_BYTES);
    if (BridgeMapping) MmUnmapIoSpace(BridgeMapping, 4096);
    GpuMapping = BridgeMapping = NULL;
}
static VOID HardwareInitialize(void)
{
    ULONG length = 0, i, sum = 0;
    if (!NT_SUCCESS(AuxKlibInitialize())) return;
    if (!NT_SUCCESS(AuxKlibGetSystemFirmwareTable(0x41435049, 0x4746434d,
        Mcfg, sizeof(Mcfg), &length))) return;
    if (length < 60 || length > sizeof(Mcfg) || (length - 44) % 16 ||
        *(UNALIGNED ULONG *)(Mcfg + 0) != 0x4746434d ||
        *(UNALIGNED ULONG *)(Mcfg + 4) != length) return;
    for (i = 0; i < length; ++i) sum += Mcfg[i];
    if (!(sum & 255)) McfgLength = length;
}
static ULONGLONG EcamAddress(ULONG bdf)
{
    ULONG i, matches = 0, bus = bdf >> 8;
    ULONGLONG result = 0;
    for (i = 44; i < McfgLength; i += 16) {
        ULONGLONG base = *(UNALIGNED ULONGLONG *)(Mcfg + i);
        USHORT segment = *(UNALIGNED USHORT *)(Mcfg + i + 8);
        UCHAR first = Mcfg[i + 10], last = Mcfg[i + 11];
        if (!base || (base & 0xfffff) || base > 0x000fffffffffffffULL - 0x10000000 || first > last)
            return 0;
        if (!segment && bus >= first && bus <= last) {
            result = base + ((ULONGLONG)bus << 20) + ((ULONGLONG)(bdf & 255) << 12);
            ++matches;
        }
    }
    return matches == 1 ? result : 0;
}
static NTSTATUS BindHardware(const CMP_TARGET *target)
{
    ULONG low, type, buses, i, boot, command;
    PHYSICAL_ADDRESS address;
#ifdef CMP_PNP_DMA
    if (!PnpReady || target->Gpu != PnpBdf) return STATUS_NO_SUCH_DEVICE;
#endif
    if (target->Gpu > 0xffff || target->Bridge > 0xffff || target->Gpu == target->Bridge)
        return STATUS_INVALID_PARAMETER;
    if (Bound) return target->Gpu == GpuBdf && target->Bridge == BridgeBdf ?
        STATUS_SUCCESS : STATUS_DEVICE_BUSY;
    if (PciValue(target->Gpu, 0, 4) != 0x220d10de ||
        (PciValue(target->Gpu, 0x0e, 1) & 0x7f) != 0 ||
        PciValue(target->Bridge, 8, 4) >> 16 != 0x0604 ||
        (PciValue(target->Bridge, 0x0e, 1) & 0x7f) != 1)
        return STATUS_NO_SUCH_DEVICE;
    buses = PciValue(target->Bridge, 0x18, 4);
    if (((buses >> 8) & 255) != target->Gpu >> 8 || ((buses >> 16) & 255) < target->Gpu >> 8)
        return STATUS_INVALID_PARAMETER;
    low = PciValue(target->Gpu, 0x10, 4); type = low & 6;
    if (!low || low == MAXULONG || (low & 1) || (type != 0 && type != 4))
        return STATUS_INVALID_DEVICE_STATE;
    GpuPhysical = low & ~15u;
    if (type == 4) GpuPhysical |= (ULONGLONG)PciValue(target->Gpu, 0x14, 4) << 32;
#ifdef CMP_PNP_DMA
    if (GpuPhysical != PnpBar) return STATUS_DEVICE_CONFIGURATION_ERROR;
#endif
    command = PciValue(target->Gpu, 4, 2);
    if (!GpuPhysical || (GpuPhysical & (CMP_BAR0_BYTES - 1)) ||
        GpuPhysical > 0x000fffffffffffffULL - CMP_BAR0_BYTES ||
        command > 65535 || !(command & 2))
        return STATUS_INVALID_DEVICE_STATE;
    GpuCap = Capability(target->Gpu, 0x10); BridgeCap = Capability(target->Bridge, 0x10);
    PmCap = Capability(target->Gpu, 1);
    if (!GpuCap || GpuCap > 0xc0 || !BridgeCap || BridgeCap > 0xc0)
        return STATUS_NOT_SUPPORTED;
    BridgePhysical = EcamAddress(target->Bridge);
    if (!BridgePhysical) return STATUS_NOT_SUPPORTED;
#ifdef CMP_PNP_DMA
    if (Buffer && (ULONGLONG)PnpLogical.QuadPart < BridgePhysical + 4096 &&
        BridgePhysical < (ULONGLONG)PnpLogical.QuadPart + CMP_ARENA_BYTES)
        return STATUS_CONFLICTING_ADDRESSES;
#endif
    address.QuadPart = (LONGLONG)BridgePhysical;
    BridgeMapping = MmMapIoSpace(address, 4096, MmNonCached);
    if (!BridgeMapping) return STATUS_INSUFFICIENT_RESOURCES;
    /* Compare immutable identity/routing/capability fields before enabling writes. */
    if (READ_REGISTER_ULONG((PULONG)BridgeMapping) != PciValue(target->Bridge, 0, 4) ||
        READ_REGISTER_ULONG((PULONG)((PUCHAR)BridgeMapping + 8)) != PciValue(target->Bridge, 8, 4) ||
        READ_REGISTER_ULONG((PULONG)((PUCHAR)BridgeMapping + 0x18)) != buses ||
        READ_REGISTER_ULONG((PULONG)((PUCHAR)BridgeMapping + BridgeCap)) != PciValue(target->Bridge, BridgeCap, 4)) {
        HardwareCleanup(); return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    address.QuadPart = (LONGLONG)GpuPhysical;
    GpuMapping = MmMapIoSpace(address, CMP_BAR0_BYTES, MmNonCached);
    if (!GpuMapping) { HardwareCleanup(); return STATUS_INSUFFICIENT_RESOURCES; }
    boot = READ_REGISTER_ULONG((PULONG)GpuMapping);
    if ((boot & 0x1f000000) != 0x17000000 || (boot & 0xf00000) != 0x200000) {
        HardwareCleanup(); return STATUS_NO_SUCH_DEVICE;
    }
    for (i = 0; i < 6; ++i) SavedBars[i] = PciValue(target->Gpu, 0x10 + i * 4, 4);
    SavedCommand = command;
    GpuBdf = target->Gpu; BridgeBdf = target->Bridge; Bound = TRUE;
    return STATUS_SUCCESS;
}
static NTSTATUS BridgeWord(ULONG offset, ULONG value)
{
    ULONG mask = cmp_bridge_mask(BridgeCap, offset), old;
    PUSHORT word;
    if (!mask || value > 65535 ||
        (offset == BridgeCap + 0x30 && (PciValue(BridgeBdf, BridgeCap + 2, 2) & 15) < 2))
        return STATUS_ACCESS_DENIED;
    word = (PUSHORT)((PUCHAR)BridgeMapping + offset);
    old = READ_REGISTER_USHORT(word);
    if (offset == 0x3e) { old &= ~0x400u; value &= ~0x400u; }
    if ((old ^ value) & ~mask) return STATUS_ACCESS_DENIED;
    /* Exact 16-bit write: never replay adjacent W1C link status. */
    WRITE_REGISTER_USHORT(word, (USHORT)(offset == 0x3e ? value & ~0x400u : value));
    return STATUS_SUCCESS;
}
