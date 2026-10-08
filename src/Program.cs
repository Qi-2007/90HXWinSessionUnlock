using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
namespace CMP90HX {
    static class Program {
        static TextWriter logFile;
        static int Main(string[] args) {
            Console.OutputEncoding=new UTF8Encoding(false);
            try {
                Dictionary<string,string> options=Parse(args);
                string command=args.Length==0 ? "" : args[0].ToLowerInvariant();
                if(command=="help" || command=="--help" || command=="") { Usage(); return 0; }
                if(options.ContainsKey("log")) {
                    string path=Path.GetFullPath(options["log"]);
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    logFile=new StreamWriter(path,true,new UTF8Encoding(false)) { AutoFlush=true };
                }
                if(command=="filter-unlock-handoff" || command=="filter-status" || command=="filter-watch" || command=="filter-resume-probe" || command=="filter-resume-dma-probe" || command=="filter-resume-gsp-init-probe" || command=="filter-probe-status" || command=="filter-resume-unlock" || command=="filter-unlock-status" || command=="filter-resume-sbr-probe" || command=="filter-pci-status" || command=="filter-start-probe" || command=="filter-start-reset-probe") {
                    if(options.ContainsKey("dma-backend")) throw new ArgumentException("Filter tracing has no DMA backend.");
                    int seconds=600;
                    if(options.ContainsKey("seconds") && (!Int32.TryParse(options["seconds"],out seconds) || seconds<1 || seconds>3600))
                        throw new ArgumentException("--seconds must be between 1 and 3600.");
                    int observeSeconds=60;
                    if(options.ContainsKey("observe-seconds") && (!Int32.TryParse(options["observe-seconds"],out observeSeconds) || observeSeconds<0 || observeSeconds>300 || observeSeconds%5!=0))
                        throw new ArgumentException("--observe-seconds must be 0..300, in multiples of 5.");
                    using(FilterDiagnostics filter=new FilterDiagnostics()) {
                        if(command=="filter-unlock-handoff") {
                            if(!options.ContainsKey("bdf")) throw new ArgumentException(command+" requires --bdf.");
                            return FilterPnpHandoff.Run(filter,ParseBdf(options["bdf"]),Log,observeSeconds);
                        } else if(command=="filter-resume-probe" || command=="filter-resume-dma-probe" || command=="filter-resume-gsp-init-probe" || command=="filter-resume-unlock" || command=="filter-resume-sbr-probe" || command=="filter-start-probe" || command=="filter-start-reset-probe") {
                            if(!options.ContainsKey("bdf")) throw new ArgumentException(command+" requires --bdf.");
                            uint target=ParseBdf(options["bdf"]);
                            bool startResult=command=="filter-start-probe" || command=="filter-start-reset-probe";
                            bool sourceResult=command=="filter-resume-unlock" || command=="filter-resume-sbr-probe" || startResult;
                            if(sourceResult) filter.ReadPci(target,Log,startResult?"before-device-restart":"before-sleep");
                            filter.ArmProbe(target,Log,command=="filter-start-reset-probe"?6U:command=="filter-start-probe"?5U:command=="filter-resume-sbr-probe"?4U:command=="filter-resume-unlock"?3U:command=="filter-resume-gsp-init-probe"?2U:command=="filter-resume-dma-probe"?1U:0U);
                            bool success=filter.Watch(Log,seconds,!sourceResult,sourceResult,target,sourceResult?observeSeconds:0); if(sourceResult) return success?0:1;
                        } else if(command=="filter-pci-status") {
                            if(!options.ContainsKey("bdf")) throw new ArgumentException(command+" requires --bdf.");
                            filter.ReadPci(ParseBdf(options["bdf"]),Log);
                        } else if(command=="filter-unlock-status") return filter.ReadUnlock(Log)?0:1;
                        else if(command=="filter-probe-status") filter.ReadProbe(Log);
                        else if(command=="filter-watch") filter.Watch(Log,seconds); else filter.Read(Log);
                    }
                    return 0;
                }
                throw new ArgumentException("This standalone worker supports only filter commands. Use help.");
            } catch(Exception error) { Log("FATAL: "+error); return 1; }
            finally { if(logFile!=null) logFile.Dispose(); }
        }
        static Dictionary<string, string> Parse(string[] args)
        {
            Dictionary<string, string> result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 1; i < args.Length; i++)
            {
                if (!args[i].StartsWith("--")) throw new ArgumentException("Unexpected argument: " + args[i]);
                string key = args[i].Substring(2);
                if (result.ContainsKey(key)) throw new ArgumentException("Duplicate option --" + key);
                if (key == "tls-only" || key == "conservative" || key == "physical-dma-experiment") { result[key] = "1"; continue; }
                if (++i >= args.Length || args[i].StartsWith("--")) throw new ArgumentException("Missing value for --" + key);
                if (result.ContainsKey(key)) throw new ArgumentException("Duplicate option --" + key);
                result[key] = args[i];
            }
            return result;
        }

        static uint ParseBdf(string text)
        {
            string value = text.Trim();
            int colon = value.LastIndexOf(':'), dot = value.LastIndexOf('.');
            if (colon < 0 || dot < colon) throw new FormatException("BDF must look like 01:00.0 or 0000:01:00.0");
            string busText = value.Substring(0, colon);
            int prior = busText.LastIndexOf(':');
            if (prior >= 0) busText = busText.Substring(prior + 1);
            uint bus = Convert.ToUInt32(busText, 16);
            uint dev = Convert.ToUInt32(value.Substring(colon + 1, dot - colon - 1), 16);
            uint fn = Convert.ToUInt32(value.Substring(dot + 1), 16);
            if (bus > 255 || dev > 31 || fn > 7) throw new FormatException("BDF out of range.");
            return (bus << 8) | (dev << 3) | fn;
        }

        static void Log(string text) {
            string line=DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff ")+text;
            Console.WriteLine(line); if(logFile!=null) logFile.WriteLine(line);
        }
        static void Usage() {
            Console.WriteLine("CMP90HX standalone Lower Filter unlock worker 0.5.6");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-status [--log FILE] (transparent Lower Filter power/PnP trace)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-watch [--seconds 600] [--log FILE] (records sleep/resume, never unlocks)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-resume-probe --bdf 02:00.0 [--seconds 600] [--log FILE] (one-shot kernel D0 register/common-buffer probe, no GPU DMA/unlock)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-resume-dma-probe --bdf 02:00.0 [--seconds 600] [--log FILE] (one-shot kernel D0 GPU DMA probe, no reset/firmware/unlock)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-resume-gsp-init-probe --bdf 02:00.0 [--seconds 600] [--log FILE] (explicit destructive GSP-only reset then DMA probe; NVIDIA resume compatibility unverified)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-resume-unlock --bdf 02:00.0 [--seconds 600] [--observe-seconds 60] [--log FILE] (one-shot source full unlock before upper D0 completion; experimental)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-resume-sbr-probe --bdf 02:00.0 [--seconds 600] [--observe-seconds 60] [--log FILE] (one SBR, no firmware/DMA/unlock; NVIDIA recovery experiment)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-unlock-handoff --bdf 02:00.0 [--observe-seconds 180] [--log FILE] (two automatic disable/enable cycles; staged unlock then NVIDIA handoff, SYS 0.5.5)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-start-probe --bdf 02:00.0 [--seconds 600] [--observe-seconds 60] [--log FILE] (read-only next PnP START; restart the target device in another admin terminal, no sleep)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-start-reset-probe --bdf 02:00.0 [--seconds 600] [--observe-seconds 180] [--log FILE] (one Windows bus function reset in next PnP START; no manual SBR/DMA/firmware/unlock)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-pci-status --bdf 02:00.0 [--log FILE] (read-only PCI/MSI/MSI-X snapshot)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-unlock-status [--log FILE] (query full unlock result)");
            Console.WriteLine("  CMP90HXUnlocker.exe filter-probe-status [--log FILE] (query last D0 probe result)");
        }
    }
}
