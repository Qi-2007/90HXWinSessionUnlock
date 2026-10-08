using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace CMP90HX
{
    // All device transitions occur in user mode, after completion of the START IRP.
    static class FilterPnpHandoff
    {
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_ID_List_SizeW(out uint size,string filter,uint flags);
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_ID_ListW(string filter,[Out] char[] ids,uint size,uint flags);
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)] static extern uint CM_Locate_DevNodeW(out uint node,string id,uint flags);
        [DllImport("cfgmgr32.dll")] static extern uint CM_Get_DevNode_Status(out uint status,out uint problem,uint node,uint flags);
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)] static extern uint CM_Get_DevNode_Registry_PropertyW(uint node,uint property,out uint type,[Out] byte[] bytes,ref uint size,uint flags);
        [DllImport("cfgmgr32.dll")] static extern uint CM_Disable_DevNode(uint node,uint flags);
        [DllImport("cfgmgr32.dll")] static extern uint CM_Enable_DevNode(uint node,uint flags);
        static void Check(uint result,string name) { if(result!=0) throw new IOException(name+" CONFIGRET=0x"+result.ToString("x")); }
        static uint Node(string id) { uint node; Check(CM_Locate_DevNodeW(out node,id,0),"Locate exact GPU"); return node; }
        static byte[] Property(uint node,uint property) {
            byte[] bytes=new byte[2048]; uint type,size=(uint)bytes.Length;
            Check(CM_Get_DevNode_Registry_PropertyW(node,property,out type,bytes,ref size,0),"Read GPU property");
            if(size>bytes.Length) throw new IOException("Invalid GPU property size.");
            Array.Resize(ref bytes,(int)size); return bytes;
        }
        static uint Status(string id,out uint problem) {
            uint status; Check(CM_Get_DevNode_Status(out status,out problem,Node(id),0),"Read GPU state");
            if((status&0x400)==0) problem=0;
            return status;
        }
        static string Target(uint bdf) {
            char[] ids=null;
            for(int attempt=0;attempt<3;attempt++) {
                uint size; Check(CM_Get_Device_ID_List_SizeW(out size,null,0),"List PnP size");
                if(size>1048576) throw new IOException("PnP list exceeds bound.");
                ids=new char[size]; uint result=CM_Get_Device_ID_ListW(null,ids,size,0);
                if(result==0) break;
                if(result!=0x1a || attempt==2) Check(result,"List devices");
            }
            string found=null;
            foreach(string id in new string(ids).Split(new[]{'\0'},StringSplitOptions.RemoveEmptyEntries)) {
                if(!id.StartsWith(@"PCI\VEN_10DE&DEV_220D&",StringComparison.OrdinalIgnoreCase)) continue;
                uint node; if(CM_Locate_DevNodeW(out node,id,0)!=0) continue;
                byte[] busBytes=Property(node,0x16),addressBytes=Property(node,0x1d);
                if(busBytes.Length!=4 || addressBytes.Length!=4) throw new IOException("Invalid GPU BDF properties.");
                uint bus=BitConverter.ToUInt32(busBytes,0),address=BitConverter.ToUInt32(addressBytes,0),dev=address>>16,fn=address&65535;
                if(bus>255 || dev>31 || fn>7) throw new IOException("Unsupported GPU PCI address.");
                if(((bus<<8)|(dev<<3)|fn)!=bdf) continue;
                if(found!=null) throw new IOException("Ambiguous target GPU.");
                found=id;
                if(!String.Equals(Encoding.Unicode.GetString(Property(node,5)).TrimEnd('\0'),"nvlddmkm",StringComparison.OrdinalIgnoreCase))
                    throw new IOException("Target must use NVIDIA nvlddmkm.");
            }
            if(found==null) throw new IOException("Requested 90HX BDF not found.");
            uint problem,status=Status(found,out problem);
            if(problem!=0 || (status&8)==0) throw new IOException("Start from a healthy NVIDIA device (Code "+problem+"); reboot after previous failed experiment.");
            return found;
        }
        static void Wait(string id,bool enabled,Action<string> log,string phase) {
            DateTime end=DateTime.UtcNow.AddSeconds(30); uint problem=0,status=0;
            while(DateTime.UtcNow<end) {
                status=Status(id,out problem);
                if(enabled ? problem==0 && (status&8)!=0 : problem==22) {
                    log("FILTER_PNP_STATE phase="+phase+" code="+problem+" status=0x"+status.ToString("x")); return;
                }
                Thread.Sleep(200);
            }
            throw new IOException("PnP state timeout phase="+phase+" code="+problem+" status=0x"+status.ToString("x"));
        }
        static void Disable(string id,Action<string> log,string phase) {
            // Polite, no UI and no persistent-disable flag: reboot remains a recovery path.
            Check(CM_Disable_DevNode(Node(id),4),"Disable target "+phase);
            Wait(id,false,log,phase);
        }
        internal static int Run(FilterDiagnostics filter,uint bdf,Action<string> log,int observeSeconds) {
            string id=Target(bdf); log("FILTER_PNP_HANDOFF_BEGIN version=0.5.5 instance="+id);
            filter.ReadPci(bdf,log,"before-staged-unlock");
            filter.ArmProbe(bdf,log,7);
            Disable(id,log,"unlock-disable");
            uint first=CM_Enable_DevNode(Node(id),0);
            log("FILTER_PNP_FIRST_ENABLE CONFIGRET=0x"+first.ToString("x")+"; intentional failed START expected after successful staged unlock");
            // Configuration Manager may return before asynchronous START has finished.
            // Wait for the published release gate AND the failed devnode before restarting.
            DateTime stagedEnd=DateTime.UtcNow.AddSeconds(135);
            do {
                filter.Read(log,true); filter.ReadUnlock(log,true);
                if(filter.StagedReady) break;
                if(filter.UnlockFinished) break;
                Thread.Sleep(500);
            } while(DateTime.UtcNow<stagedEnd);
            if(!filter.StagedReady) throw new IOException("Staged unlock/DMA release not confirmed; no second restart. Reboot before another attempt.");
            uint firstProblem; DateTime stoppedEnd=DateTime.UtcNow.AddSeconds(30);
            do {
                Status(id,out firstProblem);
                if(firstProblem==10) break;
                Thread.Sleep(200);
            } while(DateTime.UtcNow<stoppedEnd);
            log("FILTER_PNP_INTERMEDIATE code="+firstProblem+"; full unlock verified, DMA released, NVIDIA handoff pending");
            if(firstProblem!=10) throw new IOException("Expected intermediate Code 10; unexpected state, refusing handoff restart.");
            Disable(id,log,"handoff-disable");
            Check(CM_Enable_DevNode(Node(id),0),"Enable target for NVIDIA handoff");
            Wait(id,true,log,"nvidia-started");
            filter.Read(log); filter.ReadPci(bdf,log,"after-nvidia-handoff");
            log("FILTER_PNP_HANDOFF_COMPLETE code=0; NVIDIA devnode started; verify telemetry and actual workload separately");
            if(observeSeconds>0) filter.ObservePci(bdf,log,observeSeconds);
            uint finalProblem; uint finalStatus=Status(id,out finalProblem);
            if(finalProblem!=0 || (finalStatus&8)==0) throw new IOException("NVIDIA lost ready state during observation; Code "+finalProblem);
            return 0;
        }
    }
}
