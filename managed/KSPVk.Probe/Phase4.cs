using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using UnityEngine;
using UnityEngine.Rendering;

namespace KSPVk.Probe
{
    /// Phase 4: every stock scene (main menu, KSC, VAB, SPH, flight + IVA + map view, tracking station) with recon or golden steps.
    public static class Phase4
    {
        static readonly string Kx = Path.Combine(Probe.Root, "Ships/VAB/Kerbal X.craft"), Aeris = Path.Combine(Probe.Root, "Ships/SPH/Aeris 3A.craft");

        public static IEnumerator Tour(Probe p, Func<string, IEnumerator> step)
        {
            string only = Environment.GetEnvironmentVariable("KSPVK_SCENES");   // optional comma list of step names
            bool Want(string n) => string.IsNullOrEmpty(only) || only.Split(',').Any(s => n.StartsWith(s.Trim()));
            if (Want("mainmenu")) yield return step("mainmenu");

            Bench.Try("seed saves", Bench.SeedSaves);
            Bench.Try("new sandbox", Bench.NewSandbox);
            yield return Bench.WaitFor(() => HighLogic.LoadedScene == GameScenes.SPACECENTER, 180);
            yield return new WaitForSecondsRealtime(4);
            if (Want("ksc")) yield return step("ksc");

            if (Want("vab"))
            {
                Bench.Try("vab", () => EditorDriver.StartAndLoadVessel(Kx, EditorFacility.VAB));
                yield return Bench.WaitFor(() => HighLogic.LoadedScene == GameScenes.EDITOR && EditorLogic.RootPart != null, 180);
                yield return new WaitForSecondsRealtime(4);
                yield return step("vab_kerbalx");
                if (Want("vab_clone"))
                {
                    // editor Alt-copy (EditorLogic.DuplicatePart = Instantiate) of a part KSPVk has hidden: the clone must render
                    Part dup = null;
                    Bench.Try("alt-copy", () =>
                    {
                        var src = EditorLogic.fetch.ship.parts.Where(x => x.children.Count == 0).OrderByDescending(x => x.GetComponentsInChildren<MeshRenderer>().Length).First();
                        dup = (Part)typeof(EditorLogic).GetMethod("DuplicatePart", BindingFlags.NonPublic | BindingFlags.Instance).Invoke(EditorLogic.fetch, new object[] { src });
                        dup.transform.position = src.transform.position + EditorLogic.fetch.editorCamera.transform.right * 4;
                    });
                    yield return null;
                    if (dup) Debug.Log($"[KSPVk.Probe] clone {dup.name}: renderers={dup.GetComponentsInChildren<Renderer>().Length} stillHidden={dup.GetComponentsInChildren<Renderer>().Count(r => r.forceRenderingOff || r.shadowCastingMode == ShadowCastingMode.ShadowsOnly)}");
                    yield return new WaitForSecondsRealtime(3);
                    yield return step("vab_clone");
                }
            }
            if (Want("sph"))
            {
                Bench.Try("sph", () => EditorDriver.StartAndLoadVessel(Aeris, EditorFacility.SPH));
                yield return Bench.WaitFor(() => HighLogic.LoadedScene == GameScenes.EDITOR && EditorDriver.editorFacility == EditorFacility.SPH && EditorLogic.RootPart != null, 180);
                yield return new WaitForSecondsRealtime(4);
                yield return step("sph_aeris");
            }
            if (Want("pad") || Want("pod") || Want("iva") || Want("ascent") || Want("map"))
            {
                Bench.Try("launch kerbal x", () => FlightDriver.StartWithNewLaunch(Kx, "Squad/Flags/default", "LaunchPad", HighLogic.CurrentGame.CrewRoster.DefaultCrewForVessel(ConfigNode.Load(Kx))));
                yield return Bench.WaitFlight();
                yield return new WaitForSecondsRealtime(4);
                if (Want("pad")) yield return step("pad_kerbalx");
                if (Want("pod"))
                {
                    // ModuleColorChanger window lights: on, then off (MPB _EmissiveColor (0,0,0,0) must switch the emission off)
                    var v = FlightGlobals.ActiveVessel;
                    Bench.Try("pod cam", () => { FlightCamera.SetTarget(v.rootPart); FlightCamera.fetch.SetDistanceImmediate(6); });
                    foreach (bool on in new[] { true, false })
                    {
                        Bench.Try("lights", () => v.ActionGroups.SetGroup(KSPActionGroup.Light, on));
                        yield return new WaitForSecondsRealtime(4);
                        yield return step(on ? "pod_lightson" : "pod_lightsoff");
                    }
                    Bench.Try("vessel cam", () => { FlightCamera.SetTarget(v); FlightCamera.fetch.SetDistanceImmediate(FlightCamera.fetch.startDistance); });
                    yield return new WaitForSecondsRealtime(2);
                }
                if (Want("iva"))
                {
                    bool iva = false;
                    Bench.Try("iva", () => iva = CameraManager.Instance.SetCameraIVA());
                    if (iva) { yield return new WaitForSecondsRealtime(3); yield return step("iva_mk1pod"); yield return step("iva_mk1pod_live"); }
                    Bench.Try("flight cam", () => CameraManager.Instance.SetCameraFlight());
                    yield return new WaitForSecondsRealtime(2);
                    if (iva)
                    {
                        Bench.Try("iva again", () => CameraManager.Instance.SetCameraIVA());
                        yield return new WaitForSecondsRealtime(3);
                        yield return step("iva_reentry_live");
                        Bench.Try("flight cam", () => CameraManager.Instance.SetCameraFlight());
                        yield return new WaitForSecondsRealtime(2);
                    }
                }
                if (Want("ascent"))
                {
                    // IVA in powered ascent: the vessel and the instruments move every frame
                    Bench.Try("launch", () => { FlightInputHandler.state.mainThrottle = 1; KSP.UI.Screens.StageManager.ActivateNextStage(); });
                    yield return new WaitForSecondsRealtime(12);
                    bool iva = false;
                    Bench.Try("iva ascent", () => iva = CameraManager.Instance.SetCameraIVA());
                    yield return new WaitForSecondsRealtime(3);
                    if (iva) yield return step("iva_ascent_live");
                    Bench.Try("flight cam", () => CameraManager.Instance.SetCameraFlight());
                    yield return new WaitForSecondsRealtime(2);
                }
                if (Want("map"))
                {
                    Bench.Try("map", MapView.EnterMapView);
                    yield return new WaitForSecondsRealtime(3);
                    yield return step("map_kerbin");
                    Bench.Try("exit map", MapView.ExitMapView);
                    yield return new WaitForSecondsRealtime(2);
                }
            }
            if (Want("cockpit"))   // IVA of every stock cockpit family, time running
            {
                // earlier test vessels keep their crews: hire spares so every cockpit launches manned (bench save only)
                Bench.Try("hire", () => { for (int i = 0; i < 4; i++) HighLogic.CurrentGame.CrewRoster.GetNewKerbal(ProtoCrewMember.KerbalType.Crew); });
                foreach (var (craft, site, label) in new[] { ("VAB/Kerbal 1", "LaunchPad", "mk1pod"), ("SPH/Aeris 3A", "Runway", "mk1cockpit"), ("SPH/Gull", "Runway", "mk2cockpit"),
                                                             ("SPH/Mallard", "Runway", "mk3cockpit") })
                {
                    string path = Path.Combine(Probe.Root, "Ships/" + craft + ".craft");
                    Bench.Try("launch " + craft, () => FlightDriver.StartWithNewLaunch(path, "Squad/Flags/default", site, HighLogic.CurrentGame.CrewRoster.DefaultCrewForVessel(ConfigNode.Load(path))));
                    yield return Bench.WaitFlight();
                    yield return new WaitForSecondsRealtime(4);
                    bool iva = false;
                    for (int t = 0; t < 10 && !iva; t++)   // IVA becomes available once the crew is aboard
                    {
                        Bench.Try("iva " + label, () => iva = CameraManager.Instance.SetCameraIVA());
                        yield return new WaitForSecondsRealtime(1);
                    }
                    yield return new WaitForSecondsRealtime(3);
                    if (iva) yield return step("cockpit_" + label + "_live");
                    Bench.Try("flight cam", () => CameraManager.Instance.SetCameraFlight());
                    yield return new WaitForSecondsRealtime(1);
                }
            }
            if (Want("tracking"))
            {
                Bench.Try("tracking station", () => HighLogic.LoadScene(GameScenes.TRACKSTATION));
                yield return Bench.WaitFor(() => HighLogic.LoadedScene == GameScenes.TRACKSTATION, 180);
                yield return new WaitForSecondsRealtime(4);
                yield return step("tracking_station");
            }
        }

        public static IEnumerator Golden(Probe p, string name, string dir) => Phase3.Golden(p, name, dir);

        /// Scene-wide rendering state that Phase 3 recon does not cover: lightmaps, probes, ambient, static batches, cameras by scene.
        public static IEnumerator Recon(Probe p, string name, string dir)
        {
            yield return Phase3.Recon(p, name, dir);
            var sb = new StringBuilder();
            sb.AppendLine($"# {name} extra recon (scene {HighLogic.LoadedScene})");
            sb.AppendLine($"lightmapsMode={LightmapSettings.lightmapsMode} count={LightmapSettings.lightmaps?.Length}");
            if (LightmapSettings.lightmaps != null)
                foreach (var lm in LightmapSettings.lightmaps)
                    sb.AppendLine($"  color={Tex(lm.lightmapColor)} dir={Tex(lm.lightmapDir)} mask={Tex(lm.shadowMask)}");
            var lp = LightmapSettings.lightProbes;
            sb.AppendLine($"lightProbes={(lp != null ? lp.count.ToString() : "none")}");
            sb.AppendLine($"ambient mode={RenderSettings.ambientMode} light={RenderSettings.ambientLight} sky={RenderSettings.ambientSkyColor} eq={RenderSettings.ambientEquatorColor} ground={RenderSettings.ambientGroundColor} int={RenderSettings.ambientIntensity}");
            var sh = RenderSettings.ambientProbe;
            sb.Append("ambientProbe:");
            for (int c = 0; c < 3; c++) { sb.Append(" ["); for (int k = 0; k < 9; k++) sb.Append(sh[c, k].ToString("G4")).Append(k < 8 ? "," : "]"); }
            sb.AppendLine();
            sb.AppendLine($"defaultReflection mode={RenderSettings.defaultReflectionMode} res={RenderSettings.defaultReflectionResolution} custom={Tex(RenderSettings.customReflection)} intensity={RenderSettings.reflectionIntensity} skybox={RenderSettings.skybox?.name}/{RenderSettings.skybox?.shader?.name}");
            foreach (var rp in Resources.FindObjectsOfTypeAll<ReflectionProbe>().Where(x => x.gameObject.scene.IsValid()))
                sb.AppendLine($"reflprobe {rp.name} en={rp.enabled && rp.gameObject.activeInHierarchy} mode={rp.mode} refresh={rp.refreshMode} tex={Tex(rp.texture)} baked={Tex(rp.bakedTexture)} custom={Tex(rp.customBakedTexture)} box={rp.boxProjection} bounds={rp.bounds} imp={rp.importance}");
            foreach (var l in Resources.FindObjectsOfTypeAll<Light>().Where(x => x.gameObject.scene.IsValid()))
                sb.AppendLine($"light {l.name} en={l.isActiveAndEnabled} type={l.type} L{l.gameObject.layer} mask={l.cullingMask:x8} col={l.color} int={l.intensity} range={l.range} spot={l.spotAngle} shadows={l.shadows} render={l.renderMode} pos={l.transform.position} fwd={l.transform.forward}");
            foreach (var c in Camera.allCameras)
                sb.AppendLine($"cam {c.name} depth={c.depth} mask={c.cullingMask:x8} clear={c.clearFlags} bg={c.backgroundColor} near={c.nearClipPlane} pos={c.transform.position}");
            int stat = 0, lmapped = 0, probes = 0, multi = 0;
            var byShader = new System.Collections.Generic.Dictionary<string, int>();
            foreach (var r in Resources.FindObjectsOfTypeAll<Renderer>())
            {
                if (!r.gameObject.scene.IsValid() || !r.enabled || !r.gameObject.activeInHierarchy) continue;
                if (r.isPartOfStaticBatch) stat++;
                if (r.lightmapIndex >= 0 && r.lightmapIndex < 0xfffe) lmapped++;
                if (r.lightProbeUsage != LightProbeUsage.Off) probes++;
                if (r.sharedMaterials.Length > 1) multi++;
                var mr = r as MeshRenderer;
                string k = $"L{r.gameObject.layer} static={r.isPartOfStaticBatch} lm={(r.lightmapIndex >= 0 && r.lightmapIndex < 0xfffe)} probes={r.lightProbeUsage} refl={r.reflectionProbeUsage} subStart={(mr ? mr.subMeshStartIndex : -1) > 0} mats={r.sharedMaterials.Length} part={r.GetComponentInParent<Part>() != null} " +
                           string.Join("+", r.sharedMaterials.Select(m => m && m.shader ? m.shader.name : "null").Distinct());
                byShader[k] = byShader.TryGetValue(k, out int c) ? c + 1 : 1;
            }
            sb.AppendLine($"enabled renderers: staticBatched={stat} lightmapped={lmapped} probeLit={probes} multiMaterial={multi}");
            foreach (var kv in byShader.OrderByDescending(x => x.Value)) sb.AppendLine($"{kv.Value,5} | {kv.Key}");
            File.WriteAllText(Path.Combine(dir, "recon4_" + name + ".txt"), sb.ToString());
        }

        static string Tex(Texture t) => t == null ? "-" : t is Texture2D t2 ? $"{t.name}:{t2.format}:{t.width}x{t.height}:m{t2.mipmapCount}" : $"{t.name}:{t.GetType().Name}:{t.dimension}:{t.width}";
    }
}
