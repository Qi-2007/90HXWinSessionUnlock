/* PROM image-chain, BIT and FWSEC descriptor parsing for the GA102 path. */
#include "core.h"
static u32 get(const unsigned char *p,u32 n) {u32 v=0;for(u32 i=0;i<n;i++)v|=(u32)p[i]<<(8*i);return v;}
static void put(unsigned char *p,u32 n,u32 v) {for(u32 i=0;i<n;i++)p[i]=(unsigned char)(v>>(8*i));}
static void clear(void *p,u32 n) {unsigned char *b=p;for(u32 i=0;i<n;i++)b[i]=0;}
static void copy(void *p,const void *q,u32 n) {unsigned char *b=p;const unsigned char *a=q;for(u32 i=0;i<n;i++)b[i]=a[i];}
static int range(u32 o,u32 n,u32 size) {return o<=size && n<=size-o;}
static unsigned char *cache;
static u32 cache_expansion,cache_refs;
static u64 cache_key;
static unsigned char cached_desc[128];
static int desc_valid;
#ifdef CMP_KERNEL_CORE
/* User-mode CoreSession owns its cache for the process/session lifetime.
 * Kernel sessions finish before D0 completes; reset static cache ownership
 * before freeing that session's host pool, including on a later resume. */
int rebuilt_vbios_kernel_release_cache(gpu *g) {
    if(cache_refs) return -1;
    if(cache) g->host_free(g,cache);
    cache=0;cache_key=0;cache_expansion=0;desc_valid=0;
    for(u32 i=0;i<sizeof(cached_desc);i++)cached_desc[i]=0;
    return 0;
}
#endif
static u32 image_size(const vbios_image *v) {return ((const u32 *)v)[2];}
static u32 expansion(const vbios_image *v) {return ((const u32 *)v)[3];}

/* PMU VDE describes image sections and optional DEVENG tables in host ROM. */
int rebuilt_vbios_vde(gpu *g,const vbios_image *v,u32 out[20]) {
    (void)g;clear(out,80);
    const unsigned char *p=(const unsigned char *)v->word[0];
    u32 size=image_size(v),exp=expansion(v),bit=0,found=0;
    if(!p||exp>size)return -1;
    for(u32 o=0;range(o,12,size);o++) {
        if(get(p+o,2)!=0xb8ff||get(p+o+2,4)!=0x544942)continue;
        u32 h=p[o+8],sum=0;if(h<12||!range(o,h,size))continue;
        for(u32 j=0;j<h;j++)sum+=p[o+j];
        if(!(sum&255)){bit=o;found=1;break;}
    }
    if(!found)return -1;
    u32 start=bit+p[bit+8],stride=p[bit+9],count=p[bit+10];
    if(stride<6||!range(start,stride*count,size))return -1;
    for(u32 t=0;t<count;t++) {
        const unsigned char *token=p+start+t*stride;
        if(token[0]!=0x70||token[1]!=2||get(token+2,2)<4)continue;
        u32 data=get(token+4,2);if(!range(data,4,size))return -1;
        u32 table=get(p+data,4);if(!range(table,6,size-exp))return -1;table+=exp;
        if(p[table]!=1||p[table+1]<6||p[table+2]<6)continue;
        u32 entries=p[table+3],es=p[table+2],base=table+p[table+1];
        if(!range(base,entries*es,size))return -1;
        for(u32 i=0;i<entries;i++) {
            const unsigned char *e=p+base+i*es;if(e[0]!=4||e[1]!=1)continue;
            u32 desc=get(e+2,4);if(!range(desc,4,size-exp))return -1;desc+=exp;
            u32 vd=get(p+desc,4),len=(vd&1)?vd>>16:48,skip=(vd&1)?4:0;
            if(len<skip+48||!range(desc,len,size))return -1;
            for(u32 j=0;j<12;j++)out[j]=get(p+desc+skip+j*4,4);
            out[12]=desc+len;out[13]=out[0];
            if(!out[13]||!range(out[12],out[13],size)||out[5]>out[13]||!range(out[9],out[11],out[13]))return -1;
            if(out[3]<out[10]||!range(out[3]-out[10],4,out[11]))return 0;
            u32 iface=out[12]+out[9]+out[3]-out[10],n=p[iface];
            if(p[iface+1]!=1||!n||!range(out[3]-out[10],4+n*8,out[11]))return 0;
            for(u32 j=0;j<n;j++) {
                const unsigned char *entry=p+iface+4+j*8;
                if(get(entry,2)!=1)continue;
                u32 ptr=get(entry+2,4);
                if(ptr<out[10]||!range(ptr-out[10],32,out[11]))return 0;
                const unsigned char *dev=p+out[12]+out[9]+ptr-out[10];
                u32 virt=get(dev+8,4),phys=get(dev+12,4),end=get(dev+16,4),script=get(dev+20,4),sn=get(dev+28,2);
                if(end<virt||!range(phys,end-virt,size)||!range(script,sn,size))return 0;
                out[14]=phys;out[15]=end-virt;out[16]=virt;out[17]=script;out[18]=sn;out[19]=end;return 0;
            }
            return 0;
        }
    }
    return -1;
}

int rebuilt_vbios_acquire(gpu *g,vbios_image *v) {
    clear(v,sizeof(*v));
    if(cache && cache_key==g->tail) {
        v->word[0]=(u64)cache;((u32 *)v)[2]=0x100000;((u32 *)v)[3]=cache_expansion;((u32 *)v)[4]=1;cache_refs++;return 0;
    }
    if(!g->host_alloc||!g->host_free)return -1;
    unsigned char *p=g->host_alloc(g,0x100000);
    if(!p)return -1;
    for(u32 o=0;o<0x100000;o+=4)put(p+o,4,ioread32((const void *)(g->bar0+0x300000+o)));
    u32 pos=0,first_legacy=0,extension=0;
    for(;;) {
        if(!range(pos,26,0x100000))goto fail;
        u32 magic=get(p+pos,2);
        if(magic!=0xaa55&&magic!=0x4e56&&magic!=0xbb77)goto fail;
        u32 rel=get(p+pos+24,2),pcir=pos+rel;
        if(!range(pcir,24,0x100000))goto fail;
        magic=get(p+pcir,4);
        if(magic!=0x52494350&&magic!=0x5344504e&&magic!=0x53494752)goto fail;
        u32 header=get(p+pcir+10,2),blocks=get(p+pcir+16,2),type=p[pcir+20],last=p[pcir+21],npde=(pcir+header+15)&~15u,npde_size=0;
        if(header<24||!range(pcir,header,0x100000))goto fail;
        if(range(npde,10,0x100000)&&get(p+npde,4)==0x4544504e) {
            u32 rev=get(p+npde+4,2);
            if(rev==0x100||rev==0x101) {
                npde_size=get(p+npde+6,2);
                if(npde_size<10||!range(npde,npde_size,0x100000))goto fail;
                blocks=get(p+npde+8,2);
                if(npde_size>10)last=p[npde+10];
                else if(blocks<get(p+pcir+16,2))last=0;
            }
        }
        u32 size=blocks<<9;
        if(!size||!range(pos,size,0x100000)||rel>size||header>size-rel||
           (npde_size&&(npde<pos||npde-pos>size||npde_size>size-(npde-pos))))goto fail;
        if(!type&&!first_legacy)first_legacy=size;
        else if(type==0xe0&&!extension)extension=pos;
        pos+=size;
        if(last&0x80)break;
    }
    u32 exp=0;
    if(extension&&first_legacy) {if(extension<first_legacy)goto fail;exp=extension-first_legacy;}
    v->word[0]=(u64)p;((u32 *)v)[2]=0x100000;((u32 *)v)[3]=exp;
    if(!cache) {
        cache=p;cache_key=g->tail;cache_expansion=exp;cache_refs=1;((u32 *)v)[4]=1;
    }
    if(g->log)g->log(g,"REBUILT_VBIOS_PROM size=0x100000 expansion=0x%x cached=%u\n",exp,((u32 *)v)[4]);
    return 0;
fail:g->host_free(g,p);return -1;
}
void rebuilt_vbios_release(gpu *g,vbios_image *v) {
    if(((u32 *)v)[4]) {if(cache_refs)cache_refs--;}
    else if(v->word[0])g->host_free(g,(void *)v->word[0]);
    clear(v,sizeof(*v));
}
int rebuilt_frts_interface(const unsigned char *d,u32 size,u32 interface,u32 *mapper,u32 *command) {
    if(!range(interface,4,size))return -1;
    u32 count=d[interface+3],map=0,found=0,co=0,cs=0;
    if(count<2||!range(interface+4,count*8,size))return -1;
    for(u32 i=0;i<count;i++) {
        const unsigned char *e=d+interface+4+i*8;
        if(get(e,4)!=4)continue;
        map=get(e+4,4);
        if(!range(map,64,size))return -1;
        co=get(d+map+8,4);cs=get(d+map+12,4);found=1;
    }
    if(!found||cs<48||!range(co,cs,size))return -1;
    if(mapper)*mapper=map;if(command)*command=co;return 0;
}
static int parse(gpu *g,const vbios_image *v,unsigned char *out) {
    (void)g;clear(out,128);
    const unsigned char *p=(const unsigned char *)v->word[0];u32 size=image_size(v),bit=0,found=0,exp=expansion(v);
    if(!p)return -1;
    for(u32 o=0;range(o,12,size);o++) {
        if(get(p+o,2)!=0xb8ff||get(p+o+2,4)!=0x544942)continue;
        u32 header=p[o+8],sum=0;
        if(header<12||!range(o,header,size))continue;
        for(u32 j=0;j<header;j++)sum+=p[o+j];
        if(!(sum&255)){bit=o;found=1;break;}
    }
    if(!found)return -1;
    u32 header=p[bit+8],stride=p[bit+9],count=p[bit+10];
    if(stride<6||!range(bit+header,stride*count,size))return -1;
    for(u32 t=0;t<count;t++) {
        const unsigned char *token=p+bit+header+t*stride;
        if(token[0]!=0x70||token[1]!=2||get(token+2,2)<4)continue;
        u32 data=get(token+4,2);
        if(!range(data,4,size))return -1;
        u32 table=get(p+data,4);
        if(exp>size||!range(table,6,size-exp))return -1;
        table+=exp;
        if(p[table]!=1||p[table+1]<6||p[table+2]<6)continue;
        u32 entries=p[table+3],start=table+p[table+1],entry_size=p[table+2];
        if(!range(start,entries*entry_size,size))return -1;
        for(u32 i=0;i<entries;i++) {
            const unsigned char *e=p+start+i*entry_size;
            if(e[0]!=0x85)continue;
            u32 desc=get(e+2,4);
            if(!range(desc,12,size-exp))return -1;
            desc+=exp;
            const unsigned char *d=p+desc;
            u32 vd=get(d,4),ver=(vd>>8)&255,len=vd>>16,base=ver==2?60:44;
            if(!(vd&1)||((ver!=2||len<60)&&(ver!=3||len<44)))continue;
            if(!range(desc,len,size))return -1;
            u32 stored=get(d+4,4),image=desc+len;
            if(!stored||stored>0xffffff00u||!range(image,stored,size))return -1;
            if(ver==2) {
                const u32 offsets[][2]={{0x34,0xc},{0x3c,0x10},{0x18,0x14},{0x14,0x18},{0x1c,0x1c},{0x2c,0x20},{0x30,0x24},{0x20,0x28},{0x28,0x2c},{0x24,0x30}};
                for(u32 j=0;j<10;j++)put(out+offsets[j][0],4,get(d+offsets[j][1],4));
            } else {
                const u32 offsets[][2]={{0x40,8},{0x3c,0xc},{0x18,0x10},{0x14,0x14},{0x1c,0x18},{0x28,0x1c},{0x24,0x20}};
                for(u32 j=0;j<7;j++)put(out+offsets[j][0],4,get(d+offsets[j][1],4));
                put(out+0x46,2,get(d+0x24,2));out[0x44]=d[0x26];out[0x60]=d[0x27];put(out+0x62,2,get(d+0x28,2));
                put(out+0x20,4,get(out+0x14,4));
            }
            out[0x38]=(unsigned char)ver;put(out+0x58,4,len-base);put(out+0x5c,4,0x180);
            u32 imem=get(out+0x14,4),physical=get(out+0x18,4),virt=get(out+0x1c,4),dmem=get(out+0x24,4),doff=get(out+0x20,4),iface=get(out+0x3c,4),pkc=get(out+0x40,4);
            if(!imem||!dmem||imem>stored||doff>stored||dmem>stored-doff||!range(iface,4,dmem))return -1;
            if(ver==2) {
                u32 secbase=get(out+0x2c,4),secsize=get(out+0x30,4);
                if(imem<secsize||secbase<virt||physical>stored||secbase-virt>stored-physical||secsize>stored-physical||
                   ((u64)secbase-virt+physical)>stored||stored-(secbase-virt+physical)<((secsize+255u)&~255u))return -1;
            } else if(virt||out[0x60]<1||out[0x60]>16||(len-base)/out[0x60]<0x180||!range(pkc,0x180,dmem))return -1;
            if(rebuilt_frts_interface(p+image+doff,dmem,iface,0,0))return -1;
            put(out+0x10,4,(stored+255)&~255u);put(out+0x70,4,image);put(out+0x74,4,stored);put(out+0x78,4,desc+base);return 0;
        }
    }
    return -1;
}
int rebuilt_vbios_fwsec(gpu *g,vbios_image *v,fwsec_image *f) {
    unsigned char desc[128];clear(f,sizeof(*f));
    if(((u32 *)v)[4]&&desc_valid)copy(desc,cached_desc,128);
    else {if(parse(g,v,desc))return -1;if(((u32 *)v)[4]){copy(cached_desc,desc,128);desc_valid=1;}}
    if(!g->dma_alloc||!g->dma_free)return -1;
    dma_buffer *image=(dma_buffer *)desc,*sigs=(dma_buffer *)(desc+72);
    u32 stored=get(desc+0x74,4),io=get(desc+0x70,4),so=get(desc+0x78,4);
    if(!range(io,stored,image_size(v))||!range(so,sigs->size,image_size(v)))return -1;
    image->cpu=g->dma_alloc(g,image->size,&image->physical);
    if(!image->cpu)return -1;
    copy(image->cpu,(const void *)(v->word[0]+io),stored);clear((unsigned char *)image->cpu+stored,image->size-stored);
    if(sigs->size) {
        sigs->cpu=g->dma_alloc(g,sigs->size,&sigs->physical);
        if(!sigs->cpu){g->dma_free(g,image->cpu,image->size,image->physical);return -1;}
        copy(sigs->cpu,(const void *)(v->word[0]+so),sigs->size);
    }
    copy(f,desc,sizeof(*f));return 0;
}
