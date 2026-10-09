using System;
using System.IO;
using System.Runtime.InteropServices;

namespace CMP90HX {
    // Keep the currently installed NVIDIA node BEFORE changing to the System-class session driver.
    // Do not enumerate a cross-class list after binding the temporary driver.
    sealed class NvidiaRestoreBinding : IDisposable {
        [StructLayout(LayoutKind.Sequential)] struct DeviceInfo {
            public uint Size; public Guid Class; public uint Instance; public IntPtr Reserved;
        }
        [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct InstallParams {
            public uint Size,Flags,FlagsEx;
            public IntPtr Parent,Callback,Context,Queue,ClassReserved;
            public uint Reserved;
            [MarshalAs(UnmanagedType.ByValTStr,SizeConst=260)] public string DriverPath;
        }
        [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct DriverInfo {
            public uint Size,Type; public IntPtr Reserved;
            [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string Description;
            [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string Manufacturer;
            [MarshalAs(UnmanagedType.ByValTStr,SizeConst=256)] public string Provider;
            public System.Runtime.InteropServices.ComTypes.FILETIME Date;
            public ulong Version;
        }
        [DllImport("setupapi.dll",SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(IntPtr classGuid,IntPtr parent);
        [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiOpenDeviceInfoW(IntPtr set,string id,IntPtr parent,uint flags,ref DeviceInfo info);
        [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiGetDeviceInstallParamsW(IntPtr set,ref DeviceInfo info,ref InstallParams parameters);
        [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiSetDeviceInstallParamsW(IntPtr set,ref DeviceInfo info,ref InstallParams parameters);
        [DllImport("setupapi.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiBuildDriverInfoList(IntPtr set,ref DeviceInfo info,uint type);
        [DllImport("setupapi.dll",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiEnumDriverInfoW(IntPtr set,ref DeviceInfo info,uint type,uint index,ref DriverInfo driver);
        [DllImport("setupapi.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiDestroyDriverInfoList(IntPtr set,ref DeviceInfo info,uint type);
        [DllImport("setupapi.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("newdev.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        static extern bool DiInstallDevice(IntPtr parent,IntPtr set,ref DeviceInfo info,ref DriverInfo driver,uint flags,[MarshalAs(UnmanagedType.Bool)] out bool reboot);
        IntPtr set; DeviceInfo device; DriverInfo driver; bool list;
        static void Check(bool result,string operation) {
            if(!result) throw Native.Failure(Marshal.GetLastWin32Error(),operation);
        }
        internal static NvidiaRestoreBinding Capture(string id,Action<string> log) {
            if(IntPtr.Size!=8 || Marshal.SizeOf(typeof(DeviceInfo))!=32 || Marshal.SizeOf(typeof(InstallParams))!=584 || Marshal.SizeOf(typeof(DriverInfo))!=1568)
                throw new IOException("Unexpected x64 SetupAPI structure layout.");
            var binding=new NvidiaRestoreBinding();
            var clock=System.Diagnostics.Stopwatch.StartNew();
            try {
                binding.set=SetupDiCreateDeviceInfoList(IntPtr.Zero,IntPtr.Zero);
                if(binding.set==new IntPtr(-1)) { binding.set=IntPtr.Zero; throw Native.Failure(Marshal.GetLastWin32Error(),"Create NVIDIA restore device set"); }
                binding.device.Size=32;
                Check(SetupDiOpenDeviceInfoW(binding.set,id,IntPtr.Zero,0,ref binding.device),"Open exact NVIDIA device instance");
                if(binding.device.Class!=new Guid("4d36e968-e325-11ce-bfc1-08002be10318")) throw new IOException("Capture NVIDIA node requires Display class.");
                var parameters=new InstallParams { Size=584 };
                Check(SetupDiGetDeviceInstallParamsW(binding.set,ref binding.device,ref parameters),"Read original device install parameters");
                parameters.Flags|=0x00800000; // DI_QUIETINSTALL
                parameters.FlagsEx|=0x04000800; // INSTALLEDDRIVER | ALLOWEXCLUDEDDRVS
                Check(SetupDiSetDeviceInstallParamsW(binding.set,ref binding.device,ref parameters),"Select installed driver enumeration only");
                Check(SetupDiBuildDriverInfoList(binding.set,ref binding.device,2),"Build installed NVIDIA driver node"); binding.list=true;
                binding.driver.Size=1568;
                Check(SetupDiEnumDriverInfoW(binding.set,ref binding.device,2,0,ref binding.driver),"Read original installed NVIDIA node");
                var extra=new DriverInfo { Size=1568 };
                bool additional=SetupDiEnumDriverInfoW(binding.set,ref binding.device,2,1,ref extra);
                int enumerationError=additional?0:Marshal.GetLastWin32Error();
                if(additional || enumerationError!=259) throw new IOException("Expected exactly one installed NVIDIA driver node; error="+enumerationError);
                if(!String.Equals(binding.driver.Provider,"NVIDIA",StringComparison.OrdinalIgnoreCase)) throw new IOException("Installed restore node provider is not NVIDIA.");
                log("SESSION_RESTORE_NODE_SAVED provider="+binding.driver.Provider+" version=0x"+binding.driver.Version.ToString("x")+" elapsedMs="+clock.ElapsedMilliseconds);
                return binding;
            } catch { binding.Dispose(); throw; }
        }
        internal bool Install(Action<string> log) {
            var parameters=new InstallParams { Size=584 };
            Check(SetupDiGetDeviceInstallParamsW(set,ref device,ref parameters),"Read saved NVIDIA install parameters");
            parameters.Flags|=0x01800000; // QUIETINSTALL | NOFILECOPY: same healthy package, no NVIDIA files replaced.
            Check(SetupDiSetDeviceInstallParamsW(set,ref device,ref parameters),"Restore installed NVIDIA without duplicate file copy");
            var clock=System.Diagnostics.Stopwatch.StartNew(); bool reboot;
            bool result=DiInstallDevice(IntPtr.Zero,set,ref device,ref driver,0,out reboot);
            int error=result?0:Marshal.GetLastWin32Error();
            log("SESSION_RESTORE_DIRECT success="+result+" reboot="+reboot+" Win32="+error+" installMs="+clock.ElapsedMilliseconds+" noFileCopy=True");
            if(reboot) throw new IOException("Direct NVIDIA restore requires reboot; retain recovery record.");
            return result;
        }
        public void Dispose() {
            if(set!=IntPtr.Zero) {
                if(list) SetupDiDestroyDriverInfoList(set,ref device,2);
                SetupDiDestroyDeviceInfoList(set); set=IntPtr.Zero; list=false;
            }
        }
    }
}