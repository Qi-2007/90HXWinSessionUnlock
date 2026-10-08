/* Managed GA102 SEC2 booter. Device firmware bytes are separate data assets. */
#include "core.h"
extern const unsigned char firmware_booter_image[],firmware_booter_signatures[];
static u32 rd(gpu *g,u32 o){return ioread32((const void *)(g->bar0+o));}
static void wr(gpu *g,u32 o,u32 v){iowrite32(v,(void *)(g->bar0+o));}
static int bad(u32 v){return v==0xffffffffu||(v&0xffff0000u)==0xbadf0000u;}
static int identity(gpu *g,u32 boot0) {
    u32 pci=0;
    return g->pci_read&&g->pci_read(g,0,&pci)==0&&pci==((u32)g->device_id<<16|0x10de)&&rd(g,0)==boot0;
}
static int observe(gpu *g,boot_observation *o) {
    u32 *w=o->word;w[0]=rd(g,0x840100);w[15]=!bad(w[0])&&!!(w[0]&16);
    w[6]=rd(g,0x840118);w[7]=rd(g,0x84010c);
    w[10]=!bad(w[6])&&!bad(w[7]);w[11]=w[10]?((w[6]>>20)&1):0;
    w[9]=w[10]&&(!w[8]||((w[6]&3)==2&&!(w[7]&6)));
    w[4]=rd(g,0x1fa824);w[5]=rd(g,0x1fa828);w[13]=!bad(w[4])&&!bad(w[5]);
    return bad(w[0])||!w[10]||(w[14]&&!w[15])||(w[8]&&!w[9])||!w[13];
}
static int queue(gpu *g,int final) {
    for(u32 i=0;i<2000;i++) {
        u32 v=rd(g,0x840118);
        if(bad(v)||(v&0x100000)||(v&3)==3)return -1001;
        if(final?(v&2):!(v&1))return 0;
    }
    return -1;
}
static int dma(gpu *g,u64 physical,u32 dest,u32 mem,u32 size,u32 command,u32 *issued) {
    int r=queue(g,0);if(r)return r;
    wr(g,0x840110,(u32)(physical>>8));wr(g,0x840128,(u32)(physical>>40)&511);
    for(u32 i=0;i<size;i+=256) {
        r=queue(g,0);if(r)return r;
        wr(g,0x840114,dest+i);wr(g,0x84011c,mem+i);*issued=1;wr(g,0x840118,command);
    }
    return queue(g,1);
}
static int reset(gpu *g) {
    u32 v;
    /* The managed 469dc0c path permits inaccessible SEC2 before reset.
     * Restore its reset sequence; require valid readback before DMA. */
    for(u32 i=0;i<2000;i++){v=rd(g,0x8400f4);if(v&0x80000000u)break;}
    v=rd(g,0x8403c0);wr(g,0x8403c0,v|1);
    for(u32 i=0;i<6;i++)(void)rd(g,0x8403c0);
    v=rd(g,0x8403c0);wr(g,0x8403c0,v&~1u);
    for(u32 i=0;i<6;i++)(void)rd(g,0x8403c0);
    u32 i;for(i=0;i<2000;i++){v=rd(g,0x8400f4);if(bad(v))return -1001;if(!(v&0x1000))break;}
    if(i==2000)return -1001;
    v=rd(g,0x841668);if(bad(v))return -1001;if(v&16)wr(g,0x841668,0);
    for(i=0;i<2000;i++){v=rd(g,0x841668);if(bad(v))return -1001;if(v&1)break;}
    if(i==2000)return -1001;
    v=rd(g,0);if(!v||bad(v))return -1001;wr(g,0x840084,v);
    if(bad(rd(g,0x840100)))return -1001;
    wr(g,0x840004,0xffffffffu);wr(g,0x84010c,0);
    v=rd(g,0x840624);if(bad(v))return -1001;wr(g,0x840624,v|128);
    v=rd(g,0x840600);if(bad(v))return -1001;wr(g,0x840600,(v&~3u)|5);return 0;
}
static const u32 status_registers[]={0x840040,0x840044,0x8411a4,0x840080,0x840108,0x840008,0x8403c0,0x841210,0x84119c,0x841198,0x841180,0x8400d0,0x8400bc,0x840094,0x8400f4};
static const u32 baseline_registers[]={0x840100,0,0x84005c,0x8400d0,0x8400bc,0x840094,0x840118,0x84010c,0x840040,0x840044};
static int sample(gpu *g,u32 target,u32 cpu,u32 values[10]) {
    values[0]=cpu;
    for(u32 i=1;i<10;i++) {
        values[i]=rd(g,i==1?target:baseline_registers[i]);
        if((values[i]&0xffff0000u)==0xbadf0000u||(i!=1&&values[i]==0xffffffffu))return -1001;
    }
    return 0;
}
static int ready(gpu *g,boot_observation *o,u64 meta,u32 lo,u32 hi,u32 state[15]) {
    (void)observe(g,o);
    if(bad(o->word[0])||!o->word[10]||!o->word[13])return -1001;
    for(u32 i=0;i<15;i++){state[i]=rd(g,status_registers[i]);if(bad(state[i]))return -1001;}
    o->word[1]=state[0];o->word[2]=state[1];o->word[3]=state[2];o->word[12]=1;
    return o->word[0]==0&&o->word[8]&&o->word[9]&&!o->word[11]&&
        o->word[4]==lo&&o->word[5]==hi&&state[0]==(u32)meta&&state[1]==(u32)(meta>>32)&&
        state[2]==0&&state[6]==0&&state[7]==16&&state[8]==1&&state[9]==3&&state[10]==1&&!(state[14]&0x1000);
}
int rebuilt_booter_target(gpu *g,u64 meta,boot_observation *o,u32 target,u32 lo,u32 hi,u32 legacy) {
    if(!o)return -1;
    for(u32 i=0;i<sizeof(*o);i++)((unsigned char *)o)[i]=0;
    if(!g||!g->pci_read||g->device_id!=0x220d||bad(lo)||bad(hi)||legacy)return -1;
    u32 boot0=rd(g,0),wpr_lo=rd(g,0x1fa824),wpr_hi=rd(g,0x1fa828);
    if(!boot0||bad(boot0)||!identity(g,boot0)||bad(wpr_lo)||bad(wpr_hi)) {
        if(g->log)g->log(g,"REBUILT_BOOTER_ABORT phase=identity-wpr boot0=%08x lo=%08x hi=%08x\n",boot0,wpr_lo,wpr_hi);
        return -1001;
    }
    if(g->log)g->log(g,"REBUILT_BOOTER_BEGIN cpu=%08x dma=%08x hwcfg2=%08x engine=%08x\n",rd(g,0x840100),rd(g,0x840118),rd(g,0x8400f4),rd(g,0x8403c0));
    if(!g->dma_alloc||!g->dma_free)return -1;
    dma_buffer image={0};image.size=0xec00;image.cpu=g->dma_alloc(g,image.size,&image.physical);
    if(!image.cpu)return -1;
    int result=-1;u32 fuse=rd(g,0x824148);
    if(fuse>1)goto done;
    for(u32 i=0;i<0xec00;i++)((unsigned char *)image.cpu)[i]=firmware_booter_image[i];
    /* Signature versions are stored newest first. */
    for(u32 i=0;i<384;i++)((unsigned char *)image.cpu)[0x8a10+i]=firmware_booter_signatures[(1-fuse)*384+i];
    result=reset(g);if(result) {
        (void)observe(g,o);o->word[15]=0;
        if(g->log)g->log(g,"REBUILT_BOOTER_ABORT phase=reset cpu=%08x dma=%08x hwcfg2=%08x bcr=%08x\n",o->word[0],o->word[6],rd(g,0x8400f4),rd(g,0x841668));
        goto done;
    }
    result=dma(g,image.physical,0,0x100,0x8900,0x614,&o->word[8]);if(result)goto submitted_failure;
    result=dma(g,image.physical+0x8a00,0,0,0x6200,0x600,&o->word[8]);if(result)goto submitted_failure;
    wr(g,0x841210,16);wr(g,0x84119c,1);wr(g,0x841198,3);wr(g,0x841180,1);wr(g,0x840104,0x100);
    wr(g,0x840040,(u32)meta);wr(g,0x840044,(u32)(meta>>32));
    u32 cpu=rd(g,0x840100),debug=rd(g,0x840094),baseline[10];
    if(bad(cpu)||bad(debug)||!identity(g,boot0)||sample(g,target,cpu,baseline))goto submitted_failure;
    o->word[14]=1;wr(g,cpu&64?0x840130:0x840100,2);
    for(u32 elapsed=0;elapsed<5000;elapsed+=10) {
        cpu=rd(g,0x840100);u32 current[10];
        if(bad(cpu)||!identity(g,boot0)||sample(g,target,cpu,current))goto submitted_failure;
        if(cpu&16) {
            if(observe(g,o))goto submitted_failure;
            u32 state[15];for(u32 i=0;i<15;i++){state[i]=rd(g,status_registers[i]);if(bad(state[i]))goto submitted_failure;}
            o->word[1]=state[0];o->word[2]=state[1];o->word[3]=state[2];o->word[12]=1;
            result=0;goto done;
        }
        if(baseline[1]!=0xffffffffu&&current[1]==0xffffffffu&&current[0]==0) {
            u32 a[15],b[15];int r=ready(g,o,meta,lo,hi,a);
            if(r<0)goto submitted_failure;
            if(r==1&&current[6]==o->word[6]&&current[7]==o->word[7]&&
               current[3]==a[11]&&current[4]==a[12]&&current[5]==a[13]&&
               current[8]==a[0]&&current[9]==a[1]) {
                u32 first[18];for(u32 i=0;i<18;i++)first[i]=o->word[i];
                r=ready(g,o,meta,lo,hi,b);if(r<0)goto submitted_failure;
                if(r==1) {
                    u32 i;for(i=0;i<15;i++)if(a[i]!=b[i])break;
                    if(i==15)for(u32 j=0;j<18;j++)if(first[j]!=o->word[j]){i=0;break;}
                    if(i==15&&rd(g,target)==0xffffffffu&&identity(g,boot0)) {
                        o->word[15]=0;o->word[16]=0;o->word[17]=0;result=1;goto done;
                    }
                }
            }
        }
        g->delay_ms(10);
    }
    o->word[16]=1;
submitted_failure:
    result=-1001;(void)observe(g,o);o->word[15]=0;o->word[17]=0;
    if(o->word[14]) {
        o->word[1]=rd(g,0x840040);o->word[2]=rd(g,0x840044);o->word[3]=rd(g,0x8411a4);
        o->word[12]=!bad(o->word[0])&&!bad(o->word[1])&&!bad(o->word[2])&&!bad(o->word[3]);
    }
done:
    if(o->word[8]||o->word[14])o->dma=image;
    else g->dma_free(g,image.cpu,image.size,image.physical);
    if(g->log)g->log(g,"REBUILT_BOOTER result=%d target-ready=%u halt=%u timeout=%u retained=%u\n",result,result==1,o->word[15],o->word[16],o->dma.cpu!=0);
    return result;
}
