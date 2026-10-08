using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace CMP90HX
{
    // Explicit one-shot kernel D0 diagnostics; GPU DMA mode is opt-in.
    // The kernel never waits for this user process in the resume path.
    sealed class FilterDiagnostics : IDisposable
    {
        SafeFileHandle handle;
        ulong last;
        ulong probeTime;
        uint probeStage;
        uint unlockLogBytes, unlockStage, unlockStatus, unlockFlags;
        internal bool StagedReady { get { return unlockStage==12 && unlockStatus==0 && (unlockFlags&0x601)==0x601 && (unlockFlags&0x24)==0; } }
        internal bool UnlockFinished { get { return unlockStage==12 || (unlockStage>=2 && unlockStatus!=0 && unlockStatus!=0x103); } }
        byte[] pciBaseline;
        internal FilterDiagnostics(bool session=false)
        {
            handle=Native.CreateFile(session?@"\\.\CMP90HXUnlockSession":@"\\.\CMP90HXLowerFilter",0xc0000000,0,IntPtr.Zero,3,0,IntPtr.Zero);
            if(handle.IsInvalid) { int error=Marshal.GetLastWin32Error(); Dispose();
                throw Native.Failure(error,session?"Open CMP90HXUnlockSession: require administrator, temporary driver Code 0 and matching worker; reboot after SYS update":"Open CMP90HXLowerFilter: require administrator, installed Extension INF and matching worker; reboot after updating SYS"); }
        }
        internal bool SessionCanRestore(Action<string> log)
        {
            byte[] packet=new byte[32]; uint returned;
            if(!Native.DeviceIoControl(handle,0x8337e044,new byte[0],0,packet,packet.Length,out returned,IntPtr.Zero))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Read temporary session state");
            if(returned!=32 || BitConverter.ToUInt32(packet,0)!=1) throw new IOException("Invalid session state packet.");
            uint busy=BitConverter.ToUInt32(packet,12),quarantine=BitConverter.ToUInt32(packet,16),retained=BitConverter.ToUInt32(packet,20),runs=BitConverter.ToUInt32(packet,24);
            log(String.Format("SESSION_STATE bdf=0x{0:x} started={1} busy={2} quarantine={3} retained={4} runs={5} status=0x{6:x8}",
                BitConverter.ToUInt32(packet,4),BitConverter.ToUInt32(packet,8),busy,quarantine,retained,runs,BitConverter.ToUInt32(packet,28)));
            if(busy!=0 || quarantine!=0 || retained!=0) return false;
            if(runs==0) return true; // no DMA transaction in this driver lifetime (including after reboot)
            ReadUnlock(log,true);
            return (unlockFlags&0x800)!=0 && UnlockFinished && ((unlockFlags&0x26)==0 || StagedReady);
        }
        internal bool RunSession(uint bdf,Action<string> log)
        {
            uint returned;
            bool result=Native.DeviceIoControl(handle,0x8337e040,BitConverter.GetBytes(bdf),4,new byte[0],0,out returned,IntPtr.Zero);
            int error=result?0:Marshal.GetLastWin32Error();
            log("SESSION_IOCTL_RETURN success="+result+" Win32="+error+"; full unlock was issued after START completed");
            ReadUnlock(log);
            return result && StagedReady && (unlockFlags&0x800)!=0;
        }
        internal void Read(Action<string> log,bool changesOnly=false)
        {
            byte[] packet=new byte[2592]; uint returned;
            if(!Native.DeviceIoControl(handle,0x8337e02c,new byte[0],0,packet,packet.Length,out returned,IntPtr.Zero))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Filter diagnostic IOCTL");
            uint count=BitConverter.ToUInt32(packet,4),devices=BitConverter.ToUInt32(packet,16),bdf=BitConverter.ToUInt32(packet,20);
            ulong latest=BitConverter.ToUInt64(packet,8);
            if(returned!=packet.Length || BitConverter.ToUInt32(packet,0)!=1 || count>64 || latest<count ||
                (bdf>0xffff && bdf!=UInt32.MaxValue) || BitConverter.ToUInt32(packet,24)>1 || BitConverter.ToUInt32(packet,28)!=0)
                throw new IOException("Invalid lower-filter diagnostic packet.");
            if(changesOnly && latest==last) return;
            log(String.Format("FILTER_STATUS devices={0} bdf={1} started={2} latest={3} retained={4}; event snapshot",
                devices,bdf==UInt32.MaxValue?"unknown":PciAddress.Bdf(bdf),BitConverter.ToUInt32(packet,24)!=0,latest,count));
            ulong first=latest-count+1;
            if(last!=0 && first>last+1) log("FILTER_TRACE_DROPPED events="+(first-last-1));
            for(uint i=0;i<count;i++) {
                int offset=32+(int)i*40; ulong sequence=BitConverter.ToUInt64(packet,offset);
                if(sequence!=first+i) throw new IOException("Invalid lower-filter event sequence.");
                if(sequence<=last) continue;
                ulong time=BitConverter.ToUInt64(packet,offset+8);
                if(time>Int64.MaxValue) throw new IOException("Invalid lower-filter timestamp.");
                uint phase=BitConverter.ToUInt32(packet,offset+16),major=BitConverter.ToUInt32(packet,offset+20),minor=BitConverter.ToUInt32(packet,offset+24),
                    status=BitConverter.ToUInt32(packet,offset+28),value=BitConverter.ToUInt32(packet,offset+32),irql=BitConverter.ToUInt32(packet,offset+36);
                string power="";
                if(major==0x16) {
                    uint type=value>>16,state=value&65535;
                    power=type==1 && state>=1 && state<=4 ? " device=D"+(state-1) : " systemState="+state;
                }
                string[] phases={"unknown","attach","enter","lower-complete","remove","probe-worker","probe-finished"};
                log(String.Format("FILTER_EVENT seq={0} time={1} phase={2} major=0x{3:x2} minor=0x{4:x2} status=0x{5:x8} value=0x{6:x8} irql={7}{8}",
                    sequence,DateTime.FromFileTimeUtc((long)time).ToString("o"),phase<phases.Length?phases[phase]:"unknown",major,minor,status,value,irql,power));
            }
            last=latest;
        }
        internal void ArmProbe(uint bdf,Action<string> log,uint mode=0)
        {
            byte[] input=new byte[8]; uint returned;
            Buffer.BlockCopy(BitConverter.GetBytes(bdf),0,input,0,4);
            Buffer.BlockCopy(BitConverter.GetBytes(mode),0,input,4,4);
            if(!Native.DeviceIoControl(handle,0x8337e030,input,input.Length,new byte[0],0,out returned,IntPtr.Zero))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Arm one-shot D0 experiment (single started 90HX, matching BDF; reboot if a previous full unlock retained its DMA arena)");
            if(returned!=0) throw new IOException("Unexpected probe arm response.");
            log("FILTER_PROBE_ARMED target="+PciAddress.Bdf(bdf)+" mode="+(mode==7?"pnp-staged-unlock":mode==6?"pnp-start-bus-function-reset":mode==5?"pnp-start-read-only":mode==4?"single-sbr-no-firmware":mode==3?"source-full-unlock":mode==2?"gsp-reset-gpu-dma":mode==1?"gpu-dma":"common-buffer")+(mode==7?"; this command will restart the exact target twice.":mode>=5?"; next successful PnP START only; restart target device in another admin terminal; keep this process open.":"; next successful D3->D0 only; keep this process open until resume."));
            if(mode==5) log("FILTER_START_EXPERIMENT: SYS 0.5.4; bus PCI configuration and reset interface query only; no MMIO, DMA allocation, reset callback, SBR or firmware; does not unlock.");
            if(mode==6) log("FILTER_START_RESET_EXPERIMENT: SYS 0.5.4; one bus FunctionLevelDeviceReset, completion callback and PCI preservation gate; no manual SBR, MMIO, DMA or firmware; one submitted reset per boot; does not unlock.");
            if(mode==3) log("FILTER_UNLOCK_EXPERIMENT: source core runs before upper D0 completion, not a proven cold-start window; SYS 0.5.2 retains DMA arena/adapter until reboot; one full attempt per boot; matching worker required.");
            if(mode==4) log("FILTER_SINGLE_SBR_EXPERIMENT: exactly one SBR plus saved BAR/Command restoration; no firmware/CPU start/DMA allocation; one destructive attempt per boot; does not unlock.");
            if(mode==2) log("FILTER_GSP_RESET_EXPERIMENT: original GSP firmware/DMEM state is lost; NVIDIA resume compatibility is unverified; reboot if initialization/probe fails.");
        }
        internal void ReadProbe(Action<string> log,bool changesOnly=false)
        {
            byte[] packet=new byte[288]; uint returned;
            if(!Native.DeviceIoControl(handle,0x8337e034,new byte[0],0,packet,packet.Length,out returned,IntPtr.Zero))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Read D0 kernel probe result (0.4.1 filter required; update SYS and reboot)");
            uint stage=BitConverter.ToUInt32(packet,4),status=BitConverter.ToUInt32(packet,8),bdf=BitConverter.ToUInt32(packet,12),flags=BitConverter.ToUInt32(packet,52);
            ulong time=BitConverter.ToUInt64(packet,16);
            if(returned!=288 || BitConverter.ToUInt32(packet,0)!=5 || stage>14 || bdf>0xffff ||
                BitConverter.ToUInt32(packet,104)>3 || BitConverter.ToUInt32(packet,124)>3 ||
                BitConverter.ToUInt32(packet,120)>64 || BitConverter.ToUInt32(packet,244)!=0 || BitConverter.ToUInt32(packet,248)>7)
                throw new IOException("Invalid D0 kernel probe result.");
            if(changesOnly && probeTime==time && probeStage==stage) return;
            probeTime=time;probeStage=stage;
            log(String.Format("FILTER_D0_PROBE stage={0} status=0x{1:x8} bdf={2} irql={3} elapsedMs={4} flags=0x{5:x} bar0=0x{6:x} deviceAddress=0x{7:x} physical=0x{8:x} bytes={9}",
                stage,status,PciAddress.Bdf(bdf),BitConverter.ToUInt32(packet,56),BitConverter.ToUInt32(packet,60),flags,
                BitConverter.ToUInt64(packet,24),BitConverter.ToUInt64(packet,32),BitConverter.ToUInt64(packet,40),BitConverter.ToUInt32(packet,48)));
            log(String.Format("FILTER_D0_GPU BOOT0=0x{0:x8} GFW_BOOT=0x{1:x8} GFW_PROGRESS=0x{2:x8} GSP_CPUCTL=0x{3:x8} SEC2_CPUCTL=0x{4:x8} PCI_COMMAND=0x{5:x4}",
                BitConverter.ToUInt32(packet,64),BitConverter.ToUInt32(packet,68),BitConverter.ToUInt32(packet,72),
                BitConverter.ToUInt32(packet,76),BitConverter.ToUInt32(packet,80),BitConverter.ToUInt32(packet,84)));
            if((flags&0x200)!=0) log(String.Format("FILTER_D0_DMA BCR=0x{0:x8} COMMAND=0x{1:x8} IN={2}/64 OUT={3}/64 restored={4} quarantined={5}",
                BitConverter.ToUInt32(packet,88),BitConverter.ToUInt32(packet,92),BitConverter.ToUInt32(packet,96),BitConverter.ToUInt32(packet,100),
                (flags&0x80)!=0,(flags&0x400)!=0));
            if((flags&0x1000)!=0) log(String.Format("FILTER_D0_GSP_INIT stage={0} status=0x{1:x8} flags=0x{2:x} engineBefore=0x{3:x8} engineAfter=0x{4:x8} bcrAfter=0x{5:x8} hwcfgAfter=0x{6:x8} cpuAfter=0x{7:x8} dmaCtlAfter=0x{8:x8} riscvAfter=0x{9:x8}; original firmware state not restored",
                BitConverter.ToUInt32(packet,248),BitConverter.ToUInt32(packet,252),BitConverter.ToUInt32(packet,256),BitConverter.ToUInt32(packet,260),
                BitConverter.ToUInt32(packet,264),BitConverter.ToUInt32(packet,268),BitConverter.ToUInt32(packet,272),BitConverter.ToUInt32(packet,276),BitConverter.ToUInt32(packet,280),BitConverter.ToUInt32(packet,284)));
            if((flags&0x200)!=0 && stage>=8 && stage<=13) {
                string[] reasons={"none","initial-protected-or-invalid-word","pio-write-readback-mismatch","pio-original-restore-readback-mismatch"};
                string[] cleanup={"none","dma-not-confirmed-idle","dmem-restore-mismatch","register-restore-mismatch"};
                uint reason=BitConverter.ToUInt32(packet,104),cleanupReason=BitConverter.ToUInt32(packet,124);
                log(String.Format("FILTER_D0_DMEM reason={0} index={1} expected=0x{2:x8} actual=0x{3:x8} matches={4}/64 portBefore=0x{5:x8} portAfterRead=0x{6:x8}",
                    reasons[reason],reason==0?"n/a":BitConverter.ToUInt32(packet,108).ToString(),
                    BitConverter.ToUInt32(packet,112),BitConverter.ToUInt32(packet,116),BitConverter.ToUInt32(packet,120),
                    BitConverter.ToUInt32(packet,140),BitConverter.ToUInt32(packet,144)));
                log("FILTER_D0_DMEM_ORIGINAL words[0..7]="+Words(packet,148));
                log("FILTER_D0_DMEM_OBSERVED words[0..7]="+Words(packet,180));
                log(String.Format("FILTER_D0_CLEANUP reason={0} indexOrRegister=0x{1:x} expected=0x{2:x8} actual=0x{3:x8}",
                    cleanup[cleanupReason],BitConverter.ToUInt32(packet,128),BitConverter.ToUInt32(packet,132),BitConverter.ToUInt32(packet,136)));
                if(reason!=1 && cleanupReason!=1) log("FILTER_D0_DMEM_CLEANUP words[0..7]="+Words(packet,212));
            }
        }
        static string Words(byte[] packet,int offset)
        {
            string[] words=new string[8];
            for(int i=0;i<words.Length;i++) words[i]=BitConverter.ToUInt32(packet,offset+i*4).ToString("x8");
            return String.Join(" ",words);
        }
        internal bool ReadUnlock(Action<string> log,bool changesOnly=false)
        {
            byte[] packet=new byte[32832]; uint returned;
            if(!Native.DeviceIoControl(handle,0x8337e038,new byte[0],0,packet,packet.Length,out returned,IntPtr.Zero))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Read source resume result (0.5.2 filter required; update SYS and reboot)");
            uint stage=BitConverter.ToUInt32(packet,4), status=BitConverter.ToUInt32(packet,8), count=BitConverter.ToUInt32(packet,60);
            uint flags=BitConverter.ToUInt32(packet,20);
            if(returned!=packet.Length || BitConverter.ToUInt32(packet,0)!=1 || stage>12 || count>=32768)
                throw new IOException("Invalid filter unlock result.");
            unlockFlags=flags;
            if(count<unlockLogBytes) unlockLogBytes=0;
            if(count>unlockLogBytes) log(Encoding.UTF8.GetString(packet,64+(int)unlockLogBytes,(int)(count-unlockLogBytes)).TrimEnd('\r','\n'));
            unlockLogBytes=count;
            if(!changesOnly || stage!=unlockStage || status!=unlockStatus) {
                log(String.Format("FILTER_UNLOCK_RESULT stage={0} status=0x{1:x8} core={2} flags=0x{3:x} elapsedMs={4} handoffs={5} live={6} deviceAddress=0x{7:x} physical=0x{8:x} GPU_LINK=0x{9:x4} BRIDGE_LINK=0x{10:x4}",
                    stage,status,BitConverter.ToInt32(packet,16),flags,BitConverter.ToUInt32(packet,24),BitConverter.ToUInt32(packet,28),
                    BitConverter.ToUInt32(packet,48),BitConverter.ToUInt64(packet,32),BitConverter.ToUInt64(packet,40),BitConverter.ToUInt32(packet,52),BitConverter.ToUInt32(packet,56)));
                unlockStage=stage;unlockStatus=status;
                if((flags&32)!=0) log("FILTER_DMA_BOOT_LIFETIME: arena/adapter/PDO retained; live=0 does not mean backing memory released; next probe/unlock requires reboot.");
                if((flags&2048)!=0) log("FILTER_RESULT_KIND=session-post-start-ioctl; DMA released="+((flags&1024)!=0)+"; close this handle before restoring NVIDIA binding.");
                else if((flags&512)!=0) log("FILTER_RESULT_KIND=pnp-staged-unlock; DMA released="+((flags&1024)!=0)+"; NVIDIA handoff still requires a second unarmed device start.");
                if((flags&64)!=0) log("FILTER_RESULT_KIND=single-sbr-no-firmware; success only means SBR readiness passed, not full unlock or NVIDIA recovery.");
                if((flags&256)!=0) log("FILTER_RESULT_KIND=pnp-start-bus-function-reset; success means bus reset completion and PCI preservation passed, not unlock or verified NVIDIA initialization; next arm requires reboot after submission.");
                else if((flags&128)!=0) log("FILTER_RESULT_KIND=pnp-start-read-only; success means PCI evidence captured, not unlock or verified NVIDIA initialization.");
            }
            return stage==12 && status==0 && (flags&1)!=0;
        }
        internal void ReadPci(uint bdf,Action<string> log,string phase="query")
        {
            byte[] packet=new byte[272]; uint returned;
            byte[] input=BitConverter.GetBytes(bdf);
            if(!Native.DeviceIoControl(handle,0x8337e03c,input,input.Length,packet,packet.Length,out returned,IntPtr.Zero))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Read scoped PCI snapshot (0.5.2 filter required)");
            if(returned!=272 || BitConverter.ToUInt32(packet,0)!=1 || BitConverter.ToUInt32(packet,4)!=bdf ||
                BitConverter.ToUInt32(packet,8)!=256 || BitConverter.ToUInt32(packet,12)!=0 || BitConverter.ToUInt32(packet,16)!=0x220d10de)
                throw new IOException("Invalid PCI diagnostic snapshot.");
            byte[] config=new byte[256]; Buffer.BlockCopy(packet,16,config,0,256);
            log(String.Format("FILTER_PCI_SNAPSHOT phase={0} bdf={1} command=0x{2:x4} (read-only; no MMIO/DMA)",phase,PciAddress.Bdf(bdf),BitConverter.ToUInt16(config,4)));
            bool[] seen=new bool[256]; int cap=config[0x34]&~3;
            for(int count=0;cap!=0 && count<48;count++) {
                if(cap<0x40 || cap>0xfc || (cap&3)!=0 || seen[cap]) throw new IOException("Invalid PCI diagnostic capability chain.");
                seen[cap]=true;
                uint ctl=BitConverter.ToUInt16(config,cap+2);
                if(config[cap]==5 && cap<=0xe8) {
                    bool wide=(ctl&0x80)!=0;
                    log(String.Format("FILTER_PCI_MSI phase={0} cap=0x{1:x2} control=0x{2:x4} enabled={3} address={4:x8}:{5:x8} data=0x{6:x4}",
                        phase,cap,ctl,(ctl&1)!=0,wide?BitConverter.ToUInt32(config,cap+8):0,
                        BitConverter.ToUInt32(config,cap+4),BitConverter.ToUInt16(config,cap+(wide?12:8))));
                }
                if(config[cap]==0x11 && cap<=0xf4)
                    log(String.Format("FILTER_PCI_MSIX phase={0} cap=0x{1:x2} control=0x{2:x4} enabled={3} functionMask={4}",phase,cap,ctl,(ctl&0x8000)!=0,(ctl&0x4000)!=0));
                cap=config[cap+1];
            }
            for(int offset=0;offset<256;offset+=16) {
                bool changed=pciBaseline==null;
                for(int i=0;i<16 && !changed;i++) changed=config[offset+i]!=pciBaseline[offset+i];
                if(changed) log("FILTER_PCI_CONFIG phase="+phase+" offset=0x"+offset.ToString("x2")+" bytes="+BitConverter.ToString(config,offset,16));
            }
            if(pciBaseline==null) pciBaseline=config;
        }
        internal void ObservePci(uint bdf,Action<string> log,int seconds)
        {
            DateTime begin=DateTime.UtcNow;
            log("FILTER_POST_RESUME_OBSERVE seconds="+seconds+"; PCI reads only; NVIDIA recovery is unverified.");
            for(int elapsed=0;elapsed<=seconds;elapsed+=5) {
                Read(log,true); ReadPci(bdf,log,"after-resume-"+elapsed+"s");
                if(elapsed+5<=seconds) Thread.Sleep(5000);
            }
            log("FILTER_POST_RESUME_OBSERVE_END wallSeconds="+(int)(DateTime.UtcNow-begin).TotalSeconds+"; confirm power telemetry and actual load separately.");
        }
        internal bool Watch(Action<string> log,int seconds,bool probe=false,bool unlock=false,uint? observeBdf=null,int observeSeconds=0)
        {
            DateTime end=DateTime.UtcNow.AddSeconds(seconds);
            log(unlock?"FILTER_WATCH_BEGIN: waiting for the armed experiment; follow FILTER_PROBE_ARMED instructions; kernel work does not wait for this process.":probe?"FILTER_WATCH_BEGIN: one-shot D0 probe armed; kernel work does not wait for this process.":"FILTER_WATCH_BEGIN: leave this process open across sleep; trace-only mode.");
            while(DateTime.UtcNow<end) {
                Read(log,true);
                if(unlock) {
                    bool success=ReadUnlock(log,true);
                    if(unlockStage>=2 && unlockStatus!=0x103) {
                        if(success && observeBdf.HasValue && observeSeconds>0) ObservePci(observeBdf.Value,log,observeSeconds);
                        return success;
                    }
                } else if(probe) ReadProbe(log,true);
                Thread.Sleep(1000);
            }
            Read(log,true);
            if(probe) ReadProbe(log,true);
            return unlock && ReadUnlock(log,true);
        }
        public void Dispose() { if(handle!=null) { handle.Dispose(); handle=null; } }
    }
}
