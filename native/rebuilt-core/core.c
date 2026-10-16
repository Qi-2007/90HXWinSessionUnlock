/* Independently written from static ABI/policy observations and public register
 * descriptions. No GPU firmware code is reconstructed here. See README.md. */
#include "core.h"

const char rebuilt_core_version[] = "GA102 source v5 / source-only host core / pinned GPU firmware data";
static const plm_target targets[] = {
    {0x8200fc,0,"PEX_PAD"}, {0x8e1b0,0,"XP3G_PLM0"},
    {0x8e1b4,0,"XP3G_PLM1"}, {0x8e1b8,0,"XP3G_PLM2"},
    {0x8e1bc,0,"XP3G_PLM3"}, {0x88fe8,0,"XVE_PLM0"},
    {0x88fec,0,"XVE_PLM1"}, {0x88ff0,0,"XVE_PLM2"},
    {0x8e1f0,0,"XP3G_PLM16"}
};
static int invalid(u32 v) { return v == 0xffffffffu || (v & 0xffff0000u) == 0xbadf0000u; }
static u32 rd(gpu *g, u32 offset) { return ioread32((const void *)(g->bar0+offset)); }
static void wr(gpu *g, u32 offset, u32 value) { iowrite32(value,(void *)(g->bar0+offset)); }
static int context(gpu *g) { return g && g->bar0 && g->device_id == 0x220d; }
static int identity(gpu *g) {
    u32 id=0, boot;
    if (!context(g) || !g->pci_read || g->pci_read(g,0,&id) || id != 0x220d10deu) return -1;
    boot=rd(g,0);
    return invalid(boot) || (boot&0x1f000000u)!=0x17000000u ||
        (boot&0x00f00000u)!=0x00200000u ? -1 : 0;
}
static int write_verified(gpu *g, u32 offset, u32 value) {
    wr(g,offset,value);
    return rd(g,offset)==value ? 0 : -1;
}
static int apply(gpu *g, u32 offset, u32 clear, u32 set) {
    u32 old=rd(g,offset), next;
    if (invalid(old)) return -1;
    next=(old&~clear)|set;
    /* Skip equal policy values; always verify actual writes. */
    return old==next ? 0 : write_verified(g,offset,next);
}

int pcie_find_first_closed_plm(gpu *g, const plm_target **out) {
    const plm_target *found=0;
    if (!out) return -1;
    *out=0;
    if (identity(g)) return -1;
    for (u32 i=0;i<sizeof(targets)/sizeof(targets[0]);i++) {
        u32 value=rd(g,targets[i].offset);
        /* Match 469dc0c: all ones is open; any other value (including
         * inaccessible BADF) needs the protected firmware transaction. */
        if (value!=0xffffffffu) { found=&targets[i]; break; }
    }
    /* Revalidate after the scan: an absent GPU must not look like all-open. */
    if (identity(g)) return -1;
    *out=found;
    return found ? 1 : 0;
}
static int policy(gpu *g, u32 generation, post_gate *out) {
    /* Only the existing Windows Gen2 path is supported, not unverified Gen3. */
    if (generation!=2 || identity(g)) return -1;
    if (apply(g,0x8872c,0xf,6) || apply(g,0x8841c,0x7800,0x2800) ||
        apply(g,0x8c2c0,4,0) || apply(g,0x8c040,0xc0000,0x80000) ||
        apply(g,0x8c1c0,0xf0000,0x20000)) return -1;
    if (identity(g)) return -1;
    if (out) {
        out->lctrl=rd(g,0x8c1c0); out->local_rate=(out->lctrl>>20)&15;
        out->vsec=rd(g,0x8860c);
        if (invalid(out->lctrl) || invalid(out->vsec)) return -1;
    }
    return 0;
}
int pcie_run_pre_reset_group(gpu *g, u32 generation) { return policy(g,generation,0); }
int pcie_check_post_reset_gate(gpu *g, u32 generation, post_gate *out) {
    if (!out || generation!=2 || identity(g)) return -1;
    out->lctrl=rd(g,0x8c1c0); out->local_rate=(out->lctrl>>20)&15;
    out->vsec=rd(g,0x8860c);
    if (invalid(out->lctrl) || invalid(out->vsec) || identity(g)) return -1;
    return !(out->vsec&1) || out->local_rate==3 ? -1 : 0;
}
int pcie_restore_post_reset_group(gpu *g, u32 generation, post_gate *out) {
    if (!out) return -1;
    return policy(g,generation,out);
}

int ga102_booter_open_plm_with_handoff(gpu *g, u32 offset, const char *name,
                                      u32 *after, handoff_fn handoff) {
    int known=0;
    if (!context(g) || !handoff || !after) return -1;
    for (u32 i=0;i<sizeof(targets)/sizeof(targets[0]);i++)
        if (offset==targets[i].offset) known=1;
    if (!known || identity(g)) return -1;
    u32 before=rd(g,offset);
    if (before==0xffffffffu) {
        if (identity(g)) return -1;
        *after=before;
        if (g->log) g->log(g,"REBUILT_PLM_ALREADY_OPEN offset=0x%08x\n",offset);
        return 0;
    }
    /* Source transaction owns handoff and DMA evidence-based reclamation. */
    int status=rebuilt_plm_transaction(g,offset,name,after,handoff);
    if (status) return status;
    return identity(g) || *after!=0xffffffffu || rd(g,offset)!=0xffffffffu ? -1 : 0;
}

/* 4c32b72 EFI .text+11bc0: FEAT PLM exact-open, then masked RMW.
 * READOUT 0x823814 is diagnostic; success proves source override only. */
static int quadro_apply(gpu *g) {
    u32 old, next, after;
    if(identity(g)) return -1001;
    if(rd(g,0x823804)!=0xffffffffu) return -2;
    old=rd(g,QUADRO_OFFSET);
    if(invalid(old)) return -1001;
    next=(old&~QUADRO_MASK)|QUADRO_TARGET;
    if(old!=next) wr(g,QUADRO_OFFSET,next);
    after=rd(g,QUADRO_OFFSET);
    if(g->log) g->log(g,"REBUILT_QUADRO_SOURCE old=%08x write=%08x after=%08x readout=%08x (diagnostic only)\n",
                      old,next,after,rd(g,0x823814));
    if(identity(g) || invalid(after)) return -1001;
    return quadro_source_valid(after) ? 0 : -4;
}
static int feature_verify(gpu *g,u32 plm,const feature_write *plan,u32 count) {
    if (rd(g,plm)!=0xffffffffu) return -2;
    for(u32 i=0;i<count;i++) if(rd(g,plan[i].offset)!=plan[i].value) return -2;
    if(plm==0x823804 && !quadro_source_valid(rd(g,QUADRO_OFFSET))) return -4;
    if(plm==0x823b04) {
        u32 raster=rd(g,0x823834);
        if(invalid(raster) || (raster&3)) return -2;
    }
    return 0;
}
static int feature_transaction(gpu *g,u32 mode,handoff_fn handoff) {
    static const feature_write compute[]={{0x823820,8,0},{0x82381c,0x88888888u,0}};
    static const feature_write graphics[]={{0x823830,4,0}};
    const feature_write *plan=mode==1?compute:graphics;
    u32 count=mode==1?2:1, plm=mode==1?0x823804:0x823b04, after=0;
    const char *label=mode==1?"compute":"graphics";
    feature_session s={0};
    int status;
    s.mode=mode;s.managed=1;s.handoff=handoff;s.reclaim=1;
    if(g->log) g->log(g,"REBUILT_FEATURE_TRANSACTION_BEGIN mode=%u\n",mode);
    status=rebuilt_session_identity(g,&s);
    if(status) {status=-1001;goto done;}
    /* Original PMU failure is diagnostic; FWSEC may still make progress. */
    if(!rebuilt_sec2_accessible(g)) {
        int pmu=rebuilt_pmu_devinit(g);
        if(pmu && g->log) g->log(g,"REBUILT_PMU_DIAGNOSTIC status=%d\n",pmu);
    }
    u32 device_enable=rd(g,0x600), enable=rd(g,0x200);
    if(g->log) g->log(g,"REBUILT_PMC_STATE enable=0x%08x device_enable=0x%08x\n",enable,device_enable);
    /* 469dc0c .text+10460 rejects all-ones on PMC_ENABLE, but only BADF
     * on PMC_DEVICE_ENABLE. That diagnostic register may legitimately read
     * all-ones; it is not written or used to build a policy here. */
    if(invalid(enable) || (device_enable&0xffff0000u)==0xbadf0000u) {
        if(g->log) g->log(g,"REBUILT_FEATURE_ABORT phase=pmc-register-check\n");
        status=-1001;goto done;
    }
    if((enable&0x2002)!=0x2002) {
        wr(g,0x200,enable|0x2002);
        if(!g->delay_ms) {status=-1001;goto done;}
        g->delay_ms(5);
        if(invalid(rd(g,0x200))) {status=-1001;goto done;}
    }
    status=rebuilt_fwsec_stage(g,&s);
    if(status) goto done;
    status=rebuilt_feature_stage(g,mode,plm,plan,count,label,&s,&after);
    if(status) goto done;
    if(mode==1) {status=quadro_apply(g);if(status) goto done;}
    if(mode==2) { (void)rebuilt_gr_power(g);rebuilt_media_probe(g); }
    if(rebuilt_session_identity(g,&s) || feature_verify(g,plm,plan,count) ||
       rebuilt_session_identity(g,&s) || handoff(g) ||
       rebuilt_session_identity(g,&s) || feature_verify(g,plm,plan,count) ||
       rebuilt_session_identity(g,&s)) {status=-1001;goto done;}
    s.handoff_complete=1;s.retained=0;status=0;
done:
    if(status && (status==-1001 || s.retained)) s.recovery=1;
    /* Cleanup remains evidence-aware: never free retained firmware DMA here. */
    rebuilt_session_cleanup(g,&s);
    if(g->log) g->log(g,"REBUILT_FEATURE_TRANSACTION_END mode=%u status=%d\n",mode,status);
    return status;
}
int do_permissive_with_handoff(gpu *g, u32 mode, handoff_fn handoff) {
    u32 first, second;
    if (!context(g) || !handoff || (mode!=1 && mode!=2) || identity(g)) return -1;
    if (mode==1) {
        first=rd(g,0x82381c); second=rd(g,0x823820);
        if (!invalid(first) && !invalid(second) && first==0x88888888u && second==8) {
            u32 quadro=rd(g,QUADRO_OFFSET);
            if(quadro_source_valid(quadro)) {
                if (g->log) g->log(g,"REBUILT_COMPUTE_QUADRO_ALREADY_ENABLED\n");
                return identity(g);
            }
            /* Complete a partial unlock without Booter or SBR only if the
             * FEAT protection gate is already exactly open. */
            if(rd(g,0x823804)==0xffffffffu) return quadro_apply(g);
        }
    } else {
        first=rd(g,0x823830); second=rd(g,0x823834);
        if (!invalid(first) && !invalid(second) && first==4 && !(second&3)) {
            if (g->log) g->log(g,"REBUILT_GRAPHICS_ALREADY_ENABLED\n");
            return identity(g);
        }
    }
    int status=feature_transaction(g,mode,handoff);
    if (status) return status;
    if (identity(g)) return -1;
    if (mode==1)
        return rd(g,0x82381c)==0x88888888u && rd(g,0x823820)==8 &&
            quadro_source_valid(rd(g,QUADRO_OFFSET)) ? 0 : -1;
    first=rd(g,0x823830); second=rd(g,0x823834);
    return first==4 && !invalid(second) && !(second&3) ? 0 : -1;
}
/* GA102 legacy reset observed at .text+9020,9160,9554 in pinned 469dc0c.
 * This replaces the exported reset only. Backend-internal direct calls to
 * its local reset implementation still belong to the firmware transaction.
 * Fail closed on inaccessible MMIO instead of using BADF as register data. */
static int falcon_switch(gpu *g) {
    u32 value=rd(g,0x111668);
    if (invalid(value)) return -1001;
    if (value&0x10) {
        wr(g,0x111668,(value&~0x10u)|1);
        g->delay_ms(1);
        value=rd(g,0x111668);
        if (invalid(value)) return -1001;
    }
    return 0;
}
int gsp_falcon_reset(gpu *g) {
    u32 value;
    if (!context(g) || !g->delay_ms || identity(g)) return -1;
    if (g->log) g->log(g,"REBUILT_GSP_RESET_BEGIN\n");
    /* Initial read plus at most 150 refreshes; original readiness polling
     * has no delay and continues even when readiness never arrives. */
    value=rd(g,0x1100f4);
    for (u32 i=0; !(value&0x80000400u) && i<150; i++) {
        if (invalid(value)) return -1001;
        value=rd(g,0x1100f4);
    }
    if (invalid(value) || falcon_switch(g)) return -1001;
    value=rd(g,0x1103c0);
    if (invalid(value)) return -1001;
    wr(g,0x1103c0,value|1);
    for (u32 i=0;i<10;i++) if (invalid(rd(g,0x1103c0))) return -1001;
    value=rd(g,0x1103c0);
    if (invalid(value)) return -1001;
    wr(g,0x1103c0,value&~1u);
    for (u32 i=0;i<10;i++) if (invalid(rd(g,0x1103c0))) return -1001;
    for (u32 i=0;i<10000;i++) {
        value=rd(g,0x11010c);
        if (invalid(value)) return -1001;
        if (!(value&6)) {
            /* Legacy GA102 writes BOOT0 into the falcon mailbox before
             * completing. Preserve this otherwise non-obvious write. */
            value=rd(g,0);
            if (invalid(value)) return -1001;
            wr(g,0x110084,value);
            if (falcon_switch(g)) return -1001;
            if (identity(g)) return -1001;
            if (g->log) g->log(g,"REBUILT_GSP_RESET_COMPLETE\n");
            return 0;
        }
        g->delay_ms(1);
    }
    if (g->log) g->log(g,"REBUILT_GSP_SCRUB_TIMEOUT status=0x%08x\n",value);
    return -1;
}
