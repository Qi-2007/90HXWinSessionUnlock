/* PRI connectivity recovery and host-written PMU VDE initialization. */
#include "core.h"
int rebuilt_vbios_vde(gpu *,const vbios_image *,u32 [20]);
static u32 rd(gpu *g,u32 o){return ioread32((const void *)(g->bar0+o));}
static void wr(gpu *g,u32 o,u32 v){iowrite32(v,(void *)(g->bar0+o));}
static int accessible(u32 v){return (v&0xfff00000u)!=0xbad00000u;}
static u32 word(const unsigned char *p,u32 n){u32 v=0;for(u32 i=0;i<n;i++)v|=(u32)p[i]<<(i*8);return v;}
static void dmem(gpu *g,u32 dest,const unsigned char *p,u32 size) {
    wr(g,0x10a1c0,dest|0x1000000);
    for(u32 o=0;o<size;o+=4)wr(g,0x10a1c4,word(p+o,size-o<4?size-o:4));
}
int rebuilt_pmu_devinit(gpu *g) {
    g->log(g,"pmu_devinit: starting GPU devinit for SEC2 access\n");
    if(accessible(rd(g,0x840100))){g->log(g,"pmu_devinit: SEC2 already accessible, skip\n");return 0;}
    wr(g,0x120060,1);g->delay_ms(1);wr(g,0x120060,0);wr(g,0x12004c,3);
    u32 i;for(i=0;i<2000;i++)if(!(rd(g,0x12004c)&63))break;
    if(i==2000){g->log(g,"pmu_devinit: PRI ring enumerate timeout\n");return -1;}
    wr(g,0x120048,0x503b4b49);wr(g,0x12004c,1);
    for(i=0;i<2000;i++)if(rd(g,0x120050)&1)break;
    if(i==2000){g->log(g,"pmu_devinit: PRI ring connectivity timeout\n");return -1;}
    if(accessible(rd(g,0x840100)))return 0;
    (void)rd(g,0x10a100);(void)rd(g,0x10a040);(void)rd(g,0x10a3c0);
    wr(g,0x10a3c0,1);for(i=0;i<6;i++)(void)rd(g,0x10a3c0);
    wr(g,0x10a3c0,0);for(i=0;i<6;i++)(void)rd(g,0x10a3c0);(void)rd(g,0x10a3c0);
    for(i=0;i<2000;i++)if(accessible(rd(g,0x10a1c0)))break;
    if(i==2000){g->log(g,"pmu_devinit: PMU DMEM scrub timeout\n");return -1;}
    vbios_image rom;u32 d[20];int result=rebuilt_vbios_acquire(g,&rom);
    if(result)return result;
    result=rebuilt_vbios_vde(g,&rom,d);if(result){rebuilt_vbios_release(g,&rom);return result;}
    wr(g,0x10a10c,0);wr(g,0x10a624,rd(g,0x10a624)|128);wr(g,0x10a600,(rd(g,0x10a600)&~3u)|5);
    u64 im=((u64)d[5]+255)&~255ull,dm=((u64)d[11]+255)&~255ull;
    if(im>0xffffffffu||dm>0xffffffffu||d[4]>d[13]||im>d[13]-d[4]||d[9]>d[13]||dm>d[13]-d[9]) {
        rebuilt_vbios_release(g,&rom);return -1;
    }
    const unsigned char *image=(const unsigned char *)(rom.word[0]+d[12]);
    for(u32 o=0;o<(u32)im;o+=256) {
        wr(g,0x10a180,o|0x1000000);wr(g,0x10a188,(d[6]+o)>>8);
        for(u32 j=0;j<256;j+=4)wr(g,0x10a184,word(image+d[4]+o+j,4));
    }
    dmem(g,d[10],image+d[9],(u32)dm);
    u32 size=((u32 *)&rom)[2];
    if(d[14]&&d[15]&&d[14]<=size&&d[15]<=size-d[14])dmem(g,d[16],(const unsigned char *)(rom.word[0]+d[14]),d[15]);
    if(d[17]&&d[18]&&d[17]<=size&&d[18]<=size-d[17])dmem(g,d[19],(const unsigned char *)(rom.word[0]+d[17]),d[18]);
    wr(g,0x10a104,d[2]);wr(g,0x10a100,2);
    for(i=0;i<1000;i++){if(rd(g,0x10a100)&16)break;g->delay_ms(10);}
    u32 m0=rd(g,0x10a040),m1=rd(g,0x10a044);
    g->log(g,"pmu_devinit: PMU after devinit: MAILBOX0=0x%08x MAILBOX1=0x%08x ret=%d\n",m0,m1,i==1000?-1:0);
    rebuilt_vbios_release(g,&rom);if(i==1000)return -1;
    g->delay_ms(100);return accessible(rd(g,0x840100))?0:-1;
}
