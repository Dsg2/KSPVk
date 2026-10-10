using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using KSP.UI.Screens;
using UnityEngine;

namespace KSPVk
{
    /// Runtime-tunable options. Saved to GameData/KSPVk/PluginData/settings.cfg (never KSP's own settings.cfg).
    /// Bench runs (-kspvk-bench) ignore the saved file so golden results always use the defaults.
    public static class Settings
    {
        public static bool Parts = true, Shadows = true;
        public static int Families = EnvFamilies;   // bit per world family (PQS 1, ocean 2, scaled 3, sun 4, atmosphere 5, galaxy 6)
        public static float ShadowDist, ShadowStrength = 1, NormalBias = 1, SunScale = 1, AmbientScale = 1;   // ShadowDist 0 = game setting
        public static int QuadsPerFrame = 256, MaxTexture = EnvTexMax;   // MaxTexture: largest side of Vulkan texture copies (0: full size)
        public static int Scenes = DefScenes;   // bit per scene (SceneNames); SPH and tracking station measured slower with KSPVk on, so off by default
        public static readonly string[] SceneNames = { "Flight", "Space Center", "VAB", "SPH", "Tracking Station", "Menus" };
        static int DefScenes => Bench ? 63 : 0b100111;

        // Enhanced mode (opt-in): feature bits mirror EF_* in native/core/enhanced.inl
        public const int EfOn = 1, EfTaa = 2, EfBloom = 4, EfAutoExp = 8, EfGbuf = 16, EfHiz = 32, EfPbr = 64, EfIbl = 128, EfSoftSh = 256, EfContact = 512, EfGtao = 1024,
                         EfSsgi = 2048, EfSpotSh = 4096, EfAtmo = 8192, EfOcean = 16384, EfFft = 32768, EfTerrain = 65536, EfPom = 131072, EfSsr = 262144;
        // tiers: High adds Hi-Z, contact shadows, SSGI, spot-light shadows, the FFT ocean and parallax terrain; Ultra arrives with content of its own
        public static readonly string[] TierNames = { "Off", "Medium", "High" }, ToneNames = { "Neutral", "AgX", "ACES", "Clip" };
        const int Medium = EfOn | EfAutoExp | EfBloom | EfTaa | EfPbr | EfIbl | EfGbuf | EfSoftSh | EfGtao | EfAtmo | EfOcean | EfTerrain;
        public static readonly int[] TierFlags = { 0, Medium, Medium | EfHiz | EfContact | EfSsgi | EfSpotSh | EfFft | EfPom | EfSsr };
        public static int Enh = EnvEnh, Tonemap;
        public static float Ev, EvRange = 1, ExpKey = 0.18f, Bloom = 0.05f, BloomThreshold = 1, TaaWeight = 0.1f, AmbFill = 0.3f, IblScale = 1, SunSoft = 1, AoStrength = 1, GiStrength = 1, SkyBright = 3;
        public static string ToggleKey = DefToggleKey;   // enables / disables KSPVk: "O" style, "None" for no key
        const string DefToggleKey = "O";
        public static float RenderScale = EnvRenderScale;   // scene resolution / screen, 0.5..1 (all modes)
        public static int Tier => Array.IndexOf(TierFlags, Enh);   // -1: custom feature set
        static int EnvEnh => int.TryParse(Environment.GetEnvironmentVariable("KSPVK_ENHANCED"), out int t) && t > 0 ? TierFlags[Math.Min(t, TierFlags.Length - 1)] : 0;

        public static bool SceneOn()
        {
            int b;
            switch (HighLogic.LoadedScene)
            {
                case GameScenes.FLIGHT: b = 0; break;
                case GameScenes.SPACECENTER: b = 1; break;
                case GameScenes.EDITOR: b = EditorDriver.editorFacility == EditorFacility.SPH ? 3 : 2; break;
                case GameScenes.TRACKSTATION: b = 4; break;
                default: b = 5; break;
            }
            return (Scenes >> b & 1) != 0;
        }

        static int EnvTexMax => int.TryParse(Environment.GetEnvironmentVariable("KSPVK_TEXMAX"), out int m) ? m : 2048;
        static float EnvRenderScale => float.TryParse(Environment.GetEnvironmentVariable("KSPVK_RENDERSCALE"), NumberStyles.Float, CultureInfo.InvariantCulture, out float s) ? Mathf.Clamp(s, 0.5f, 1) : 1;
        static int EnvFamilies => int.TryParse(Environment.GetEnvironmentVariable("KSPVK_FAMMASK"), out int m) ? m : -1;
        static string FilePath => Path.Combine(Core.PluginData, "settings.cfg");
        public static bool Bench => Environment.CommandLine.Contains("-kspvk-bench");

        public static void Reset()
        {
            Parts = Shadows = true; Families = EnvFamilies;
            ShadowDist = 0; ShadowStrength = NormalBias = SunScale = AmbientScale = 1; QuadsPerFrame = 256; MaxTexture = EnvTexMax; Scenes = DefScenes;
            Enh = EnvEnh; Tonemap = 0; Ev = 0; EvRange = 1; ExpKey = 0.18f; Bloom = 0.05f; BloomThreshold = 1; TaaWeight = 0.1f; AmbFill = 0.3f; IblScale = 1;
            SunSoft = AoStrength = GiStrength = 1; SkyBright = 3; RenderScale = EnvRenderScale; ToggleKey = DefToggleKey;
        }

        public static void Load() { if (!Bench) LoadFrom(FilePath); }
        public static void Save() { if (!Bench) SaveTo(FilePath); }

        public static void LoadFrom(string path)
        {
            if (!File.Exists(path)) return;
            var n = ConfigNode.Load(path)?.GetNode("KSPVK");
            if (n == null) return;
            Parts = B(n, "parts", Parts); Shadows = B(n, "shadows", Shadows);
            if (EnvFamilies == -1) Families = (int)F(n, "families", Families);
            ShadowDist = F(n, "shadowDistance", ShadowDist); ShadowStrength = F(n, "shadowStrength", ShadowStrength);
            NormalBias = F(n, "normalBias", NormalBias); SunScale = F(n, "sunScale", SunScale); AmbientScale = F(n, "ambientScale", AmbientScale);
            QuadsPerFrame = Mathf.Clamp((int)F(n, "quadsPerFrame", QuadsPerFrame), 16, 2048);
            MaxTexture = (int)F(n, "maxTextureSize", MaxTexture);
            Scenes = (int)F(n, "scenes", Scenes);
            Enh = (int)F(n, "enhanced", Enh); Tonemap = (int)F(n, "tonemap", Tonemap);
            Ev = F(n, "exposureEV", Ev); EvRange = F(n, "exposureRange", EvRange); ExpKey = F(n, "exposureKey", ExpKey);
            Bloom = F(n, "bloom", Bloom); BloomThreshold = F(n, "bloomThreshold", BloomThreshold); TaaWeight = F(n, "taaWeight", TaaWeight);
            AmbFill = F(n, "ambientFill", AmbFill); IblScale = F(n, "iblIntensity", IblScale);
            SunSoft = F(n, "sunSoftness", SunSoft); AoStrength = F(n, "aoStrength", AoStrength); GiStrength = F(n, "giStrength", GiStrength); SkyBright = F(n, "skyBrightness", SkyBright);
            RenderScale = Mathf.Clamp(F(n, "renderScale", RenderScale), 0.5f, 1); ToggleKey = n.GetValue("toggleKey") ?? ToggleKey;
        }

        public static void SaveTo(string path)
        {
            var root = new ConfigNode();
            var n = root.AddNode("KSPVK");
            n.AddValue("parts", Parts); n.AddValue("shadows", Shadows); n.AddValue("families", Families);
            foreach (var kv in new[] { ("shadowDistance", ShadowDist), ("shadowStrength", ShadowStrength), ("normalBias", NormalBias), ("sunScale", SunScale), ("ambientScale", AmbientScale),
                                    ("exposureEV", Ev), ("exposureRange", EvRange), ("exposureKey", ExpKey), ("bloom", Bloom), ("bloomThreshold", BloomThreshold), ("taaWeight", TaaWeight), ("ambientFill", AmbFill), ("iblIntensity", IblScale),
                                    ("sunSoftness", SunSoft), ("aoStrength", AoStrength), ("giStrength", GiStrength), ("skyBrightness", SkyBright), ("renderScale", RenderScale) })
                n.AddValue(kv.Item1, kv.Item2.ToString("R", CultureInfo.InvariantCulture));
            n.AddValue("quadsPerFrame", QuadsPerFrame); n.AddValue("maxTextureSize", MaxTexture); n.AddValue("scenes", Scenes); n.AddValue("enhanced", Enh); n.AddValue("tonemap", Tonemap); n.AddValue("toggleKey", ToggleKey);
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            root.Save(path);
        }

        static string keyParsed; static KeyCode keyCode; static int keyMods;
        public static bool ToggleKeyDown()
        {
            if (keyParsed != ToggleKey)
            {
                keyParsed = ToggleKey; var p = ToggleKey.Split('+');
                keyCode = Enum.TryParse(p[p.Length - 1], out KeyCode k) ? k : KeyCode.None;
                keyMods = (p.Contains("Ctrl") ? 1 : 0) | (p.Contains("Alt") ? 2 : 0) | (p.Contains("Shift") ? 4 : 0);
            }
            return keyCode != KeyCode.None && Input.GetKeyDown(keyCode) && Mods() == keyMods;
        }
        static int Mods() => (Input.GetKey(KeyCode.LeftControl) || Input.GetKey(KeyCode.RightControl) ? 1 : 0) | (Input.GetKey(KeyCode.LeftAlt) || Input.GetKey(KeyCode.RightAlt) ? 2 : 0) |
                             (Input.GetKey(KeyCode.LeftShift) || Input.GetKey(KeyCode.RightShift) ? 4 : 0);
        static readonly KeyCode[] ModKeys = { KeyCode.LeftControl, KeyCode.RightControl, KeyCode.LeftAlt, KeyCode.RightAlt, KeyCode.AltGr, KeyCode.LeftShift, KeyCode.RightShift,
                                              KeyCode.LeftCommand, KeyCode.RightCommand, KeyCode.LeftWindows, KeyCode.RightWindows };
        /// The binding for a key-down event (modifier-only presses: null).
        public static string KeyName(Event e) =>
            e.keyCode == KeyCode.None || Array.IndexOf(ModKeys, e.keyCode) >= 0 ? null : (e.control ? "Ctrl+" : "") + (e.alt ? "Alt+" : "") + (e.shift ? "Shift+" : "") + e.keyCode;

        static float F(ConfigNode n, string k, float d) => float.TryParse(n.GetValue(k), NumberStyles.Float, CultureInfo.InvariantCulture, out float v) ? v : d;
        static bool B(ConfigNode n, string k, bool d) => bool.TryParse(n.GetValue(k), out bool v) ? v : d;
    }

    /// In-game settings and stats window: toolbar button or Ctrl+Alt+F9.
    public class SettingsWindow : MonoBehaviour
    {
        const int WinId = 0x4B535056;
        const string Lock = "KSPVkSettings";
        static readonly string[] FamNames = { "Objects (parts, scenery, interiors)", "PQS terrain", "Ocean", "Scaled planets", "Sun", "Atmosphere", "Galaxy skybox" };
        Rect rect = new Rect(-1, 60, 460, 10);
        bool show, diag, stats = true, dirty;
        int dbgFlags;
        string statText = "", census;
        float statTime; int statFrames; long statKick;
        public static SettingsWindow Instance;
        public bool Visible => show;
        public static bool Capturing;   // waiting for a new enable / disable key
        ApplicationLauncherButton button;
        Texture2D icon;
        Vector2 scroll;
        float bodyH = 400;

        void Awake()
        {
            Instance = this;
            Settings.Load();
            GameEvents.onGUIApplicationLauncherReady.Add(AddButton);
            GameEvents.onGUIApplicationLauncherDestroyed.Add(RemoveButton);
        }

        void AddButton()
        {
            if (button != null || ApplicationLauncher.Instance == null) return;
            button = ApplicationLauncher.Instance.AddModApplication(() => Show(true), () => Show(false), null, null, null, null,
                ApplicationLauncher.AppScenes.FLIGHT | ApplicationLauncher.AppScenes.MAPVIEW | ApplicationLauncher.AppScenes.SPACECENTER |
                ApplicationLauncher.AppScenes.VAB | ApplicationLauncher.AppScenes.SPH | ApplicationLauncher.AppScenes.TRACKSTATION, Icon());
            if (show) button.SetTrue(false);
        }

        void RemoveButton()
        {
            if (button != null && ApplicationLauncher.Instance != null) ApplicationLauncher.Instance.RemoveModApplication(button);
            button = null;
        }

        Texture2D Icon()   // procedural "V" so no asset ships with the mod
        {
            if (icon != null) return icon;
            const int S = 38;
            icon = new Texture2D(S, S, TextureFormat.ARGB32, false);
            var px = new Color32[S * S];
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                {
                    float u = Mathf.Abs(x - (S - 1) * 0.5f), v = y - 6f;   // V: two strokes meeting at the bottom
                    bool stroke = v >= 0 && v <= 26 && Mathf.Abs(u - v * 0.5f) < 2.6f;
                    px[y * S + x] = stroke ? new Color32(255, 140, 40, 255) : new Color32(0, 0, 0, 0);
                }
            icon.SetPixels32(px); icon.Apply();
            return icon;
        }

        public void Show(bool on)
        {
            if (on && !show) { statTime = Time.unscaledTime; statFrames = 0; statKick = Core.Instance != null ? Core.Instance.KickTicks : 0; statText = "measuring..."; }
            show = on;
            if (!on) { InputLockManager.RemoveControlLock(Lock); if (dirty) { Settings.Save(); dirty = false; } }
            if (button != null) { if (on) button.SetTrue(false); else button.SetFalse(false); }
        }

        void Update()
        {
            if (Input.GetKey(KeyCode.LeftControl) && Input.GetKey(KeyCode.LeftAlt) && Input.GetKeyDown(KeyCode.F9)) Show(!show);
            if (!show) return;
            statFrames++;
            if (Time.unscaledTime - statTime >= 0.5f) UpdateStats();
        }

        void UpdateStats()
        {
            var core = Core.Instance;
            float dt = Time.unscaledTime - statTime;
            long kick = core != null ? core.KickTicks : 0;
            double kickMs = statFrames > 0 ? (kick - statKick) * 1000.0 / Stopwatch.Frequency / statFrames : 0;
            float frameMs = statFrames > 0 ? dt * 1000f / statFrames : 0;
            statTime = Time.unscaledTime; statKick = kick; statFrames = 0;
            if (core == null) { statText = "core not loaded"; return; }
            var n = core.Stats();
            var sc = core.Scene;
            string t = $"Frame {frameMs:F2} ms ({(frameMs > 0 ? 1000 / frameMs : 0):F0} fps)   main thread (KSPVk) {kickMs:F2} ms\n" +
                       $"Vulkan GPU {n.vkGpuMs:F2} ms   {n.width}x{n.height}" + (Settings.RenderScale < 0.999f ? $", scene at {Settings.RenderScale * 100:F0}%" : "");
            if (sc != null && sc.Active)
            {
                var s = sc.Stats();
                t += $"   record {s.recordMs:F2} ms\n" +
                     $"Objects: {sc.Hidden} taken over, {sc.Draws} draws\n" +
                     $"World: {sc.WorldHidden} taken over, {sc.WorldDraws} drawn, {sc.QuadPending} quads queued\n" +
                     $"GPU draws {s.draws} (visible {s.visible}, shadow {s.shadowDraws})\n" +
                     $"Meshes {s.meshReady}/{s.meshes}, textures {s.texReady}/{s.textures}, uploads pending {s.pending}, total {s.uploadedBytes / 1048576.0:F0} MB";
            }
            else t += "\nScene renderer idle (Vulkan draws the scene in flight only)";
            int ef = core.Enhanced(enhBuf);
            if (ef != 0)
                t += $"\nEnhanced GPU ms: IBL {enhBuf[0]:F2}, pre-scene {enhBuf[1]:F2}, scene {enhBuf[2]:F2}, Hi-Z {enhBuf[3]:F2}, AO/GI/SSR {enhBuf[4]:F2}, TAA {enhBuf[5]:F2}, " +
                     $"exposure {enhBuf[6]:F2}, bloom {enhBuf[7]:F2}, tonemap {enhBuf[8]:F2}\nexposure x{enhBuf[9]:F2} (scene mean {Mathf.Pow(2, enhBuf[10]):F3}), images {enhBuf[11]:F0} MB";
            statText = t;
        }

        void OnGUI()
        {
            if (!show) { Capturing = false; return; }
            var ev = Event.current;
            if (Capturing && ev.type == EventType.KeyDown && ev.keyCode != KeyCode.None)
            {
                string k = ev.keyCode == KeyCode.Escape ? Settings.ToggleKey : ev.keyCode == KeyCode.Backspace ? "None" : Settings.KeyName(ev);
                if (k != null) { if (k != Settings.ToggleKey) { Settings.ToggleKey = k; Changed(false); } Capturing = false; }
                ev.Use();
            }
            GUI.skin = HighLogic.Skin;
            if (rect.x < 0) rect.x = Screen.width - 520;   // beside the toolbar
            rect.height = 0;   // re-fit to the content every frame (sections fold)
            rect = GUILayout.Window(WinId, rect, Draw, "KSPVk renderer", GUILayout.Width(460));
            rect.x = Mathf.Clamp(rect.x, 0, Screen.width - rect.width); rect.y = Mathf.Clamp(rect.y, 0, Mathf.Max(0, Screen.height - rect.height));
            if (rect.Contains(Event.current.mousePosition)) InputLockManager.SetControlLock(ControlTypes.CAMERACONTROLS | ControlTypes.MAP_UI, Lock);
            else InputLockManager.RemoveControlLock(Lock);
        }

        void Draw(int id)
        {
            var core = Core.Instance;
            int state = core != null ? core.State : Core.StateNone;
            GUILayout.BeginHorizontal();
            GUILayout.Label(state == Core.StateOk ? "Vulkan: running" : state == Core.StateFailed ? "Vulkan: FAILED (stock renderer, see KSPVk.log)" : "Vulkan: not initialised");
            if (core != null)
            {
                bool en = GUILayout.Toggle(core.Enabled, Settings.ToggleKey != "None" ? $" Enabled ({Settings.ToggleKey})" : " Enabled");
                if (en != core.Enabled) core.Enabled = en;
            }
            GUILayout.EndHorizontal();

            scroll = GUILayout.BeginScrollView(scroll, GUILayout.Height(Mathf.Min(bodyH, Screen.height - 170)));
            if (Compat.Notes.Count > 0) { Header("Installed mods"); foreach (var n in Compat.Notes) GUILayout.Label(n); }
            Header("Drawn by Vulkan (off = Unity draws it)");
            for (int f = 0; f < FamNames.Length; f += 2)
            {
                GUILayout.BeginHorizontal();
                Owner(f); if (f + 1 < FamNames.Length) Owner(f + 1);
                GUILayout.EndHorizontal();
            }

            Header("Use Vulkan in");
            for (int i = 0; i < Settings.SceneNames.Length; i += 3)
            {
                GUILayout.BeginHorizontal();
                for (int j = i; j < i + 3 && j < Settings.SceneNames.Length; j++)
                {
                    bool on = (Settings.Scenes >> j & 1) != 0, n = GUILayout.Toggle(on, " " + Settings.SceneNames[j], GUILayout.Width(140));
                    if (n != on) { Settings.Scenes ^= 1 << j; Changed(false); }
                }
                GUILayout.EndHorizontal();
            }

            Header("Keys");
            GUILayout.BeginHorizontal();
            GUILayout.Label("Enable / disable KSPVk", GUILayout.Width(230));
            if (GUILayout.Button(Capturing ? "Press a key (Esc: keep, Backspace: none)" : Settings.ToggleKey)) Capturing = !Capturing;
            GUILayout.EndHorizontal();

            Header("Lighting and shadows");
            bool sh = GUILayout.Toggle(Settings.Shadows, " Shadows");
            if (sh != Settings.Shadows) { Settings.Shadows = sh; Changed(false); }
            float game = Mathf.Min(QualitySettings.shadowDistance, 2500f);
            Slider(Settings.ShadowDist > 0 ? $"Shadow distance {Settings.ShadowDist:F0} m" : $"Shadow distance: game ({game:F0} m)", ref Settings.ShadowDist, 0, 2500);
            Slider($"Shadow strength x{Settings.ShadowStrength:F2}", ref Settings.ShadowStrength, 0, 1.5f);
            Slider($"Shadow normal bias {Settings.NormalBias:F2}", ref Settings.NormalBias, 0, 4);
            Slider($"Sun intensity x{Settings.SunScale:F2}", ref Settings.SunScale, 0, 2);
            Slider($"Ambient x{Settings.AmbientScale:F2}", ref Settings.AmbientScale, 0, 3);

            Header("Resolution");
            float rs = Settings.RenderScale;
            bool taau = (Settings.Enh & (Settings.EfOn | Settings.EfTaa)) == (Settings.EfOn | Settings.EfTaa);
            Slider(rs < 0.999f ? $"Render scale {rs * 100:F0}% ({Mathf.RoundToInt(Screen.width * rs)}x{Mathf.RoundToInt(Screen.height * rs)}, {(taau ? "TAA" : "spatial")} upscale)" : "Render scale 100% (native)", ref rs, 0.5f, 1);
            if (rs != Settings.RenderScale) Settings.RenderScale = Mathf.Round(rs * 20) / 20;

            Header("Enhanced (opt-in; Off = stock look and cost)");
            int tier = GUILayout.Toolbar(Settings.Tier, Settings.TierNames);
            if (tier != Settings.Tier && tier >= 0) { Settings.Enh = Settings.TierFlags[tier]; Changed(false); }
            if (Settings.Enh != 0)
            {
                if (core != null) core.Enhanced(enhBuf);
                int failed = core != null ? (int)enhBuf[12] : 0;
                GUILayout.BeginHorizontal(); Feature(" TAA", Settings.EfTaa, failed); Feature(" Bloom", Settings.EfBloom, failed); Feature(" Auto exposure", Settings.EfAutoExp, failed); GUILayout.EndHorizontal();
                GUILayout.BeginHorizontal(); Feature(" G-buffer extras", Settings.EfGbuf, failed); Feature(" Hi-Z", Settings.EfHiz, failed); Feature(" PBR parts", Settings.EfPbr, failed); GUILayout.EndHorizontal();
                GUILayout.BeginHorizontal(); Feature(" IBL", Settings.EfIbl, failed); Feature(" Soft sun shadows", Settings.EfSoftSh, failed); Feature(" Contact shadows", Settings.EfContact, failed); GUILayout.EndHorizontal();
                GUILayout.BeginHorizontal(); Feature(" GTAO", Settings.EfGtao, failed); Feature(" SSGI", Settings.EfSsgi, failed); Feature(" Spot shadows", Settings.EfSpotSh, failed); GUILayout.EndHorizontal();
                GUILayout.BeginHorizontal(); Feature(" Atmosphere", Settings.EfAtmo, failed); Feature(" Ocean", Settings.EfOcean, failed); Feature(" FFT waves", Settings.EfFft, failed); GUILayout.EndHorizontal();
                GUILayout.BeginHorizontal(); Feature(" Terrain", Settings.EfTerrain, failed); Feature(" Parallax", Settings.EfPom, failed); Feature(" SSR", Settings.EfSsr, failed); GUILayout.EndHorizontal();
                if ((Settings.Enh & (Settings.EfGtao | Settings.EfSsgi | Settings.EfContact | Settings.EfSsr)) != 0 && (Settings.Enh & Settings.EfGbuf) == 0) GUILayout.Label("GTAO, SSGI, SSR and contact shadows need the G-buffer extras");
                if ((failed & Settings.EfOn) != 0) GUILayout.Label("Enhanced is unavailable on this GPU (see KSPVk.log)");
                GUILayout.BeginHorizontal(); GUILayout.Label("Tonemap", GUILayout.Width(80));
                int top = GUILayout.Toolbar(Settings.Tonemap, Settings.ToneNames);
                if (top != Settings.Tonemap) { Settings.Tonemap = top; Changed(false); }
                GUILayout.EndHorizontal();
                Slider($"Exposure {Settings.Ev:+0.0;-0.0;0} EV", ref Settings.Ev, -2, 2);
                if ((Settings.Enh & Settings.EfAutoExp) != 0) Slider($"Auto-exposure range +-{Settings.EvRange:F1} EV", ref Settings.EvRange, 0, 3);
                if ((Settings.Enh & Settings.EfBloom) != 0) { Slider($"Bloom {Settings.Bloom:F2}", ref Settings.Bloom, 0, 0.3f); Slider($"Bloom threshold {Settings.BloomThreshold:F2}", ref Settings.BloomThreshold, 0.25f, 4); }
                if ((Settings.Enh & Settings.EfTaa) != 0) Slider($"TAA new-frame weight {Settings.TaaWeight:F2}", ref Settings.TaaWeight, 0.04f, 0.5f);
                if ((Settings.Enh & Settings.EfIbl) != 0) { Slider($"IBL intensity x{Settings.IblScale:F2}", ref Settings.IblScale, 0, 2); Slider($"Stock ambient kept under IBL x{Settings.AmbFill:F2}", ref Settings.AmbFill, 0, 1); }
                if ((Settings.Enh & Settings.EfSoftSh) != 0) Slider($"Sun shadow softness x{Settings.SunSoft:F2}", ref Settings.SunSoft, 0.25f, 4);
                if ((Settings.Enh & Settings.EfGtao) != 0) Slider($"AO strength {Settings.AoStrength:F2}", ref Settings.AoStrength, 0, 1);
                if ((Settings.Enh & Settings.EfSsgi) != 0) Slider($"Indirect light (SSGI) x{Settings.GiStrength:F2}", ref Settings.GiStrength, 0, 2);
                if ((Settings.Enh & Settings.EfAtmo) != 0) Slider($"Sky brightness x{Settings.SkyBright:F2}", ref Settings.SkyBright, 0.5f, 4);
            }

            Header("Streaming");
            float q = Settings.QuadsPerFrame;
            Slider($"Terrain uploads/frame {Settings.QuadsPerFrame}", ref q, 16, 1024);
            Settings.QuadsPerFrame = Mathf.RoundToInt(q);
            float tl = Settings.MaxTexture > 0 ? Mathf.Log(Settings.MaxTexture, 2) : 14;   // 9..13: 512..8192, 14: full size
            Slider(Settings.MaxTexture > 0 ? $"Texture size cap {Settings.MaxTexture}" : "Texture size cap: off (full size)", ref tl, 9, 14);
            int tm = Mathf.RoundToInt(tl) >= 14 ? 0 : 1 << Mathf.RoundToInt(tl);
            if (tm != Settings.MaxTexture) { Settings.MaxTexture = tm; Changed(false); }

            diag = GUILayout.Toggle(diag, diag ? " Diagnostics" : " Diagnostics...");
            if (diag && core != null)
            {
                GUILayout.BeginHorizontal();
                bool p = GUILayout.Toggle(core.DebugPattern, " Depth pattern (Ctrl+Alt+F10)");
                if (p != core.DebugPattern) core.DebugPattern = p;
                Flag(" No culling", 4); Flag(" Flip winding", 1);
                GUILayout.EndHorizontal();
                if (GUILayout.Button("Residual census (what Unity still draws)")) census = Census(core);
                if (census != null) GUILayout.Label(census);
            }

            stats = GUILayout.Toggle(stats, stats ? " Stats" : " Stats...");
            if (stats) GUILayout.Label(statText);
            if (Event.current.type == EventType.Repaint) bodyH = GUILayoutUtility.GetLastRect().yMax + 8;
            GUILayout.EndScrollView();

            GUILayout.BeginHorizontal();
            if (GUILayout.Button("Reset to defaults")) { Settings.Reset(); dbgFlags = 0; core?.SetSceneDebug(0); Changed(true); }
            if (GUILayout.Button("Close")) Show(false);
            GUILayout.EndHorizontal();
            if (Settings.Bench) GUILayout.Label("Bench run: settings are not saved");
            GUI.DragWindow();
        }

        void Owner(int f)   // 0 = objects, else a world family bit
        {
            bool on = f == 0 ? Settings.Parts : (Settings.Families >> f & 1) != 0;
            if (f == 0 ? Compat.PartsOff : (Compat.FamOff >> f & 1) != 0) { GUILayout.Label("  " + FamNames[f] + ": Unity (mod)", GUILayout.Width(210)); return; }
            bool n = GUILayout.Toggle(on, " " + FamNames[f], GUILayout.Width(210));
            if (n == on) return;
            if (f == 0) Settings.Parts = n; else Settings.Families ^= 1 << f;
            Changed(true);
        }

        GUIStyle bold;
        void Header(string t) { GUILayout.Space(6); GUILayout.Label(t, bold ?? (bold = new GUIStyle(GUI.skin.label) { fontStyle = FontStyle.Bold })); }

        void Slider(string label, ref float v, float lo, float hi)
        {
            GUILayout.BeginHorizontal();
            GUILayout.Label(label, GUILayout.Width(230));
            GUILayout.BeginVertical(); GUILayout.Space(8);
            float n = GUILayout.HorizontalSlider(v, lo, hi);
            GUILayout.EndVertical();
            GUILayout.EndHorizontal();
            if (n != v) { v = n; Changed(false); }
        }

        readonly float[] enhBuf = new float[13];
        void Feature(string label, int bit, int failed)
        {
            if ((failed & bit) != 0) { GUILayout.Label(label + " (unavailable)", GUILayout.Width(140)); return; }
            bool on = (Settings.Enh & bit) != 0, n = GUILayout.Toggle(on, label, GUILayout.Width(140));
            if (n != on) { Settings.Enh ^= bit; Changed(false); }
        }

        void Flag(string label, int bit)
        {
            bool on = (dbgFlags & bit) != 0, n = GUILayout.Toggle(on, label);
            if (n != on) { dbgFlags ^= bit; Core.Instance.SetSceneDebug(dbgFlags); }
        }

        void Changed(bool ownership)
        {
            dirty = true;
            if (ownership) Core.Instance?.Scene?.ApplyOwnership();
        }

        static string Census(Core core)
        {
            var sc = core.Scene;
            if (sc == null || !sc.Active) return "Only available in flight with KSPVk enabled.";
            var res = sc.Residuals(out int residual);
            sc.WorldResiduals(res, ref residual);
            return $"Residual (should be drawn by Vulkan but is not): {residual}\n" +
                   string.Join("\n", res.OrderByDescending(kv => kv.Value).Select(kv => $"{kv.Value,5}  {kv.Key}"));
        }

        void OnApplicationQuit() { if (dirty) { Settings.Save(); dirty = false; } }

        void OnDestroy()
        {
            if (dirty) Settings.Save();
            InputLockManager.RemoveControlLock(Lock);
            GameEvents.onGUIApplicationLauncherReady.Remove(AddButton);
            GameEvents.onGUIApplicationLauncherDestroyed.Remove(RemoveButton);
            RemoveButton();
        }
    }
}
