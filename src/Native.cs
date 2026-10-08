using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace CMP90HX {
    static class Native {
        internal static Win32Exception Failure(int error,string operation) {
            return new Win32Exception(error,String.Format("{0}: Win32={1} (0x{2:x8}): {3}",operation,error,unchecked((uint)error),new Win32Exception(error).Message));
        }
        [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true,EntryPoint="CreateFileW")]
        internal static extern SafeFileHandle CreateFile(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
        [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]
        internal static extern bool DeviceIoControl(SafeFileHandle handle,uint code,byte[] input,int inputSize,byte[] output,int outputSize,out uint returned,IntPtr overlapped);
    }
    static class PciAddress {
        internal static string Bdf(uint bdf) { return String.Format("0000:{0:x2}:{1:x2}.{2}",bdf>>8,(bdf>>3)&31,bdf&7); }
    }
}