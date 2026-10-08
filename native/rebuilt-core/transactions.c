/* Source-owned GA102 managed transactions and device DMA ownership. */
#include "core.h"
static u32 read_reg(gpu *g,u32 o) { return ioread32((const void *)(g->bar0+o)); }
static void write_reg(gpu *g,u32 o,u32 v) { iowrite32(v,(void *)(g->bar0+o)); }
static int bad(u32 v) { return v==0xffffffffu || (v&0xffff0000u)==0xbadf0000u; }

void rebuilt_session_cleanup(gpu *g,feature_session *s) {
    /* Managed GA102 never resets speculatively in cleanup. A submitted
     * descriptor before verified handoff must remain owned for this boot. */
    if(!s->handoff_complete && s->observed.dma.cpu) {s->recovery=1;s->retained=1;}
    if(s->retained) s->phase=6;
    else if(s->handoff_complete) {s->phase=4;s->reclaim=1;}
    else s->phase=s->recovery?6:5;
    if(g->log) g->log(g,"REBUILT_DMA_CLEANUP phase=%u handoff=%u retained=%u fwsec_retained=%u recovery=%u\n",
                       s->phase,s->handoff_complete,s->retained,s->fwsec_retained,s->recovery);
    if(s->retained) {
        if(!s->fwsec_retained) rebuilt_fwsec_free(g,&s->fwsec);
        rebuilt_vbios_release(g,&s->vbios);
        return;
    }
    if(s->observed.dma.cpu) {
        g->dma_free(g,s->observed.dma.cpu,s->observed.dma.size,s->observed.dma.physical);
        s->observed.dma=(dma_buffer){0};
    }
    rebuilt_inputs_release(g,&s->inputs);
    if(!s->fwsec_retained) rebuilt_fwsec_free(g,&s->fwsec);
    rebuilt_vbios_release(g,&s->vbios);
    if(s->reclaim) {s->phase=7;if(s->reclaim!=3) s->reclaim=2;}
}

int rebuilt_fwsec_stage(gpu *g,feature_session *s) {
    u32 wpr=read_reg(g,0x1fa828), boot, progress;
    u64 fb;
    int status;
    if(bad(wpr)) return -1001;
    /* Match managed GA102's 2100-poll GFW_BOOT budget. */
    for(u32 i=0;;i++) {
        boot=read_reg(g,0x118128);progress=read_reg(g,0x118234);
        /* 469dc0c tests BADF here only for its alternate (GA104) path.
         * Managed GA102 uses the raw readiness bits, even for all-one reads.
         * WPR and post-submission checks still enforce access validity. */
        if((boot&1) && (progress&0xff)==0xff) break;
        if(i==2100) return -1;
        g->delay_ms(1);
    }
    if(rebuilt_vbios_acquire(g,&s->vbios) || rebuilt_vbios_fwsec(g,&s->vbios,&s->fwsec)) return -1;
    if(rebuilt_local_memory_size(g,&fb) || fb<=0x200000) return -1;
    status=rebuilt_fwsec_execute(g,&s->fwsec,fb-0x200000);
    if(status==-1001) s->fwsec_retained=1;
    return status;
}

static int patch_plm_payload(boot_inputs *inputs,u32 offset) {
    unsigned char *p=inputs->signature.cpu;
    if(!p || inputs->signature.size<0xf9f0) return -1;
    for(u32 i=0;i+4<=inputs->signature.size;i+=4) {
        p[i]=0x9c;p[i+1]=1;p[i+2]=p[i+3]=0;
    }
    /* Fixed GA102 payload words observed in 469dc0c .text+d227:d2e3.
     * Byte stores avoid unaligned host accesses and preserve LE encoding. */
    static const feature_write words[]={
        {0x1100,7,0},{0xf948,0xffffffffu,0},{0xf950,0xd44,0},
        {0xf968,0x1fce,0},{0xf974,0,0},{0xf97c,0x1101,0},
        {0xf980,0x84c8,0},{0xf984,0x8e18,0},{0xf98c,0x84c8,0},
        {0xf990,0,0},{0xf998,0x1fce,0},{0xf9a4,0xffbc,0},
        {0xf9ac,0x5789,0},{0xf9bc,0xd44,0},{0xf9cc,3,0},
        {0xf9d4,0x1fce,0},{0xf9e8,0xd52,0},{0xf9ec,0x81ee,0}
    };
    for(u32 i=0;i<sizeof(words)/sizeof(words[0]);i++) {
        u32 value=words[i].value,o=words[i].offset;
        for(u32 j=0;j<4;j++) p[o+j]=(unsigned char)(value>>(8*j));
    }
    for(u32 j=0;j<4;j++) p[0xf960+j]=(unsigned char)(offset>>(8*j));
    return 0;
}

int rebuilt_feature_stage(gpu *g,u32 mode,u32 plm,const feature_write *plan,u32 count,
                          const char *label,feature_session *s,u32 *after) {
    boot_layout layout={firmware_feature_image,0x6000,0x1800,0x800};
    u32 offset=0,length=0,lo,hi,value;
    int status;
    (void)label;
    if(mode!=1 && mode!=2) return -1;
    if(rebuilt_find_signature(g,firmware_feature_signature,0x2168,&offset,&length) ||
       rebuilt_inputs_prepare(g,&s->inputs,firmware_feature_signature,0x2168,&layout) ||
       rebuilt_inputs_metadata_feature(g,&s->inputs,0x2168,&s->inputs.metadata)) return -1;
    lo=read_reg(g,0x1fa824);hi=read_reg(g,0x1fa828);value=read_reg(g,plm);
    if(bad(lo) || bad(hi) || (value&0xffff0000u)==0xbadf0000u) {
        if(g->log)g->log(g,"REBUILT_FEATURE_ABORT phase=pre-booter-readback lo=%08x hi=%08x plm=%08x\n",lo,hi,value);
        return -1001;
    }
    if(patch_plm_payload(&s->inputs,plm) || !s->inputs.metadata.cpu) return -1;
    rebuilt_meta_signature(s->inputs.metadata.cpu,s->inputs.signature.physical,s->inputs.signature.size);
    u64 region=((const u64 *)s->inputs.metadata.cpu)[14];
    s->attempts++;
    status=rebuilt_booter_target(g,s->inputs.metadata.physical,&s->observed,plm,
                                (u32)(region>>12)<<4,hi,0);
    s->execution=(u32)status;s->phase=2;s->retained=s->observed.dma.cpu!=0;
    if(status==-1001) {s->retained|=(s->observed.word[8]|s->observed.word[14])!=0;return status;}
    if(status!=0 && status!=1) return status;
    s->target_ready=status==1;
    lo=read_reg(g,0x1fa824);hi=read_reg(g,0x1fa828);
    if(bad(lo) || bad(hi) || (s->target_ready && (lo!=s->observed.word[4] || hi!=s->observed.word[5]))) return -1001;
    if(s->observed.word[11] || !s->observed.word[12] || !s->observed.word[13]) return -1;
    value=read_reg(g,plm);
    if(value!=0xffffffffu) return -2;
    if(rebuilt_session_identity(g,s)) return -1001;
    /* The original GA102 backend writes selectors from the host after the
     * firmware has opened PLM. Do not issue these writes before that proof. */
    for(u32 i=0;i<count;i++) if(bad(read_reg(g,plan[i].offset))) return -1001;
    for(u32 i=0;i<count;i++) {
        write_reg(g,plan[i].offset,plan[i].value);
        if(read_reg(g,plan[i].offset)!=plan[i].value) {
            if(mode==2 && plan[i].offset==0x823830) {
                u32 raster=read_reg(g,0x823834);
                if(!bad(raster) && !(raster&3)) continue;
            }
            return -3;
        }
    }
    s->target=1;s->phase=3;*after=value;return 0;
}

int rebuilt_plm_transaction(gpu *g,u32 offset,const char *name,u32 *after,handoff_fn handoff) {
    feature_session s={0};
    boot_layout layout={firmware_plm_image,0x6000,0x1800,0x800};
    u64 fb;
    u32 sig_offset=0,sig_size=0,wpr_lo,wpr_hi,value;
    int status=-1;
    s.mode=6;s.managed=1;s.handoff=handoff;s.reclaim=1;*after=0;
    if(g->log) g->log(g,"REBUILT_PLM_TRANSACTION_BEGIN offset=0x%08x name=%s\n",offset,name?name:"PLM");
    if(rebuilt_session_identity(g,&s)) {status=-1001;goto done;}
    if(!rebuilt_sec2_accessible(g)) (void)rebuilt_pmu_devinit(g);
    value=read_reg(g,0x200);
    if(bad(value)) {status=-1001;goto done;}
    if((value&0x2002)!=0x2002) {
        write_reg(g,0x200,value|0x2002);g->delay_ms(5);
        if(bad(read_reg(g,0x200))) {status=-1001;goto done;}
    }
    if(rebuilt_vbios_acquire(g,&s.vbios) || rebuilt_vbios_fwsec(g,&s.vbios,&s.fwsec)) goto done;
    if(rebuilt_local_memory_size(g,&fb) || fb<=0xaf00000) goto done;
    status=rebuilt_fwsec_execute(g,&s.fwsec,fb-0x200000);
    if(status) {if(status==-1001) s.fwsec_retained=1;goto done;}
    status=rebuilt_reset_managed(g);
    if(status) {status=-1001;goto done;}
    status=-1;
    if(rebuilt_find_signature(g,firmware_plm_signature,0x2168,&sig_offset,&sig_size) ||
       rebuilt_inputs_prepare(g,&s.inputs,firmware_plm_signature,0x2168,&layout) ||
       patch_plm_payload(&s.inputs,offset) ||
       rebuilt_inputs_metadata(g,&s.inputs,fb,0x2168,&s.inputs.metadata)) goto done;
    wpr_lo=read_reg(g,0x1fa824);wpr_hi=read_reg(g,0x1fa828);value=read_reg(g,offset);
    if(bad(wpr_lo) || bad(wpr_hi) || (value&0xffff0000u)==0xbadf0000u) {status=-1001;goto done;}
    if(!s.inputs.metadata.cpu) {status=-1001;goto done;}
    u64 region=((const u64 *)s.inputs.metadata.cpu)[14];
    s.submitted=1;s.attempts++;
    status=rebuilt_booter_target(g,s.inputs.metadata.physical,&s.observed,offset,
                                (u32)(region>>12)<<4,wpr_hi,0);
    s.execution=(u32)status;s.phase=2;
    s.retained=s.observed.dma.cpu!=0;
    if(status==-1001) {
        s.retained|=(s.observed.word[8]|s.observed.word[14])!=0;
        goto done;
    }
    if(status!=0 && status!=1) {status=-1;goto done;}
    if(rebuilt_session_identity(g,&s)) {status=-1001;goto done;}
    value=read_reg(g,offset);
    if(value!=0xffffffffu) {status=-1001;*after=value;goto done;}
    if(rebuilt_session_identity(g,&s)) {status=-1001;goto done;}
    s.target=1;
    if(handoff(g) || rebuilt_session_identity(g,&s)) {status=-1001;goto done;}
    value=read_reg(g,offset);
    if(value!=0xffffffffu || rebuilt_session_identity(g,&s)) {status=-1001;*after=value;goto done;}
    s.handoff_complete=1;s.retained=0;*after=value;status=0;
done:
    if(status==-1001) s.recovery=1;
    rebuilt_session_cleanup(g,&s);
    if(g->log) g->log(g,"REBUILT_PLM_TRANSACTION_END offset=0x%08x status=%d\n",offset,status);
    return status;
}
