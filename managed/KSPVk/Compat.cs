using System;
using System.Collections.Generic;
using System.Linq;
using UnityEngine;

namespace KSPVk
{
    /// Mod conflict handling. KSPVK_COMPAT nodes (GameData/KSPVk/compat.cfg, or any mod's own cfg) name an assembly and what KSPVk hands back to Unity
    /// while it is loaded; KSPVK_EXCLUDE nodes list shaders Unity keeps drawing. Applied once the GameDatabase is loaded.
    public static class Compat
    {
        public static bool Applied, PartsOff, AllOff;
        public static int FamOff;   // world family bits handed back to Unity
        public static readonly List<string> Notes = new List<string>();
        static readonly List<string> shaderExact = new List<string>(), shaderPrefix = new List<string>();
        static readonly string[] Fams = { "objects", "terrain", "ocean", "scaled", "sun", "atmosphere", "galaxy" };
        static readonly List<(string pat, int code)> pbr = new List<(string, int)>();
        static readonly Dictionary<string, int> pbrCache = new Dictionary<string, int>();

        public static void Apply()
        {
            if (Applied || GameDatabase.Instance == null || !GameDatabase.Instance.IsReady()) return;
            Applied = true;
            var loaded = new HashSet<string>(AssemblyLoader.loadedAssemblies.Select(a => a.name), StringComparer.OrdinalIgnoreCase);
            foreach (var n in GameDatabase.Instance.GetConfigNodes("KSPVK_COMPAT"))
            {
                string mod = n.GetValue("assembly");
                if (string.IsNullOrEmpty(mod) || !loaded.Contains(mod)) continue;
                string note = n.GetValue("note") ?? "";
                if (bool.TryParse(n.GetValue("disable"), out bool off) && off) AllOff = true;
                foreach (var f in (n.GetValue("unity") ?? "").Split(',').Select(x => x.Trim().ToLowerInvariant()).Where(x => x.Length > 0))
                {
                    int i = Array.IndexOf(Fams, f);
                    if (i == 0) PartsOff = true; else if (i > 0) FamOff |= 1 << i; else Log.Line($"compat: {mod}: unknown family '{f}'", true);
                }
                string what = off ? "KSPVk disabled" : n.GetValue("unity") is string u && u.Length > 0 ? "Unity draws: " + u : "no changes";
                Notes.Add($"{mod}: {what}{(note.Length > 0 ? " (" + note + ")" : "")}");
            }
            foreach (var n in GameDatabase.Instance.GetConfigNodes("KSPVK_EXCLUDE"))
                foreach (var s in n.GetValues("shader"))
                    if (s.EndsWith("*")) shaderPrefix.Add(s.Substring(0, s.Length - 1)); else shaderExact.Add(s);
            foreach (var n in GameDatabase.Instance.GetConfigNodes("KSPVK_PBR"))   // Enhanced material overrides: part = name (trailing * = prefix), roughness, metal (0..1)
            {
                string part = n.GetValue("part"); int code = 0;
                if (string.IsNullOrEmpty(part)) continue;
                if (float.TryParse(n.GetValue("roughness"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float r)) code |= 1 | (int)(Mathf.Clamp01(r) * 255 + 0.5f) << 8;
                if (float.TryParse(n.GetValue("metal"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float m)) code |= 2 | (int)(Mathf.Clamp01(m) * 255 + 0.5f) << 16;
                if (code != 0) pbr.Add((part, code));
            }
            if (pbr.Count > 0) Log.Line($"compat: {pbr.Count} PBR material overrides");
            foreach (var n in GameDatabase.Instance.GetConfigNodes("KSPVK_ATMO"))   // Enhanced atmosphere overrides: body, density, mie (x), mieG, scaleHeight, mieScaleHeight (m)
            {
                string b = n.GetValue("body"); if (string.IsNullOrEmpty(b)) continue;
                var a = AtmoDefault;
                a.density = Fl(n, "density", a.density); a.mie = Fl(n, "mie", a.mie); a.mieG = Mathf.Clamp(Fl(n, "mieG", a.mieG), 0, 0.95f);
                a.scaleHeight = Fl(n, "scaleHeight", 0); a.mieScaleHeight = Fl(n, "mieScaleHeight", 0);
                atmo[b] = a;
            }
            foreach (var n in GameDatabase.Instance.GetConfigNodes("KSPVK_TERRAIN"))   // Enhanced terrain detail: body (or *), albedo / normal = GameData texture URLs per band, comma-separated
            {
                string b = n.GetValue("body"); if (string.IsNullOrEmpty(b)) continue;
                var t = new Texture2D[8]; int got = 0;
                for (int k = 0; k < 2; k++)
                {
                    var urls = (n.GetValue(k == 0 ? "albedo" : "normal") ?? "").Split(',').Select(x => x.Trim()).ToArray();
                    for (int i = 0; i < 4; i++)
                    {
                        string u = urls.Length == 0 ? "" : urls[Math.Min(i, urls.Length - 1)];
                        if (u.Length == 0) continue;
                        var tex = GameDatabase.Instance.GetTexture(u, false);
                        if (tex == null) { Log.Line($"compat: KSPVK_TERRAIN {b}: texture '{u}' not found", true); continue; }
                        tex.wrapMode = TextureWrapMode.Repeat; tex.filterMode = FilterMode.Trilinear; tex.anisoLevel = 8;
                        t[k * 4 + i] = tex; got++;
                    }
                }
                if (got > 0) terrain[b] = t;
            }
            if (atmo.Count + terrain.Count > 0) Log.Line($"compat: {atmo.Count} atmosphere, {terrain.Count} terrain overrides");
            foreach (var s in Notes) Log.Line("compat: " + s);
            if (shaderExact.Count + shaderPrefix.Count > 0) Log.Line($"compat: {shaderExact.Count + shaderPrefix.Count} excluded shader patterns");
        }

        public struct AtmoOv { public float density, mie, mieG, scaleHeight, mieScaleHeight; }
        static readonly AtmoOv AtmoDefault = new AtmoOv { density = 1, mie = 1, mieG = 0.8f };
        static readonly Dictionary<string, AtmoOv> atmo = new Dictionary<string, AtmoOv>();
        static readonly Dictionary<string, Texture2D[]> terrain = new Dictionary<string, Texture2D[]>();
        internal static AtmoOv Atmo(string body) => atmo.TryGetValue(body, out var a) ? a : AtmoDefault;
        /// Detail textures for a body: 0..3 albedo, 4..7 normal per band (null: none)
        internal static Texture2D[] Terrain(string body) => terrain.TryGetValue(body, out var t) || terrain.TryGetValue("*", out t) ? t : null;
        static float Fl(ConfigNode n, string k, float d) => float.TryParse(n.GetValue(k), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out float v) ? v : d;

        /// Packed Enhanced material override for a part (0: none): bit 0 roughness, bit 1 metal, 8-bit values from bits 8 and 16. First match wins.
        internal static int PbrOverride(Part p)
        {
            string name = p != null && p.partInfo != null ? p.partInfo.name : null;
            if (pbr.Count == 0 || name == null) return 0;
            if (pbrCache.TryGetValue(name, out int c)) return c;
            c = 0;
            foreach (var (pat, code) in pbr)
                if (pat.EndsWith("*") ? name.StartsWith(pat.Substring(0, pat.Length - 1), StringComparison.Ordinal) : pat == name) { c = code; break; }
            return pbrCache[name] = c;
        }

        /// Renderers that stay with Unity: excluded through the API or by shader.
        internal static bool Excluded(Renderer r)
        {
            if (API.kept.Count > 0 && API.kept.Contains(r)) return true;
            if (shaderExact.Count + shaderPrefix.Count == 0) return false;
            var m = r.sharedMaterial; if (m == null || m.shader == null) return false;
            string n = m.shader.name;
            return shaderExact.Contains(n) || shaderPrefix.Any(p => n.StartsWith(p, StringComparison.Ordinal));
        }
    }

    /// Public API for other mods. Use it through reflection (Type.GetType("KSPVk.API, KSPVk")) to avoid a hard dependency.
    public static class API
    {
        public const int Version = 1;
        internal static readonly HashSet<Renderer> kept = new HashSet<Renderer>();

        /// True while Vulkan is drawing this frame's 3D scene (some objects may still be Unity's: unsupported shaders, transparents, mods).
        public static bool IsActive => Core.Instance != null && Core.Instance.Enabled && Core.Instance.State == Core.StateOk && Core.Instance.Scene != null && Core.Instance.Scene.Active;

        /// True if KSPVk currently draws this renderer instead of Unity.
        public static bool IsDrawnByVulkan(Renderer r) => Core.Instance?.Scene != null && Core.Instance.Scene.Owns(r);

        /// Keep this renderer with Unity (e.g. a mod that changes its material every frame, or draws it with a custom shader). Takes effect immediately.
        public static void KeepInUnity(Renderer r)
        {
            if (r == null || !kept.Add(r)) return;
            Core.Instance?.Scene?.Release(r);
        }

        /// Undo KeepInUnity; KSPVk picks the renderer up again on its next scan (within about a second).
        public static void AllowVulkan(Renderer r) { if (r != null) kept.Remove(r); Core.Instance?.Scene?.Unignore(r); }

        /// Raised when IsActive changes (argument: the new value). Called on the main thread.
        public static event Action<bool> ActiveChanged;
        static bool last;
        internal static void Tick() { if (kept.Count > 0 && Time.frameCount % 600 == 0) kept.RemoveWhere(r => r == null); bool a = IsActive; if (a == last) return; last = a; try { ActiveChanged?.Invoke(a); } catch (Exception e) { Log.Line("API.ActiveChanged handler threw: " + e, true); } }
    }
}
