#include "core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"TRANSACTION FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static gpu dev;
static unsigned char signature[0xfa00];
static u64 metadata[32];
static u32 target_value, target_offset, pmc, boot, progress, wpr, expected_offset;
static int execute_status, fwsec_status, input_status, vbios_status, reset_status;
static int handed, handoff_error, freed, input_released, fwsec_freed, vbios_released, identities, fail_identity, delays, bad_after;
static u32 sm0,sm1,gfx,raster;
static int ignore_selectors, metadata_updates;
static u32 le32(u32 o) {u32 v;memcpy(&v,signature+o,4);return v;}
u32 ioread32(const void *p) {
    u32 o=(u32)((u64)p-dev.bar0);
    if(o==0x200) return pmc;
    if(o==0x118128) return boot;
    if(o==0x118234) return progress;
    if(o==0x1fa824 || o==0x1fa828) return wpr;
    if(o==target_offset) return target_value;
    if(o==0x82381c) return sm0;
    if(o==0x823820) return sm1;
    if(o==0x823830) return gfx;
    if(o==0x823834) return raster;
    return 0;
}
void iowrite32(u32 v,void *p) {
    u32 o=(u32)((u64)p-dev.bar0);
    if(o==0x200) pmc=v;
    if(!ignore_selectors) {
        if(o==0x82381c) sm0=v;
        if(o==0x823820) sm1=v;
        if(o==0x823830) gfx=v;
    }
}
static void delay(u64 ms) {delays+=(int)ms;}
static void dma_free(gpu *g,void *p,u64 size,u64 physical) {(void)g;(void)p;(void)size;(void)physical;freed++;}
int rebuilt_session_identity(gpu *g,feature_session *s) {(void)g;identities++;if(fail_identity==identities){s->recovery=1;return -1001;}return 0;}
int rebuilt_sec2_accessible(gpu *g) {(void)g;return 1;}
int rebuilt_pmu_devinit(gpu *g) {(void)g;return -1;}
int rebuilt_vbios_acquire(gpu *g,vbios_image *v) {(void)g;(void)v;return vbios_status;}
int rebuilt_vbios_fwsec(gpu *g,vbios_image *v,fwsec_image *f) {(void)g;(void)v;(void)f;return 0;}
void rebuilt_vbios_release(gpu *g,vbios_image *v) {(void)g;(void)v;vbios_released++;}
void rebuilt_fwsec_free(gpu *g,fwsec_image *f) {(void)g;(void)f;fwsec_freed++;}
int rebuilt_local_memory_size(gpu *g,u64 *fb) {(void)g;*fb=0x280000000ull;return 0;}
int rebuilt_fwsec_execute(gpu *g,fwsec_image *f,u64 offset) {(void)g;(void)f;CHECK(offset==0x27fe00000ull);return fwsec_status;}
int rebuilt_reset_managed(gpu *g) {(void)g;return reset_status;}
const unsigned char firmware_plm_image[1]={0},firmware_plm_signature[1]={0};
const unsigned char firmware_feature_image[1]={0},firmware_feature_signature[1]={0};
int rebuilt_find_signature(gpu *g,const void *p,u32 len,u32 *o,u32 *s) {(void)g;CHECK((p==firmware_plm_signature || p==firmware_feature_signature) && len==0x2168);*o=0;*s=0x2168;return 0;}
int rebuilt_inputs_prepare(gpu *g,boot_inputs *i,const void *sig,u32 len,const boot_layout *l) {
    (void)g;CHECK((sig==firmware_plm_signature || sig==firmware_feature_signature) && len==0x2168 && l->code_offset==0x6000 && l->data_offset==0x1800 && l->data_size==0x800);
    if(input_status) return input_status;
    i->signature.cpu=signature;i->signature.size=sizeof(signature);return 0;
}
int rebuilt_inputs_metadata(gpu *g,boot_inputs *i,u64 fb,u32 len,dma_buffer *out) {
    (void)g;CHECK(fb==0x280000000ull && len==0x2168);
    CHECK(i->signature.cpu==signature && le32(0x1100)==7 && le32(0xf948)==0xffffffffu);
    CHECK(le32(0xf960)==expected_offset && le32(0xf97c)==0x1101 && le32(0xf980)==0x84c8);
    CHECK(le32(0xf9e8)==0xd52 && le32(0xf9ec)==0x81ee && le32(0)==0x19c);
    out->cpu=metadata;out->physical=0x60000000;out->size=256;metadata[14]=0x100000;return 0;
}
void rebuilt_inputs_release(gpu *g,boot_inputs *i) {(void)g;input_released++;memset(i,0,sizeof(*i));}
int rebuilt_inputs_metadata_feature(gpu *g,boot_inputs *i,u32 len,dma_buffer *out) {
    (void)g;(void)i;CHECK(len==0x2168);out->cpu=metadata;out->physical=0x60000000;out->size=256;metadata[14]=0x100000;return 0;
}
void rebuilt_meta_signature(void *p,u64 physical,u32 size) {(void)physical;CHECK(p==metadata && size==0xfa00);metadata_updates++;}
int rebuilt_booter_target(gpu *g,u64 phys,boot_observation *o,u32 target,u32 region,u32 hi,u32 legacy) {
    (void)g;CHECK(phys==0x60000000 && target==expected_offset && region==0x1000 && hi==wpr && !legacy);
    o->dma=(dma_buffer){(void *)0x1234,0x5000,32,0};
    o->word[8]=o->word[14]=1;
    o->word[4]=o->word[5]=wpr;o->word[12]=o->word[13]=1;
    if(execute_status==0 || execute_status==1) target_value=0xffffffffu;
    return execute_status;
}
static int handoff(gpu *g) {(void)g;handed++;if(bad_after) target_value=0xffffffcf;return handoff_error;}
static void reset(void) {
    memset(&dev,0,sizeof(dev));dev.bar0=0x100000000ull;dev.delay_ms=delay;dev.dma_free=dma_free;
    memset(signature,0xa5,sizeof(signature));memset(metadata,0,sizeof(metadata));
    target_offset=expected_offset=0x8e1b0;target_value=0xffffff8f;pmc=0x2002;boot=1;progress=0xff;wpr=1;
    execute_status=1;fwsec_status=input_status=vbios_status=reset_status=0;
    handed=handoff_error=freed=input_released=fwsec_freed=vbios_released=identities=fail_identity=delays=bad_after=0;
    sm0=sm1=gfx=raster=0;ignore_selectors=metadata_updates=0;
}
int main(int argc,char **argv) {
    u32 after;
    if(argc==3) {
        reset();target_offset=expected_offset=(u32)strtoul(argv[1],0,0);
        CHECK(!rebuilt_plm_transaction(&dev,target_offset,"dump",&after,handoff));
        FILE *file=fopen(argv[2],"wb");CHECK(file);
        CHECK(fwrite(signature,1,sizeof(signature),file)==sizeof(signature));CHECK(!fclose(file));
        return 0;
    }
    u32 offsets[]={0x8200fc,0x8e1b0,0x8e1b4,0x8e1b8,0x8e1bc,0x88fe8,0x88fec,0x88ff0,0x8e1f0};
    for(u32 n=0;n<9;n++) {
        reset();target_offset=expected_offset=offsets[n];
        CHECK(!rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff));
        CHECK(after==0xffffffffu && handed==1 && freed==1 && input_released==1 && fwsec_freed==1 && vbios_released==1);
    }
    reset();handoff_error=-1;
    CHECK(rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff)==-1001 && handed==1 && !freed && !input_released);
    reset();bad_after=1;
    CHECK(rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff)==-1001 && !freed && !input_released);
    for(int i=1;i<=5;i++) {
        reset();fail_identity=i;
        CHECK(rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff)==-1001 && !freed);
        CHECK(input_released==(i==1?1:0));
    }
    reset();execute_status=-1001;
    CHECK(rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff)==-1001 && !handed && !freed && !input_released);
    reset();fwsec_status=-1001;
    CHECK(rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff)==-1001 && !handed && !fwsec_freed && input_released==1);
    reset();vbios_status=-1;
    CHECK(rebuilt_plm_transaction(&dev,target_offset,"test",&after,handoff)==-1 && !handed && input_released==1);
    reset();feature_session s={0};s.managed=1;s.reclaim=1;
    CHECK(!rebuilt_fwsec_stage(&dev,&s));
    reset();memset(&s,0,sizeof(s));s.managed=1;progress=0;
    CHECK(rebuilt_fwsec_stage(&dev,&s)==-1 && delays==2100);
    reset();memset(&s,0,sizeof(s));s.managed=1;boot=0xffffffffu;progress=0;
    CHECK(rebuilt_fwsec_stage(&dev,&s)==-1 && delays==2100);
    reset();memset(&s,0,sizeof(s));s.managed=1;boot=progress=0xffffffffu;
    CHECK(!rebuilt_fwsec_stage(&dev,&s) && !delays);
    reset();memset(&s,0,sizeof(s));s.managed=1;fwsec_status=-1001;
    CHECK(rebuilt_fwsec_stage(&dev,&s)==-1001 && s.fwsec_retained);
    feature_write compute[]={{0x823820,8,0},{0x82381c,0x88888888u,0}}, graphics[]={{0x823830,4,0}};
    for(u32 mode=1;mode<=2;mode++) {
        reset();memset(&s,0,sizeof(s));s.managed=1;
        target_offset=expected_offset=mode==1?0x823804:0x823b04;
        CHECK(!rebuilt_feature_stage(&dev,mode,target_offset,mode==1?compute:graphics,mode==1?2:1,"test",&s,&after));
        CHECK(s.retained && s.target && !s.handoff_complete && metadata_updates==1 && !freed);
        CHECK(mode==1?(sm0==0x88888888u && sm1==8):gfx==4);
        rebuilt_session_cleanup(&dev,&s);CHECK(!freed && !input_released);
    }
    reset();memset(&s,0,sizeof(s));s.managed=1;target_offset=expected_offset=0x823804;ignore_selectors=1;
    CHECK(rebuilt_feature_stage(&dev,1,target_offset,compute,2,"test",&s,&after)==-3 && s.retained);
    puts("REBUILT_TRANSACTION_TEST_PASSED: nine PLM payloads, target/handoff/identity failures, DMA retention/release, FWSEC timeout and uncertain DMA; stub firmware only.");
    return 0;
}
