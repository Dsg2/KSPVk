using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using Debug = UnityEngine.Debug;

namespace KSPVk.Probe
{
    /// Scripted scene tour, enabled with the launch argument  -kspvk-bench[=label]
    /// label containing "quick" runs a reduced tour (used for the -force-vulkan check).
    /// Uses its own save folder (saves/KSPVk_Bench) seeded from the stock scenario saves; never touches user saves.
    public static class Bench
    {
        const string SaveFolder = "KSPVk_Bench";
        const float Settle = 8f, Duration = 10f;
        static readonly string[] Scenarios = { "Mun Orbit", "Space Station 1", "Jool Aerobrake", "EVA on Duna", "Dynawing Re-entry" };

        static string outDir;
        static bool phase1, exp, phase2, phase3, phase4, phase5, recon;
        static readonly List<string> summary = new List<string>();

        static Probe probe;

        public static IEnumerator Run(Probe p, string label)
        {
            probe = p;
            bool quick = label.Contains("quick");
            phase1 = label.Contains("phase1");
            exp = label.Contains("exp");
            phase2 = label.Contains("phase2"); phase3 = label.Contains("phase3"); phase4 = label.Contains("phase4"); phase5 = label.Contains("phase5"); recon = label.Contains("recon");
            if (phase2) quick = false;
            outDir = Path.Combine(Probe.PluginData, "bench/" + Report.Sanitize(label) + "_" + DateTime.Now.ToString("yyyyMMdd_HHmmss"));
            Directory.CreateDirectory(outDir);
            p.Uncapped = true;
            summary.Add(Report.SummaryHeader);
            p.Status = "waiting for main menu";
            yield return WaitFor(() => HighLogic.LoadedScene == GameScenes.MAINMENU, 900);
            yield return new WaitForSecondsRealtime(5);
            Try("static", () => Report.WriteStatic(outDir));
            if (phase4) { yield return Phase4.Tour(p, Step4); Finish(p); yield break; }
            if (phase5) { yield return Phase5.Tour(p, Step4, outDir); Finish(p); yield break; }
            yield return Step(p, "mainmenu");

            Try("seed saves", SeedSaves);
            Try("new sandbox", NewSandbox);
            yield return WaitFor(() => HighLogic.LoadedScene == GameScenes.SPACECENTER, 180);
            yield return Step(p, "ksc");

            string kx = Path.Combine(Probe.Root, "Ships/VAB/Kerbal X.craft"), cv = Path.Combine(Probe.Root, "Ships/VAB/Capture Vessel.craft");
            if ((!quick || exp) && !phase2 && !phase3)
            {
                Try("vab", () => EditorDriver.StartAndLoadVessel(kx, EditorFacility.VAB));
                yield return WaitFor(() => HighLogic.LoadedScene == GameScenes.EDITOR && EditorLogic.RootPart != null, 180);
                yield return Step(p, "vab_kerbalx");
            }

            Try("launch kerbal x", () => FlightDriver.StartWithNewLaunch(kx, "Squad/Flags/default", "LaunchPad", VesselCrewManifest.FromConfigNode(ConfigNode.Load(kx))));
            yield return WaitFlight();
            if (label.Contains("uitest"))
            {
                yield return new WaitForSecondsRealtime(5);
                yield return UiTest.Run(p, outDir);
                Finish(p);
                yield break;
            }
            yield return Step(p, "pad_kerbalx");

            if (phase3)
            {
                yield return Phase3.Tour(p, quick, Step3);
                Finish(p);
                yield break;
            }

            if (!quick)
            {
                Try("launch capture vessel", () => FlightDriver.StartWithNewLaunch(cv, "Squad/Flags/default", "LaunchPad", VesselCrewManifest.FromConfigNode(ConfigNode.Load(cv))));
                yield return WaitFlight();
                yield return Step(p, "pad_capturevessel_358p");
            }

            foreach (string sc in quick ? new[] { Scenarios[0] } : Scenarios)
            {
                Try("load " + sc, () =>
                {
                    var g = GamePersistence.LoadGame(sc, SaveFolder, true, true);
                    HighLogic.CurrentGame = g; HighLogic.SaveFolder = SaveFolder;
                    FlightDriver.StartAndFocusVessel(g, g.flightState.activeVesselIdx);
                });
                yield return WaitFlight();
                string n = Report.Sanitize(sc.ToLowerInvariant().Replace(' ', '_'));
                yield return Step(p, n);
                if (sc == "Mun Orbit" && !quick)
                {
                    bool iva = false;
                    Try("iva", () => iva = CameraManager.Instance.SetCameraIVA());
                    if (iva) { yield return Step(p, n + "_iva"); Try("flight cam", () => CameraManager.Instance.SetCameraFlight()); }
                    Try("map", MapView.EnterMapView);
                    yield return Step(p, n + "_map");
                    Try("exit map", MapView.ExitMapView);
                }
            }

            if (!quick && !phase2)
            {
                Try("tracking station", () => HighLogic.LoadScene(GameScenes.TRACKSTATION));
                yield return WaitFor(() => HighLogic.LoadedScene == GameScenes.TRACKSTATION, 180);
                yield return Step(p, "tracking_station");
            }

            if (phase1 && !exp) yield return Phase1.ResizeTest(p, outDir);
            Finish(p);
        }

        static void Finish(Probe p)
        {
            File.WriteAllLines(Path.Combine(outDir, "summary.txt"), summary.ToArray());
            p.Status = "done -> " + outDir;
            Debug.Log("[KSPVk.Probe] bench complete: " + outDir);
            p.StartCoroutine(Quit());
        }

        static IEnumerator Quit() { yield return new WaitForSecondsRealtime(2); Application.Quit(); }

        internal static void NewSandbox()
        {
            var g = GamePersistence.CreateNewGame(SaveFolder, Game.Modes.SANDBOX, new GameParameters(), "Squad/Flags/default", GameScenes.SPACECENTER, EditorFacility.None);
            HighLogic.CurrentGame = g; HighLogic.SaveFolder = SaveFolder;
            GamePersistence.SaveGame(g, "persistent", SaveFolder, SaveMode.OVERWRITE);
            g.Start();
        }

        static IEnumerator Step4(string name)
        {
            probe.Status = name + ": settling";
            yield return new WaitForSecondsRealtime(Settle);
            yield return recon ? Phase4.Recon(probe, name, outDir) : Phase4.Golden(probe, name, outDir);
            summary.Add(name);
            File.WriteAllLines(Path.Combine(outDir, "summary.txt"), summary.ToArray());
            Debug.Log("[KSPVk.Probe] phase4 step done: " + name);
        }

        static IEnumerator Step3(string name)
        {
            yield return recon ? Phase3.Recon(probe, name, outDir) : Phase3.Golden(probe, name, outDir);
            summary.Add(name);
            File.WriteAllLines(Path.Combine(outDir, "summary.txt"), summary.ToArray());
            Debug.Log("[KSPVk.Probe] phase3 step done: " + name);
        }

        static IEnumerator Step(Probe p, string name)
        {
            p.Status = name + ": settling";
            yield return new WaitForSecondsRealtime(Settle);
            if (phase3) { if (HighLogic.LoadedScene == GameScenes.FLIGHT) yield return Step3(name); yield break; }
            if (phase2)
            {
                if (HighLogic.LoadedScene != GameScenes.FLIGHT) yield break;
                yield return recon ? Phase2.Recon(p, name, outDir) : Phase2.Golden(p, name, outDir);
                summary.Add(name);
                File.WriteAllLines(Path.Combine(outDir, "summary.txt"), summary.ToArray());
                Debug.Log("[KSPVk.Probe] phase2 step done: " + name);
                yield break;
            }
            if (phase1)
            {
                yield return exp ? Phase1.Experiment(p, name, outDir) : Phase1.Step(p, name, outDir);
                summary.Add(name);
                File.WriteAllLines(Path.Combine(outDir, "summary.txt"), summary.ToArray());
                Debug.Log("[KSPVk.Probe] phase1 step done: " + name);
                yield break;
            }
            p.Status = name + ": measuring";
            var cap = new Capture(name);
            yield return p.Measure(cap, Duration);
            Try("report " + name, () => summary.Add(Report.WriteStep(outDir, cap)));
            p.Status = "";   // hide overlay for the screenshot
            yield return null;
            ScreenCapture.CaptureScreenshot(Path.Combine(outDir, name + ".png"));
            yield return null; yield return null;
            File.WriteAllLines(Path.Combine(outDir, "summary.txt"), summary.ToArray());
            Debug.Log("[KSPVk.Probe] step done: " + name);
        }

        internal static IEnumerator WaitFlight() => WaitFor(() => HighLogic.LoadedScene == GameScenes.FLIGHT && FlightGlobals.ready
                                                         && FlightGlobals.ActiveVessel != null && !FlightGlobals.ActiveVessel.packed, 240);

        internal static IEnumerator WaitFor(Func<bool> cond, float timeout)
        {
            float t0 = Time.realtimeSinceStartup;
            while (true)
            {
                bool ok = false;
                try { ok = cond(); } catch { }
                if (ok) yield break;
                if (Time.realtimeSinceStartup - t0 > timeout) { Debug.LogWarning("[KSPVk.Probe] wait timed out"); summary.Add("# wait timed out"); yield break; }
                yield return null;
            }
        }

        internal static void SeedSaves()
        {
            string dst = Path.Combine(Probe.Root, "saves/" + SaveFolder);
            Directory.CreateDirectory(dst);
            foreach (string sc in Scenarios)
                File.Copy(Path.Combine(Probe.Root, "saves/scenarios/" + sc + ".sfs"), Path.Combine(dst, sc + ".sfs"), true);
        }

        internal static void Try(string what, Action a)
        {
            try { a(); }
            catch (Exception e) { Debug.LogError("[KSPVk.Probe] " + what + " failed: " + e); summary.Add("# " + what + " failed: " + e.Message); }
        }
    }
}
