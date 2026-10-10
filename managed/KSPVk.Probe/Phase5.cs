using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEngine;

namespace KSPVk.Probe
{
    /// Phase 5: FX & characters. Highlighting, craft thumbnails, EVA Kerbal, planted flag, aero / re-entry FX; golden or recon steps.
    public static class Phase5
    {
        static readonly string Kx = Path.Combine(Probe.Root, "Ships/VAB/Kerbal X.craft"), Aeris = Path.Combine(Probe.Root, "Ships/SPH/Aeris 3A.craft");

        public static IEnumerator Tour(Probe p, Func<string, IEnumerator> step, string dir)
        {
            string only = Environment.GetEnvironmentVariable("KSPVK_SCENES");
            bool Want(string n) => string.IsNullOrEmpty(only) || only.Split(',').Any(s => n.StartsWith(s.Trim()));
            Bench.Try("seed saves", Bench.SeedSaves);
            Bench.Try("new sandbox", Bench.NewSandbox);
            yield return Bench.WaitFor(() => HighLogic.LoadedScene == GameScenes.SPACECENTER, 180);
            yield return new WaitForSecondsRealtime(4);
            Bench.Try("hire", () => { for (int i = 0; i < 4; i++) HighLogic.CurrentGame.CrewRoster.GetNewKerbal(ProtoCrewMember.KerbalType.Crew); });

            if (Want("vab"))
            {
                Bench.Try("vab", () => EditorDriver.StartAndLoadVessel(Kx, EditorFacility.VAB));
                yield return Bench.WaitFor(() => HighLogic.LoadedScene == GameScenes.EDITOR && EditorLogic.RootPart != null, 180);
                yield return new WaitForSecondsRealtime(4);
                // craft thumbnail (a Camera.Render of the real ship): coverage with KSPVk on must match off
                foreach (bool en in new[] { false, true })
                {
                    Core.Instance.Enabled = en;
                    for (int i = 0; i < 10; i++) yield return null;
                    Bench.Try("thumbnail", () => Debug.Log($"[KSPVk.Probe] thumbnail kspvk={en}: coverage {Thumb(dir, en):P1}"));
                }
                Core.Instance.Enabled = true;
                Bench.Try("highlight", () =>
                {
                    var r = EditorLogic.RootPart;
                    r.SetHighlightType(Part.HighlightType.AlwaysOn); r.SetHighlightColor(Color.cyan); r.SetHighlight(true, false);
                });
                yield return new WaitForSecondsRealtime(2);
                yield return step("vab_highlight");
                Bench.Try("unhighlight", () => { EditorLogic.RootPart.SetHighlightType(Part.HighlightType.OnMouseOver); EditorLogic.RootPart.SetHighlight(false, false); });
            }

            if (Want("eva"))
            {
                Bench.Try("launch aeris", () => FlightDriver.StartWithNewLaunch(Aeris, "Squad/Flags/default", "Runway", HighLogic.CurrentGame.CrewRoster.DefaultCrewForVessel(ConfigNode.Load(Aeris))));
                yield return Bench.WaitFlight();
                yield return new WaitForSecondsRealtime(4);
                Vessel plane = FlightGlobals.ActiveVessel;
                KerbalEVA eva = null;
                for (int t = 0; t < 10 && eva == null; t++)
                {
                    Bench.Try("eva", () =>
                    {
                        Part pod = plane.parts.FirstOrDefault(x => x.protoModuleCrew.Count > 0);
                        if (pod != null) eva = FlightEVA.fetch.spawnEVA(pod.protoModuleCrew[0], pod, pod.airlock, true);
                    });
                    yield return new WaitForSecondsRealtime(1);
                }
                if (eva != null)
                {
                    Bench.Try("focus eva", () => FlightGlobals.ForceSetActiveVessel(eva.vessel));
                    yield return new WaitForSecondsRealtime(3);
                    // the Kerbal spawns on the cockpit ladder behind the plane: let go, stand it 12 m off the wingtip side, camera close in
                    Bench.Try("let go", () => { if (eva.OnALadder) eva.fsm.RunEvent(eva.On_ladderLetGo); });
                    yield return new WaitForSecondsRealtime(2);
                    // the Kerbal stands beside the cockpit (a teleport is undone when it unpacks): view it from the open side, plane behind it
                    Bench.Try("eva camera", () => CamOutside(eva.vessel, plane));
                    yield return new WaitForSecondsRealtime(3);
                    Bench.Try("eva pos", () => Debug.Log($"[KSPVk.Probe] eva {Vector3d.Distance(eva.vessel.CoM, plane.CoM):F1} m from plane, ladder={eva.OnALadder}"));
                    yield return step("eva_kerbal");
                    Bench.Try("plane camera", () => { CamOutside(eva.vessel, plane); FlightCamera.fetch.SetDistanceImmediate(12); });
                    Bench.Try("highlight plane", () => { var r = plane.rootPart; r.SetHighlightType(Part.HighlightType.AlwaysOn); r.SetHighlightColor(Color.green); r.SetHighlight(true, true); });
                    yield return new WaitForSecondsRealtime(2);
                    yield return step("flight_highlight");
                    Bench.Try("unhighlight plane", () => { plane.rootPart.SetHighlightType(Part.HighlightType.OnMouseOver); plane.rootPart.SetHighlight(false, true); });
                    Bench.Try("plant flag", () => eva.PlantFlag());
                    // close the "Name Site" dialog (Cancel keeps the default name) as soon as it appears
                    var cancel = typeof(FlagSite).GetMethod("CancelSiteRename", BindingFlags.NonPublic | BindingFlags.Instance);
                    var dlg = typeof(FlagSite).GetField("SiteRenameDialog", BindingFlags.NonPublic | BindingFlags.Instance);
                    for (int t = 0; t < 12; t++)
                    {
                        yield return new WaitForSecondsRealtime(1);
                        Bench.Try("name site", () => { foreach (var fs in UnityEngine.Object.FindObjectsOfType<FlagSite>()) if (dlg.GetValue(fs) != null) cancel.Invoke(fs, null); });
                    }
                    Bench.Try("flag pos", () => { foreach (var fs in UnityEngine.Object.FindObjectsOfType<FlagSite>()) Debug.Log($"[KSPVk.Probe] flag {fs.vessel.vesselName} {Vector3d.Distance(fs.vessel.CoM, eva.vessel.CoM):F1} m from kerbal"); });
                    Bench.Try("flag camera", () => CamOutside(eva.vessel, plane));
                    yield return new WaitForSecondsRealtime(2);
                    yield return step("eva_flag");
                }
            }

            if (Want("aero"))
            {
                Bench.Try("launch kerbal x", () => FlightDriver.StartWithNewLaunch(Kx, "Squad/Flags/default", "LaunchPad", HighLogic.CurrentGame.CrewRoster.DefaultCrewForVessel(ConfigNode.Load(Kx))));
                yield return Bench.WaitFlight();
                yield return new WaitForSecondsRealtime(4);
                Bench.Try("stage", () => { CheatOptions.IgnoreMaxTemperature = CheatOptions.UnbreakableJoints = CheatOptions.NoCrashDamage = true; KSP.UI.Screens.StageManager.ActivateNextStage(); });
                yield return new WaitForSecondsRealtime(2);
                // mach 4 at low altitude: shock / re-entry flames on the whole vessel
                Bench.Try("boost", () => { var v = FlightGlobals.ActiveVessel; v.ChangeWorldVelocity(v.upAxis * 1800); });
                yield return new WaitForSecondsRealtime(2.5f);
                Bench.Try("particles", ParticleCost);
                yield return step("aero_reentry");
                Bench.Try("cheat off", () => CheatOptions.IgnoreMaxTemperature = CheatOptions.UnbreakableJoints = CheatOptions.NoCrashDamage = false);
            }
        }

        static void CamOutside(Vessel k, Vessel plane)
        {
            Vector3 up = (k.CoM - k.mainBody.position).normalized, away = Vector3.ProjectOnPlane(k.CoM - plane.CoM, up).normalized;
            FlightCamera.fetch.SetCamCoordsFromPosition(k.CoM + away * 4 + up * 1);
        }

        /// What moving stock particles to Vulkan would cost on the main thread: read every live particle once (GetParticles), as the plan proposed.
        static void ParticleCost()
        {
            var all = UnityEngine.Object.FindObjectsOfType<ParticleSystem>();
            var buf = new ParticleSystem.Particle[4096];
            int alive = 0, sys = 0;
            var sw = System.Diagnostics.Stopwatch.StartNew();
            for (int rep = 0; rep < 10; rep++)
                foreach (var ps in all)
                {
                    if (!ps.gameObject.activeInHierarchy || ps.particleCount == 0) continue;
                    if (buf.Length < ps.particleCount) buf = new ParticleSystem.Particle[ps.particleCount];
                    int n = ps.GetParticles(buf);
                    if (rep == 0) { alive += n; sys++; }
                }
            Debug.Log($"[KSPVk.Probe] particles: {all.Length} systems, {sys} emitting, {alive} alive | GetParticles all: {sw.Elapsed.TotalMilliseconds / 10:F3} ms/frame");
        }

        /// Writes a 256 px craft thumbnail and returns the share of non-transparent pixels.
        static float Thumb(string dir, bool en)
        {
            string rel = dir.Substring(KSPUtil.ApplicationRootPath.Length).Replace('\\', '/');
            string name = "thumb_" + (en ? "on" : "off");
            CraftThumbnail.TakeSnaphot(EditorLogic.fetch.ship, 256, rel, name);
            var t = new Texture2D(2, 2);
            t.LoadImage(File.ReadAllBytes(Path.Combine(dir, name + ".png")));
            var px = t.GetPixels32();
            UnityEngine.Object.Destroy(t);
            return px.Count(c => c.a > 0) / (float)px.Length;
        }
    }
}
