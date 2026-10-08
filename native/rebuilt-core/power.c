/* GA102 post-unlock power and media registers, reconstructed from 469dc0c. */
#include "core.h"
static u32 rd(gpu *g,u32 o){return ioread32((const void *)(g->bar0+o));}
static void wr(gpu *g,u32 o,u32 v){iowrite32(v,(void *)(g->bar0+o));}
static int valid(u32 v){return v!=0xffffffffu&&(v&0xfff00000u)!=0xbad00000u;}
int rebuilt_gr_power(gpu *g) {
    u32 reset=rd(g,0x41a610),pmc=rd(g,0x200),heads=rd(g,0x22448),enable=rd(g,0x600);
    g->log(g,"permissive: GR power pre: DEV_EN0=0x%08x NUM_DISP_HEADS=0x%08x PMC_EN=0x%08x GPCS_RESET_CTL=0x%08x\n",enable,heads,pmc,reset);
    wr(g,0x600,rd(g,0x600)|0x107e);wr(g,0x200,rd(g,0x200)|0x1000);g->delay_ms(5);
    enable=rd(g,0x600);pmc=rd(g,0x200);
    u32 config=rd(g,0x400614),post=rd(g,0x41a610);
    int power=valid(enable)&&valid(pmc)&&(enable&0x1000)&&(pmc&0x1000);
    int released=valid(post)&&(post&1);
    g->log(g,"permissive: GR power post: DEV_EN0=0x%08x (valid=%u GRbit=%u) PMC_EN=0x%08x (valid=%u GRbit=%u)\n",enable,valid(enable),(enable>>12)&1,pmc,valid(pmc),(pmc>>12)&1);
    g->log(g,"permissive: PGRAPH ENGINE_CONFIG=0x%08x (valid=%u 2D/3D/Compute/I2M capability; not GR running)\n",config,valid(config));
    g->log(g,"permissive: GPCS broadcast ENGINE_RESET_CTL pre=0x%08x post=0x%08x (valid=%u GPC_ENGINE_RESET_DISABLED=%u)\n",reset,post,valid(post),released);
    g->log(g,"permissive: GR PMC power %s; GPC broadcast reset %s\n",power?"CONFIRMED":"UNVERIFIED",released?"RELEASED":"UNVERIFIED_OR_ASSERTED");
    g->log(g,"permissive: GR power/reset broadcast %s; R515 grIsEnabled also requires every connected GPC, which needs unavailable GR topology\n",power&&released?"CONFIRMED":"UNVERIFIED");
    g->log(g,"permissive: FECS/GPCCS and context-switch readiness are separate checks\n");
    return 0;
}
void rebuilt_media_probe(gpu *g) {
    g->log(g,"permissive: FLOORSWEEP_PLM pre=0x%08x\n",rd(g,0x8200dc));
    wr(g,0x8200dc,0xffffffffu);
    g->log(g,"permissive: FLOORSWEEP_PLM post=0x%08x (target=0xFFFFFFFF)\n",rd(g,0x8200dc));
    u32 dec=rd(g,0x820378),enc=rd(g,0x820414),ofa=rd(g,0x8202d8);
    g->log(g,"permissive: Media fuse pre: NVDEC=0x%08x NVENC=0x%08x OFA=0x%08x\n",dec,enc,ofa);
    wr(g,0x820378,dec&~1u);wr(g,0x820414,0);wr(g,0x8202d8,0);
    dec=rd(g,0x820378);enc=rd(g,0x820414);ofa=rd(g,0x8202d8);
    g->log(g,"permissive: Media fuse post: NVDEC=0x%08x NVENC=0x%08x OFA=0x%08x\n",dec,enc,ofa);
    u32 jpg=rd(g,0x820c0c);ofa=rd(g,0x820e34);enc=rd(g,0x820d68);dec=rd(g,0x820c24);
    g->log(g,"permissive: Media STATUS: NVDEC=0x%08x NVENC=0x%08x OFA=0x%08x NVJPG=0x%08x\n",dec,enc,ofa,jpg);
}
