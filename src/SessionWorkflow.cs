using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Threading;
using Microsoft.Win32;

namespace CMP90HX {
    // All installation happens in user mode. Full unlock follows completed START.
    static class SessionWorkflow {
        const string Service="CMP90HXUnlockSession";
        const uint CmDrpHardwareId=0x02; // cfgmgr32: REG_MULTI_SZ; 0x01 is device description.
        [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_ID_List_SizeW(out uint size,string filter,uint flags);
        [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_Device_ID_ListW(string filter,[Out] char[] ids,uint size,uint flags);
        [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Locate_DevNodeW(out uint node,string id,uint flags);
        [DllImport("cfgmgr32.dll")] static extern uint CM_Get_DevNode_Status(out uint status,out uint problem,uint node,uint flags);
        [DllImport("cfgmgr32.dll",CharSet=CharSet.Unicode)] static extern uint CM_Get_DevNode_Registry_PropertyW(uint node,uint property,out uint type,[Out] byte[] bytes,ref uint size,uint flags);
        [DllImport("cfgmgr32.dll")] static extern uint CM_Disable_DevNode(uint node,uint flags);
        [DllImport("cfgmgr32.dll")] static extern uint CM_Enable_DevNode(uint node,uint flags);
        [DllImport("newdev.dll",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent,string hardwareId,string inf,uint flags,[MarshalAs(UnmanagedType.Bool)] out bool reboot);
        [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupGetInfDriverStoreLocationW(string file,IntPtr platform,string locale,StringBuilder buffer,uint chars,out uint required);
        [DataContract] internal sealed class Recovery {
            [DataMember] public int Schema;
            [DataMember] public uint Bdf;
            [DataMember] public string Instance,HardwareId,NvidiaInf,NvidiaHash;
            [DataMember] public bool MayHaveDma;
        }
        static string RecoveryPath { get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),@"CMP90HX\session-restore.json"); } }
        static void Check(uint result,string name) { if(result!=0) throw new IOException(name+" CONFIGRET=0x"+result.ToString("x")); }
        static uint Node(string id) { uint node; Check(CM_Locate_DevNodeW(out node,id,0),"Locate target 90HX"); return node; }
        static byte[] Property(uint node,uint property) {
            byte[] bytes=new byte[2048]; uint type,size=(uint)bytes.Length;
            Check(CM_Get_DevNode_Registry_PropertyW(node,property,out type,bytes,ref size,0),"Read PnP property");
            if(size>bytes.Length) throw new IOException("Invalid PnP property size.");
            Array.Resize(ref bytes,(int)size); return bytes;
        }
        static string TextProperty(uint node,uint property) { return Encoding.Unicode.GetString(Property(node,property)).TrimEnd('\0'); }
        static uint Status(string id,out uint problem) {
            uint status; Check(CM_Get_DevNode_Status(out status,out problem,Node(id),0),"Read PnP state");
            if((status&0x400)==0) problem=0; return status;
        }
        static string Unique(uint bdf) {
            char[] ids=null;
            for(int attempt=0;attempt<3;attempt++) {
                uint size; Check(CM_Get_Device_ID_List_SizeW(out size,null,0x100),"List present devices size");
                if(size>1048576) throw new IOException("PnP list exceeds bound.");
                ids=new char[size]; uint result=CM_Get_Device_ID_ListW(null,ids,size,0x100);
                if(result==0) break;
                if(result!=0x1a || attempt==2) Check(result,"List present devices");
            }
            List<string> matches=new List<string>();
            foreach(string id in new string(ids).Split(new[]{'\0'},StringSplitOptions.RemoveEmptyEntries))
                if(id.StartsWith(@"PCI\VEN_10DE&DEV_220D&",StringComparison.OrdinalIgnoreCase)) matches.Add(id);
            // UpdateDriver matches hardware IDs. Refuse ALL multi-90HX systems.
            if(matches.Count!=1) throw new IOException("Session binding requires exactly one present 90HX; found "+matches.Count);
            uint node=Node(matches[0]); byte[] busBytes=Property(node,0x16),addrBytes=Property(node,0x1d);
            if(busBytes.Length!=4 || addrBytes.Length!=4) throw new IOException("Invalid PCI location.");
            uint bus=BitConverter.ToUInt32(busBytes,0),addr=BitConverter.ToUInt32(addrBytes,0),dev=addr>>16,fn=addr&65535;
            if(bus>255 || dev>31 || fn>7 || ((bus<<8)|(dev<<3)|fn)!=bdf) throw new IOException("Target BDF mismatch.");
            return matches[0];
        }
        static string Hash(string path) { using(var hash=SHA256.Create()) using(var file=File.OpenRead(path)) return BitConverter.ToString(hash.ComputeHash(file)).Replace("-",""); }
        static void Save(Recovery recovery,bool create) {
            Directory.CreateDirectory(Path.GetDirectoryName(RecoveryPath));
            string temporary=RecoveryPath+"."+Guid.NewGuid().ToString("N")+".tmp";
            using(var stream=new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.None,4096,FileOptions.WriteThrough)) {
                new DataContractJsonSerializer(typeof(Recovery)).WriteObject(stream,recovery); stream.Flush(true);
            }
            if(create) File.Move(temporary,RecoveryPath);
            else File.Replace(temporary,RecoveryPath,null);
        }
        static Recovery Capture(string id,uint bdf) {
            uint node=Node(id); string key=TextProperty(node,0x0a);
            string published=(string)Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\"+key,"InfPath",null);
            if(String.IsNullOrEmpty(published) || Path.GetFileName(published)!=published) throw new IOException("Cannot identify installed NVIDIA INF.");
            StringBuilder path=new StringBuilder(260); uint required;
            if(!SetupGetInfDriverStoreLocationW(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"INF",published),IntPtr.Zero,null,path,260,out required))
                throw Native.Failure(Marshal.GetLastWin32Error(),"Resolve original NVIDIA INF in Driver Store");
            string hardware=null;
            foreach(string candidate in TextProperty(node,CmDrpHardwareId).Split(new[]{'\0'},StringSplitOptions.RemoveEmptyEntries)) {
                if(String.Equals(candidate,@"PCI\VEN_10DE&DEV_220D",StringComparison.OrdinalIgnoreCase) ||
                    candidate.StartsWith(@"PCI\VEN_10DE&DEV_220D&",StringComparison.OrdinalIgnoreCase)) {
                    hardware=candidate; break; // Preserve the first (most specific) matching hardware ID.
                }
            }
            if(hardware==null) throw new IOException("Target has no matching 90HX hardware ID (CM_DRP_HARDWAREID=0x02).");
            return new Recovery { Schema=1,Bdf=bdf,Instance=id,HardwareId=hardware,NvidiaInf=path.ToString(),NvidiaHash=Hash(path.ToString()) };
        }
        static void Wait(string id,string service,uint problem,Action<string> log) {
            DateTime end=DateTime.UtcNow.AddSeconds(30);
            while(DateTime.UtcNow<end) {
                uint actual; uint status=Status(id,out actual);
                if(actual==problem && (problem!=0 || (status&8)!=0) &&
                    (service==null || String.Equals(TextProperty(Node(id),5),service,StringComparison.OrdinalIgnoreCase))) {
                    log("SESSION_PNP_READY code="+actual+" service="+(service??"disabled")); return;
                }
                Thread.Sleep(200);
            }
            throw new IOException("Target failed to reach requested PnP state/service: "+service+" Code "+problem);
        }
        static void Bind(Recovery recovery,string inf,Action<string> log) {
            if(Unique(recovery.Bdf)!=recovery.Instance) throw new IOException("Device instance changed; refusing bind.");
            bool reboot; DateTime begin=DateTime.UtcNow;
            bool result=UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero,recovery.HardwareId,Path.GetFullPath(inf),5,out reboot);
            int error=result?0:Marshal.GetLastWin32Error();
            log("SESSION_BIND inf="+inf+" success="+result+" reboot="+reboot+" Win32="+error+" elapsedMs="+(long)(DateTime.UtcNow-begin).TotalMilliseconds);
            if(!result) throw Native.Failure(error,"Change exact 90HX function-driver binding");
            if(reboot) throw new IOException("Driver binding requires reboot; no unlock IOCTL will be submitted.");
        }
        static void Restore(Recovery recovery,Action<string> log,NvidiaRestoreBinding direct=null) {
            if(Hash(recovery.NvidiaInf)!=recovery.NvidiaHash) throw new IOException("Original NVIDIA INF changed; refusing replacement package.");
            log("SESSION_RESTORE_BEGIN exactOriginalInf="+recovery.NvidiaInf);
            if(Unique(recovery.Bdf)!=recovery.Instance) throw new IOException("Restore device instance changed.");
            bool installed=direct!=null && direct.Install(log);
            if(!installed) {
                log("SESSION_RESTORE_UPDATE: using original package update path");
                Bind(recovery,recovery.NvidiaInf,log);
            }
            var readyClock=System.Diagnostics.Stopwatch.StartNew();
            Check(CM_Enable_DevNode(Node(recovery.Instance),0),"Enable original NVIDIA device");
            Wait(recovery.Instance,"nvlddmkm",0,log);
            log("SESSION_RESTORE_READY elapsedMs="+readyClock.ElapsedMilliseconds);
            File.Delete(RecoveryPath); log("SESSION_RESTORE_COMPLETE NVIDIA Code 0; recovery record removed");
        }
        internal static int Run(uint bdf,string sessionInf,Action<string> log,int observeSeconds,bool directRestore) {
            if(!new WindowsPrincipal(WindowsIdentity.GetCurrent()).IsInRole(WindowsBuiltInRole.Administrator)) throw new IOException("Administrator required.");
            using(var mutex=new Mutex(false,@"Global\CMP90HX_FullUnlock")) {
                bool acquired=false; NvidiaRestoreBinding direct=null;
                try { try { acquired=mutex.WaitOne(0); } catch(AbandonedMutexException) { acquired=true; }
                    if(!acquired) throw new IOException("Another unlock workflow is active.");
                    if(File.Exists(RecoveryPath)) throw new IOException("Pending session recovery: run session-restore --bdf "+PciAddress.Bdf(bdf));
                    string id=Unique(bdf); uint problem; uint status=Status(id,out problem);
                    if(problem!=0 || (status&8)==0 || !String.Equals(TextProperty(Node(id),5),"nvlddmkm",StringComparison.OrdinalIgnoreCase))
                        throw new IOException("Start with healthy NVIDIA Code 0.");
                    sessionInf=Path.GetFullPath(sessionInf);
                    if(Path.GetFileName(sessionInf)!="CMP90HXUnlockSession.inf" || !File.ReadAllText(sessionInf).Contains("AddService=CMP90HXUnlockSession"))
                        throw new IOException("Expected the signed CMP90HXUnlockSession package INF.");
                    Recovery recovery=Capture(id,bdf);
                    if(directRestore) {
                        try { direct=NvidiaRestoreBinding.Capture(id,log); }
                        catch(Exception error) { log("SESSION_RESTORE_NODE_UNAVAILABLE: "+error.Message+"; using original update path"); }
                    }
                    Save(recovery,true);
                    log("SESSION_RECOVERY_SAVED file="+RecoveryPath+" original="+recovery.NvidiaInf);
                    bool mayRestore=true,unlocked=false;
                    try {
                        Check(CM_Disable_DevNode(Node(id),4),"Finish NVIDIA disable before session binding");
                        Wait(id,null,22,log); Thread.Sleep(750);
                        Bind(recovery,sessionInf,log);
                        Check(CM_Enable_DevNode(Node(id),0),"Enable temporary function driver");
                        Wait(id,Service,0,log);
                        log("SESSION_START_COMPLETED: temporary device Code 0; submitting independent unlock IOCTL now");
                        // ProcessNotify starts at driver load: use a new matching worker after START.
                        recovery.MayHaveDma=true; Save(recovery,false); mayRestore=false;
                        string worker=System.Reflection.Assembly.GetExecutingAssembly().Location;
                        var start=new System.Diagnostics.ProcessStartInfo(worker,"session-run-worker --bdf "+PciAddress.Bdf(bdf)) {
                            UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,
                            StandardOutputEncoding=new UTF8Encoding(false),WorkingDirectory=AppDomain.CurrentDomain.BaseDirectory
                        };
                        using(var child=System.Diagnostics.Process.Start(start)) {
                            log("SESSION_CHILD_STARTED pid="+child.Id+"; created after temporary driver initialization");
                            string line;
                            while((line=child.StandardOutput.ReadLine())!=null) log("SESSION_CHILD "+line);
                            child.WaitForExit();
                            // Only explicit safe completion codes permit rebind; crash/exception stays blocked.
                            mayRestore=child.ExitCode==0 || child.ExitCode==2;
                            unlocked=child.ExitCode==0;
                            log("SESSION_CHILD_EXIT code="+child.ExitCode+" safeRestore="+mayRestore);
                        }
                        if(!unlocked) throw new IOException("Session full unlock failed; see kernel result.");
                    } finally {
                        if(mayRestore) {
                            recovery.MayHaveDma=false; Save(recovery,false);
                            Restore(recovery,log,direct);
                        }
                        else log("SESSION_RESTORE_BLOCKED: DMA state not confirmed safe; retain recovery file and reboot before session-restore");
                    }
                    log("SESSION_DEVICE_READY: unlock and NVIDIA restore complete; observationSeconds="+observeSeconds);
                    DateTime end=DateTime.UtcNow.AddSeconds(observeSeconds);
                    while(DateTime.UtcNow<end) { Wait(id,"nvlddmkm",0,log); Thread.Sleep(5000); }
                    log("SESSION_UNLOCK_COMPLETE; verify retained unlock, power telemetry, workload and rendering responsiveness"); return 0;
                } finally { if(direct!=null) direct.Dispose(); if(acquired) mutex.ReleaseMutex(); }
            }
        }
        // Internal post-load worker. Parent holds the workflow mutex and saved recovery first.
        internal static int RunWorker(uint bdf,Action<string> log) {
            using(var mutex=new Mutex(false,@"Global\CMP90HX_FullUnlock")) {
                bool acquired=false;
                try { try { acquired=mutex.WaitOne(0); } catch(AbandonedMutexException) { acquired=true; }
                    if(acquired) throw new IOException("Session worker requires an active parent workflow.");
                } finally { if(acquired) mutex.ReleaseMutex(); }
            }
            Recovery recovery;
            using(var file=File.OpenRead(RecoveryPath)) recovery=(Recovery)new DataContractJsonSerializer(typeof(Recovery)).ReadObject(file);
            if(recovery.Schema!=1 || recovery.Bdf!=bdf || !recovery.MayHaveDma || Unique(bdf)!=recovery.Instance)
                throw new IOException("Session worker recovery target mismatch.");
            Wait(recovery.Instance,Service,0,log);
            bool unlocked=false,safe=false;
            using(var session=new FilterDiagnostics(true)) {
                if(!session.SessionCanRestore(log)) return 3;
                try { unlocked=session.RunSession(bdf,log); }
                catch(Exception error) { log("SESSION_WORKER_ERROR: "+error); }
                finally { safe=session.SessionCanRestore(log); }
            }
            // Handle is closed before parent sees this code and restores NVIDIA.
            return safe ? (unlocked?0:2) : 3;
        }
        internal static int Recover(uint bdf,Action<string> log) {
            using(var mutex=new Mutex(false,@"Global\CMP90HX_FullUnlock")) {
            bool acquired=false;
            try {
            try { acquired=mutex.WaitOne(0); } catch(AbandonedMutexException) { acquired=true; }
            if(!acquired) throw new IOException("Active unlock workflow; cannot recover concurrently.");
            Recovery recovery;
            using(var file=File.OpenRead(RecoveryPath)) recovery=(Recovery)new DataContractJsonSerializer(typeof(Recovery)).ReadObject(file);
            if(recovery.Schema!=1 || recovery.Bdf!=bdf || Unique(bdf)!=recovery.Instance) throw new IOException("Recovery target mismatch.");
            if(recovery.MayHaveDma) using(var session=new FilterDiagnostics(true)) {
                if(!session.SessionCanRestore(log)) throw new IOException("Busy/quarantined session: reboot before restoring NVIDIA.");
            }
            Restore(recovery,log); return 0;
            } finally { if(acquired) mutex.ReleaseMutex(); }
            }
        }
    }
}
