/* Host-only tests. gen2-under-test.h is extracted from the actual driver. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef int BOOLEAN;
typedef void VOID;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s) >= 0)
static struct { ULONG GpuWidth, BridgeWidth, SbrCount; } context = {16,16,1}, *ActiveCore = &context;
static const ULONG GpuBdf=0x400, BridgeBdf=0x18, GpuCap=0x78, BridgeCap=0x90;
static ULONG attempts, mockPolls, inAttempt, gpuTarget, bridgeTarget, succeedOn;
static int scenario, failed;
static ULONGLONG ticks;
static char trace[32768];
static size_t logUsed;
static ULONGLONG KeQueryInterruptTime(void) { return ticks; }
static void CoreLog(void *g, const char *format, ...) {
    va_list args; int count; (void)g;
    va_start(args,format);
    count=vsnprintf(trace+logUsed,sizeof(trace)-logUsed,format,args);
    va_end(args);
    if(count>0 && (size_t)count<sizeof(trace)-logUsed) logUsed+=(size_t)count;
}
static BOOLEAN CoreBudget(void) { return !((scenario==5 || scenario==24) && mockPolls>=10); }
static void CoreDelay(ULONGLONG ms) { ticks+=(scenario==25?100:ms)*10000; mockPolls++; inAttempt++; }
static ULONG PciValue(ULONG bdf, ULONG off, ULONG width) {
    ULONG cap=bdf==GpuBdf?GpuCap:BridgeCap, speed=1;
    (void)width;
    if(scenario>=20) {
        if(off==0) return scenario==21 || scenario==25 || ticks<2000ULL*10000?0xffffffff:0x220d10de;
        if(off==cap+0x12) return 0x101 | (scenario==22?0x800:0);
        return 0;
    }
    if(off==cap+2) return 2;
    if(off==cap+0x2c) return 6;
    if(off==cap+0x30) return bdf==GpuBdf?gpuTarget:bridgeTarget;
    if(off==cap+0x10) return bdf==GpuBdf?0:0x40;
    if(off!=cap+0x12) return 0;
    if(succeedOn && attempts>=succeedOn && inAttempt>=1) speed=2;
    /* Two good samples before timeout plus one after the next request must
     * not be mistaken for three consecutive samples in one attempt. */
    if(scenario==6) speed=(attempts==1 && inAttempt>=49) || (attempts==2 && inAttempt==1)?2:1;
    if(scenario==7 && bdf==BridgeBdf) speed=1;
    return speed | ((scenario==3?8:16)<<4) | (scenario==4?0x800:0);
}
static BOOLEAN CorePciWrite(ULONG off, ULONG width, ULONG value) {
    (void)width;
    if(off==GpuCap+0x30) gpuTarget=value;
    return TRUE;
}
static int BridgeWord(ULONG off, ULONG value) {
    if(off==BridgeCap+0x30) bridgeTarget=value;
    if(off==BridgeCap+0x10 && (value&0x20)) {
        attempts++; inAttempt=0;
        if(scenario==8) return -1;
    }
    return 0;
}
static BOOLEAN CoreLinkReady(ULONG bdf,ULONG cap) {
    ULONG value=PciValue(bdf,cap+0x12,2);
    return value!=65535 && (value&15) && (value&0x3f0) && !(value&0x800);
}
#include "gen2-under-test.h"
static void Check(const char *name, int condition) {
    printf("%s %s\n",condition?"PASS":"FAIL",name);
    if(!condition) failed++;
}
static BOOLEAN Run(int mode, ULONG successAttempt) {
    scenario=mode; succeedOn=successAttempt;
    attempts=mockPolls=inAttempt=0; gpuTarget=bridgeTarget=1;
    ticks=0; logUsed=0; trace[0]=0;
    return CoreGen2();
}
int main(void) {
    BOOLEAN result;
    result=Run(0,1);
    Check("first request succeeds after three samples",result && attempts==1 && mockPolls==3);
    result=Run(0,2);
    Check("customer regression: second request succeeds",result && attempts==2 && mockPolls==53);
    Check("attempts and final success logged",strstr(trace,"attempt=2 maxAttempts=3") && strstr(trace,"reason=ready"));
    result=Run(0,3);
    Check("third request succeeds",result && attempts==3 && mockPolls==103);
    result=Run(0,0);
    Check("permanent Gen1 stops after three requests",!result && attempts==3 && mockPolls==150);
    result=Run(3,1);
    Check("width loss remains a failure",!result && attempts==3 && mockPolls==150);
    result=Run(4,1);
    Check("active training remains a failure",!result && attempts==3 && mockPolls==150);
    result=Run(5,0);
    Check("budget stops without another request",!result && attempts==1 && mockPolls==10 && strstr(trace,"reason=budget-or-fault"));
    result=Run(6,0);
    Check("stable samples reset between attempts",!result && attempts==3 && mockPolls==150);
    result=Run(7,1);
    Check("both endpoints must reach Gen2",!result && attempts==3 && mockPolls==150);
    result=Run(8,0);
    Check("write failure stops without retry",!result && attempts==1 && mockPolls==0 && strstr(trace,"reason=bridge-retrain-write"));
    scenario=20;ticks=0;mockPolls=0;logUsed=0;trace[0]=0;
    result=CoreWaitSbrConfig();
    Check("SBR config can recover after two seconds",result && ticks==2000ULL*10000);
    Check("SBR logs pending and ready state",strstr(trace,"phase=pending") && strstr(trace,"phase=ready"));
    scenario=21;ticks=0;mockPolls=0;logUsed=0;trace[0]=0;
    result=CoreWaitSbrConfig();
    Check("missing GPU times out at eight seconds",!result && ticks==8000ULL*10000 && strstr(trace,"phase=timeout"));
    scenario=22;ticks=0;mockPolls=0;logUsed=0;trace[0]=0;
    result=CoreWaitSbrConfig();
    Check("SBR config refuses active training",!result && ticks==8000ULL*10000);
    scenario=24;ticks=0;mockPolls=0;logUsed=0;trace[0]=0;
    result=CoreWaitSbrConfig();
    Check("SBR config obeys transaction budget",!result && mockPolls==10 && strstr(trace,"phase=budget-or-fault"));
    scenario=25;ticks=0;mockPolls=0;logUsed=0;trace[0]=0;
    /* With delayed scheduling the same wall-clock deadline still applies. */
    result=CoreWaitSbrConfig();
    Check("SBR delayed scheduling remains bounded",!result && ticks==8000ULL*10000 && mockPolls==80);
    return failed?1:0;
}
