using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Collections.Generic;

namespace CMP90HX
{
    // Runs actual relocated reference/candidate ELF code against MockHardware.
    static class RebuiltCoreTests
    {
        static void Check(bool value,string message) { if(!value) throw new IOException(message); }
        sealed class ResetHardware : IHardware
        {
            internal readonly MockHardware Inner=new MockHardware();
            internal readonly List<string> Trace=new List<string>();
            internal int Pending;
            readonly bool switchAfterReset;
            public uint ReadPci(uint b,uint o,int s) { return Inner.ReadPci(b,o,s); }
            public void WritePci(uint b,uint o,uint v,int s) { Inner.WritePci(b,o,v,s); }
            public uint ReadMmio(ulong a) {
                if(a==MockHardware.Bar+0x11010c && Pending>0) { Pending--; return 6; }
                return Inner.ReadMmio(a);
            }
            public void WriteMmio(ulong a,uint v) {
                Trace.Add("W:"+(a-MockHardware.Bar).ToString("x")+":"+v.ToString("x"));
                Inner.WriteMmio(a,v);
                if(switchAfterReset && a==MockHardware.Bar+0x1103c0 && (v&1)==0)
                    Inner.WriteMmio(MockHardware.Bar+0x111668,0x10);
            }
            public void Delay(int ms) { Trace.Add("D:"+ms); }
            public void Dispose() { Inner.Dispose(); }
            internal ResetHardware(uint switchValue,int pending,bool switchAgain) {
                switchAfterReset=switchAgain;
                Inner.WriteMmio(MockHardware.Bar+0x1100f4,0x400);
                Inner.WriteMmio(MockHardware.Bar+0x111668,1);
                Inner.WriteMmio(MockHardware.Bar+0x111668,switchValue);
                Inner.WriteMmio(MockHardware.Bar+0x1103c0,0x80);
                Inner.WriteMmio(MockHardware.Bar+0x11010c,0);
                Pending=pending;
            }
        }
        sealed class FalconHardware : IHardware {
            internal readonly MockHardware Inner=new MockHardware();
            internal readonly List<string> Trace=new List<string>();
            internal bool Timeout,DmaFailure;
            public uint ReadPci(uint b,uint o,int s){return Inner.ReadPci(b,o,s);}
            public void WritePci(uint b,uint o,uint v,int s){Inner.WritePci(b,o,v,s);}
            public uint ReadMmio(ulong a) {
                if(a==MockHardware.Bar+0x110100)return Timeout?0u:16u;
                if(a==MockHardware.Bar+0x110118)return DmaFailure?0xbadf1234u:2u;
                return Inner.ReadMmio(a);
            }
            public void WriteMmio(ulong a,uint v){Trace.Add("W:"+(a-MockHardware.Bar).ToString("x")+":"+v.ToString("x"));Inner.WriteMmio(a,v);}
            public void Delay(int ms){Trace.Add("D:"+ms);}
            public void Dispose(){Inner.Dispose();}
            internal FalconHardware() {
                foreach(uint o in new uint[]{0x110624,0x110600,0x1103c0,0x111668,0x11010c,0x110040,0x110044,0x1438,0x8241e0})Inner.WriteMmio(MockHardware.Bar+o,0);
                Inner.WriteMmio(MockHardware.Bar+0x1100f4,0x400);
                Inner.WriteMmio(MockHardware.Bar+0x111668,1);
                Inner.WriteMmio(MockHardware.Bar+0x1fa824,0x27fe000);Inner.WriteMmio(MockHardware.Bar+0x1fa828,0x27fee00);
            }
        }
        sealed class PmuHardware : IHardware {
            internal readonly MockHardware Inner=new MockHardware();
            internal readonly List<string> Trace=new List<string>();
            internal byte[] Rom;internal int Mode;bool Connected,Started;
            public uint ReadPci(uint b,uint o,int z){return Inner.ReadPci(b,o,z);}
            public void WritePci(uint b,uint o,uint v,int z){Inner.WritePci(b,o,v,z);}
            public uint ReadMmio(ulong a) {
                uint o=(uint)(a-MockHardware.Bar);
                if(o>=0x300000&&o<0x400000){int at=(int)(o-0x300000);return at+4<=Rom.Length?BitConverter.ToUInt32(Rom,at):0;}
                if(o==0x840100)return Mode==0||(Mode==1&&Connected)||(Mode==4&&Started)?16u:0xbadf5620u;
                if(o==0x12004c)return Mode==2?3u:0;
                if(o==0x120050)return Mode==3?0u:1u;
                if(o==0x10a1c0)return 0;
                if(o==0x10a100)return Mode==5&&Started?0u:16u;
                return Inner.ReadMmio(a);
            }
            public void WriteMmio(ulong a,uint v){Trace.Add("W:"+(a-MockHardware.Bar).ToString("x")+":"+v.ToString("x"));Inner.WriteMmio(a,v);if(a==MockHardware.Bar+0x12004c&&v==1)Connected=true;if(a==MockHardware.Bar+0x10a100&&v==2)Started=true;}
            public void Delay(int ms){Trace.Add("D:"+ms);}
            public void Dispose(){Inner.Dispose();}
            internal PmuHardware(int mode){Mode=mode;Rom=VdeRom(false);Put(Rom,0,0xaa55,2);Put(Rom,24,0x40,2);Put(Rom,0x40,0x52494350);Put(Rom,0x4a,24,2);Put(Rom,0x50,16,2);Rom[0x55]=0x80;if(mode==6)Rom[0x306]=0x85;}
        }
        sealed class BooterHardware : IHardware {
            internal readonly MockHardware Inner=new MockHardware();internal readonly List<string> Trace=new List<string>();
            internal int Mode;bool Started,Issued,ResetDone;
            public uint ReadPci(uint b,uint o,int z){return Inner.ReadPci(b,o,z);}
            public void WritePci(uint b,uint o,uint v,int z){Inner.WritePci(b,o,v,z);}
            public uint ReadMmio(ulong a) {
                uint o=(uint)(a-MockHardware.Bar);
                if(o==0x840100)return Mode==3||((Mode==11||Mode==12||Mode==13)&&!ResetDone)?0xbadf5620u:Started?((Mode==0||Mode==9||Mode==10||Mode>=11)?16u:0u):16u;
                if(o==0x8400f4&&Mode==12&&!ResetDone)return 0xbadf1234u;
                if(o==0x8403c0&&Mode==13&&!ResetDone)return 0xbadf0000u;
                if(o==0x8403c0&&Mode==13&&ResetDone)return 0;
                if(o==0x840118)return Mode==2&&Issued?0xbadf1234u:Mode==8&&Issued?0u:2u;
                if(o==0x840040||o==0x840044)return Started&&(Mode==0||Mode==9||Mode==10)?(o==0x840040&&Mode==9?1u:0u):Inner.ReadMmio(a);
                if(o==0x1fa824)return Started?0x2720100u:0x27fe000u;
                if(o==0x1fa828)return 0x27fee00;
                if(o==0x8411a4&&Started&&Mode==10)return 1;
                if(o==0x8400d0&&Started&&Mode==6)return 0x1234;
                if(o==0x400c00)return Started&&(Mode==1||Mode==6)?0xffffffffu:1u;
                return Inner.ReadMmio(a);
            }
            public void WriteMmio(ulong a,uint v){Trace.Add("W:"+(a-MockHardware.Bar).ToString("x")+":"+v.ToString("x"));Inner.WriteMmio(a,v);if(a==MockHardware.Bar+0x8403c0&&(v&1)==0)ResetDone=true;if(a==MockHardware.Bar+0x840118)Issued=true;if((a==MockHardware.Bar+0x840100||a==MockHardware.Bar+0x840130)&&v==2)Started=true;}
            public void Delay(int ms){Trace.Add("D:"+ms);}
            public void Dispose(){Inner.Dispose();}
            internal BooterHardware(int mode){Mode=mode;foreach(uint o in new uint[]{0x8403c0,0x84010c,0x840624,0x840600,0x824148,0x840094,0x84005c,0x8400d0,0x8400bc,0x840040,0x840044,0x8411a4,0x840080,0x840108,0x840008})Inner.WriteMmio(MockHardware.Bar+o,0);Inner.WriteMmio(MockHardware.Bar+0x8400f4,0x80000400);Inner.WriteMmio(MockHardware.Bar+0x841668,1);}
        }
        static CoreSession Session(byte[] bytes,IHardware hardware)
        {
            Snapshot snapshot=new Gen2Engine(hardware,delegate(string text) {}).Capture(null);
            return new CoreSession(bytes,hardware,snapshot,
                delegate(ulong size,ulong physical) { throw new IOException("Unexpected firmware/DMA in differential policy test."); },
                delegate(ulong address,ulong size,ulong physical) { throw new IOException("Unexpected DMA free."); },
                delegate { throw new IOException("Unexpected reset."); },delegate(string text) {},null,false,
                delegate { throw new IOException("Unexpected handoff."); });
        }
        sealed class PrimitiveArena : IDisposable {
            internal readonly List<IntPtr> Pointers=new List<IntPtr>();
            internal readonly List<ulong> Sizes=new List<ulong>();
            internal readonly List<int> Freed=new List<int>();
            internal int FailAt=-1;
            internal ulong Physical=0x20000000;
            internal ulong Allocate(ulong size,ulong output) {
                if(Pointers.Count==FailAt)return 0;
                IntPtr p=Marshal.AllocHGlobal(checked((int)size));
                Marshal.Copy(new byte[checked((int)size)],0,p,checked((int)size));
                Marshal.WriteInt64(new IntPtr((long)output),(long)Physical);
                Physical+=(size+4095)&~4095UL;
                Pointers.Add(p);Sizes.Add(size);return (ulong)p.ToInt64();
            }
            internal void Free(ulong pointer,ulong size,ulong physical) {
                int index=Pointers.IndexOf(new IntPtr((long)pointer));
                Check(index>=0 && Sizes[index]==size && !Freed.Contains(index),"Primitive DMA release mismatch.");
                Freed.Add(index);
            }
            public void Dispose() {foreach(IntPtr p in Pointers)Marshal.FreeHGlobal(p);}
        }
        static CoreSession PrimitiveSession(byte[] bytes,IHardware io,PrimitiveArena arena) {
            Snapshot snapshot=new Gen2Engine(io,delegate(string text){}).Capture(null);
            return new CoreSession(bytes,io,snapshot,arena.Allocate,arena.Free,
                delegate{return 0;},delegate(string text){},null,false,delegate{return 0;});
        }
        static byte[] Bytes(IntPtr p,int size) {byte[] b=new byte[size];Marshal.Copy(p,b,0,size);return b;}
        static void Put(byte[] bytes,int offset,uint value,int count=4) {for(int j=0;j<count;j++)bytes[offset+j]=unchecked((byte)(value>>(j*8)));}
        static byte[] TestRom(int version) {
            byte[] p=new byte[0x2000];
            Put(p,0x100,0xb8ff,2);Put(p,0x102,0x544942);p[0x108]=12;p[0x109]=6;p[0x10a]=1;
            int sum=0;for(int i=0x100;i<0x10b;i++)sum+=p[i];p[0x10b]=unchecked((byte)-sum);
            p[0x10c]=0x70;p[0x10d]=2;Put(p,0x10e,4,2);Put(p,0x110,0x200,2);Put(p,0x200,0x300);
            p[0x300]=1;p[0x301]=6;p[0x302]=6;p[0x303]=1;p[0x306]=0x85;Put(p,0x308,0x400);
            int header=version==3?44:60,len=header+384,image=0x400+len;
            Put(p,0x400,(uint)((len<<16)|(version<<8)|1));Put(p,0x404,0x1000);
            if(version==3) {
                Put(p,0x408,0x5a4);Put(p,0x40c,0x1c);Put(p,0x410,0);Put(p,0x414,0x800);Put(p,0x418,0);
                Put(p,0x41c,0);Put(p,0x420,0x800);Put(p,0x424,0x400,2);p[0x426]=9;p[0x427]=1;
            } else {
                Put(p,0x410,0x1c);Put(p,0x418,0x800);Put(p,0x424,0x800);Put(p,0x428,0x800);Put(p,0x430,0x800);
            }
            for(int i=0;i<384;i++)p[0x400+header+i]=unchecked((byte)i);
            int dm=image+0x800;p[dm+0x1f]=2;Put(p,dm+0x20,4);Put(p,dm+0x24,0x100);Put(p,dm+0x108,0x200);Put(p,dm+0x10c,0x40);
            return p;
        }
        static void VbiosTests(byte[] original,byte[] rebuilt) {
            IntPtr rom=Marshal.AllocHGlobal(0x2000),v=Marshal.AllocHGlobal(24),fa=Marshal.AllocHGlobal(112),fb=Marshal.AllocHGlobal(112);
            try {
                foreach(int version in new int[]{2,3}) foreach(int mutation in new int[]{0,1,2,3,4})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware())
                using(PrimitiveArena aa=new PrimitiveArena()) using(PrimitiveArena ab=new PrimitiveArena())
                using(CoreSession ca=PrimitiveSession(original,a,aa)) using(CoreSession cb=PrimitiveSession(rebuilt,b,ab)) {
                    byte[] bytes=TestRom(version);
                    if(mutation==1)bytes[0x10b]++;if(mutation==2)bytes[0x10d]=1;
                    if(mutation==3)Put(bytes,0x400,0xffff0301);if(mutation==4)Put(bytes,0x420,0xffffffff);
                    Marshal.Copy(bytes,0,rom,bytes.Length);Marshal.Copy(new byte[24],0,v,24);
                    Marshal.WriteInt64(v,rom.ToInt64());Marshal.WriteInt32(v,8,bytes.Length);
                    int x=ca.Call("vbios_extract_fwsec",(ulong)v.ToInt64(),(ulong)fa.ToInt64(),0),y=cb.Call("rebuilt_vbios_fwsec",(ulong)v.ToInt64(),(ulong)fb.ToInt64(),0);
                    Check(x==y,"VBIOS parser return differs: version="+version+" mutation="+mutation);
                    if(x==0) {
                        Check(aa.Pointers.Count==ab.Pointers.Count,"FWSEC allocation count differs.");
                        for(int i=0;i<aa.Pointers.Count;i++)Check(BitConverter.ToString(Bytes(aa.Pointers[i],(int)aa.Sizes[i]))==BitConverter.ToString(Bytes(ab.Pointers[i],(int)ab.Sizes[i])),"FWSEC image/signature bytes differ.");
                        for(int off=0;off<112;off+=4)if(off!=0&&off!=4&&off!=72&&off!=76)Check(Marshal.ReadInt32(fa,off)==Marshal.ReadInt32(fb,off),"FWSEC descriptor differs at "+off.ToString("x"));
                        ca.Call("fwsec_ucode_free",(ulong)fa.ToInt64(),0,0);cb.Call("rebuilt_fwsec_free",(ulong)fb.ToInt64(),0,0);
                        Check(String.Join(",",aa.Freed)==String.Join(",",ab.Freed),"FWSEC release order differs.");
                    }
                }
                Console.WriteLine("REBUILT_VBIOS_DIFFERENTIAL_PASSED: V2/V3 BIT/FWSEC parser, malformed descriptors, exact image/signature bytes and descriptor/release ABI; synthetic ROM on actual ELF.");
            } finally {foreach(IntPtr p in new IntPtr[]{rom,v,fa,fb})Marshal.FreeHGlobal(p);}
        }
        static void FalconTests(byte[] original,byte[] rebuilt) {
            IntPtr rom=Marshal.AllocHGlobal(0x2000),v=Marshal.AllocHGlobal(24),fa=Marshal.AllocHGlobal(112),fb=Marshal.AllocHGlobal(112);
            try {
                foreach(int mode in new int[]{0,1,2})
                using(FalconHardware a=new FalconHardware()) using(FalconHardware b=new FalconHardware())
                using(PrimitiveArena aa=new PrimitiveArena()) using(PrimitiveArena ab=new PrimitiveArena()) {
                    a.Timeout=b.Timeout=mode==1;a.DmaFailure=b.DmaFailure=mode==2;
                    Func<byte[],FalconHardware,PrimitiveArena,CoreSession> make=delegate(byte[] code,FalconHardware h,PrimitiveArena arena) {
                        Snapshot snapshot=new Gen2Engine(h,delegate(string text){}).Capture(null);
                        return new CoreSession(code,h,snapshot,arena.Allocate,arena.Free,delegate{return 0;},delegate(string text){},null,false,delegate{return 0;});
                    };
                    using(CoreSession ca=make(original,a,aa)) using(CoreSession cb=make(rebuilt,b,ab)) {
                        byte[] bytes=TestRom(3);Put(bytes,0x428,1,2);Marshal.Copy(bytes,0,rom,bytes.Length);
                        Marshal.Copy(new byte[24],0,v,24);Marshal.WriteInt64(v,rom.ToInt64());Marshal.WriteInt32(v,8,bytes.Length);
                        Check(ca.Call("vbios_extract_fwsec",(ulong)v.ToInt64(),(ulong)fa.ToInt64(),0)==0 && cb.Call("rebuilt_vbios_fwsec",(ulong)v.ToInt64(),(ulong)fb.ToInt64(),0)==0,"Falcon fixture extraction failed.");
                        int x=ca.Call("nvperm_fwsec_execute_frts_managed",(ulong)fa.ToInt64(),0x27fe00000,0),y=cb.Call("rebuilt_fwsec_execute",(ulong)fb.ToInt64(),0x27fe00000,0);
                        Check(x==y,"FWSEC executor return differs: mode="+mode+" reference="+x+" source="+y);
                        Check(x==(mode==0?0:-1001),"FWSEC fixture did not exercise intended path.");
                        Check(String.Join(",",a.Trace)==String.Join(",",b.Trace),"FWSEC write/delay trace differs: mode="+mode);
                        Check(Marshal.ReadInt32(fa,100)==Marshal.ReadInt32(fb,100)&&Marshal.ReadInt32(fa,104)==Marshal.ReadInt32(fb,104),"FWSEC submission/start evidence differs.");
                        Check(BitConverter.ToString(Bytes(aa.Pointers[0],(int)aa.Sizes[0]))==BitConverter.ToString(Bytes(ab.Pointers[0],(int)ab.Sizes[0])),"Patched FRTS/signature bytes differ.");
                    }
                }
                Console.WriteLine("REBUILT_FALCON_DIFFERENTIAL_PASSED: managed reset and FWSEC success, DMA error, halt timeout, exact MMIO/delay trace and FRTS/signature bytes; synthetic device on actual ELF.");
            } finally {foreach(IntPtr p in new IntPtr[]{rom,v,fa,fb})Marshal.FreeHGlobal(p);}
        }
        static void PowerTests(byte[] original,byte[] rebuilt) {
            foreach(uint value in new uint[]{0,1,0x12345678,0xffffffff,0xbadf1234})
            using(FalconHardware a=new FalconHardware()) using(FalconHardware b=new FalconHardware())
            using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                foreach(uint o in new uint[]{0x200,0x600,0x41a610,0x400614,0x820378,0x820414,0x8202d8}) {a.Inner.WriteMmio(MockHardware.Bar+o,value);b.Inner.WriteMmio(MockHardware.Bar+o,value);}
                ca.Call("nvperm_gr_power_enable",0,0,0);cb.Call("rebuilt_gr_power",0,0,0);
                ca.Call("nvperm_media_fuse_probe",0,0,0);cb.Call("rebuilt_media_probe",0,0,0);
                Check(String.Join(",",a.Trace)==String.Join(",",b.Trace),"Power/media write and delay trace differs.");
            }
            Console.WriteLine("REBUILT_POWER_DIFFERENTIAL_PASSED: exact GR and media writes/delays, normal and invalid readbacks.");
        }
        static byte[] VdeRom(bool legacy) {
            byte[] bytes=TestRom(3);bytes[0x306]=4;bytes[0x307]=1;
            int len=legacy?48:52,skip=legacy?0:4;
            Array.Clear(bytes,0x400,0x100);if(!legacy)Put(bytes,0x400,0x340201);
            uint[] fields={0x1000,0x1000,0x20,0x1c,0,0x800,0,0,0,0x800,0,0x800};
            for(int j=0;j<fields.Length;j++)Put(bytes,0x400+skip+j*4,fields[j]);
            int dm=0x400+len+0x800;bytes[dm+0x1c]=1;bytes[dm+0x1d]=1;
            Put(bytes,dm+0x20,1,2);Put(bytes,dm+0x22,0x100);
            Put(bytes,dm+0x108,0x400);Put(bytes,dm+0x10c,0x1800);Put(bytes,dm+0x110,0x420);Put(bytes,dm+0x114,0x1900);Put(bytes,dm+0x11c,7,2);
            return bytes;
        }
        static void PmuTests(byte[] original,byte[] rebuilt) {
            foreach(int mode in new int[]{0,1,2,3,4,5,6})
            using(PmuHardware a=new PmuHardware(mode)) using(PmuHardware b=new PmuHardware(mode))
            using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                int x=ca.Call("pmu_devinit_execute",0,0,0),y=cb.Call("rebuilt_pmu_devinit",0,0,0);
                Check(x==y,"PMU return differs: mode="+mode+" reference="+x+" source="+y);
                Check(x==(mode<=1||mode==4?0:-1),"PMU fixture wrong path: "+mode);
                Check(String.Join(",",a.Trace)==String.Join(",",b.Trace),"PMU write/delay trace differs: mode="+mode);
            }
            Console.WriteLine("REBUILT_PMU_DIFFERENTIAL_PASSED: already accessible, PRI recovery/timeouts, PROM/VDE load, halt timeout and invalid VDE; exact writes/delays.");
        }
        static void VdeTests(byte[] original,byte[] rebuilt) {
            IntPtr rom=Marshal.AllocHGlobal(0x2000),v=Marshal.AllocHGlobal(24),da=Marshal.AllocHGlobal(80),db=Marshal.AllocHGlobal(80);
            try {
                foreach(int legacy in new int[]{0,1}) foreach(int mode in new int[]{0,1,2,3,4,5})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware())
                using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                    byte[] bytes=TestRom(3);bytes[0x306]=4;bytes[0x307]=1;
                    int len=legacy==0?52:48,skip=legacy==0?4:0;
                    Array.Clear(bytes,0x400,0x100);if(legacy==0)Put(bytes,0x400,0x340201);
                    uint[] fields={0x1000,0x1000,0x20,0x1c,0,0x800,0,0,0,0x800,0,0x800};
                    for(int j=0;j<fields.Length;j++)Put(bytes,0x400+skip+j*4,fields[j]);
                    int dm=0x400+len+0x800;bytes[dm+0x1c]=1;bytes[dm+0x1d]=1;
                    Put(bytes,dm+0x20,1,2);Put(bytes,dm+0x22,0x100);
                    Put(bytes,dm+0x108,0x400);Put(bytes,dm+0x10c,0x1800);Put(bytes,dm+0x110,0x420);Put(bytes,dm+0x114,0x1900);Put(bytes,dm+0x11c,7,2);
                    if(mode==1)bytes[dm+0x1d]=2;
                    if(mode==2)Put(bytes,dm+0x22,0xffff);
                    if(mode==3)Put(bytes,dm+0x10c,0xffff);
                    if(mode==4)Put(bytes,0x400+skip,0xffff);
                    if(mode==5)bytes[0x307]=2;
                    Marshal.Copy(bytes,0,rom,bytes.Length);Marshal.Copy(new byte[24],0,v,24);Marshal.WriteInt64(v,rom.ToInt64());Marshal.WriteInt32(v,8,bytes.Length);
                    int x=ca.Call("vbios_extract_vde",(ulong)v.ToInt64(),(ulong)da.ToInt64(),0),y=cb.Call("rebuilt_vbios_vde",(ulong)v.ToInt64(),(ulong)db.ToInt64(),0);
                    Check(x==y,"VDE parse return differs: legacy="+legacy+" mode="+mode);
                    if(x==0)for(int off=0;off<80;off+=4)Check(Marshal.ReadInt32(da,off)==Marshal.ReadInt32(db,off),"VDE descriptor differs: legacy="+legacy+" mode="+mode+" offset="+off.ToString("x"));
                }
                Console.WriteLine("REBUILT_VDE_DIFFERENTIAL_PASSED: legacy and versioned PMU images, DEVENG tables/scripts and invalid ranges.");
            }finally{foreach(IntPtr p in new IntPtr[]{rom,v,da,db})Marshal.FreeHGlobal(p);}
        }
        static void BooterTests(byte[] rebuilt) {
            IntPtr oa=Marshal.AllocHGlobal(96),ob=Marshal.AllocHGlobal(96);
            try {
                foreach(int mode in new int[]{0,1,2,3,4,6,8,9,10,11,12,13}) foreach(uint fuse in new uint[]{0,1})
                using(BooterHardware a=new BooterHardware(mode)) using(BooterHardware b=new BooterHardware(mode))
                using(PrimitiveArena aa=new PrimitiveArena()) using(PrimitiveArena ab=new PrimitiveArena())
                using(CoreSession ca=PrimitiveSession(rebuilt,a,aa)) using(CoreSession cb=PrimitiveSession(rebuilt,b,ab)) {
                    a.Inner.WriteMmio(MockHardware.Bar+0x824148,fuse);b.Inner.WriteMmio(MockHardware.Bar+0x824148,fuse);
                    ulong meta=0x123450000,wpr=0x027fee0002720100;
                    int x=ca.Core.Call("reference_booter_test",ca.G,meta,(ulong)oa.ToInt64(),0x400c00,wpr),y=cb.Core.Call("rebuilt_booter_test",cb.G,meta,(ulong)ob.ToInt64(),0x400c00,wpr);
                    Check(x==y,"Booter return differs: mode="+mode+" reference="+x+" source="+y);
                    Check(x==((mode==0||mode==9||mode==10||mode>=11)?0:(mode==1||mode==6)?1:-1001),"Booter fixture wrong path: "+mode+" return="+x);
                    if(mode==3) {Check(!b.Trace.Exists(delegate(string t){return t.StartsWith("W:840118:")||t=="W:840100:2";})&&ab.Freed.Count==ab.Pointers.Count,"Inaccessible SEC2 after reset submitted DMA/started firmware or leaked allocation.");continue;}
                    Check(String.Join(",",a.Trace)==String.Join(",",b.Trace),"Booter write/delay trace differs: mode="+mode);
                    Check(aa.Pointers.Count==ab.Pointers.Count&&String.Join(",",aa.Freed)==String.Join(",",ab.Freed),"Booter allocation/release differs.");
                    for(int j=0;j<aa.Pointers.Count;j++)Check(BitConverter.ToString(Bytes(aa.Pointers[j],(int)aa.Sizes[j]))==BitConverter.ToString(Bytes(ab.Pointers[j],(int)ab.Sizes[j])),"Booter image/signature differs.");
                    for(int off=0;off<96;off+=4)if(off!=72&&off!=76)Check(Marshal.ReadInt32(oa,off)==Marshal.ReadInt32(ob,off),"Booter evidence differs: mode="+mode+" off="+off.ToString("x")+" reference="+Marshal.ReadInt32(oa,off).ToString("x")+" source="+Marshal.ReadInt32(ob,off).ToString("x"));
                }
                foreach(int mode in new int[]{0,1,2})
                using(BooterHardware a=new BooterHardware(0)) using(BooterHardware b=new BooterHardware(0))
                using(PrimitiveArena aa=new PrimitiveArena()) using(PrimitiveArena ab=new PrimitiveArena())
                using(CoreSession ca=PrimitiveSession(rebuilt,a,aa)) using(CoreSession cb=PrimitiveSession(rebuilt,b,ab)) {
                    if(mode==0)aa.FailAt=ab.FailAt=0;
                    else {a.Inner.WriteMmio(MockHardware.Bar+0x824148,mode==1?2u:0xffffffffu);b.Inner.WriteMmio(MockHardware.Bar+0x824148,mode==1?2u:0xffffffffu);}
                    int x=ca.Core.Call("reference_booter_test",ca.G,0x123450000,(ulong)oa.ToInt64(),0x400c00,0x027fee0002720100),y=cb.Core.Call("rebuilt_booter_test",cb.G,0x123450000,(ulong)ob.ToInt64(),0x400c00,0x027fee0002720100);
                    Check(x==y&&x==-1,"Booter allocation/signature reject differs.");
                    Check(a.Trace.Count==0&&b.Trace.Count==0&&aa.Pointers.Count==ab.Pointers.Count&&String.Join(",",aa.Freed)==String.Join(",",ab.Freed),"Booter rejected input changed DMA ownership or device state.");
                }
                Console.WriteLine("REBUILT_BOOTER_DIFFERENTIAL_PASSED: both signatures; CPU/HWCFG2/engine inaccessible before reset recover with exact original writes, delays, image and evidence; halt/target-ready/error/timeout paths. Persistently inaccessible SEC2 after reset submits no DMA.");
            }finally{Marshal.FreeHGlobal(oa);Marshal.FreeHGlobal(ob);}
        }
        static void PrimitiveTests(byte[] original,byte[] rebuilt) {
            IntPtr input=Marshal.AllocHGlobal(4097),boot=Marshal.AllocHGlobal(0x6000),layout=Marshal.AllocHGlobal(24);
            IntPtr ia=Marshal.AllocHGlobal(480),ib=Marshal.AllocHGlobal(480),ma=Marshal.AllocHGlobal(24),mb=Marshal.AllocHGlobal(24);
            try {
                byte[] pattern=new byte[4097];new Random(902).NextBytes(pattern);Marshal.Copy(pattern,0,input,pattern.Length);
                Marshal.Copy(new byte[0x6000],0,boot,0x6000);
                Marshal.WriteInt64(layout,boot.ToInt64());Marshal.WriteInt32(layout,8,0x6000);Marshal.WriteInt32(layout,12,0x1800);Marshal.WriteInt64(layout,16,0x800);
                foreach(int size in new int[]{1,4096,4097}) foreach(int fail in new int[]{-1,0,1,2})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware())
                using(PrimitiveArena aa=new PrimitiveArena()) using(PrimitiveArena ab=new PrimitiveArena())
                using(CoreSession ca=PrimitiveSession(original,a,aa)) using(CoreSession cb=PrimitiveSession(rebuilt,b,ab)) {
                    aa.FailAt=ab.FailAt=fail;
                    Marshal.Copy(new byte[480],0,ia,480);Marshal.Copy(new byte[480],0,ib,480);
                    int x=ca.Core.Call("nvperm_boot_inputs_prepare",ca.G,(ulong)ia.ToInt64(),(ulong)input.ToInt64(),(ulong)size,(ulong)layout.ToInt64());
                    int y=cb.Core.Call("rebuilt_inputs_prepare",cb.G,(ulong)ib.ToInt64(),(ulong)input.ToInt64(),(ulong)size,(ulong)layout.ToInt64());
                    Check(x==y && aa.Pointers.Count==ab.Pointers.Count,"Input preparation result/allocation differs.");
                    for(int n=0;n<aa.Pointers.Count;n++)Check(BitConverter.ToString(Bytes(aa.Pointers[n],(int)aa.Sizes[n]))==BitConverter.ToString(Bytes(ab.Pointers[n],(int)ab.Sizes[n])),"Radix/image bytes differ.");
                    if(x==0) foreach(ulong fb in new ulong[]{0xaf00000,0x100000000,0x280000000,0x300000000}) {
                        int rx=ca.Core.Call("nvperm_boot_inputs_metadata_legacy_for_fb_size",ca.G,(ulong)ia.ToInt64(),fb,(ulong)size,(ulong)ma.ToInt64());
                        int ry=cb.Core.Call("rebuilt_inputs_metadata",cb.G,(ulong)ib.ToInt64(),fb,(ulong)size,(ulong)mb.ToInt64());
                        Check(rx==ry,"Metadata return differs.");
                        if(rx==0) {
                            IntPtr px=new IntPtr(Marshal.ReadInt64(ma)),py=new IntPtr(Marshal.ReadInt64(mb));
                            for(int off=0;off<4096;off+=8)Check(Marshal.ReadInt64(px,off)==Marshal.ReadInt64(py,off),"WPR metadata differs at "+off.ToString("x")+": original="+Marshal.ReadInt64(px,off).ToString("x")+" source="+Marshal.ReadInt64(py,off).ToString("x"));
                        }
                    }
                    ca.Core.Call("nvperm_boot_inputs_release",ca.G,(ulong)ia.ToInt64(),0,0);
                    cb.Core.Call("rebuilt_inputs_release",cb.G,(ulong)ib.ToInt64(),0,0);
                    Check(String.Join(",",aa.Freed)==String.Join(",",ab.Freed),"Input release order differs.");
                    Check(BitConverter.ToString(Bytes(ia,120))==BitConverter.ToString(Bytes(ib,120)),"Released descriptors differ.");
                }
                foreach(uint lmr in new uint[]{0,0xffffffff,0xbadf1234,0x288,0x10,0x3ff})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware())
                using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                    a.WriteMmio(MockHardware.Bar+0x100ce0,lmr);b.WriteMmio(MockHardware.Bar+0x100ce0,lmr);
                    Check(ca.Call("ga102_read_local_memory_size",(ulong)ma.ToInt64(),0,0)==cb.Call("rebuilt_local_memory_size",(ulong)mb.ToInt64(),0,0) && Marshal.ReadInt64(ma)==Marshal.ReadInt64(mb),"Local memory decode differs.");
                }
                Console.WriteLine("REBUILT_PRIMITIVE_DIFFERENTIAL_PASSED: real ELF radix/input allocations, failure injection, reverse release, full 4096-byte WPR metadata and memory decode.");
            } finally {foreach(IntPtr p in new IntPtr[]{input,boot,layout,ia,ib,ma,mb})Marshal.FreeHGlobal(p);}
        }
        internal static int Run(string referencePath,string candidatePath)
        {
            byte[] original=ElfCore.PinnedBytes(referencePath), rebuilt=ElfCore.PinnedBytes(candidatePath);
            PrimitiveTests(original,rebuilt);
            VbiosTests(original,rebuilt);
            FalconTests(original,rebuilt);
            PowerTests(original,rebuilt);
            VdeTests(original,rebuilt);
            PmuTests(original,rebuilt);
            BooterTests(rebuilt);
            IntPtr scratch=Marshal.AllocHGlobal(32);
            try {
                Random random=new Random(469);
                int samples=128;
                foreach(uint switchValue in new uint[]{0,1,0x10,0x91})
                foreach(int pending in new int[]{0,3,10000})
                foreach(bool switchAgain in new bool[]{false,true})
                using(ResetHardware a=new ResetHardware(switchValue,pending,switchAgain))
                using(ResetHardware b=new ResetHardware(switchValue,pending,switchAgain))
                using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                    int expected=ca.Call("gsp_falcon_reset",0,0,0);
                    int actual=cb.Call("gsp_falcon_reset",0,0,0);
                    Check(actual==expected,"GSP reset return differs.");
                    Check(String.Join(",",a.Trace)==String.Join(",",b.Trace),"GSP reset writes/delays differ.");
                }
                Console.WriteLine("REBUILT_GSP_RESET_DIFFERENTIAL_PASSED: 24 reset/switch/scrub scenarios, identical return codes and write/delay traces; actual ELF on mock hardware.");
                int failureSamples=0;
                foreach(uint mode in new uint[]{1,2})
                foreach(uint pmc in new uint[]{0xffffffff,0xbadf1234,0x2002})
                foreach(uint deviceEnable in new uint[]{0,0xffffffff,0xbadf5620})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware()) {
                    foreach(MockHardware h in new MockHardware[]{a,b}) {
                        h.WriteMmio(MockHardware.Bar+(mode==1?0x82381cu:0x823830u),0);
                        h.WriteMmio(MockHardware.Bar+0x200,pmc);
                        h.WriteMmio(MockHardware.Bar+0x600,deviceEnable);
                    }
                    using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                        int expected=ca.Call("do_permissive_with_handoff",mode,ca.Handoff,0);
                        int actual=cb.Call("do_permissive_with_handoff",mode,cb.Handoff,0);
                        Check(expected!=0 && actual==expected,"Feature early failure differs, mode="+mode+" PMC="+pmc.ToString("x"));
                    }
                    failureSamples++;
                }
                Console.WriteLine("REBUILT_FEATURE_FAILURE_DIFFERENTIAL_PASSED: "+failureSamples+" actual ELF transactions fail closed before handoff on inaccessible PMC or unavailable firmware.");
                foreach(uint wpr in new uint[]{0xffffffff,0xbadf1234,0})
                foreach(uint boot in new uint[]{0xffffffff,0,1})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware()) {
                    foreach(MockHardware h in new MockHardware[]{a,b}) {
                        h.WriteMmio(MockHardware.Bar+0x82381c,0);
                        h.WriteMmio(MockHardware.Bar+0x200,0x2002);h.WriteMmio(MockHardware.Bar+0x600,0);
                        h.WriteMmio(MockHardware.Bar+0x1fa828,wpr);
                        h.WriteMmio(MockHardware.Bar+0x118128,boot);h.WriteMmio(MockHardware.Bar+0x118234,0);
                    }
                    using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                        int expected=ca.Call("do_permissive_with_handoff",1,ca.Handoff,0), actual=cb.Call("do_permissive_with_handoff",1,cb.Handoff,0);
                        Check(expected==actual,"FWSEC readiness failure differs: wpr="+wpr.ToString("x")+" boot="+boot.ToString("x")+" original="+expected+" rebuilt="+actual);
                    }
                }
                foreach(uint target in Gen2Engine.PlmOffsets)
                foreach(uint pmc in new uint[]{0xffffffff,0xbadf1234,0x2002})
                using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware()) {
                    foreach(MockHardware h in new MockHardware[]{a,b}) {
                        h.WriteMmio(MockHardware.Bar+target,0xffffff8f);h.WriteMmio(MockHardware.Bar+0x200,pmc);
                    }
                    using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                        int expected=ca.Call("ga102_booter_open_plm_with_handoff",target,0,(ulong)scratch.ToInt64(),ca.Handoff);
                        int actual=cb.Call("ga102_booter_open_plm_with_handoff",target,0,(ulong)scratch.ToInt64(),cb.Handoff);
                        Check(expected!=0 && actual==expected,"PLM preparation failure differs.");
                    }
                }
                Console.WriteLine("REBUILT_TRANSACTION_FAILURE_DIFFERENTIAL_PASSED: nine FWSEC readiness and 27 PLM preparation cases; actual ELF on mock hardware.");
                for(int trial=0;trial<samples;trial++) using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware()) {
                    foreach(RegisterRule rule in Gen2Engine.Rules) {
                        uint value=(uint)random.Next();
                        a.WriteMmio(MockHardware.Bar+rule.Offset,value); b.WriteMmio(MockHardware.Bar+rule.Offset,value);
                    }
                    using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                        Check(ca.Call("pcie_run_pre_reset_group",2,0,0)==0 && cb.Call("pcie_run_pre_reset_group",2,0,0)==0,"Policy call failed.");
                        foreach(RegisterRule rule in Gen2Engine.Rules)
                            Check(a.ReadMmio(MockHardware.Bar+rule.Offset)==b.ReadMmio(MockHardware.Bar+rule.Offset),"Policy differs at "+rule.Name);
                        int aw=a.MmioWrites,bw=b.MmioWrites;
                        Check(ca.Call("pcie_run_pre_reset_group",2,0,0)==0 && cb.Call("pcie_run_pre_reset_group",2,0,0)==0,"Repeat policy failed.");
                        Check(a.MmioWrites-aw==5 && b.MmioWrites==bw,"Repeat policy write reduction failed.");
                        Check(ca.Call("pcie_restore_post_reset_group",2,(ulong)scratch.ToInt64(),0)==0,"Reference restore failed.");
                        byte[] expected=new byte[12]; Marshal.Copy(scratch,expected,0,12);
                        Check(cb.Call("pcie_restore_post_reset_group",2,(ulong)scratch.ToInt64(),0)==0,"Candidate restore failed.");
                        byte[] actual=new byte[12]; Marshal.Copy(scratch,actual,0,12);
                        Check(BitConverter.ToString(actual)==BitConverter.ToString(expected),"Restore output ABI differs.");
                    }
                }
                for(int closed=-1;closed<Gen2Engine.PlmOffsets.Length;closed++) using(MockHardware a=new MockHardware()) using(MockHardware b=new MockHardware()) {
                    if(closed>=0) { a.WriteMmio(MockHardware.Bar+Gen2Engine.PlmOffsets[closed],0xffffff8f); b.WriteMmio(MockHardware.Bar+Gen2Engine.PlmOffsets[closed],0xffffff8f); }
                    using(CoreSession ca=Session(original,a)) using(CoreSession cb=Session(rebuilt,b)) {
                        int ar=a.PciReads,br=b.PciReads;
                        int ra=ca.Call("pcie_find_first_closed_plm",(ulong)scratch.ToInt64(),0,0);
                        int ac=a.PciReads-ar;
                        uint expected=ra==1?unchecked((uint)Marshal.ReadInt32(new IntPtr(Marshal.ReadInt64(scratch)))):0;
                        int rb=cb.Call("pcie_find_first_closed_plm",(ulong)scratch.ToInt64(),0,0);
                        int bc=b.PciReads-br;
                        uint actual=rb==1?unchecked((uint)Marshal.ReadInt32(new IntPtr(Marshal.ReadInt64(scratch)))):0;
                        Check(ra==rb && expected==actual,"First-closed PLM differs.");
                        Check(bc==2,"Candidate identity validation count differs.");
                        if(closed<0) { Check(ac==9,"Reference scan count differs."); Console.WriteLine("PLM_SCAN_PCI_READS reference="+ac+" rebuilt="+bc); }
                    }
                }
                using(MockHardware b=new MockHardware()) using(CoreSession cb=Session(rebuilt,b)) {
                    b.WriteMmio(MockHardware.Bar+0x823834,0); // Explicit readable graphics protection status.
                    int writes=b.MmioWrites;
                    Check(cb.Call("do_permissive_with_handoff",1,cb.Handoff,0)==0,"Compute skip failed.");
                    Check(cb.Call("do_permissive_with_handoff",2,cb.Handoff,0)==0,"Graphics skip failed.");
                    uint after=0;
                    Marshal.WriteInt32(scratch,0);
                    Check(cb.Call("ga102_booter_open_plm_with_handoff",0x8200fc,0,(ulong)scratch.ToInt64(),cb.Handoff)==0,"Open PLM skip failed.");
                    after=unchecked((uint)Marshal.ReadInt32(scratch));
                    Check(after==0xffffffff && b.MmioWrites==writes && cb.HandoffCalls==0,"Skip path changed state or invoked handoff.");
                }
                using(MockHardware b=new MockHardware()) using(CoreSession cb=Session(rebuilt,b)) {
                    b.IgnoreMmioWrites=true;
                    Check(cb.Call("pcie_run_pre_reset_group",2,0,0)!=0,"Ignored policy writes were accepted.");
                }
                using(MockHardware b=new MockHardware()) using(CoreSession cb=Session(rebuilt,b)) {
                    b.SetPci(MockHardware.Gpu,0,0xffffffff,4);
                    Check(cb.Call("pcie_find_first_closed_plm",(ulong)scratch.ToInt64(),0,0)==-1,"Lost device was accepted.");
                }
                Console.WriteLine("REBUILT_ELF_DIFFERENTIAL_TEST_PASSED: "+samples+" randomized reference/candidate policies, restore ABI, all PLM positions, skips and failure gates; actual ELF execution on mock hardware only.");
                return 0;
            } finally { Marshal.FreeHGlobal(scratch); }
        }
    }
}
