/* Host-side GA102 primitives reconstructed from pinned ABI observations. */
#include "core.h"
static u32 rd(gpu *g,u32 o) {return ioread32((const void *)(g->bar0+o));}
static int bad(u32 v) {return !v || v==0xffffffffu || (v&0xffff0000u)==0xbadf0000u;}
static void zero(void *p,u64 n) {unsigned char *b=p;for(u64 i=0;i<n;i++)b[i]=0;}
static void copy(void *p,const void *q,u64 n) {unsigned char *b=p;const unsigned char *a=q;for(u64 i=0;i<n;i++)b[i]=a[i];}
static u64 le(const unsigned char *p,u32 n) {u64 v=0;for(u32 i=0;i<n;i++)v|=(u64)p[i]<<(8*i);return v;}
int rebuilt_session_identity(gpu *g,feature_session *s) {
    u32 id=0,boot;
    if(!g->pci_read || g->pci_read(g,0,&id) || id!=((u32)g->device_id<<16|0x10de)) goto fail;
    boot=rd(g,0);
    if(bad(boot) || (s->reserved && s->reserved!=boot)) goto fail;
    s->reserved=boot;return 0;
fail:s->recovery=1;return -1001;
}
int rebuilt_local_memory_size(gpu *g,u64 *size) {
    if(!size)return -1;
    *size=0;u32 v=rd(g,0x100ce0),mantissa=(v>>4)&63;
    if(bad(v)||!mantissa)return -1;
    *size=(u64)mantissa<<((v&15)+20);return 0;
}
int rebuilt_sec2_accessible(gpu *g) {
    /* Exact raw BADF test; the original permits an all-one diagnostic read. */
    return (rd(g,0x840100)&0xfff00000u)!=0xbad00000u;
}
void rebuilt_inputs_release(gpu *g,boot_inputs *inputs) {
    dma_buffer *buffers[]={&inputs->aux,&inputs->metadata,&inputs->bootbin,&inputs->signature,&inputs->radix};
    for(u32 i=0;i<5;i++) {
        dma_buffer *b=buffers[i];
        if(b->cpu)g->dma_free(g,b->cpu,b->size,b->physical);
        *b=(dma_buffer){0};
    }
}
void rebuilt_fwsec_free(gpu *g,fwsec_image *f) {
    /* Descriptor ABI: image at 0, secondary loader at 72, both DMA-backed. */
    dma_buffer *a=(dma_buffer *)f,*b=(dma_buffer *)((unsigned char *)f+72);
    if(a->cpu)g->dma_free(g,a->cpu,a->size,a->physical);
    if(b->cpu)g->dma_free(g,b->cpu,b->size,b->physical);
    zero(f,sizeof(*f));
}
void rebuilt_meta_signature(void *meta,u64 physical,u32 size) {
    u64 *m=meta;m[9]=physical;m[10]=size;
}
static int radix_create(gpu *g,const void *data,u32 size,dma_buffer *out) {
    *out=(dma_buffer){0};
    if(!size||!data)return -1;
    u64 pages=((u64)size+4095)>>12,l2=((pages-1)>>9)+1,l1=((pages-1)>>18)+1;
    u64 table_pages=1+l1+l2,total=(table_pages+pages)<<12,physical=0;
    if(total>0xffffffffu)return -1;
    unsigned char *p=g->dma_alloc(g,total,&physical);
    if(!p)return -1;
    if((physical&4095)||physical>~0ull-(total-1)) {g->dma_free(g,p,total,physical);return -1;}
    zero(p,total);copy(p+(table_pages<<12),data,size);
    u64 *root=(u64 *)p,*middle=(u64 *)(p+4096),*leaf=(u64 *)(p+((1+l1)<<12));
    for(u64 i=0;i<l1;i++)root[i]=physical+((1+i)<<12);
    for(u64 i=0;i<l2;i++)middle[i]=physical+((1+l1+i)<<12);
    for(u64 i=0;i<pages;i++)leaf[i]=physical+((table_pages+i)<<12);
    *out=(dma_buffer){p,physical,(u32)total,0};return 0;
}
int rebuilt_inputs_prepare(gpu *g,boot_inputs *i,const void *data,u32 size,const boot_layout *layout) {
    if(radix_create(g,data,size,&i->radix))return -1;
    i->signature.size=0xfa00;i->signature.cpu=g->dma_alloc(g,0xfa00,&i->signature.physical);
    if(!layout||!i->signature.cpu||!layout->image||!layout->code_offset)return -1;
    i->bootbin.cpu=g->dma_alloc(g,layout->code_offset,&i->bootbin.physical);
    if(!i->bootbin.cpu)return -1;
    i->bootbin.size=layout->code_offset;copy(i->bootbin.cpu,layout->image,i->bootbin.size);return 0;
}
int rebuilt_inputs_metadata(gpu *g,boot_inputs *i,u64 fb,u32 elf_size,dma_buffer *out) {
    u64 m[32]={0},boot_size=i->bootbin.size,sig_size=i->signature.size;
    *out=(dma_buffer){0};
    if(!elf_size||!boot_size||!sig_size ||
       ((i->radix.physical|i->bootbin.physical)&255) ||
       (i->radix.physical&4095) || ((i->signature.physical|sig_size|boot_size)&3) ||
       boot_size<=0x1800 || i->bootbin.physical>~0ull-(boot_size-1) ||
       i->signature.physical>~0ull-(sig_size-1) || fb<=0xaf00000) return -1;
    m[0]=0xdc3aae21371a60b3ull;m[1]=1;
    m[2]=i->radix.physical;m[3]=elf_size;m[4]=i->bootbin.physical;m[5]=boot_size;
    m[6]=0x1800;m[7]=0x800;m[9]=i->signature.physical;m[10]=sig_size;
    m[11]=m[12]=fb-0xaf00000;m[13]=0x100000;
    m[14]=(fb-0xade0001)&~0x1ffffull;m[15]=m[14]+0x1000;
    m[19]=((fb-0x100000)&~0x1ffffull)-0x100000;
    if(m[19]<boot_size)return -1;
    m[18]=(m[19]-boot_size)&~4095ull;
    if(m[18]<elf_size)return -1;
    m[17]=(m[18]-elf_size)&~65535ull;
    if(m[17]>=m[18]||m[15]>=m[17]||m[18]-m[17]<elf_size||m[19]-m[18]<boot_size)return -1;
    m[16]=m[17]-m[15];m[20]=0x100000;
    m[21]=(fb-0x100000)&~0x1ffffull;m[22]=fb;m[23]=fb-0x100000;m[24]=0x100000;
    u64 phys=0;void *p=g->dma_alloc(g,4096,&phys);
    if(!p)return -1;
    if(phys&4095) {g->dma_free(g,p,4096,phys);return -1;}
    zero(p,4096);copy(p,m,sizeof(m));*out=(dma_buffer){p,phys,4096,0};return 0;
}
int rebuilt_inputs_metadata_feature(gpu *g,boot_inputs *i,u32 size,dma_buffer *out) {
    u64 fb;if(rebuilt_local_memory_size(g,&fb))fb=0x280000000ull;
    return rebuilt_inputs_metadata(g,i,fb,size,out);
}
static int find_section(const unsigned char *p,u32 size,const char *wanted,u32 *offset,u32 *length) {
    if(!p||size<64||le(p,4)!=0x464c457f)return -1;
    u64 sh=le(p+40,8);u32 n=(u32)le(p+60,2),strings=(u32)le(p+62,2);
    if(!sh||!n||le(p+58,2)!=64||sh>size||(u64)n*64>size-sh||strings>=n)return -1;
    const unsigned char *st=p+sh+strings*64;
    u64 so=le(st+24,8),sn=le(st+32,8);
    if(so>size||sn>size-so)return -1;
    for(u32 j=0;j<n;j++) {
        const unsigned char *s=p+sh+j*64;u64 name=le(s,4),k=0;
        if(name>=sn)continue;
        while(name+k<sn&&p[so+name+k]&&wanted[k]&&p[so+name+k]==(unsigned char)wanted[k])k++;
        if(name+k>=sn||p[so+name+k]||wanted[k])continue;
        u64 o=le(s+24,8),len=le(s+32,8);
        if(o>size||len>size-o)return -1;
        *offset=(u32)o;*length=(u32)len;return 0;
    }
    return -1;
}
int rebuilt_find_signature(gpu *g,const void *p,u32 size,u32 *offset,u32 *length) {
    (void)g;if(!offset||!length)return -1;
    if(!find_section(p,size,".fwsignature_ga10x",offset,length))return 0;
    return find_section(p,size,".fwsignature_tu10x",offset,length);
}
