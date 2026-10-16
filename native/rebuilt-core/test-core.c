#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { u32 offset,value; } reg;
static reg regs[40];
static int nr, writes, pci_calls, disappear, ignore_writes, backend_calls, handed, backend_error;
static int delayed, scrub_pending, lose_quadro_on_handoff;
static reg trace[16];
static int cleanups, cleanup_recovery, cleanup_retained, stage_failure, fwsec_failure, identity_failure, handoff_failure;
static int identity_calls, fail_identity_at, corrupt_target, cleanup_handoff, fwsec_calls;
static u32 offsets[]={0x8200fc,0x8e1b0,0x8e1b4,0x8e1b8,0x8e1bc,0x88fe8,0x88fec,0x88ff0,0x8e1f0};
static void set(u32 offset,u32 value) {
    for (int i=0;i<nr;i++) if (regs[i].offset==offset) { regs[i].value=value; return; }
    if(nr>=40) abort();
    regs[nr++]=(reg){offset,value};
}
u32 ioread32(const void *address) {
    u32 offset=(u32)((u64)address-0x100000000ull);
    if(offset==0x11010c && scrub_pending>0) {scrub_pending--;return 6;}
    for(int i=0;i<nr;i++) if(regs[i].offset==offset) return regs[i].value;
    return 0;
}
void iowrite32(u32 value,void *address) {
    if(writes<16) trace[writes]=(reg){(u32)((u64)address-0x100000000ull),value};
    writes++;
    if(!ignore_writes) set((u32)((u64)address-0x100000000ull),value);
}
static int pci(gpu *g,u32 offset,u32 *value) {
    (void)g;(void)offset;pci_calls++;
    *value=disappear && pci_calls>1 ? 0xffffffffu : 0x220d10deu;
    return 0;
}
static int handoff(gpu *g) { (void)g;handed++;if(lose_quadro_on_handoff)set(QUADRO_OFFSET,0);return handoff_failure; }
static void delay(u64 ms) { delayed+=(int)ms; }
static gpu device;
static void reset(void) {
    memset(&device,0,sizeof(device));device.bar0=0x100000000ull;device.device_id=0x220d;device.pci_read=pci;
    nr=writes=pci_calls=disappear=ignore_writes=backend_calls=handed=backend_error=0;
    delayed=scrub_pending=lose_quadro_on_handoff=0;device.delay_ms=delay;
    cleanups=cleanup_recovery=cleanup_retained=stage_failure=fwsec_failure=identity_failure=handoff_failure=0;
    identity_calls=fail_identity_at=corrupt_target=cleanup_handoff=fwsec_calls=0;
    set(0,0x17200000);set(0x82381c,0x88888888);set(0x823820,8);set(0x823830,4);set(0x823834,0);
    set(0x8860c,1);
    set(QUADRO_OFFSET,QUADRO_TARGET);
    for(int i=0;i<9;i++) set(offsets[i],0xffffffffu);
}
/* Firmware stubs verify source routing only, not actual firmware execution. */
int rebuilt_session_identity(gpu *g,feature_session *s) {
    (void)g;(void)s;identity_calls++;
    return identity_failure || (fail_identity_at && identity_calls==fail_identity_at);
}
int rebuilt_sec2_accessible(gpu *g) { (void)g;return 1; }
int rebuilt_pmu_devinit(gpu *g) { (void)g;return -1; }
int rebuilt_fwsec_stage(gpu *g,feature_session *s) { (void)g;(void)s;fwsec_calls++;return fwsec_failure; }
void rebuilt_session_cleanup(gpu *g,feature_session *s) {
    (void)g;cleanups++;cleanup_recovery=(int)s->recovery;cleanup_retained=(int)s->retained;
    cleanup_handoff=(int)s->handoff_complete;
}
int rebuilt_gr_power(gpu *g) { (void)g;return 0; }
void rebuilt_media_probe(gpu *g) { (void)g; }
int rebuilt_feature_stage(gpu *g,u32 mode,u32 plm,const feature_write *plan,u32 count,
                          const char *name,feature_session *s,u32 *after) {
    (void)g;(void)name;(void)mode;
    if(s->managed!=1 || s->arch!=0 || s->reclaim!=1 || !s->handoff) abort();
    backend_calls++;
    if(backend_error) return backend_error;
    s->retained=1;
    if(stage_failure) return stage_failure;
    for(u32 i=0;i<count;i++) set(plan[i].offset,plan[i].value);
    set(0x823834,0);set(plm,corrupt_target?0xffffffcfu:0xffffffffu);*after=0xffffffffu;
    return 0;
}
int rebuilt_plm_transaction(gpu *g,u32 off,const char *name,u32 *after,handoff_fn cb) {
    (void)name;backend_calls++;
    if(backend_error) return backend_error;
    if(cb(g)) return -7;
    set(off,0xffffffffu);*after=0xffffffffu;return 0;
}
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
int main(void) {
    const plm_target *target=0;
    post_gate gate;
    reset();
    CHECK(!do_permissive_with_handoff(&device,1,handoff));
    CHECK(!do_permissive_with_handoff(&device,2,handoff));
    CHECK(!backend_calls && !handed && !writes);
    reset();set(QUADRO_OFFSET,0x00100283);set(0x823804,0xffffffffu);
    CHECK(!do_permissive_with_handoff(&device,1,handoff) && !backend_calls && !handed);
    CHECK(ioread32((void *)(device.bar0+QUADRO_OFFSET))==QUADRO_TARGET && writes==1);
    CHECK(!do_permissive_with_handoff(&device,1,handoff) && writes==1);
    reset();set(QUADRO_OFFSET,0x00100283);set(0x823804,0xffffffcfu);
    CHECK(!do_permissive_with_handoff(&device,1,handoff) && backend_calls==1 && handed==1);
    reset();set(QUADRO_OFFSET,0);set(0x823804,0xffffffffu);ignore_writes=1;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-4 && !handed);
    u32 bad_quadro[]={0xffffffffu,0xbadf5620u};
    for(int i=0;i<2;i++) {
        reset();set(QUADRO_OFFSET,bad_quadro[i]);set(0x823804,0xffffffffu);
        CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && !writes && !backend_calls);
    }
    for(int trial=0;trial<1000;trial++) {
        reset();u32 old=(u32)trial*1664525u+1013904223u;
        if(old==0xffffffffu || (old&0xffff0000u)==0xbadf0000u) continue;
        set(QUADRO_OFFSET,old);set(0x823804,0xffffffffu);
        CHECK(!do_permissive_with_handoff(&device,1,handoff));
        CHECK(ioread32((void *)(device.bar0+QUADRO_OFFSET))==((old&~QUADRO_MASK)|QUADRO_TARGET));
    }
    for(u32 mode=1;mode<=2;mode++) {
        reset();set(mode==1?0x82381c:0x823830,0);
        CHECK(!do_permissive_with_handoff(&device,mode,handoff));
        CHECK(backend_calls==1 && handed==1);
        CHECK(cleanups==1 && cleanup_handoff && !cleanup_retained && !cleanup_recovery && identity_calls==5);
        reset();set(mode==1?0x82381c:0x823830,0);backend_error=-1001;
        CHECK(do_permissive_with_handoff(&device,mode,handoff)==-1001 && !handed);
    }
    reset();CHECK(do_permissive_with_handoff(&device,1,0)==-1 && !backend_calls);
    reset();set(0x82381c,0);handoff_failure=-7;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && handed==1 && cleanups==1 && cleanup_recovery && cleanup_retained);
    reset();set(0x82381c,0);stage_failure=-2;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-2 && !handed && cleanup_recovery && cleanup_retained);
    reset();set(0x82381c,0);fwsec_failure=-1001;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && !backend_calls && !handed && cleanups==1 && cleanup_recovery);
    reset();set(0x82381c,0);identity_failure=-1;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && !backend_calls && !handed && cleanups==1);
    for(int failed=2;failed<=5;failed++) {
        reset();set(0x82381c,0);fail_identity_at=failed;
        CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && cleanups==1 && cleanup_recovery && cleanup_retained);
        CHECK(handed==(failed>=4?1:0) && !cleanup_handoff);
    }
    reset();set(0x82381c,0);corrupt_target=1;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-2 && !handed && cleanup_retained && cleanup_recovery);
    reset();set(0x82381c,0);lose_quadro_on_handoff=1;
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && handed==1 && cleanup_retained && cleanup_recovery);
    reset();set(0x82381c,0);set(0x600,0xffffffffu);set(0x200,0x2002);
    CHECK(!do_permissive_with_handoff(&device,1,handoff) && fwsec_calls==1 && backend_calls==1 && handed==1);
    reset();set(0x82381c,0);set(0x600,0xbadf5620u);set(0x200,0x2002);
    CHECK(do_permissive_with_handoff(&device,1,handoff)==-1001 && !fwsec_calls && !backend_calls);
    CHECK(do_permissive_with_handoff(&device,3,handoff)==-1);
    reset();CHECK(!pcie_find_first_closed_plm(&device,&target) && !target && pci_calls==2);
    for(int i=0;i<9;i++) {
        reset();set(offsets[i],0xffffff8f);
        CHECK(pcie_find_first_closed_plm(&device,&target)==1 && target->offset==offsets[i]);
        u32 after=0;
        CHECK(!ga102_booter_open_plm_with_handoff(&device,offsets[i],"test",&after,handoff));
        CHECK(after==0xffffffffu && backend_calls==1 && handed==1);
        CHECK(!ga102_booter_open_plm_with_handoff(&device,offsets[i],"test",&after,handoff));
        CHECK(backend_calls==1 && handed==1);
    }
    reset();disappear=1;CHECK(pcie_find_first_closed_plm(&device,&target)==-1 && !target);
    reset();set(offsets[2],0xbadf5620);CHECK(pcie_find_first_closed_plm(&device,&target)==1 && target->offset==offsets[2]);
    reset();set(0x82381c,0xbadf5620);CHECK(!do_permissive_with_handoff(&device,1,handoff) && backend_calls==1);
    reset();set(0x823834,0xbadf5620);CHECK(!do_permissive_with_handoff(&device,2,handoff) && backend_calls==1);
    reset();u32 after=0;CHECK(ga102_booter_open_plm_with_handoff(&device,0x1234,"bad",&after,handoff)==-1 && !backend_calls);
    u32 policy_offsets[]={0x8872c,0x8841c,0x8c2c0,0x8c040,0x8c1c0};
    u32 masks[]={0xf,0x7800,4,0xc0000,0xf0000};
    u32 sets[]={6,0x2800,0,0x80000,0x20000};
    u32 rng=123456789;
    for(int trial=0;trial<1000;trial++) {
        reset();u32 values[5];
        for(int i=0;i<5;i++) {rng=rng*1664525u+1013904223u;values[i]=rng&0x7fffffffu;set(policy_offsets[i],values[i]);}
        CHECK(!pcie_run_pre_reset_group(&device,2));
        for(int i=0;i<5;i++) CHECK(ioread32((void *)(device.bar0+policy_offsets[i]))==((values[i]&~masks[i])|sets[i]));
        int previous=writes;
        CHECK(!pcie_run_pre_reset_group(&device,2) && writes==previous);
        CHECK(!pcie_restore_post_reset_group(&device,2,&gate) && writes==previous);
    }
    reset();ignore_writes=1;CHECK(pcie_run_pre_reset_group(&device,2)==-1 && writes==1);
    reset();CHECK(pcie_run_pre_reset_group(&device,3)==-1 && !writes);
    CHECK(pcie_restore_post_reset_group(&device,2,0)==-1);
    reset();CHECK(!pcie_check_post_reset_gate(&device,2,&gate));
    set(0x8c1c0,3u<<20);CHECK(pcie_check_post_reset_gate(&device,2,&gate)==-1);
    set(0x8c1c0,0);set(0x8860c,0);CHECK(pcie_check_post_reset_gate(&device,2,&gate)==-1);
    set(0x8860c,0xbadf5620);CHECK(pcie_check_post_reset_gate(&device,2,&gate)==-1);
    reset();set(0x1100f4,0x400);set(0x111668,0x10);set(0x1103c0,0x80);
    scrub_pending=3;
    CHECK(!gsp_falcon_reset(&device) && !backend_calls && delayed==4 && writes==4);
    CHECK(trace[0].offset==0x111668 && trace[0].value==1);
    CHECK(trace[1].offset==0x1103c0 && trace[1].value==0x81);
    CHECK(trace[2].offset==0x1103c0 && trace[2].value==0x80);
    CHECK(trace[3].offset==0x110084 && trace[3].value==0x17200000);
    reset();set(0x11010c,6);
    CHECK(gsp_falcon_reset(&device)==-1 && delayed==10000 && writes==2);
    u32 reset_offsets[]={0x1100f4,0x111668,0x1103c0,0x11010c};
    for(int i=0;i<4;i++) {
        reset();set(reset_offsets[i],0xbadf1234);
        CHECK(gsp_falcon_reset(&device)==-1001 && !backend_calls && !delayed);
    }
    reset();device.delay_ms=0;CHECK(gsp_falcon_reset(&device)==-1 && !writes);
    reset();disappear=1;CHECK(gsp_falcon_reset(&device)==-1001);
    puts("REBUILT_SOURCE_TEST_PASSED: 1000 randomized policies; source feature transactions, stage failure, retained DMA, all identity/handoff gates, GSP reset and readback failures; stub firmware only.");
    return 0;
}
