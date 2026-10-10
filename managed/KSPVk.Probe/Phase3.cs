using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace KSPVk.Probe
{
    /// Phase 3: all-bodies flight tour (orbit / surface / atmosphere views) with world recon or golden steps.
    public static class Phase3
    {
        [System.Runtime.InteropServices.DllImport("psapi.dll")] static extern bool GetProcessMemoryInfo(IntPtr h, out Pmc c, int cb);
        [System.Runtime.InteropServices.DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
        struct Pmc { public uint cb, faults; public UIntPtr peakWs, ws, qPeakPaged, qPaged, qPeakNonPaged, qNonPaged, commit, peakCommit, privateUsage; }   // PROCESS_MEMORY_COUNTERS_EX, 80 B
        static bool texDumped;
        /// Every texture KSPVk has registered, largest first, with totals by format (Unity's size: native copies the D3D11 texture as-is).
        static void TexDump(Scene sc, string path)
        {
            var ids = sc.TexIdsDiag;
            var rows = Resources.FindObjectsOfTypeAll<Texture>().Where(t => ids.TryGetValue(t.GetInstanceID(), out int id) && id >= 0)
                .Select(t => new { t, mb = UnityEngine.Profiling.Profiler.GetRuntimeMemorySizeLong(t) / 1048576.0 }).OrderByDescending(r => r.mb).ToList();
            var sb = new StringBuilder($"{rows.Count} textures, {rows.Sum(r => r.mb):F0} MB\n");
            foreach (var g in rows.GroupBy(r => r.t.graphicsFormat).OrderByDescending(g => g.Sum(r => r.mb))) sb.AppendLine($"  {g.Key}: {g.Count()} textures, {g.Sum(r => r.mb):F0} MB");
            foreach (var g in rows.GroupBy(r => Mathf.Max(r.t.width, r.t.height)).OrderByDescending(g => g.Key)) sb.AppendLine($"  max side {g.Key}: {g.Count()} textures, {g.Sum(r => r.mb):F0} MB");
            foreach (var r in rows) sb.AppendLine($"{r.mb,7:F1} MB  {r.t.width}x{r.t.height} {r.t.graphicsFormat} mips {r.t.mipmapCount}  {r.t.name}");
            File.WriteAllText(path, sb.ToString());
        }

        public static long PrivateMB() => GetProcessMemoryInfo(GetCurrentProcess(), out var c, 80) ? (long)c.privateUsage.ToUInt64() >> 20 : -1;

        public struct View { public string Name; public Action Setup; public float Pitch, Dist; }

        // Fixed surface sites (lat, lon); other bodies use a daylit equatorial point (morning side of the subsolar longitude).
        static readonly Dictionary<string, Vector2> Sites = new Dictionary<string, Vector2> { { "Kerbin", new Vector2(-0.1f, -74.3f) } };

        static double DayLon(CelestialBody b) => b.GetLongitude(Planetarium.fetch.Sun.position) - 35;

        public static List<View> Views(bool quick)
        {
            var v = new List<View>();
            var bodies = FlightGlobals.Bodies;
            string only = Environment.GetEnvironmentVariable("KSPVK_BODIES");   // optional comma list, e.g. "laythe,eve"
            for (int i = 0; i < bodies.Count; i++)
            {
                var b = bodies[i]; int bi = i; string n = b.bodyName.ToLowerInvariant();
                if (quick && n != "kerbin" && n != "mun" && n != "jool") continue;
                if (!string.IsNullOrEmpty(only) && Array.IndexOf(only.ToLowerInvariant().Split(','), n) < 0) continue;
                double alt = b.isStar ? b.Radius : Math.Max(b.atmosphere ? b.atmosphereDepth * 1.3 : 0, Math.Max(b.Radius * 0.15, 20000));
                if (!b.isStar && double.TryParse(Environment.GetEnvironmentVariable("KSPVK_ALTMULT"), out double am)) alt *= am;   // e.g. 30: scaled-space views
                float dip = (float)Math.Acos(b.Radius / (b.Radius + alt));   // horizon below local horizontal
                if (b.isStar) { v.Add(new View { Name = "orbit_" + n, Pitch = dip, Dist = 60, Setup = () => FlightGlobals.fetch.SetShipOrbit(bi, 0, b.Radius + alt, 0, 0, 0, 0, 0) }); continue; }
                v.Add(new View { Name = "orbit_" + n, Pitch = dip + 0.12f, Dist = 60, Setup = () => FlightGlobals.fetch.SetVesselPosition(bi, 0, DayLon(b), alt, 0, 0, true) });
                if (b.atmosphere)
                    v.Add(new View { Name = "atmo_" + n, Pitch = 0.08f, Dist = 60, Setup = () => FlightGlobals.fetch.SetVesselPosition(bi, 0, DayLon(b), b.atmosphereDepth * 0.35, 0, 0, true) });
                if (b.pqsController != null)
                    v.Add(new View { Name = "surface_" + n, Pitch = 0.2f, Dist = 40, Setup = () =>
                    {
                        var s = Sites.TryGetValue(b.bodyName, out var ll) ? ll : new Vector2(0, (float)DayLon(b));
                        if (b.ocean) FlightGlobals.fetch.SetVesselPosition(bi, s.x, s.y, Math.Max(0, b.TerrainAltitude(s.x, s.y, true)) + 1500, 0, 90, true);   // above the sea, not the seafloor
                        else FlightGlobals.fetch.SetVesselPosition(bi, s.x, s.y, 1500, 0, 90, false);
                    } });
            }
            return v;
        }

        public static IEnumerator Tour(Probe p, bool quick, Func<string, IEnumerator> step)
        {
            CheatOptions.IgnoreMaxTemperature = CheatOptions.NoCrashDamage = CheatOptions.UnbreakableJoints = true;
            foreach (var v in Views(quick))
            {
                p.Status = v.Name + ": moving";
                Time.timeScale = 1;
                bool ok = true;
                try { v.Setup(); } catch (Exception e) { ok = false; Debug.LogError("[KSPVk.Probe] " + v.Name + " failed: " + e); }
                if (!ok) continue;
                float t0 = Time.realtimeSinceStartup;
                while (Time.realtimeSinceStartup - t0 < 30 && (FlightGlobals.ActiveVessel == null || FlightGlobals.ActiveVessel.packed)) yield return null;
                yield return new WaitForSecondsRealtime(1);
                FloatingOrigin.ResetTerrainShaderOffset();   // like the stock SetPosition cheat; a teleport otherwise leaves a huge offset and garbage terrain UVs
                Time.timeScale = 0;   // freeze the vessel in place; PQS keeps building in Update
                try { FlightCamera.SetModeImmediate(FlightCamera.Modes.FREE); FlightCamera.CamPitch = v.Pitch; FlightCamera.CamHdg = 0; FlightCamera.fetch.SetDistanceImmediate(v.Dist); } catch { }
                yield return new WaitForSecondsRealtime(6);
                yield return step(v.Name);
                Time.timeScale = 1;
            }
        }

        public static readonly List<string> Lines = new List<string>();

        /// Golden test: paused frame with KSPVk off / on / off, plus an off frame with the UI hidden to find the UI mask.
        /// Metrics over the 3D region (outside the UI mask): SSIM of Sobel luminance-edge maps (8x8 blocks), mean abs per-channel
        /// difference, share of pixels with a max-channel difference > 30%. UI mask: share of byte-identical pixels on vs off.
        public static IEnumerator Golden(Probe p, string name, string dir)
        {
            var core = Core.Instance; var sc = core?.Scene;
            if (sc == null) { Lines.Add($"## {name}\nKSPVk core / scene missing"); Flush(dir); yield break; }
            float ts = Time.timeScale;
            if (!name.EndsWith("_live")) Time.timeScale = 0;   // _live: time keeps running (animated instruments, flight state)
            core.Enabled = true;
            p.Status = name + ": waiting for world upload";
            float t0 = Time.realtimeSinceStartup; int last = -1, stable = 0;
            while (Time.realtimeSinceStartup - t0 < 40 && stable < 30)
            {
                yield return null;
                var st = sc.Stats();
                int h = sc.Hidden + sc.WorldHidden;
                stable = st.pending == 0 && sc.QuadPending == 0 && h == last && h > 0 ? stable + 1 : 0;
                last = h;
            }
            float settle = Time.realtimeSinceStartup - t0;
            p.Status = "";
            yield return Grab(core, true, true, t => { });
            Texture2D a = null, b = null, c = null, noUi = null;
            yield return Grab(core, false, true, t => a = t);
            yield return Grab(core, true, true, t => b = t);
            var res = sc.Residuals(out int residual);
            sc.WorldResiduals(res, ref residual);
            int hidden = sc.Hidden, whidden = sc.WorldHidden, wdraws = sc.WorldDraws, draws = sc.Draws;
            var ss = sc.Stats();
            File.WriteAllText(Path.Combine(dir, "worldmats_" + name + ".txt"), sc.WorldMatDump());
            if (Environment.GetEnvironmentVariable("KSPVK_DUMPSCALED") == "1" && FlightGlobals.currentMainBody?.scaledBody?.GetComponent<Renderer>()?.sharedMaterial?.mainTexture is Texture stx)
            {
                var rt = RenderTexture.GetTemporary(stx.width / 2, stx.height / 2, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
                Graphics.Blit(stx, rt); var prev = RenderTexture.active; RenderTexture.active = rt;
                var cp = new Texture2D(rt.width, rt.height, TextureFormat.RGB24, false); cp.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0); cp.Apply();
                RenderTexture.active = prev; RenderTexture.ReleaseTemporary(rt);
                File.WriteAllBytes(Path.Combine(dir, "scaledtex_" + name + ".png"), cp.EncodeToPNG()); UnityEngine.Object.Destroy(cp);
            }
            yield return Grab(core, false, true, t => c = t);
            yield return Grab(core, false, false, t => noUi = t);
            string m = Metrics(a, b, noUi, out Texture2D diff, out bool pass), noise = Metrics(a, c, noUi, out Texture2D nd, out _);
            File.WriteAllBytes(Path.Combine(dir, name + "_off.png"), a.EncodeToPNG());
            File.WriteAllBytes(Path.Combine(dir, name + "_on.png"), b.EncodeToPNG());
            if (diff) File.WriteAllBytes(Path.Combine(dir, name + "_diff4x.png"), diff.EncodeToPNG());
            foreach (var t in new[] { a, b, c, noUi, diff, nd }) if (t) UnityEngine.Object.Destroy(t);
            Time.timeScale = ts;

            var off = new Capture(name + "_off"); var on = new Capture(name + "_on");
            long kick = 0; int kf = 0; double subWait = 0; var prof = new long[sc.Prof.Length];
            for (int r = 0; r < 2; r++)
                foreach (bool en in new[] { false, true })
                {
                    core.Enabled = en;
                    p.Status = $"{name}: frame time round {r + 1} ({(en ? "on" : "off")})";
                    for (int i = 0; i < 15; i++) yield return null;
                    long k0 = core.KickTicks; int f0 = Time.frameCount; double w0 = core.Stats().subWaitMs; var p0 = (long[])sc.Prof.Clone();
                    yield return p.Measure(en ? on : off, 3f);
                    if (en) { subWait += core.Stats().subWaitMs - w0; kick += core.KickTicks - k0; kf += Time.frameCount - f0; for (int i = 0; i < prof.Length; i++) prof[i] += sc.Prof[i] - p0[i]; }
                }
            core.Enabled = true;
            yield return null;
            var tog = new List<string>();
            if (Settings.Enh != 0 && Environment.GetEnvironmentVariable("KSPVK_ENHTOGGLE") == "1")   // live toggles: every feature off in turn, HDR only, Enhanced off, back on (per-feature cost; nothing may fail)
            {
                int all = Settings.Enh;
                var tv = new float[13];
                foreach (int fl in new[] { all, all & ~Settings.EfTaa, all & ~Settings.EfBloom, all & ~Settings.EfAutoExp, all & ~Settings.EfGbuf, all & ~Settings.EfHiz,
                                           all & ~Settings.EfIbl, all & ~(Settings.EfPbr | Settings.EfIbl), all & ~Settings.EfSoftSh, all & ~Settings.EfContact, all & ~Settings.EfGtao,
                                           all & ~Settings.EfSsgi, all & ~Settings.EfSpotSh, all & ~Settings.EfAtmo, all & ~Settings.EfFft, all & ~(Settings.EfOcean | Settings.EfFft),
                                           all & ~(Settings.EfTerrain | Settings.EfPom), all & ~Settings.EfSsr, Settings.EfOn, 0, all })
                {
                    Settings.Enh = fl;
                    p.Status = $"{name}: Enhanced flags {fl:x}";
                    for (int i = 0; i < 20; i++) yield return null;
                    float tt = Time.realtimeSinceStartup, gpu = 0; int n = 0;
                    for (; n < 60; n++) { yield return null; gpu += core.Stats().vkGpuMs; }
                    int te = core.Enhanced(tv);
                    yield return Grab(core, true, false, s => { File.WriteAllBytes(Path.Combine(dir, $"{name}_tog{fl:x5}.png"), s.EncodeToPNG()); UnityEngine.Object.Destroy(s); });
                    tog.Add($"toggle {fl:x5} -> active {te:x5} failed {(int)tv[12]:x} | frame {(Time.realtimeSinceStartup - tt) * 1000 / n:F2} ms | VK GPU {gpu / n:F2} ms | ibl {tv[0]:F2} pre {tv[1]:F2} scene {tv[2]:F2} " +
                            $"hiz {tv[3]:F2} aogi {tv[4]:F2} taa {tv[5]:F2} exp {tv[6]:F2} bloom {tv[7]:F2} tone {tv[8]:F2} | images {tv[11]:F0} MB | state {core.State}");
                }
                Settings.Enh = all;
            }
            var ss2 = sc.Stats(); var ns2 = core.Stats();
            p.Status = "";

            Lines.Add($"## {name} {(pass ? "PASS" : "FAIL")}");
            Lines.Add($"settle {settle:F1} s | on vs off: {m} | noise off vs off: {noise}");
            Lines.Add($"residual {residual} | parts hidden {hidden} / draws {draws} | world hidden {whidden} / draws {wdraws} | visible {ss.visible} | shadow draws {ss.shadowDraws}");
            Lines.Add($"native: meshes {ss.meshReady}/{ss.meshes} | textures {ss.texReady}/{ss.textures} | uploaded {ss.uploadedBytes / 1048576.0:F1} MB | record {ss2.recordMs:F2} ms | VK GPU {ns2.vkGpuMs:F2} ms (uploads + shadows {ns2.vkPrepMs:F2}) | state {ns2.state} | composite waits for record {subWait / Math.Max(1, kf):F2} ms/frame");
            Lines.Add($"frame ms off / on: {Mean(off.FrameMs):F2} / {Mean(on.FrameMs):F2} | GPU span off / on: {Mean(off.GpuSpan):F2} / {Mean(on.GpuSpan):F2} | Kick {kick * 1000.0 / System.Diagnostics.Stopwatch.Frequency / Math.Max(1, kf):F2} ms/frame (" +
                      string.Join(" ", prof.Select((t, i) => new[] { "rescan", "mats", "objects", "world", "header", "| expose", "xwait" }[i] + " " + (t * 1000.0 / System.Diagnostics.Stopwatch.Frequency / Math.Max(1, kf)).ToString("F2"))) + ")");
            var ev = new float[13]; int ef = core.Enhanced(ev);
            if (ef != 0) Lines.Add($"enhanced flags {ef:x} (failed {(int)ev[12]:x}) | GPU ms ibl {ev[0]:F2} pre {ev[1]:F2} scene {ev[2]:F2} hiz {ev[3]:F2} aogi {ev[4]:F2} taa {ev[5]:F2} exposure {ev[6]:F2} " +
                                   $"bloom {ev[7]:F2} tonemap {ev[8]:F2} | exposure {ev[9]:F3} mean log2 lum {ev[10]:F2} | images {ev[11]:F0} MB");
            Lines.AddRange(tog);
            Lines.Add("micro: " + sc.MicroProf());
            Lines.Add($"memory: KSP private {PrivateMB()} MB | Vulkan textures {ss2.texBytes >> 20} MB | Unity allocated {UnityEngine.Profiling.Profiler.GetTotalAllocatedMemoryLong() >> 20} MB | managed {GC.GetTotalMemory(false) >> 20} MB | native meshes {ss2.meshes} textures {ss2.textures}");
            if (!texDumped) { texDumped = true; TexDump(sc, Path.Combine(dir, "textures_" + name + ".txt")); }
            Lines.Add("systems ms off/on: " + SysDiff(off, on));
            foreach (var kv in res.OrderBy(x => x.Key)) Lines.Add($"    {kv.Value,5} | {kv.Key}");
            Lines.Add("");
            Flush(dir);
        }

        static IEnumerator Grab(Core core, bool enabled, bool ui, Action<Texture2D> got)
        {
            core.Enabled = enabled;
            if (!ui) GameEvents.onHideUI.Fire();
            for (int i = 0; i < 8; i++) yield return null;
            // re-enabling re-registers every quad (QuadsPerFrame per frame): wait for the queue to drain, as the settle loop does
            for (int i = 0; enabled && i < 120 && (core.Scene.QuadPending > 0 || core.Scene.Stats().pending > 0); i++) yield return null;
            yield return new WaitForEndOfFrame();
            got(ScreenCapture.CaptureScreenshotAsTexture());
            if (!ui) GameEvents.onShowUI.Fire();
        }

        static float[] Edges(Color32[] px, int w, int h)
        {
            var y = new float[w * h]; var e = new float[w * h];
            for (int i = 0; i < y.Length; i++) y[i] = 0.299f * px[i].r + 0.587f * px[i].g + 0.114f * px[i].b;
            for (int j = 1; j < h - 1; j++)
                for (int i = 1; i < w - 1; i++)
                {
                    int k = j * w + i;
                    float gx = y[k - w + 1] + 2 * y[k + 1] + y[k + w + 1] - y[k - w - 1] - 2 * y[k - 1] - y[k + w - 1];
                    float gy = y[k + w - 1] + 2 * y[k + w] + y[k + w + 1] - y[k - w - 1] - 2 * y[k - w] - y[k - w + 1];
                    e[k] = Mathf.Sqrt(gx * gx + gy * gy);
                }
            return e;
        }

        static string Metrics(Texture2D x, Texture2D y, Texture2D noUi, out Texture2D diff, out bool pass)
        {
            diff = null; pass = false;
            if (x == null || y == null || noUi == null || x.width != y.width || x.height != y.height) return "size mismatch";
            int w = x.width, h = x.height;
            var A = x.GetPixels32(); var B = y.GetPixels32(); var U = noUi.GetPixels32(); var D = new Color32[A.Length];
            var ui = new bool[A.Length];
            long sum = 0; int n3 = 0, big = 0, nui = 0, uiSame = 0;
            for (int i = 0; i < A.Length; i++)
            {
                ui[i] = A[i].r != U[i].r || A[i].g != U[i].g || A[i].b != U[i].b;
                int dr = Math.Abs(A[i].r - B[i].r), dg = Math.Abs(A[i].g - B[i].g), db = Math.Abs(A[i].b - B[i].b);
                D[i] = new Color32((byte)Math.Min(255, dr * 4), (byte)Math.Min(255, dg * 4), (byte)Math.Min(255, db * 4), 255);
                if (ui[i]) { nui++; if (dr + dg + db == 0) uiSame++; continue; }
                n3++; sum += dr + dg + db;
                if (Math.Max(dr, Math.Max(dg, db)) > 77) big++;
            }
            diff = new Texture2D(w, h, TextureFormat.RGBA32, false);
            diff.SetPixels32(D); diff.Apply();
            // SSIM over 8x8 blocks of the edge maps; blocks touching the UI mask are skipped
            float[] ea = Edges(A, w, h), eb = Edges(B, w, h);
            const double C1 = 6.5025 * 16, C2 = 58.5225 * 16;   // (0.01 L)^2, (0.03 L)^2 with L = 4 * 255 (Sobel range)
            double ssim = 0; int blocks = 0;
            for (int by = 0; by + 8 <= h; by += 8)
                for (int bx = 0; bx + 8 <= w; bx += 8)
                {
                    double ma = 0, mb = 0, va = 0, vb = 0, cv = 0; bool skip = false;
                    for (int j = 0; j < 8 && !skip; j++)
                        for (int i = 0; i < 8; i++)
                        {
                            int k = (by + j) * w + bx + i;
                            if (ui[k]) { skip = true; break; }
                            ma += ea[k]; mb += eb[k];
                        }
                    if (skip) continue;
                    ma /= 64; mb /= 64;
                    for (int j = 0; j < 8; j++)
                        for (int i = 0; i < 8; i++)
                        {
                            int k = (by + j) * w + bx + i;
                            double da = ea[k] - ma, db = eb[k] - mb;
                            va += da * da; vb += db * db; cv += da * db;
                        }
                    va /= 63; vb /= 63; cv /= 63;
                    ssim += (2 * ma * mb + C1) * (2 * cv + C2) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
                    blocks++;
                }
            ssim = blocks > 0 ? ssim / blocks : 1;
            double mean = n3 > 0 ? sum / (3.0 * n3) : 0;
            pass = ssim >= 0.9 && mean <= 0.3 * 255;
            return $"edge SSIM {ssim:F3} | mean abs {mean:F2}/255 ({100 * mean / 255:F1}%) | >30%: {100.0 * big / Math.Max(1, n3):F2}% px | UI mask {100.0 * nui / A.Length:F1}% of screen, {100.0 * uiSame / Math.Max(1, nui):F1}% identical";
        }

        /// Top player-loop systems and cameras, mean ms per frame with KSPVk off / on.
        static string SysDiff(Capture off, Capture on)
        {
            if (off.SysSumMs == null || on.SysSumMs == null || off.SysNames.Length != on.SysNames.Length) return "n/a";
            double a = Math.Max(1, off.FrameMs.Count), b = Math.Max(1, on.FrameMs.Count);
            string sys = string.Join(" | ", Enumerable.Range(0, on.SysNames.Length).OrderByDescending(i => Math.Max(off.SysSumMs[i] / a, on.SysSumMs[i] / b)).Take(10)
                .Select(i => $"{on.SysNames[i]} {off.SysSumMs[i] / a:F2}/{on.SysSumMs[i] / b:F2}"));
            string cams = string.Join(" | ", on.CamCpu.Keys.Union(off.CamCpu.Keys).Select(k =>
                $"{k} {(off.CamCpu.TryGetValue(k, out var x) ? x.Sum() / a : 0):F2}/{(on.CamCpu.TryGetValue(k, out var y) ? y.Sum() / b : 0):F2}"));
            return sys + " || cameras: " + cams;
        }

        static double Mean(List<double> v) => v.Count > 0 ? v.Average() : double.NaN;
        static void Flush(string dir) => File.WriteAllLines(Path.Combine(dir, "phase3.txt"), Lines.ToArray());

        static string Key(Dictionary<string, int> d, string k) { d[k] = d.TryGetValue(k, out int c) ? c + 1 : 1; return k; }
        static string Vec(Vector3d v) => $"({v.x:G6},{v.y:G6},{v.z:G6})";

        public static IEnumerator Recon(Probe p, string name, string dir)
        {
            var sb = new StringBuilder();
            var body = FlightGlobals.currentMainBody;
            var av = FlightGlobals.ActiveVessel;
            sb.AppendLine($"# {name}: body {body?.bodyName} alt {av?.altitude:F0} m, radar {av?.radarAltitude:F0} m, situation {av?.situation}, frame {Time.frameCount}");
            sb.AppendLine($"ScaledSpace factor {ScaledSpace.ScaleFactor} origin {ScaledSpace.Instance?.transform.position} | Planetarium.fetch.Sun {Planetarium.fetch?.Sun?.bodyName} | krakensbane {Vec(Krakensbane.GetFrameVelocity())}");

            sb.AppendLine("\n## cameras (all, incl. disabled)");
            foreach (var c in Resources.FindObjectsOfTypeAll<Camera>().Where(c => c.gameObject.scene.IsValid()).OrderBy(c => c.depth))
            {
                var comps = string.Join(",", c.GetComponents<Component>().Select(x => x.GetType().Name).Where(x => x != "Transform" && x != "Camera"));
                var cbs = string.Join(",", ((CameraEvent[])Enum.GetValues(typeof(CameraEvent))).Where(e => c.GetCommandBuffers(e).Length > 0).Select(e => e + ":" + c.GetCommandBuffers(e).Length));
                sb.AppendLine($"{c.name} depth={c.depth} en={c.enabled && c.gameObject.activeInHierarchy} near={c.nearClipPlane:G4} far={c.farClipPlane:G6} fov={c.fieldOfView:F1} clear={c.clearFlags} mask={c.cullingMask:x8} rt={(c.targetTexture ? c.targetTexture.name : "-")} pos={c.transform.position} fwd={c.transform.forward} hdr={c.allowHDR} depthMode={c.depthTextureMode} comps=[{comps}] cbs=[{cbs}]");
            }

            var groups = new Dictionary<string, int>();
            var verts = new Dictionary<string, long>();
            var layouts = new Dictionary<string, int>();
            var shaders = new Dictionary<Shader, Material>();
            var texFmt = new Dictionary<string, int>();
            var seenTex = new HashSet<Texture>();
            int partR = 0;
            foreach (var r in Resources.FindObjectsOfTypeAll<Renderer>())
            {
                if (!r.gameObject.scene.IsValid()) continue;
                if (r.GetComponentInParent<Part>() != null) { partR++; continue; }
                var mf = r.GetComponent<MeshFilter>();
                Mesh m = r is SkinnedMeshRenderer sk ? sk.sharedMesh : mf ? mf.sharedMesh : null;
                var mats = r.sharedMaterials;
                bool on = r.enabled && r.gameObject.activeInHierarchy;
                string sh = string.Join("+", mats.Select(x => x && x.shader ? x.shader.name + "@" + x.renderQueue : "null").Distinct());
                string owner = r.GetComponent<PQ>() ? "PQ" : r.GetComponentInParent<PQSCity>() ? "PQSCity" : r.GetComponentInParent<PQSCity2>() ? "PQSCity2" : r.GetComponentInParent<ScaledMovement>() ? "Scaled" : r.GetComponentInParent<PQS>() ? "PQS-child" : r.GetComponentInParent<KerbalEVA>() ? "EVA" : "";
                string k = Key(groups, $"{r.GetType().Name} L{r.gameObject.layer}:{LayerMask.LayerToName(r.gameObject.layer)} {(on ? "on" : "off")}{(r.isVisible ? "/vis" : "")} {sh} owner={owner} readable={m?.isReadable} mpb={r.HasPropertyBlock()} shadows={r.shadowCastingMode}/{r.receiveShadows}");
                if (m != null && on)
                {
                    verts[k] = (verts.TryGetValue(k, out long vc) ? vc : 0) + m.vertexCount;
                    Key(layouts, $"{owner,-9} streams={m.vertexBufferCount} idx={m.indexFormat} " + string.Join(" ", m.GetVertexAttributes().Select(x => $"{x.attribute}:{x.format}x{x.dimension}@{x.stream}")));
                }
                foreach (var mat in mats)
                {
                    if (!mat || !mat.shader) continue;
                    if (!shaders.ContainsKey(mat.shader)) shaders[mat.shader] = mat;
                    foreach (string tn in mat.GetTexturePropertyNames())
                    {
                        var t = mat.GetTexture(tn);
                        if (t == null || !seenTex.Add(t)) continue;
                        Key(texFmt, t is Texture2D t2 ? $"{mat.shader.name} {tn} {t2.format} {t2.width}x{t2.height} mips={t2.mipmapCount} wrap={t2.wrapModeU}/{t2.wrapModeV} filter={t2.filterMode} readable={t2.isReadable}"
                                                      : $"{mat.shader.name} {tn} {t.GetType().Name} {t.dimension} {t.width}x{t.height}");
                    }
                }
            }
            sb.AppendLine($"\n## non-part renderers (part renderers skipped: {partR})  count | verts(on) | key");
            foreach (var kv in groups.OrderByDescending(x => x.Value)) sb.AppendLine($"{kv.Value,5} | {(verts.TryGetValue(kv.Key, out long vv) ? vv : 0),9} | {kv.Key}");
            sb.AppendLine("\n## vertex layouts (enabled)");
            foreach (var kv in layouts.OrderByDescending(x => x.Value)) sb.AppendLine($"{kv.Value,5} | {kv.Key}");
            sb.AppendLine("\n## textures (unique)");
            foreach (var kv in texFmt.OrderBy(x => x.Key)) sb.AppendLine($"{kv.Value,5} | {kv.Key}");

            sb.AppendLine("\n## PQS spheres");
            foreach (var q in Resources.FindObjectsOfTypeAll<PQS>().Where(q => q.gameObject.scene.IsValid()))
            {
                var pqs = Resources.FindObjectsOfTypeAll<PQ>().Where(x => x.sphereRoot == q).ToArray();
                sb.AppendLine($"{q.name} active={q.isActive} go={q.gameObject.activeInHierarchy} r={q.radius:G6} parent={q.parentSphere?.name} quads={pqs.Length} built={pqs.Count(x => x.isBuilt)} visible={pqs.Count(x => x.isVisible)} rendererOn={pqs.Count(x => x.meshRenderer && x.meshRenderer.enabled)} maxSub={(pqs.Length > 0 ? pqs.Max(x => x.subdivision) : 0)} maxLevel={q.maxLevel} cache={PQS.cacheVertCount}"
                              + $" surf={q.surfaceMaterial?.shader?.name}({q.surfaceMaterial?.name}) fallback={q.fallbackMaterial?.shader?.name} lightCam pos={q.transform.position}");
                sb.AppendLine("    mods: " + string.Join(", ", q.GetComponentsInChildren<PQSMod>(true).Select(x => x.GetType().Name + (x.modEnabled ? "" : "(off)"))));
                if (q.isActive && pqs.Length > 0)
                {
                    var hist = pqs.Where(x => x.meshRenderer && x.meshRenderer.enabled).GroupBy(x => x.subdivision).OrderBy(g => g.Key).Select(g => $"L{g.Key}:{g.Count()}");
                    sb.AppendLine("    enabled quads by level: " + string.Join(" ", hist));
                }
            }

            sb.AppendLine("\n## celestial bodies");
            foreach (var b in FlightGlobals.Bodies)
            {
                var sr = b.scaledBody ? b.scaledBody.GetComponent<Renderer>() : null;
                sb.AppendLine($"{b.bodyName} R={b.Radius:G6} atmo={b.atmosphere}/{b.atmosphereDepth:G4} ocean={b.ocean} pqs={b.pqsController?.name} scaled={b.scaledBody?.name} L{b.scaledBody?.layer} shader={sr?.sharedMaterial?.shader?.name} on={sr && sr.enabled && sr.gameObject.activeInHierarchy} vis={sr?.isVisible} children=[{(b.scaledBody ? string.Join(",", b.scaledBody.GetComponentsInChildren<Renderer>(true).Where(x => x != sr).Select(x => x.name + ":" + x.sharedMaterial?.shader?.name)) : "")}]");
            }

            sb.AppendLine("\n## sun / galaxy / atmosphere objects");
            foreach (var t in new[] { typeof(Sun), typeof(ScaledSun), typeof(SunFlare), typeof(SunCoronas), typeof(GalaxyCubeControl), typeof(GalaxyCameraControl), typeof(AtmosphereFromGround), typeof(SunShaderController), typeof(ScaledSpaceFader), typeof(LensFlare), typeof(FlareLayer), typeof(Skybox) })
                foreach (var o in Resources.FindObjectsOfTypeAll(t).OfType<Component>().Where(x => x.gameObject.scene.IsValid()))
                    sb.AppendLine($"{t.Name} on {o.name} L{o.gameObject.layer} active={o.gameObject.activeInHierarchy} {(o is Behaviour bh ? "en=" + bh.enabled : "")} {(o is LensFlare lf ? $"flare={lf.flare?.name} bright={lf.brightness} col={lf.color}" : "")} pos={o.transform.position}");
            sb.AppendLine($"RenderSettings fog={RenderSettings.fog} mode={RenderSettings.fogMode} col={RenderSettings.fogColor} dens={RenderSettings.fogDensity} skybox={RenderSettings.skybox?.shader?.name} ambient={RenderSettings.ambientMode}/{RenderSettings.ambientLight} sun={RenderSettings.sun?.name}");
            foreach (var l in UnityEngine.Object.FindObjectsOfType<Light>())
                sb.AppendLine($"light {l.name} {l.type} en={l.enabled && l.gameObject.activeInHierarchy} mask={l.cullingMask:x8} int={l.intensity} col={l.color} shadows={l.shadows} range={l.range} fwd={l.transform.forward}");

            sb.AppendLine("\n## shader properties (first material seen)");
            foreach (var kv in shaders.OrderBy(x => x.Key.name))
            {
                var s = kv.Key; var mat = kv.Value;
                if (s.name.StartsWith("KSP/") && !s.name.Contains("Scenery") && !s.name.Contains("Particle")) continue;
                sb.AppendLine($"{s.name} ({mat.name}) queue={mat.renderQueue} keywords=[{string.Join(",", mat.shaderKeywords)}] passes={mat.passCount}");
                for (int i = 0; i < s.GetPropertyCount(); i++)
                {
                    string pn = s.GetPropertyName(i); var pt = s.GetPropertyType(i);
                    string val = pt == ShaderPropertyType.Color ? mat.GetColor(pn).ToString() : pt == ShaderPropertyType.Vector ? mat.GetVector(pn).ToString()
                        : pt == ShaderPropertyType.Texture ? $"{(mat.GetTexture(pn) ? mat.GetTexture(pn).name : "none")} ST={mat.GetTextureScale(pn)}{mat.GetTextureOffset(pn)}" : mat.GetFloat(pn).ToString("G4");
                    sb.AppendLine($"    {pn} {pt} = {val}");
                }
            }
            File.WriteAllText(Path.Combine(dir, "recon3_" + name + ".txt"), sb.ToString());
            p.Status = "";
            yield return null;
            ScreenCapture.CaptureScreenshot(Path.Combine(dir, name + ".png"));
            yield return null; yield return null;
        }
    }
}
