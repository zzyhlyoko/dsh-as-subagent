// dsh-subagent.exe source.
// A small native launcher that drives "dsh --profile headless" as an out-of-process
// subagent WITHOUT PowerShell: it locates the dsh install, spawns node.exe + bin.js
// directly, pipes the task via stdin, and parses the --json NDJSON stream into a
// summary: {"exitCode","sessionId","answer","reason","error"}.
//
// Build (Windows, .NET Framework csc.exe):
//   C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /target:exe ^
//     /out:dsh-subagent.exe /r:System.Web.Extensions.dll dsh-subagent.cs
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace DshSubagent
{
    sealed class RunResult
    {
        public int ExitCode = 1;
        public string SessionId;
        public string Answer;
        public string Error;
        public object Reason;
    }

    static class Program
    {
        static readonly JavaScriptSerializer Json = new JavaScriptSerializer();

        static int Main(string[] args)
        {
            string task = null, sessionId = null, cwd = null;
            string provider = null, model = null, reasoningEffort = null;
            var patches = new List<string>();
            int retries = 1;
            double retryDelay = 2;
            bool raw = false;

            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i];
                if (a == "--task" || a == "-t") task = TakeValue(args, ref i);
                else if (a == "--session-id" || a == "-s") sessionId = TakeValue(args, ref i);
                else if (a == "--cwd" || a == "-c") cwd = TakeValue(args, ref i);
                else if (a == "--provider") provider = TakeValue(args, ref i);
                else if (a == "--model") model = TakeValue(args, ref i);
                else if (a == "--reasoning-effort") reasoningEffort = TakeValue(args, ref i);
                else if (a == "--patch" || a == "-p") { string v = TakeValue(args, ref i); if (v != null) patches.Add(v); }
                else if (a == "--retries") { string v = TakeValue(args, ref i); if (v != null) int.TryParse(v, out retries); }
                else if (a == "--retry-delay") { string v = TakeValue(args, ref i); if (v != null) double.TryParse(v, out retryDelay); }
                else if (a == "--raw") raw = true;
                else if (a == "--help" || a == "-h") { PrintHelp(); return 0; }
                else if (a != "-" && a.Length > 0 && a[0] == '-')
                {
                    Console.Error.WriteLine("dsh-subagent: unknown option " + a);
                    return 2;
                }
                else if (a != "-")
                {
                    task = task == null ? a : task + " " + a;
                }
            }

            if (task == null) task = ReadStdin();
            task = task == null ? "" : task.Trim();
            if (task.Length == 0)
            {
                Console.Error.WriteLine("dsh-subagent: empty task (use --task or pipe stdin).");
                return 2;
            }

            if (retries < 1) retries = 1;
            if (retryDelay < 0) retryDelay = 0;
            if (reasoningEffort != null &&
                reasoningEffort != "low" && reasoningEffort != "off" &&
                reasoningEffort != "high" && reasoningEffort != "max")
            {
                Console.Error.WriteLine("dsh-subagent: reasoning-effort must be low/off/high/max.");
                return 2;
            }

            string nodeExe, binJs;
            if (!FindInstall(out nodeExe, out binJs))
            {
                Console.Error.WriteLine("dsh-subagent: cannot locate dsh install (node.exe + @deepseek-ai/dsh/lib/bin.js).");
                return 2;
            }

            string oldCwd = null;
            if (cwd != null)
            {
                if (!Directory.Exists(cwd))
                {
                    Console.Error.WriteLine("dsh-subagent: cwd is not a directory: " + cwd);
                    return 2;
                }
                oldCwd = Directory.GetCurrentDirectory();
                Directory.SetCurrentDirectory(cwd);
            }

            string tmpPatch = null;
            if (provider != null || model != null || reasoningEffort != null)
            {
                string p = provider != null ? provider : "deepseek-official";
                string m = model != null ? model : "deepseek-flash";
                var lines = new List<string>();
                lines.Add("- id: agent-default-model");
                lines.Add("  config:");
                lines.Add("    provider: " + p);
                lines.Add("    model: " + m);
                if (reasoningEffort != null) lines.Add("    reasoningEffort: " + reasoningEffort);
                tmpPatch = Path.Combine(Path.GetTempPath(), "dsh-subagent-model-" + Guid.NewGuid().ToString("N") + ".yml");
                File.WriteAllLines(tmpPatch, lines.ToArray(), new UTF8Encoding(false));
            }

            try
            {
                var dshArgs = new List<string>();
                dshArgs.Add("--profile"); dshArgs.Add("headless");
                foreach (string f in patches) { dshArgs.Add("--patch"); dshArgs.Add(f); }
                if (tmpPatch != null) { dshArgs.Add("--patch"); dshArgs.Add(tmpPatch); }
                dshArgs.Add("--json");
                if (sessionId != null) { dshArgs.Add("--session-id"); dshArgs.Add(sessionId); }
                dshArgs.Add("-");

                string rSession = null, rAnswer = null, rError = null;
                object rReason = null;
                int exitCode = 1;

                for (int attempt = 1; attempt <= retries; attempt++)
                {
                    RunResult r = RunOnce(nodeExe, binJs, dshArgs, task);
                    rSession = r.SessionId; rAnswer = r.Answer; rError = r.Error; rReason = r.Reason;
                    exitCode = r.ExitCode;
                    if (exitCode == 0) break;
                    if (attempt < retries)
                    {
                        Console.Error.WriteLine("dsh-subagent: attempt " + attempt + "/" + retries +
                            " failed (exit=" + exitCode + "), retry in " + retryDelay + "s.");
                        System.Threading.Thread.Sleep((int)Math.Round(retryDelay * 1000.0));
                    }
                }

                if (raw)
                {
                    if (rAnswer != null) Console.WriteLine(rAnswer);
                }
                else
                {
                    var o = new Dictionary<string, object>();
                    o["exitCode"] = exitCode;
                    o["sessionId"] = rSession;
                    o["answer"] = rAnswer;
                    o["reason"] = rReason;
                    o["error"] = rError;
                    Console.WriteLine(Json.Serialize(o));
                }
                return exitCode;
            }
            finally
            {
                if (tmpPatch != null) { try { File.Delete(tmpPatch); } catch { } }
                if (oldCwd != null) Directory.SetCurrentDirectory(oldCwd);
            }
        }

        static RunResult RunOnce(string nodeExe, string binJs, List<string> dshArgs, string task)
        {
            var res = new RunResult();
            var all = new List<string>();
            all.Add(binJs);
            all.AddRange(dshArgs);

            var psi = new ProcessStartInfo();
            psi.FileName = nodeExe;
            psi.Arguments = JoinArgs(all);
            psi.UseShellExecute = false;
            psi.RedirectStandardInput = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = false; // pass stderr through to caller
            psi.CreateNoWindow = true;
            psi.StandardOutputEncoding = new UTF8Encoding(false);

            using (var proc = new Process())
            {
                proc.StartInfo = psi;
                proc.Start();
                byte[] taskBytes = new UTF8Encoding(false).GetBytes(task);
                proc.StandardInput.BaseStream.Write(taskBytes, 0, taskBytes.Length);
                proc.StandardInput.Close();
                string stdout = proc.StandardOutput.ReadToEnd();
                proc.WaitForExit();
                res.ExitCode = proc.ExitCode;

                foreach (string rawLine in stdout.Split('\n'))
                {
                    string line = rawLine.TrimEnd('\r', ' ', '\t');
                    if (line.Length == 0) continue;
                    try
                    {
                        var d = Json.DeserializeObject(line) as Dictionary<string, object>;
                        if (d == null || !d.ContainsKey("type")) continue;
                        string type = d["type"] as string;
                        if (type == "session") res.SessionId = d["sessionId"] as string;
                        else if (type == "final") res.Answer = d["text"] as string;
                        else if (type == "error") res.Error = d["message"] as string;
                        else if (type == "status" &&
                                 "turn_end".Equals(d["phase"] as string) &&
                                 d.ContainsKey("reason"))
                        {
                            res.Reason = d["reason"];
                        }
                    }
                    catch { }
                }
            }
            return res;
        }

        static bool FindInstall(out string nodeExe, out string binJs)
        {
            nodeExe = null; binJs = null;
            string path = Environment.GetEnvironmentVariable("PATH");
            if (path == null) return false;
            string[] dirs = path.Split(';');

            foreach (string d in dirs)
            {
                string dd = d == null ? "" : d.Trim();
                if (dd.Length == 0) continue;
                bool isDsh = File.Exists(Path.Combine(dd, "dsh.cmd")) ||
                             File.Exists(Path.Combine(dd, "dsh.ps1")) ||
                             File.Exists(Path.Combine(dd, "dsh.exe"));
                if (!isDsh) continue;
                string ne = Path.Combine(dd, "node.exe");
                string bj = Path.Combine(dd, "node_modules", "@deepseek-ai", "dsh", "lib", "bin.js");
                if (File.Exists(ne) && File.Exists(bj)) { nodeExe = ne; binJs = bj; return true; }
            }

            foreach (string d in dirs)
            {
                string dd = d == null ? "" : d.Trim();
                if (dd.Length == 0) continue;
                string ne = Path.Combine(dd, "node.exe");
                string bj = Path.Combine(dd, "node_modules", "@deepseek-ai", "dsh", "lib", "bin.js");
                if (File.Exists(ne) && File.Exists(bj)) { nodeExe = ne; binJs = bj; return true; }
            }
            return false;
        }

        static string JoinArgs(List<string> a)
        {
            var q = new List<string>(a.Count);
            foreach (string s in a) q.Add(QuoteArg(s));
            return string.Join(" ", q.ToArray());
        }

        static string QuoteArg(string a)
        {
            if (a == null || a.Length == 0) return "\"\"";
            if (a.IndexOfAny(new char[] { ' ', '\t', '"' }) < 0) return a;
            return "\"" + a.Replace("\"", "\\\"") + "\"";
        }

        static string TakeValue(string[] args, ref int i)
        {
            if (i + 1 < args.Length) { i++; return args[i]; }
            return null;
        }

        static string ReadStdin()
        {
            return Console.In.ReadToEnd();
        }

        static void PrintHelp()
        {
            Console.WriteLine("Usage: dsh-subagent [options] [task...]");
            Console.WriteLine("Drive `dsh --profile headless` as an out-of-process subagent.");
            Console.WriteLine("Options:");
            Console.WriteLine("  --task <text>          task text (default: positional args, else stdin)");
            Console.WriteLine("  --session-id <id>      resume a persisted session");
            Console.WriteLine("  --cwd <dir>            working directory for the child session");
            Console.WriteLine("  --provider <name>      provider route (default deepseek-official)");
            Console.WriteLine("  --model <id>           model id (default deepseek-flash when overriding)");
            Console.WriteLine("  --reasoning-effort <e> low / off / high / max");
            Console.WriteLine("  --patch <file>         extra --patch overlay (repeatable)");
            Console.WriteLine("  --retries <n>          total attempts on failure (default 1)");
            Console.WriteLine("  --retry-delay <sec>    delay between retries (default 2)");
            Console.WriteLine("  --raw                  print only the final answer");
            Console.WriteLine("  --help                 show this help");
        }
    }
}