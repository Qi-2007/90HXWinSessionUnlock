/* Source-owned Windows GA102 unlock core ABI. */
#ifndef REBUILT_CORE_H
#define REBUILT_CORE_H
#include <stddef.h>
#if defined(_MSC_VER)
#define __builtin_offsetof(type, member) offsetof(type, member)
#endif
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
#define QUADRO_OFFSET 0x823808u
#define QUADRO_MASK 0x03900bbbu
#define QUADRO_TARGET 0x03900888u
static inline int quadro_source_valid(u32 v) {
    return v!=0xffffffffu && (v&0xffff0000u)!=0xbadf0000u &&
        (v&QUADRO_MASK)==QUADRO_TARGET;
}
typedef struct gpu gpu;
typedef int (*handoff_fn)(gpu *);
struct gpu {
    u64 bar0;
    u16 device_id;
    unsigned char reserved[30];
    void *(*host_alloc)(gpu *, u64);
    void (*host_free)(gpu *, void *);
    void *(*dma_alloc)(gpu *, u64, u64 *);
    void (*dma_free)(gpu *, void *, u64, u64);
    void *(*dma_alloc_low)(gpu *, u64, u64 *);
    int (*pci_read)(gpu *, u32, u32 *);
    void (*delay_ms)(u64);
    int (*reset)(gpu *);
    void (*log)(gpu *, const char *, ...);
    u64 tail;
};
_Static_assert(sizeof(gpu) == 120, "GPU ABI size");
_Static_assert(__builtin_offsetof(gpu, log) == 104, "GPU log ABI");
_Static_assert(__builtin_offsetof(gpu, pci_read) == 80, "GPU PCI ABI");
typedef struct { u32 offset, reserved; const char *name; } plm_target;
typedef struct { u32 lctrl, local_rate, vsec; } post_gate;
typedef struct { u32 offset, value; u64 reserved; } feature_write;
typedef struct { void *cpu; u64 physical; u32 size, reserved; } dma_buffer;
typedef struct { dma_buffer radix, signature, bootbin, metadata, aux; } boot_inputs;
typedef struct { u32 word[18]; dma_buffer dma; } boot_observation;
typedef struct { u64 word[3]; } vbios_image;
typedef struct { u64 word[14]; } fwsec_image;
typedef struct { const void *image; u32 code_offset, data_offset; u64 data_size; } boot_layout;
typedef struct {
    u32 mode, arch, alternate, managed, legacy, reserved;
    handoff_fn handoff;
    unsigned char payload[24];
    u32 phase, reclaim, target, firmware_complete, handoff_complete, recovery, retained;
    u32 fwsec_retained, target_ready, attempts, execution, submitted;
    boot_inputs inputs;
    boot_observation observed;
    unsigned char reserved_backend[24];
    vbios_image vbios;
    fwsec_image fwsec;
} feature_session;
_Static_assert(sizeof(feature_session)==480,"Pinned session size");
_Static_assert(__builtin_offsetof(feature_session,retained)==80,"Session retention ABI");
_Static_assert(sizeof(boot_inputs)==120 && sizeof(boot_observation)==96,"Boot input/result ABI");
_Static_assert(__builtin_offsetof(feature_session,vbios)==344,"Session VBIOS ABI");
_Static_assert(__builtin_offsetof(feature_session,fwsec)==368,"Session FWSEC ABI");
int rebuilt_session_identity(gpu *, feature_session *);
int rebuilt_gr_power(gpu *);
void rebuilt_media_probe(gpu *);
int rebuilt_sec2_accessible(gpu *);
int rebuilt_pmu_devinit(gpu *);
int rebuilt_vbios_acquire(gpu *,vbios_image *);
int rebuilt_vbios_fwsec(gpu *,vbios_image *,fwsec_image *);
void rebuilt_vbios_release(gpu *,vbios_image *);
#ifdef CMP_KERNEL_CORE
int rebuilt_vbios_kernel_release_cache(gpu *);
#endif
int rebuilt_fwsec_execute(gpu *,fwsec_image *,u64);
int rebuilt_reset_managed(gpu *);
int rebuilt_inputs_prepare(gpu *,boot_inputs *,const void *,u32,const boot_layout *);
int rebuilt_inputs_metadata(gpu *,boot_inputs *,u64,u32,dma_buffer *);
void rebuilt_inputs_release(gpu *,boot_inputs *);
int rebuilt_booter_target(gpu *,u64,boot_observation *,u32,u32,u32,u32);
int rebuilt_local_memory_size(gpu *,u64 *);
int rebuilt_find_signature(gpu *,const void *,u32,u32 *,u32 *);
void rebuilt_fwsec_free(gpu *,fwsec_image *);
extern const unsigned char firmware_plm_image[], firmware_plm_signature[];
extern const unsigned char firmware_feature_image[], firmware_feature_signature[];
int rebuilt_inputs_metadata_feature(gpu *,boot_inputs *,u32,dma_buffer *);
void rebuilt_meta_signature(void *,u64,u32);
int rebuilt_feature_stage(gpu *,u32,u32,const feature_write *,u32,const char *,feature_session *,u32 *);
int rebuilt_fwsec_stage(gpu *,feature_session *);
void rebuilt_session_cleanup(gpu *,feature_session *);
int rebuilt_plm_transaction(gpu *,u32,const char *,u32 *,handoff_fn);
extern u32 ioread32(const void *);
extern void iowrite32(u32, void *);
int do_permissive_with_handoff(gpu *, u32, handoff_fn);
int ga102_booter_open_plm_with_handoff(gpu *, u32, const char *, u32 *, handoff_fn);
int pcie_find_first_closed_plm(gpu *, const plm_target **);
int pcie_run_pre_reset_group(gpu *, u32);
int pcie_check_post_reset_gate(gpu *, u32, post_gate *);
int pcie_restore_post_reset_group(gpu *, u32, post_gate *);
int gsp_falcon_reset(gpu *);
#endif
