using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using UnityEngine;

namespace KSPVk.Probe
{
    /// -kspvk-bench=uitest: drives the settings window and every setting on the pad and checks the renderer reacts.
    public static class UiTest
    {
        public static IEnumerator Run(Probe p, string dir)
        {
            var lines = new List<string>();
            var core = Core.Instance; var sc = core?.Scene; var win = SettingsWindow.Instance;
            void Check(string what, bool ok, string detail) { lines.Add($"{(ok ? "PASS" : "FAIL")} {what}: {detail}"); Debug.Log("[KSPVk.Probe] uitest " + lines[lines.Count - 1]); }
            void Write() => File.WriteAllLines(Path.Combine(dir, "uitest.txt"), lines.ToArray());
            if (sc == null || win == null) { Check("setup", false, "core, scene or settings window missing"); Write(); yield break; }

            p.Status = "uitest: settling";
            yield return Settle(sc, 60);
            int parts0 = sc.Hidden, world0 = sc.WorldHidden, r = Residual(sc);
            Check("baseline", parts0 > 0 && world0 > 0 && r == 0, $"parts {parts0}, world {world0}, residual {r}");
            float lum0 = 0;
            yield return Shot(dir, "baseline", l => lum0 = l);

            win.Show(true);
            yield return Frames(20);
            Check("window opens", win.Visible, "visible " + win.Visible);
            yield return Shot(dir, "ui_window", l => { });

            p.Status = "uitest: parts";
            Settings.Parts = false; sc.ApplyOwnership();
            yield return Frames(30);
            r = Residual(sc);
            Check("parts off", sc.Hidden == 0 && sc.Draws == 0 && r > 0 && sc.WorldHidden > 0, $"parts {sc.Hidden}, draws {sc.Draws}, residual {r} (Unity draws the parts), world {sc.WorldHidden}");
            Settings.Parts = true; sc.ApplyOwnership();
            yield return Settle(sc, 30);
            r = Residual(sc);
            Check("parts on", sc.Hidden == parts0 && r == 0, $"parts {sc.Hidden}/{parts0}, residual {r}");

            p.Status = "uitest: world families";
            Settings.Families = 0; sc.ApplyOwnership();
            yield return Frames(30);
            r = Residual(sc);
            Check("world off", sc.WorldHidden == 0 && sc.WorldDraws == 0 && sc.Hidden == parts0 && r > 0, $"world {sc.WorldHidden}, draws {sc.WorldDraws}, residual {r}, parts {sc.Hidden}");
            yield return Shot(dir, "world_off", l => { });
            Settings.Families = ~(1 << 5); sc.ApplyOwnership();   // atmosphere only goes back to Unity
            yield return Settle(sc, 30);
            int worldNoAtmo = sc.WorldHidden; r = Residual(sc);
            Check("atmosphere off", worldNoAtmo > 0 && r > 0, $"world {worldNoAtmo}, residual {r}");
            Settings.Families = -1; sc.ApplyOwnership();
            yield return Settle(sc, 30);
            r = Residual(sc);
            Check("world on", sc.WorldHidden > 0 && r == 0, $"world {sc.WorldHidden} (was {worldNoAtmo}), residual {r}");

            win.Show(false);
            yield return Frames(5);
            Check("window closes", !win.Visible, "visible " + win.Visible);

            p.Status = "uitest: lighting";
            Settings.Shadows = false;
            yield return Frames(10);
            int sd = sc.Stats().shadowDraws;
            Settings.Shadows = true;
            yield return Frames(10);
            int sd1 = sc.Stats().shadowDraws;
            Check("shadows toggle", sd == 0 && sd1 > 0, $"shadow draws off {sd}, on {sd1}");
            Settings.SunScale = 0.3f;
            float lumDim = 0;
            yield return Frames(5);
            yield return Shot(dir, "sun_x0.3", l => lumDim = l);
            Settings.SunScale = 1; Settings.AmbientScale = 3;
            float lumAmb = 0;
            yield return Frames(5);
            yield return Shot(dir, "ambient_x3", l => lumAmb = l);
            Settings.AmbientScale = 1;
            Check("sun / ambient scale", lumDim < lum0 - 1 && lumAmb > lum0 + 1, $"mean luminance baseline {lum0:F1}, sun x0.3 {lumDim:F1}, ambient x3 {lumAmb:F1}");

            string tmp = Path.Combine(dir, "settings_roundtrip.cfg");
            Settings.Parts = false; Settings.Families = 0x2A; Settings.ShadowDist = 750; Settings.SunScale = 1.25f; Settings.QuadsPerFrame = 512;
            Settings.SaveTo(tmp);
            Settings.Reset();
            Settings.LoadFrom(tmp);
            bool rt = !Settings.Parts && Settings.Families == 0x2A && Settings.ShadowDist == 750 && Settings.SunScale == 1.25f && Settings.QuadsPerFrame == 512;
            Check("save / load", rt, File.Exists(tmp) ? File.ReadAllText(tmp).Replace("\n", " ").Replace("\t", "") : "file missing");
            Settings.Reset(); sc.ApplyOwnership();

            Write();
        }

        static int Residual(Scene sc)
        {
            var res = sc.Residuals(out int r);
            sc.WorldResiduals(res, ref r);
            return r;
        }

        static IEnumerator Frames(int n) { for (int i = 0; i < n; i++) yield return null; }

        static IEnumerator Settle(Scene sc, float timeout)
        {
            float t0 = Time.realtimeSinceStartup;
            int stable = 0, last = -1;
            while (Time.realtimeSinceStartup - t0 < timeout && stable < 30)
            {
                yield return null;
                int h = sc.Hidden + sc.WorldHidden;
                stable = sc.Stats().pending == 0 && sc.QuadPending == 0 && h == last ? stable + 1 : 0;
                last = h;
            }
        }

        static IEnumerator Shot(string dir, string name, Action<float> lum)
        {
            yield return new WaitForEndOfFrame();
            var t = ScreenCapture.CaptureScreenshotAsTexture();
            File.WriteAllBytes(Path.Combine(dir, name + ".png"), t.EncodeToPNG());
            var px = t.GetPixels32();
            double s = 0; int n = 0;
            for (int y = t.height / 4; y < t.height * 3 / 4; y++)   // central half: mostly 3D, little UI
                for (int x = t.width / 4; x < t.width * 3 / 4; x++, n++) { var c = px[y * t.width + x]; s += 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }
            UnityEngine.Object.Destroy(t);
            lum((float)(s / n));
        }
    }
}
