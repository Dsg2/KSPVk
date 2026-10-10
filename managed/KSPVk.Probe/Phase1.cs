using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEngine;

namespace KSPVk.Probe
{
    /// Phase 1 selftest, run per bench step when the label contains "phase1":
    ///  - pixel identity: paused frame captured with KSPVk off / on / off (the off-off pair measures scene noise)
    ///  - depth hand-off: screenshot with the native test pattern (rects at 3 m, 30 m, 300 m, 3000 km)
    ///  - overhead: alternating on/off rounds, frame time + GPU span
    /// plus a resize / fullscreen round trip at the end of the tour.
    public static class Phase1
    {
        const int Rounds = 4;
        const float RoundSec = 3f;
        public static readonly List<string> Lines = new List<string> { "step | identity on vs off (pixels differing, max diff) | noise off vs off | frame ms off / on | GPU span ms off / on | Kick ms | VK GPU ms | composites/frame | skips | state" };

        public static IEnumerator Step(Probe p, string name, string dir)
        {
            var core = Core.Instance;
            if (core == null) { Lines.Add(name + " | KSPVk core missing"); yield break; }
            var s0 = core.Stats();
            float ts = Time.timeScale;
            Time.timeScale = 0;
            p.Status = "";
            // PQS keeps refining terrain while paused: wait until two consecutive KSPVk-off frames match, then test on vs off.
            Texture2D a = null, b = null, c = null;
            int tries = 0, nn = -1;
            for (; tries < 15 && nn != 0; tries++)
            {
                if (a) UnityEngine.Object.Destroy(a);
                if (c) UnityEngine.Object.Destroy(c);
                yield return Grab(core, false, t => a = t);
                yield return Grab(core, false, t => c = t);
                Diff(a, c, out nn);
                if (nn != 0) yield return new WaitForSecondsRealtime(1);
            }
            UnityEngine.Object.Destroy(c);
            yield return Grab(core, true, t => b = t);
            yield return Grab(core, false, t => c = t);
            string ident = Diff(a, b), noise = Diff(a, c) + $" ({tries} tries)";
            Diff(a, b, out int n); Diff(a, c, out nn);
            if (n > 0 || nn > 0)
            {
                File.WriteAllBytes(Path.Combine(dir, name + "_id_off1.png"), a.EncodeToPNG());
                File.WriteAllBytes(Path.Combine(dir, name + "_id_on.png"), b.EncodeToPNG());
                File.WriteAllBytes(Path.Combine(dir, name + "_id_off2.png"), c.EncodeToPNG());
            }
            core.DebugPattern = true; core.Enabled = true;
            for (int i = 0; i < 4; i++) yield return null;
            ScreenCapture.CaptureScreenshot(Path.Combine(dir, name + "_pattern.png"));
            yield return null; yield return null;
            core.DebugPattern = false;
            foreach (var t in new[] { a, b, c }) UnityEngine.Object.Destroy(t);
            Time.timeScale = ts;

            var off = new Capture(name + "_off"); var on = new Capture(name + "_on");
            double kickTicks = 0; int kickFrames = 0;
            for (int r = 0; r < Rounds; r++)
                foreach (bool en in new[] { false, true })
                {
                    core.Enabled = en;
                    p.Status = $"{name}: overhead round {r + 1}/{Rounds} ({(en ? "on" : "off")})";
                    for (int i = 0; i < 10; i++) yield return null;
                    long k0 = core.KickTicks; int f0 = Time.frameCount;
                    yield return p.Measure(en ? on : off, RoundSec);
                    if (en) { kickTicks += core.KickTicks - k0; kickFrames += Time.frameCount - f0; }
                }
            core.Enabled = true;
            var s1 = core.Stats();
            ulong frames = s1.begun - s0.begun;
            Lines.Add($"{name} | {ident} | {noise} | {Mean(off.FrameMs):F2} / {Mean(on.FrameMs):F2} | {Mean(off.GpuSpan):F2} / {Mean(on.GpuSpan):F2} | " +
                      $"{kickTicks * 1000.0 / System.Diagnostics.Stopwatch.Frequency / Math.Max(1, kickFrames):F3} | {s1.vkGpuMs:F3} | " +
                      $"{(frames > 0 ? (double)(s1.composites - s0.composites) / frames : 0):F2} | {s1.skipped - s0.skipped} frame / {s1.compositeSkips - s0.compositeSkips} composite | {s1.state}");
            Flush(dir);
        }

        public static IEnumerator ResizeTest(Probe p, string dir)
        {
            var core = Core.Instance;
            if (core == null) yield break;
            int w = Screen.width, h = Screen.height; var mode = Screen.fullScreenMode;
            var res = Screen.currentResolution;
            var tests = new (string name, int w, int h, FullScreenMode m)[]
            {
                ("resize_1024x576", 1024, 576, FullScreenMode.Windowed),
                ("fullscreen_native", res.width, res.height, FullScreenMode.FullScreenWindow),
                ("restore", w, h, mode),
            };
            core.DebugPattern = true;
            foreach (var t in tests)
            {
                p.Status = t.name;
                int r0 = core.Stats().resizes;
                Screen.SetResolution(t.w, t.h, t.m);
                yield return new WaitForSecondsRealtime(3);
                p.Status = "";
                yield return null;
                ScreenCapture.CaptureScreenshot(Path.Combine(dir, "p1_" + t.name + ".png"));
                yield return null; yield return null;
                var s = core.Stats();
                Lines.Add($"{t.name} | screen {Screen.width}x{Screen.height} {Screen.fullScreenMode} | shared images {s.width}x{s.height} | resizes +{s.resizes - r0} | state {s.state} | composites {s.composites}");
            }
            core.DebugPattern = false;
            Flush(dir);
        }

        /// Splits the composite overhead: KSPVk off vs each native composite mode, interleaved rounds.
        public static IEnumerator Experiment(Probe p, string name, string dir)
        {
            var core = Core.Instance;
            if (core == null) yield break;
            // "+p" = native test pattern on (content to composite); without it frames are empty and composites are skipped
            string[] labels = { "off", "0 empty", "4 skip +p", "1 sync only +p", "2 tiles no-wait +p", "3 full-screen +p", "0 tiles +p" };
            var caps = labels.Select(l => new Capture(name + " " + l)).ToArray();
            var waits = new ulong[labels.Length]; var begun = new ulong[labels.Length];
            for (int r = 0; r < 3; r++)
                for (int m = 0; m < labels.Length; m++)
                {
                    core.Enabled = m > 0;
                    if (m > 0) core.SetCompositeMode(int.Parse(labels[m].Split(' ')[0]));
                    core.DebugPattern = labels[m].EndsWith("+p");
                    p.Status = $"{name}: exp round {r + 1} {labels[m]}";
                    for (int i = 0; i < 10; i++) yield return null;
                    ulong w0 = core.Stats().gpuWaits, b0 = core.Stats().begun;
                    string t0 = DateTime.Now.ToString("HH:mm:ss.fff");
                    yield return p.Measure(caps[m], 4f);
                    File.AppendAllText(Path.Combine(dir, "exp_rounds.txt"), $"{name}\t{labels[m]}\t{t0}\t{DateTime.Now:HH:mm:ss.fff}\t{Mean(caps[m].FrameMs):F3}\n");
                    var st = core.Stats(); waits[m] += st.gpuWaits - w0; begun[m] += st.begun - b0;
                }
            core.SetCompositeMode(0); core.Enabled = true; core.DebugPattern = false;
            for (int m = 0; m < labels.Length; m++)
                Lines.Add($"exp {name} | {labels[m]} | frame {Mean(caps[m].FrameMs):F2} ms | GPU span {Mean(caps[m].GpuSpan):F2} ms | cameras GPU {caps[m].CamGpuSum.Values.Sum() / Math.Max(1, caps[m].GpuFrames):F2} ms | GPU waits/frame {(begun[m] > 0 ? (double)waits[m] / begun[m] : 0):F2}");
            Flush(dir);
        }

        static IEnumerator Grab(Core core, bool enabled, Action<Texture2D> got)
        {
            core.Enabled = enabled;
            for (int i = 0; i < 6; i++) yield return null;
            yield return new WaitForEndOfFrame();
            got(ScreenCapture.CaptureScreenshotAsTexture());
        }

        static string Diff(Texture2D x, Texture2D y) => Diff(x, y, out _);

        static string Diff(Texture2D x, Texture2D y, out int n)
        {
            n = -1;
            if (x == null || y == null || x.width != y.width || x.height != y.height) return "size mismatch";
            var A = x.GetPixels32(); var B = y.GetPixels32();
            int max = 0; n = 0;
            for (int i = 0; i < A.Length; i++)
            {
                int d = Math.Max(Math.Max(Math.Abs(A[i].r - B[i].r), Math.Abs(A[i].g - B[i].g)), Math.Abs(A[i].b - B[i].b));
                if (d > 0) { n++; if (d > max) max = d; }
            }
            return $"{n} ({100.0 * n / A.Length:F3}%), max {max}";
        }

        static double Mean(List<double> v) => v.Count > 0 ? v.Average() : double.NaN;
        static void Flush(string dir) => File.WriteAllLines(Path.Combine(dir, "phase1.txt"), Lines.ToArray());
    }
}
