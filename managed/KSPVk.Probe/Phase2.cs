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
    /// Phase 2: recon dump of part renderers (label "recon") and golden tests for parts in flight.
    public static class Phase2
    {
        public static readonly List<string> Lines = new List<string>();

        /// Golden test: paused frame with KSPVk off / on / off. Reports the share of pixels whose max channel differs by
        /// more than 30% (77/255), the mean abs difference, the residual Unity-drawn part renderers by reason, and frame time.
        public static IEnumerator Golden(Probe p, string name, string dir)
        {
            var core = Core.Instance;
            if (core == null || core.Scene == null) { Lines.Add($"## {name}\nKSPVk core / scene missing"); Flush(dir); yield break; }
            float ts = Time.timeScale;
            Time.timeScale = 0;
            p.Status = name + ": waiting for scene upload";
            core.Enabled = true;
            float t0 = Time.realtimeSinceStartup; int lastHidden = -1, stable = 0;
            while (Time.realtimeSinceStartup - t0 < 20 && stable < 30)
            {
                if (Time.realtimeSinceStartup - t0 > 2 && !core.Scene.Active) break;   // map view: the near camera is off
                yield return null;
                var st = core.Scene.Stats();
                stable = st.pending == 0 && core.Scene.Hidden == lastHidden && core.Scene.Hidden > 0 ? stable + 1 : 0;
                lastHidden = core.Scene.Hidden;
            }
            float settle = Time.realtimeSinceStartup - t0;
            if (!core.Scene.Active)
            {
                Lines.Add($"## {name}\nnear camera inactive (map view): extractor idle, part renderers untouched, nothing to compare\n");
                Time.timeScale = ts; p.Status = ""; Flush(dir); yield break;
            }
            p.Status = "";
            yield return Grab(core, true, null, t => { });   // warm-up after the overlay change
            Texture2D a = null, b = null, c = null;
            yield return Grab(core, false, null, t => a = t);
            yield return Grab(core, true, null, t => b = t);
            var res = core.Scene.Residuals(out int residual);
            int hidden = core.Scene.Hidden, draws = core.Scene.Draws;
            var ss = core.Scene.Stats(); var ns = core.Stats();
            yield return Grab(core, false, null, t => c = t);
            string m = Metrics(a, b, out Texture2D diff), noise = Metrics(a, c, out Texture2D nd);
            File.WriteAllBytes(Path.Combine(dir, name + "_off.png"), a.EncodeToPNG());
            File.WriteAllBytes(Path.Combine(dir, name + "_on.png"), b.EncodeToPNG());
            if (diff) File.WriteAllBytes(Path.Combine(dir, name + "_diff4x.png"), diff.EncodeToPNG());
            foreach (var t in new[] { a, b, c, diff, nd }) if (t) UnityEngine.Object.Destroy(t);
            core.Enabled = true;
            Time.timeScale = ts;

            var off = new Capture(name + "_off"); var on = new Capture(name + "_on");
            long kick = 0; int kf = 0;
            for (int r = 0; r < 2; r++)
                foreach (bool en in new[] { false, true })
                {
                    core.Enabled = en;
                    p.Status = $"{name}: frame time round {r + 1} ({(en ? "on" : "off")})";
                    for (int i = 0; i < 15; i++) yield return null;
                    long k0 = core.KickTicks; int f0 = Time.frameCount;
                    yield return p.Measure(en ? on : off, 3f);
                    if (en) { kick += core.KickTicks - k0; kf += Time.frameCount - f0; }
                }
            core.Enabled = true;
            yield return null;
            var ss2 = core.Scene.Stats(); var ns2 = core.Stats();
            p.Status = "";

            Lines.Add($"## {name}");
            Lines.Add($"settle {settle:F1} s | on vs off: {m} | noise off vs off: {noise}");
            Lines.Add($"residual (opaque L0 part renderers still drawn by Unity) = {residual} | hidden {hidden} | draws {draws} | visible {ss.visible} | shadow draws {ss.shadowDraws} | texture fallbacks {core.Scene.TexFallbacks}");
            Lines.Add($"native: meshes {ss.meshReady}/{ss.meshes} | textures {ss.texReady}/{ss.textures} | uploaded {ss.uploadedBytes / 1048576.0:F1} MB | record {ss2.recordMs:F2} ms | VK GPU {ns2.vkGpuMs:F2} ms | state {ns2.state}");
            Lines.Add($"frame ms off / on: {Mean(off.FrameMs):F2} / {Mean(on.FrameMs):F2} | GPU span off / on: {Mean(off.GpuSpan):F2} / {Mean(on.GpuSpan):F2} | Kick (extractor + BeginFrame) {kick * 1000.0 / System.Diagnostics.Stopwatch.Frequency / Math.Max(1, kf):F2} ms/frame");
            foreach (var kv in res.OrderBy(x => x.Key)) Lines.Add($"    {kv.Value,5} | {kv.Key}");
            Lines.Add("");
            Flush(dir);
        }

        static IEnumerator Grab(Core core, bool enabled, object _, Action<Texture2D> got)
        {
            core.Enabled = enabled;
            for (int i = 0; i < 8; i++) yield return null;
            yield return new WaitForEndOfFrame();
            got(ScreenCapture.CaptureScreenshotAsTexture());
        }

        static string Metrics(Texture2D x, Texture2D y, out Texture2D diff)
        {
            diff = null;
            if (x == null || y == null || x.width != y.width || x.height != y.height) return "size mismatch";
            var A = x.GetPixels32(); var B = y.GetPixels32(); var D = new Color32[A.Length];
            long sum = 0; int any = 0, big = 0;
            for (int i = 0; i < A.Length; i++)
            {
                int dr = Math.Abs(A[i].r - B[i].r), dg = Math.Abs(A[i].g - B[i].g), db = Math.Abs(A[i].b - B[i].b), mx = Math.Max(dr, Math.Max(dg, db));
                sum += dr + dg + db;
                if (mx > 0) any++;
                if (mx > 77) big++;
                D[i] = new Color32((byte)Math.Min(255, dr * 4), (byte)Math.Min(255, dg * 4), (byte)Math.Min(255, db * 4), 255);
            }
            diff = new Texture2D(x.width, x.height, TextureFormat.RGBA32, false);
            diff.SetPixels32(D); diff.Apply();
            double n = A.Length;
            return $">30%: {100 * big / n:F2}% px, any: {100 * any / n:F2}% px, mean abs {sum / (3 * n):F2}/255";
        }

        static double Mean(List<double> v) => v.Count > 0 ? v.Average() : double.NaN;
        static void Flush(string dir) => File.WriteAllLines(Path.Combine(dir, "phase2.txt"), Lines.ToArray());

        public static IEnumerator Recon(Probe p, string name, string dir)
        {
            var sb = new StringBuilder();
            var shaders = new Dictionary<Shader, Material>();
            var groups = new Dictionary<string, int>();
            var texFmt = new Dictionary<string, int>();
            var attrs = new Dictionary<string, int>();
            var seenTex = new HashSet<Texture>();
            int parts = 0;
            foreach (var v in FlightGlobals.VesselsLoaded)
                foreach (var part in v.parts)
                {
                    parts++;
                    foreach (var r in part.GetComponentsInChildren<Renderer>(true))
                    {
                        var mf = r.GetComponent<MeshFilter>();
                        Mesh m = r is SkinnedMeshRenderer sk ? sk.sharedMesh : mf ? mf.sharedMesh : null;
                        var mats = r.sharedMaterials;
                        string sh = string.Join("+", mats.Select(x => x && x.shader ? x.shader.name : "null").Distinct());
                        string key = $"{r.GetType().Name} L{r.gameObject.layer} {(r.enabled && r.gameObject.activeInHierarchy ? "on" : "off")} {sh} readable={m?.isReadable} subs={m?.subMeshCount}/{mats.Length} mpb={r.HasPropertyBlock()} hl={part.HighlightRenderer?.Contains(r)}";
                        groups[key] = groups.TryGetValue(key, out int c) ? c + 1 : 1;
                        if (m != null)
                        {
                            string a = $"streams={m.vertexBufferCount} idx={m.indexFormat} " + string.Join(" ", m.GetVertexAttributes().Select(x => $"{x.attribute}:{x.format}x{x.dimension}@{x.stream}"));
                            attrs[a] = attrs.TryGetValue(a, out c) ? c + 1 : 1;
                        }
                        foreach (var mat in mats)
                        {
                            if (!mat || !mat.shader) continue;
                            if (!shaders.ContainsKey(mat.shader)) shaders[mat.shader] = mat;
                            foreach (string tn in mat.GetTexturePropertyNames())
                            {
                                var t = mat.GetTexture(tn);
                                if (t == null || !seenTex.Add(t)) continue;
                                string tk = t is Texture2D t2 ? $"{tn} {t2.format} mips={t2.mipmapCount} wrap={t2.wrapModeU}/{t2.wrapModeV} filter={t2.filterMode} aniso={t2.anisoLevel}" : $"{tn} {t.GetType().Name} {t.dimension}";
                                texFmt[tk] = texFmt.TryGetValue(tk, out c) ? c + 1 : 1;
                            }
                        }
                    }
                }
            sb.AppendLine($"# {name}: {FlightGlobals.VesselsLoaded.Count} loaded vessels, {parts} parts");
            sb.AppendLine("\n## renderer groups (count | type layer enabled shader readable subs mpb inHighlightList)");
            foreach (var kv in groups.OrderByDescending(x => x.Value)) sb.AppendLine($"{kv.Value,5} | {kv.Key}");
            sb.AppendLine("\n## vertex layouts");
            foreach (var kv in attrs.OrderByDescending(x => x.Value)) sb.AppendLine($"{kv.Value,5} | {kv.Key}");
            sb.AppendLine("\n## textures (unique)");
            foreach (var kv in texFmt.OrderByDescending(x => x.Value)) sb.AppendLine($"{kv.Value,5} | {kv.Key}");
            sb.AppendLine("\n## shader properties (first material seen)");
            foreach (var kv in shaders)
            {
                var s = kv.Key; var mat = kv.Value;
                sb.AppendLine($"{s.name} queue={mat.renderQueue} keywords=[{string.Join(",", mat.shaderKeywords)}]");
                for (int i = 0; i < s.GetPropertyCount(); i++)
                {
                    string pn = s.GetPropertyName(i); var pt = s.GetPropertyType(i);
                    string val = pt == ShaderPropertyType.Color ? mat.GetColor(pn).ToString() : pt == ShaderPropertyType.Vector ? mat.GetVector(pn).ToString()
                        : pt == ShaderPropertyType.Texture ? $"{(mat.GetTexture(pn) ? mat.GetTexture(pn).name : "none")} ST={mat.GetTextureScale(pn)}{mat.GetTextureOffset(pn)}" : mat.GetFloat(pn).ToString("G4");
                    sb.AppendLine($"    {pn} {pt} = {val}");
                }
            }
            sb.AppendLine("\n## lights");
            foreach (var l in UnityEngine.Object.FindObjectsOfType<Light>())
                sb.AppendLine($"{l.name} {l.type} en={l.enabled && l.gameObject.activeInHierarchy} mask={l.cullingMask:x} int={l.intensity} col={l.color} shadows={l.shadows} str={l.shadowStrength} bias={l.shadowBias} nbias={l.shadowNormalBias} near={l.shadowNearPlane} render={l.renderMode} fwd={l.transform.forward}");
            sb.AppendLine($"ambient mode={RenderSettings.ambientMode} light={RenderSettings.ambientLight} intensity={RenderSettings.ambientIntensity} sky={RenderSettings.ambientSkyColor}");
            sb.AppendLine($"shadows dist={QualitySettings.shadowDistance} cascades={QualitySettings.shadowCascades} split4={QualitySettings.shadowCascade4Split} proj={QualitySettings.shadowProjection} res={QualitySettings.shadowResolution} nearOffset={QualitySettings.shadowNearPlaneOffset}");
            var fc = FlightCamera.fetch ? FlightCamera.fetch.mainCamera : null;
            if (fc) sb.AppendLine($"flight camera {fc.name} near={fc.nearClipPlane} far={fc.farClipPlane} fov={fc.fieldOfView} proj=\n{fc.projectionMatrix}");
            File.WriteAllText(Path.Combine(dir, "recon_" + name + ".txt"), sb.ToString());
            yield break;
        }
    }
}
