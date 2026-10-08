/* Managed GA102 GSP/FWSEC execution, preserving device DMA evidence. */
#include "core.h"
int rebuilt_frts_interface(const unsigned char *,u32,u32,u32 *,u32 *);
static u32 rd(gpu *g,u32 o){return ioread32((const void *)(g->bar0+o));}
static void wr(gpu *g,u32 o,u32 v){iowrite32(v,(void *)(g->bar0+o));}
static int bad(u32 v){return v==0xffffffffu||(v&0xffff0000u)==0xbadf0000u;}
static int switch_core(gpu *g) {
    u32 v=rd(g,0x111668);if(bad(v))return -1001;
    if(v&16)wr(g,0x111668,0);
    else if(v&1)return 0;
    for(u32 i=0;i<10000;i++) {
        v=rd(g,0x111668);if(bad(v))return -1001;
        if((v&17)==1)return 0;
        g->delay_ms(1);
    }
    return -1001;
}
int rebuilt_reset_managed(gpu *g) {
    if(g->device_id!=0x220d)return -1;
    u32 v=rd(g,0x1100f4);
    for(u32 i=0;!(v&0x80000400u)&&i<150;i++){if(bad(v))return -1001;v=rd(g,0x1100f4);}
    if(bad(v))return -1001;
    v=rd(g,0x1103c0);if(bad(v))return -1001;wr(g,0x1103c0,v|1);
    for(u32 i=0;i<10;i++){v=rd(g,0x1103c0);if(bad(v))return -1001;}
    if(!(v&1))return -1001;
    v=rd(g,0x1103c0);if(bad(v))return -1001;wr(g,0x1103c0,v&~1u);
    for(u32 i=0;i<10;i++){v=rd(g,0x1103c0);if(bad(v))return -1001;}
    if(v&1)return -1001;
    for(u32 i=0;i<10000;i++) {
        v=rd(g,0x1100f4);if(bad(v))return -1001;
        if(!(v&0x1000)) {
            if(switch_core(g)||bad(rd(g,0x110100)))return -1001;
            v=rd(g,0x11010c);if(bad(v)||(v&6))return -1001;
            v=rd(g,0);if(bad(v)||!v)return -1001;
            wr(g,0x110084,v);return 0;
        }
        g->delay_ms(1);
    }
    return -1001;
}
static int stopped(gpu *g) {
    u32 cpu=rd(g,0x110100);if(bad(cpu)||!(cpu&16))return -1001;
    u32 dma=rd(g,0x110118),ctl=rd(g,0x11010c);
    if(bad(dma)||bad(ctl)||(dma&0x100003)!=2||(ctl&6))return -1001;
    cpu=rd(g,0x110100);return bad(cpu)||!(cpu&16)?-1001:0;
}
static int dma_transfer(gpu *g,u32 dest,u32 virt,u64 physical,u32 size,u32 command,u32 *issued) {
    wr(g,0x110110,(u32)(physical>>8));wr(g,0x110128,(u32)(physical>>40)&511);
    for(u32 offset=0;offset<size;offset+=256) {
        wr(g,0x110114,dest+offset);wr(g,0x11011c,virt+offset);*issued=1;wr(g,0x110118,command);
        u32 i;
        for(i=0;i<2000;i++) {
            u32 v=rd(g,0x110118);
            if(bad(v)||(v&0x100000)||(v&3)==3)return -1001;
            if(v&2)break;
        }
        if(i==2000)return -1;
    }
    return 0;
}
static int hs_execute(gpu *g,fwsec_image *image,u32 fuse) {
    unsigned char *f=(unsigned char *)image;u32 *d=(u32 *)f;
    d[25]=d[26]=0;
    u32 mask=(u32)f[98]|(u32)f[99]<<8,count=f[96],sigsize=d[23],index=0;
    if(f[56]!=3||!d[4]||!d[5]||!d[9]||!d[23]||!count||count>16||fuse>15||
       d[22]/count<sigsize||!(mask&(1u<<fuse))||!image->word[9])return -1;
    for(u32 i=0;i<fuse;i++)if(mask&(1u<<i))index++;
    if(index>=count)return -1;
    u64 target=(u64)d[8]+d[16],offset=(u64)sigsize*index;
    if(target>d[4]||sigsize>d[4]-target||offset>d[22]||sigsize>d[22]-offset)return -1;
    unsigned char *cpu=(unsigned char *)image->word[0],*sig=(unsigned char *)image->word[9];
    for(u32 i=0;i<sigsize;i++)cpu[target+i]=sig[offset+i];
    wr(g,0x11010c,0);u32 value=rd(g,0x110624);
    if(bad(value))return -1001;wr(g,0x110624,value|128);
    value=rd(g,0x110600);if(bad(value))return -1001;wr(g,0x110600,(value&~3u)|5);
    if(d[4]<d[5]||d[8]>d[4]||d[9]>d[4]-d[8]||d[7])return -1;
    int status=dma_transfer(g,d[6],d[7],image->word[1],d[5],0x614,&d[25]);
    if(status)return d[25]?-1001:status;
    /* Preserve GA102 secure IMEM readback port accesses. */
    wr(g,0x110180,0x12000000);for(u32 i=0;i<4;i++)(void)rd(g,0x110184);
    status=dma_transfer(g,d[10],0,image->word[1]+d[8],d[9],0x600,&d[25]);
    if(status)return d[25]?-1001:status;
    wr(g,0x111210,d[16]);wr(g,0x11119c,(u32)f[70]|(u32)f[71]<<8);
    wr(g,0x111198,f[68]);wr(g,0x111180,1);wr(g,0x110104,d[7]);
    d[26]=1;wr(g,0x110100,2);
    for(u32 elapsed=0;elapsed<5000;elapsed+=10) {
        value=rd(g,0x110100);if(bad(value))return -1001;
        if(value&16)return stopped(g);
        g->delay_ms(10);
    }
    return -1001;
}
int rebuilt_fwsec_execute(gpu *g,fwsec_image *image,u64 frts) {
    unsigned char *f=(unsigned char *)image;u32 *d=(u32 *)f;
    d[25]=d[26]=0;
    if(g->device_id!=0x220d||f[56]!=3||!image->word[0]||!d[4]||!d[5]||!d[9]||
       d[4]<d[5]||d[8]>d[4]||d[9]>d[4]-d[8]||d[7]||!d[23]||
       f[96]<1||f[96]>16||d[22]/f[96]<d[23]||d[16]>d[9]||d[23]>d[9]-d[16]||!image->word[9])return -1;
    unsigned char *dm=(unsigned char *)image->word[0]+d[8];u32 map,cmd;
    if(rebuilt_frts_interface(dm,d[9],d[15],&map,&cmd))return -1;
    u32 request[12]={1,24,0,0,0,2,1,20,(u32)(frts>>12),0x100,2,0};
    for(u32 i=0;i<4;i++)dm[map+44+i]=(unsigned char)(0x15u>>(8*i));
    for(u32 i=0;i<48;i++)dm[cmd+i]=((unsigned char *)request)[i];
    int status=rebuilt_reset_managed(g);if(status)return -1001;
    u32 engine=(u32)f[70]|(u32)f[71]<<8,id=f[68];
    if(id<1||id>16||!(engine&0x400))return -1;
    u32 raw=rd(g,0x8241bc+id*4),fuse=0;
    for(u32 v=raw;v;v>>=1)fuse++;
    status=hs_execute(g,image,fuse);if(status)return status;
    u32 mailbox0=rd(g,0x110040),mailbox1=rd(g,0x110044),cpu=rd(g,0x110100);
    if(bad(mailbox0)||bad(mailbox1)||bad(cpu)||!(cpu&16))return -1001;
    for(u32 i=0;i<20;i++)(void)rd(g,0x1400+i*4);
    u32 scratch=rd(g,0x1438);if(bad(scratch))return -1001;
    if(scratch>>16)return stopped(g)?-1001:-1;
    u32 hi=rd(g,0x1fa828),lo=rd(g,0x1fa824);
    if(bad(hi)||bad(lo)||stopped(g))return -1001;
    if(!(hi>>4)||(lo>>4)!=(u32)(frts>>12))return -1;
    if(g->log)g->log(g,"REBUILT_FWSEC_COMPLETE lo=0x%08x hi=0x%08x submitted=%u started=%u\n",lo,hi,d[25],d[26]);
    return 0;
}
