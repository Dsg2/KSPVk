using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace KSPVk.Probe
{
    /// Minimal streaming JSON writer (no Unity/.NET JSON lib available that handles dictionaries).
    public class Json
    {
        readonly StringBuilder sb = new StringBuilder();
        bool first = true;
        void Sep() { if (!first) sb.Append(','); first = false; }
        public Json Obj(string key = null) { Key(key); sb.Append('{'); first = true; return this; }
        public Json Arr(string key = null) { Key(key); sb.Append('['); first = true; return this; }
        public Json EndObj() { sb.Append('}'); first = false; return this; }
        public Json EndArr() { sb.Append(']'); first = false; return this; }
        void Key(string k) { Sep(); if (k != null) { Str(k); sb.Append(':'); } }
        void Str(string s)
        {
            sb.Append('"');
            foreach (char c in s ?? "") { if (c == '"' || c == '\\') sb.Append('\\').Append(c); else if (c < 32) sb.AppendFormat("\\u{0:x4}", (int)c); else sb.Append(c); }
            sb.Append('"');
        }
        public Json V(string k, string v) { Key(k); if (v == null) sb.Append("null"); else Str(v); return this; }
        public Json V(string k, double v) { Key(k); sb.Append(double.IsNaN(v) || double.IsInfinity(v) ? "null" : Math.Round(v, 4).ToString(CultureInfo.InvariantCulture)); return this; }
        public Json V(string k, bool v) { Key(k); sb.Append(v ? "true" : "false"); return this; }
        public Json V(string k, Color c) => Arr(k).V(null, c.r).V(null, c.g).V(null, c.b).V(null, c.a).EndArr();
        public override string ToString() => sb.ToString();
    }

    public static class Report
    {
        static double Pct(List<double> v, double p) { if (v.Count == 0) return double.NaN; var s = v.OrderBy(x => x).ToList(); return s[Math.Min(s.Count - 1, (int)(p * s.Count))]; }

        static string Category(string sys)
        {
            string s = sys.Substring(sys.IndexOf('.') + 1);
            if (s.Contains("Physics") || s.Contains("Cloth")) return "physics";
            if (sys.StartsWith("FixedUpdate.Script")) return "scripts_fixed";
            if (s.StartsWith("ScriptRun") || s == "TriggerEndOfFrameCallbacks" || s.StartsWith("Director")) return "scripts";
            if (s == "PresentAfterDraw" || s == "PresentBeforeUpdate" || s == "GfxDeviceUpdate") return "present_gpu_wait";
            if (s == "FinishFrameRendering" || s.Contains("Renderer") || s.Contains("SkinnedMesh") || s.Contains("Canvas") || s.Contains("RectTransform")
                || s.Contains("ParticleSystem") || s.Contains("RenderTexture") || s.Contains("ReflectionProbe") || s.Contains("LightProbe")
                || s.Contains("Enlighten") || s.Contains("TextureStreaming") || s.Contains("Video") || s.Contains("Shadow")) return "render_cpu";
            return "other";
        }

        /// Writes <dir>/<name>.json and returns a one-line summary for summary.txt
        public static string WriteStep(string dir, Capture cap)
        {
            int n = cap.FrameMs.Count;
            var j = new Json().Obj();
            j.V("step", cap.Name).V("scene", HighLogic.LoadedScene.ToString()).V("frames", n)
             .V("frame_ms_mean", n > 0 ? cap.FrameMs.Average() : double.NaN).V("frame_ms_p50", Pct(cap.FrameMs, .5))
             .V("frame_ms_p95", Pct(cap.FrameMs, .95)).V("frame_ms_p99", Pct(cap.FrameMs, .99));

            var cats = new Dictionary<string, double>();
            j.Arr("subsystems_ms_per_frame");
            if (n > 0 && cap.SysSumMs != null)
                foreach (int i in Enumerable.Range(0, cap.SysNames.Length).OrderByDescending(i => cap.SysSumMs[i]))
                {
                    double ms = cap.SysSumMs[i] / n;
                    string c = Category(cap.SysNames[i]);
                    cats.TryGetValue(c, out double v); cats[c] = v + ms;
                    if (ms >= 0.01) j.Obj().V("name", cap.SysNames[i]).V("cat", c).V("ms", ms).EndObj();
                }
            j.EndArr();
            j.Obj("categories_ms_per_frame"); foreach (var kv in cats.OrderByDescending(k => k.Value)) j.V(kv.Key, kv.Value); j.EndObj();

            j.V("gpu_timer", cap.GpuTimer).V("gpu_frames", cap.GpuFrames)
             .V("gpu_span_ms_mean", cap.GpuSpan.Count > 0 ? cap.GpuSpan.Average() : double.NaN);
            j.Arr("camera_timing");
            double camCpuTotal = 0, camGpuTotal = 0;
            foreach (var kv in cap.CamCpu.OrderByDescending(k => k.Value.Sum()))
            {
                double cpu = kv.Value.Sum() / Math.Max(1, n); camCpuTotal += cpu;
                cap.CamGpuSum.TryGetValue(kv.Key, out double g); cap.CamGpuCalls.TryGetValue(kv.Key, out int gc);
                double gpu = cap.GpuFrames > 0 ? g / cap.GpuFrames : double.NaN; if (!double.IsNaN(gpu)) camGpuTotal += gpu;
                j.Obj().V("name", kv.Key).V("frames_seen", kv.Value.Count).V("cpu_ms_per_frame", cpu)
                 .V("gpu_timeline_ms_per_frame", gpu).V("gpu_calls_per_frame", cap.GpuFrames > 0 ? (double)gc / cap.GpuFrames : 0).EndObj();
            }
            j.EndArr();
            SceneDump.Write(j);
            j.EndObj();
            File.WriteAllText(Path.Combine(dir, Sanitize(cap.Name) + ".json"), j.ToString());

            Func<string, double> C = k => cats.TryGetValue(k, out double v) ? v : 0;
            return string.Format(CultureInfo.InvariantCulture,
                "{0,-26} {1,6:F1} {2,7:F2} {3,7:F2} | {4,6:F2} {5,6:F2} {6,6:F2} {7,6:F2} {8,6:F2} {9,6:F2} | {10,6:F2} {11,6:F2} {12,6:F2}",
                cap.Name, n > 0 ? 1000.0 / cap.FrameMs.Average() : 0, n > 0 ? cap.FrameMs.Average() : 0, Pct(cap.FrameMs, .95),
                C("physics"), C("scripts_fixed"), C("scripts"), C("render_cpu"), C("present_gpu_wait"), C("other"),
                camCpuTotal, camGpuTotal, cap.GpuSpan.Count > 0 ? cap.GpuSpan.Average() : double.NaN);
        }

        public const string SummaryHeader =
            "step                          fps  frameMs   p95ms | physic fixScr  scripts rendCPU presWt  other | camCPU camGPU gpuSpan";

        public static string Sanitize(string s) => new string(s.Select(c => char.IsLetterOrDigit(c) || c == '_' || c == '-' ? c : '_').ToArray());

        /// One-off environment / settings / shader-support dump.
        public static void WriteStatic(string dir)
        {
            var j = new Json().Obj();
            j.V("gfx_api", SystemInfo.graphicsDeviceType.ToString()).V("gfx_version", SystemInfo.graphicsDeviceVersion)
             .V("gpu", SystemInfo.graphicsDeviceName).V("vram_mb", SystemInfo.graphicsMemorySize).V("cpu", SystemInfo.processorType)
             .V("cpu_threads", SystemInfo.processorCount).V("ram_mb", SystemInfo.systemMemorySize)
             .V("graphics_mt", SystemInfo.graphicsMultiThreaded).V("rendering_threading", SystemInfo.renderingThreadingMode.ToString())
             .V("color_space", QualitySettings.activeColorSpace.ToString()).V("reversed_z", SystemInfo.usesReversedZBuffer)
             .V("max_tex", SystemInfo.maxTextureSize).V("supports_compute", SystemInfo.supportsComputeShaders)
             .V("screen", Screen.width + "x" + Screen.height).V("fullscreen", Screen.fullScreen);
            var shaders = Resources.FindObjectsOfTypeAll<Shader>().OrderBy(s => s.name).ToList();
            j.V("shaders_total", shaders.Count).V("shaders_unsupported", shaders.Count(s => !s.isSupported));
            j.Arr("shaders");
            foreach (var s in shaders) j.Obj().V("name", s.name).V("supported", s.isSupported).V("queue", s.renderQueue).EndObj();
            j.EndArr().EndObj();
            File.WriteAllText(Path.Combine(dir, "static.json"), j.ToString());
        }
    }

    public static class SceneDump
    {
        static string Layers(int mask)
        {
            if (mask == -1) return "Everything";
            var l = new List<string>();
            for (int i = 0; i < 32; i++) if ((mask & (1 << i)) != 0) l.Add(i + ":" + LayerMask.LayerToName(i));
            return string.Join(",", l.ToArray());
        }

        class Agg { public int count, visible, tris, visTris, mpb, readable, staticBatch, lightmapped, shadowCasters; public HashSet<int> queues = new HashSet<int>(); public HashSet<string> layers = new HashSet<string>(); }

        public static void Write(Json j)
        {
            j.Obj("settings")
             .V("quality_level", QualitySettings.names[QualitySettings.GetQualityLevel()]).V("pixel_lights", QualitySettings.pixelLightCount)
             .V("shadows", QualitySettings.shadows.ToString()).V("shadow_res", QualitySettings.shadowResolution.ToString())
             .V("shadow_projection", QualitySettings.shadowProjection.ToString()).V("shadow_distance", QualitySettings.shadowDistance)
             .V("shadow_cascades", QualitySettings.shadowCascades).V("shadow_cascade4", QualitySettings.shadowCascade4Split.ToString("F4"))
             .V("msaa", QualitySettings.antiAliasing).V("lod_bias", QualitySettings.lodBias).V("aniso", QualitySettings.anisotropicFiltering.ToString())
             .V("texture_limit", QualitySettings.masterTextureLimit).V("rt_reflection_probes", QualitySettings.realtimeReflectionProbes)
             .V("ambient_mode", RenderSettings.ambientMode.ToString()).V("ambient_light", RenderSettings.ambientLight)
             .V("ambient_intensity", RenderSettings.ambientIntensity).V("fog", RenderSettings.fog)
             .V("skybox", RenderSettings.skybox ? RenderSettings.skybox.shader.name : null).V("sun", RenderSettings.sun ? RenderSettings.sun.name : null)
             .V("screen", Screen.width + "x" + Screen.height).V("KSP_LIGHT_QUALITY", GameSettings.LIGHT_QUALITY)
             .V("KSP_SHADOWS_QUALITY", GameSettings.SHADOWS_QUALITY).V("KSP_AA", GameSettings.ANTI_ALIASING)
             .V("KSP_TERRAIN_SHADER_QUALITY", GameSettings.TERRAIN_SHADER_QUALITY).V("KSP_AERO_FX_QUALITY", GameSettings.AERO_FX_QUALITY)
             .EndObj();

            j.Arr("cameras");
            foreach (var c in Resources.FindObjectsOfTypeAll<Camera>().Where(c => c.gameObject.scene.IsValid()).OrderBy(c => c.depth))
            {
                j.Obj().V("name", c.name).V("active", c.isActiveAndEnabled).V("depth", c.depth).V("layers", Layers(c.cullingMask))
                 .V("clear", c.clearFlags.ToString()).V("near", c.nearClipPlane).V("far", c.farClipPlane).V("fov", c.fieldOfView)
                 .V("ortho", c.orthographic).V("target", c.targetTexture ? c.targetTexture.name + " " + c.targetTexture.width + "x" + c.targetTexture.height : null)
                 .V("path", c.actualRenderingPath.ToString()).V("hdr", c.allowHDR).V("msaa", c.allowMSAA).V("occlusion", c.useOcclusionCulling)
                 .V("cmd_buffers", c.commandBufferCount)
                 .V("layer_cull_distances", string.Join(",", c.layerCullDistances.Select((d, i) => d > 0 ? i + "=" + d : null).Where(s => s != null).ToArray()));
                if (c.commandBufferCount > 0)
                {
                    j.Arr("cmd_buffer_events");
                    foreach (CameraEvent ev in Enum.GetValues(typeof(CameraEvent)))
                        foreach (var cb in c.GetCommandBuffers(ev)) j.V(null, ev + ":" + cb.name);
                    j.EndArr();
                }
                j.EndObj();
            }
            j.EndArr();

            j.Arr("lights");
            foreach (var l in UnityEngine.Object.FindObjectsOfType<Light>())
                j.Obj().V("name", l.name).V("type", l.type.ToString()).V("enabled", l.enabled).V("color", l.color).V("intensity", l.intensity)
                 .V("range", l.range).V("spot", l.spotAngle).V("shadows", l.shadows.ToString()).V("shadow_strength", l.shadowStrength)
                 .V("render_mode", l.renderMode.ToString()).V("layers", Layers(l.cullingMask)).V("go_layer", l.gameObject.layer).EndObj();
            j.EndArr();

            j.Arr("reflection_probes");
            foreach (var p in UnityEngine.Object.FindObjectsOfType<ReflectionProbe>())
                j.Obj().V("name", p.name).V("mode", p.mode.ToString()).V("refresh", p.refreshMode.ToString()).V("slicing", p.timeSlicingMode.ToString())
                 .V("res", p.resolution).V("layers", Layers(p.cullingMask)).V("hdr", p.hdr).EndObj();
            j.EndArr();

            var canv = UnityEngine.Object.FindObjectsOfType<Canvas>();
            j.Obj("canvases").V("total", canv.Length).V("root", canv.Count(c => c.isRootCanvas));
            foreach (var g in canv.Where(c => c.isRootCanvas).GroupBy(c => c.renderMode + (c.worldCamera ? "@" + c.worldCamera.name : ""))) j.V(g.Key, g.Count());
            j.EndObj();

            // renderers, aggregated by type / layer / shader
            var rs = UnityEngine.Object.FindObjectsOfType<Renderer>();
            var byType = new Dictionary<string, Agg>(); var byLayer = new Dictionary<string, Agg>(); var byShader = new Dictionary<string, Agg>();
            int mats = 0, subs = 0, visSubs = 0; var uniqueMats = new HashSet<int>(); var uniqueMeshes = new HashSet<int>();
            foreach (var r in rs)
            {
                if (!r.enabled) continue;
                Mesh m = r is SkinnedMeshRenderer smr ? smr.sharedMesh : (r.GetComponent<MeshFilter>()?.sharedMesh);
                int tris = 0;
                if (m) { uniqueMeshes.Add(m.GetInstanceID()); for (int s = 0; s < m.subMeshCount; s++) tris += (int)(m.GetIndexCount(s) / 3); }
                bool vis = r.isVisible;
                string layer = r.gameObject.layer + ":" + LayerMask.LayerToName(r.gameObject.layer);
                foreach (var a in new[] { Get(byType, r.GetType().Name), Get(byLayer, layer) }) Add(a, r, m, tris, vis, layer);
                var sm = r.sharedMaterials;
                subs += sm.Length; if (vis) visSubs += sm.Length;
                foreach (var mat in sm)
                {
                    if (!mat) continue;
                    mats++; uniqueMats.Add(mat.GetInstanceID());
                    var a = Get(byShader, mat.shader ? mat.shader.name : "<null>");
                    Add(a, r, m, tris / Math.Max(1, sm.Length), vis, layer);
                    a.queues.Add(mat.renderQueue);
                }
            }
            j.Obj("renderer_totals").V("renderers", rs.Length).V("material_slots", subs).V("visible_material_slots", visSubs)
             .V("unique_materials", uniqueMats.Count).V("unique_meshes", uniqueMeshes.Count).EndObj();
            WriteAgg(j, "renderers_by_type", byType);
            WriteAgg(j, "renderers_by_layer", byLayer);
            WriteAgg(j, "renderers_by_shader", byShader);
        }

        static Agg Get(Dictionary<string, Agg> d, string k) { if (!d.TryGetValue(k, out var a)) d[k] = a = new Agg(); return a; }

        static void Add(Agg a, Renderer r, Mesh m, int tris, bool vis, string layer)
        {
            a.count++; a.tris += tris; a.layers.Add(layer);
            if (vis) { a.visible++; a.visTris += tris; }
            if (r.HasPropertyBlock()) a.mpb++;
            if (m && m.isReadable) a.readable++;
            if (r.isPartOfStaticBatch) a.staticBatch++;
            if (r.lightmapIndex >= 0 && r.lightmapIndex < 0xFFFE) a.lightmapped++;
            if (r.shadowCastingMode != ShadowCastingMode.Off) a.shadowCasters++;
        }

        static void WriteAgg(Json j, string key, Dictionary<string, Agg> d)
        {
            j.Arr(key);
            foreach (var kv in d.OrderByDescending(k => k.Value.count))
            {
                var a = kv.Value;
                j.Obj().V("key", kv.Key).V("count", a.count).V("visible", a.visible).V("tris", a.tris).V("visible_tris", a.visTris)
                 .V("with_mpb", a.mpb).V("mesh_readable", a.readable).V("static_batched", a.staticBatch).V("lightmapped", a.lightmapped)
                 .V("shadow_casters", a.shadowCasters);
                if (a.queues.Count > 0) j.V("queues", string.Join(",", a.queues.OrderBy(q => q).Select(q => q.ToString()).ToArray()));
                if (key == "renderers_by_shader") j.V("layers", string.Join(",", a.layers.ToArray()));
                j.EndObj();
            }
            j.EndArr();
        }
    }
}
