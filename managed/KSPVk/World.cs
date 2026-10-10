using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;

namespace KSPVk
{
    /// Injected into every PQS: reports quad (re)builds, normal updates and destruction to the scene extractor.
    public class KSPVkPqsHook : PQSMod
    {
        public static Scene S;
        // called from inside PQS: an exception must never escape into the terrain update
        public override void OnQuadBuilt(PQ q) { try { S?.QuadDirty(q); } catch (Exception e) { Core.Fault(e); } }
        public override void OnQuadUpdateNormals(PQ q) { try { S?.QuadDirty(q); } catch (Exception e) { Core.Fault(e); } }
        public override void OnQuadDestroy(PQ q) { try { S?.QuadGone(q); } catch (Exception e) { Core.Fault(e); } }
    }

    /// Phase 3 world extractor: PQS terrain and ocean quads (local view), scaled-space bodies, the sun and atmospheres
    /// (scaled view) and the galaxy cube (galaxy view). Same rule as parts: Unity stops drawing an object only once its
    /// mesh and textures are resident on the Vulkan side.
    public unsafe partial class Scene
    {
        const int FamPqs = 1, FamOcean = 2, FamScaled = 3, FamSun = 4, FamAtmo = 5, FamGalaxy = 6;
        const float Inf = 1e30f;
        public delegate int SharedFn(IntPtr idx, IntPtr counts, int sets);
        public delegate void ReleaseFn(int id);
        RegisterFn registerCpu; SharedFn setShared; ReleaseFn releaseMesh;
        public bool WorldEnabled = true;
        public int FamMask => Settings.Families & ~Compat.FamOff;   // world families drawn by Vulkan (settings window / KSPVK_FAMMASK)
        public int WorldHidden, WorldDraws, QuadPending, resets;
        public Camera ScaledCam { get; private set; }
        public Camera GalaxyCam { get; private set; }

        class Quad { public PQ q; public int key, idx, id = -1, next = -1, slot = -1, xi = -1; public bool dirty, hidden, dead, matOk, cast; public Material mat; public Transform t; }
        class WObj { public Renderer r; public int meshId = -1, slot = -1, view; public bool hidden; public string reason; }

        readonly Dictionary<int, Quad> quads = new Dictionary<int, Quad>();
        readonly List<Quad> quadList = new List<Quad>(), dirty = new List<Quad>();
        readonly Dictionary<int, WObj> wByR = new Dictionary<int, WObj>();
        readonly List<WObj> wobjs = new List<WObj>();
        readonly Dictionary<int, int> worldSlots = new Dictionary<int, int>();
        readonly List<int> worldSlotList = new List<int>();
        readonly IntPtr meshCpu = Marshal.AllocHGlobal(88);
        readonly List<Vector3> lv = new List<Vector3>(), ln = new List<Vector3>();
        readonly List<Vector4> lt = new List<Vector4>();
        readonly List<Color> lc = new List<Color>();
        readonly List<Vector2> lu0 = new List<Vector2>(), lu1 = new List<Vector2>();
        int[][] sharedSrc;
        GalaxyCubeControl galaxyCtl;

        static readonly FieldInfo fMods = typeof(PQS).GetField("mods", BindingFlags.NonPublic | BindingFlags.Instance),
                                  fModCount = typeof(PQS).GetField("modCount", BindingFlags.NonPublic | BindingFlags.Instance);
        static class Items<T> { public static readonly FieldInfo F = typeof(List<T>).GetField("_items", BindingFlags.NonPublic | BindingFlags.Instance); }
        static T[] Arr<T>(List<T> l) => l.Count > 0 ? (T[])Items<T>.F.GetValue(l) : null;

        public void BindWorld(RegisterFn cpu, SharedFn shared, ReleaseFn release)
        {
            registerCpu = cpu; setShared = shared; releaseMesh = release;
            KSPVkPqsHook.S = this;
        }

        int WorldCap() => quadList.Count + wobjs.Count;

        // ---------- scanning ----------
        readonly HashSet<PQS> hooked = new HashSet<PQS>();
        /// Hooks new spheres; their existing quads are walked once (later builds arrive through the hook), all of them on a full scan.
        void RescanWorld(bool full = true)
        {
            if (!WorldEnabled || registerCpu == null) return;
            ScaledCam = ScaledCamera.Instance ? ScaledCamera.Instance.cam : null;
            GalaxyCam = null;
            foreach (var c in Camera.allCameras) if (c.cullingMask == 1 << 18 && c.targetTexture == null) { GalaxyCam = c; break; }
            foreach (var b in FlightGlobals.Bodies)
            {
                if (b.pqsController != null)
                    foreach (var pqs in b.pqsController.GetComponentsInChildren<PQS>(true))
                    {
                        if (!hooked.Add(pqs) && !full) continue;
                        Hook(pqs);
                        if (pqs.isActive && pqs.quads != null) foreach (var q in pqs.quads) Walk(q);
                    }
                if (b.scaledBody == null) continue;
                AddW(b.scaledBody.GetComponent<Renderer>(), 1);
                foreach (var a in b.scaledBody.GetComponentsInChildren<AtmosphereFromGround>(true)) AddW(a.GetComponent<Renderer>(), 1, a);
            }
            if (galaxyCtl == null) galaxyCtl = UnityEngine.Object.FindObjectOfType<GalaxyCubeControl>();
            if (galaxyCtl != null) foreach (var r in galaxyCtl.GetComponentsInChildren<Renderer>(true)) AddW(r, 2);
            for (int i = wobjs.Count - 1; i >= 0; i--)
                if (wobjs[i].r == null) { wByR.Remove(wByR.FirstKey(wobjs[i])); wobjs.RemoveAt(i); }
        }

        void Hook(PQS pqs)
        {
            KSPVkPqsHook hook = null;
            foreach (Transform ch in pqs.transform) { hook = ch.GetComponent<KSPVkPqsHook>(); if (hook != null) break; }
            if (hook == null)
            {
                var go = new GameObject("KSPVk hook");
                go.transform.SetParent(pqs.transform, false);
                hook = go.AddComponent<KSPVkPqsHook>();
                hook.sphere = pqs; hook.modEnabled = true; hook.order = int.MaxValue;
            }
            var mods = (PQSMod[])fMods.GetValue(pqs) ?? new PQSMod[0];
            int n = (int)fModCount.GetValue(pqs);
            if (Array.IndexOf(mods, hook, 0, Math.Min(n, mods.Length)) >= 0) return;
            var nm = new PQSMod[n + 1];
            Array.Copy(mods, nm, Math.Min(n, mods.Length));
            nm[n] = hook;
            fMods.SetValue(pqs, nm); fModCount.SetValue(pqs, n + 1);
        }

        void Walk(PQ q)
        {
            if (q == null) return;
            if (q.mesh != null && q.mesh.vertexCount > 0 && !quads.ContainsKey(q.GetInstanceID())) QuadDirty(q);   // stale unbuilt quads still render
            if (q.subNodes != null) foreach (var s in q.subNodes) Walk(s);
        }

        void ApplyFamilies()
        {
            foreach (var qd in quadList)
            {
                var mr = qd.q != null ? qd.q.meshRenderer : null;
                if (!qd.hidden || mr == null) continue;
                var m = mr.sharedMaterial;
                if (m != null && worldSlots.TryGetValue(m.GetInstanceID(), out int s) && s >= 0 && (FamMask >> matRecs[s].fam & 1) != 0) continue;
                mr.forceRenderingOff = false; qd.hidden = false;
            }
            foreach (var w in wobjs)
                if (w.hidden && w.r != null && (FamMask >> matRecs[w.slot].fam & 1) == 0) { w.r.forceRenderingOff = false; w.hidden = false; }
        }

        public void QuadDirty(PQ q)
        {
            if (!Active || !WorldEnabled || registerCpu == null || q == null) return;
            int key = q.GetInstanceID();
            if (!quads.TryGetValue(key, out var qd))
            {
                qd = new Quad { q = q, key = key, idx = quadList.Count };
                quads[key] = qd; quadList.Add(qd);
                qd.xi = XAdd(qd.t = q.transform, qd, -1, default);
            }
            qd.matOk = false;
            if (!qd.dirty) { qd.dirty = true; dirty.Add(qd); }
        }

        public void QuadGone(PQ q)
        {
            if (q != null && quads.TryGetValue(q.GetInstanceID(), out var qd)) DropQuad(qd);
        }

        void DropQuad(Quad qd)
        {
            if (qd.hidden && qd.q != null && qd.q.meshRenderer != null) qd.q.meshRenderer.forceRenderingOff = false;
            if (qd.id >= 0) releaseMesh(qd.id);
            if (qd.next >= 0) releaseMesh(qd.next);
            qd.dead = true;
            XRemove(qd.xi); qd.xi = -1;
            var last = quadList[quadList.Count - 1];
            quadList[qd.idx] = last; last.idx = qd.idx; quadList.RemoveAt(quadList.Count - 1);
            quads.Remove(qd.key);
        }

        void AddW(Renderer r, int view, Component c = null)
        {
            if (r == null || wByR.ContainsKey(r.GetInstanceID())) return;
            var w = new WObj { r = r, view = view };
            wByR[r.GetInstanceID()] = w; wobjs.Add(w);
            var mesh = r.GetComponent<MeshFilter>()?.sharedMesh;
            var m = r.sharedMaterial;
            if (mesh == null || m == null) { w.reason = "no mesh / material"; return; }
            int fam = Classify(m, out _);
            int want = c is AtmosphereFromGround ? FamAtmo : view == 2 ? FamGalaxy : fam == FamSun ? FamSun : FamScaled;
            if (fam != want) { w.reason = "shader " + m.shader.name; return; }
            w.meshId = MeshId(mesh);
            if (w.meshId < 0) { w.reason = "mesh unsupported"; return; }
            w.slot = WorldSlot(r, m, r, c);
            if (w.slot < 0) w.reason = "material";
        }

        static int Classify(Material m, out int kind)
        {
            kind = 0;
            string n = m.shader != null ? m.shader.name : "";
            switch (n)
            {
                case "Terrain/PQS/Ocean Surface Quad": return FamOcean;
                case "Terrain/PQS/PQS Main Shader": case "Terrain/PQS/PQS Main Shader - Fast Blend": return FamPqs;   // Fast Blend: the far / high-altitude quads
                case "Terrain/PQS/PQS Main - Optimised": case "Terrain/PQS/PQS Main - Optimised With Fast Blend": kind = 1; return FamPqs;
                case "Terrain/Scaled Planet (Simple)": case "Terrain/Gas Giant": return FamScaled;
                case "Terrain/Scaled Planet (RimAerial)": kind = 1; return FamScaled;
                case "Terrain/Scaled Planet (RimAerial) Standard": kind = 2; return FamScaled;
                case "Emissive Multi Ramp Sunspots": return FamSun;
                case "AtmosphereFromGround": return FamAtmo;
                case "Sprite/Simple Texture (Unlit)": return FamGalaxy;
            }
            if (n.StartsWith("Terrain/PQS/Sphere Projection SURFACE QUAD") && !n.Contains("Fallback")) { kind = n.Contains("(AP)") ? 3 : 2; return FamPqs; }
            return 0;
        }

        /// Material slot for a world object: keyed by the material (PQS, shared by all quads) or by the renderer (MPB / per-object state).
        int WorldSlot(UnityEngine.Object key, Material m, Renderer r = null, Component c = null)
        {
            int k = key.GetInstanceID();
            if (worldSlots.TryGetValue(k, out int s)) return s;
            int fam = Classify(m, out int kind);
            if (fam == 0 || (r == null) != (fam == FamPqs || fam == FamOcean)) return worldSlots[k] = -1;
            s = matRecs.Count;
            matRecs.Add(new MatRec { m = m, fam = fam, flags = kind, r = r, c = c });
            Grow(ref mats, ref matCap, matRecs.Count, MatF * 4);
            worldSlots[k] = s; worldSlotList.Add(s);
            WriteMat(s);
            return s;
        }

        PQS sphLast; bool sphOn; int sphFrame;
        bool SphereOn(PQS p)
        {
            if (ReferenceEquals(p, sphLast) && sphFrame == frame) return sphOn;
            sphLast = p; sphFrame = frame;
            return sphOn = p != null && p.isActive && p.gameObject.activeInHierarchy;
        }

        // PQ keeps meshRenderer.enabled == isVisible && !isForcedInvisible (subdividing hides the parent through SetInvisible), except that a
        // deactivated sphere hides all its quads (SetMasterInvisible). Tracked quads are always active: PQS drops them before caching them inactive.
        bool QuadOn(PQ q) => q.isVisible && !q.isForcedInvisible && SphereOn(q.sphereRoot);

        // ---------- per frame ----------
        bool BuildWorld(Camera cam, ref int n)
        {
            WorldDraws = WorldHidden = 0;
            if (!WorldEnabled || registerCpu == null) return true;
            int r0 = resets;
            var ci = PQS.cacheIndices;
            if (ci != null && ci != sharedSrc && !UploadShared(ci)) return false;
            foreach (int s in worldSlotList) { WriteMat(s); if (resets != r0) return false; }
            // oldest first; a quad re-dirtied while its upload is in flight waits for it (replacing it would starve the quad forever)
            int keep = 0;
            for (int i = 0, up = 0; i < dirty.Count; i++)
            {
                var qd = dirty[i];
                if (qd.dead || qd.q == null) { qd.dirty = false; continue; }
                Promote(qd);
                if (up >= Settings.QuadsPerFrame || qd.next >= 0) { dirty[keep++] = qd; continue; }
                qd.dirty = false;
                UploadQuad(qd); up++;
                if (resets != r0) return false;
            }
            dirty.RemoveRange(keep, dirty.Count - keep);
            QuadPending = dirty.Count;

            float* D = (float*)draws;
            for (int i = 0; cam != null && i < quadList.Count; i++)   // quads belong to the local view
            {
                var qd = quadList[i]; var q = qd.q;
                if (q == null) { DropQuad(qd); i--; continue; }
                Promote(qd);
                var mr = q.meshRenderer;
                if (qd.id < 0 || mr == null) continue;
                // PQS assigns the material when it builds the quad (texture atlas: per quad): re-read a 32nd of the quads per frame
                if (!qd.matOk || ((i + frame) & 31) == 0)
                {
                    var mat = mr.sharedMaterial;
                    if (!ReferenceEquals(mat, qd.mat)) { qd.mat = mat; qd.slot = mat != null ? WorldSlot(mat, mat) : -1; }
                    qd.cast = mr.shadowCastingMode != ShadowCastingMode.Off; qd.matOk = true;
                }
                int slot = qd.slot;
                if (resets != r0) return false;
                if (slot < 0) { if (qd.hidden) { mr.forceRenderingOff = false; qd.hidden = false; } continue; }
                var rec = matRecs[slot];
                if ((FamMask >> rec.fam & 1) == 0) continue;
                if (!qd.hidden) { if (!rec.ready) continue; mr.forceRenderingOff = true; qd.hidden = true; }
                WorldHidden++;
                if (!QuadOn(q)) continue;
                float* d = D + n * 20; int* di = (int*)(d + 12);
                XRows(qd.xi, qd.t, d);
                di[0] = qd.id; di[1] = Math.Max(0, (int)q.edgeState); di[2] = slot; di[3] = 0;
                di[4] = rec.fam == FamPqs && qd.cast ? DfCast : 0; di[5] = 0; di[6] = rec.fam; di[7] = 0;
                n++; WorldDraws++;
            }
            foreach (var w in wobjs)
            {
                var r = w.r;
                if (r == null || w.reason != null) continue;
                var rec = matRecs[w.slot];
                if ((FamMask >> rec.fam & 1) == 0) continue;
                if (!w.hidden)
                {
                    if (meshState[w.meshId] == RsFailed) { w.reason = "mesh failed"; continue; }
                    if (meshState[w.meshId] != RsReady || !rec.ready) continue;
                    if ((w.view == 1 ? ScaledCam : GalaxyCam) == null) continue;
                    r.forceRenderingOff = true; w.hidden = true;
                }
                WorldHidden++;
                if (!r.enabled || !r.gameObject.activeInHierarchy) continue;
                float* d = D + n * 20; int* di = (int*)(d + 12);
                Rows(r.localToWorldMatrix, d);
                di[0] = w.meshId; di[1] = 0; di[2] = w.slot; di[3] = di[4] = di[5] = 0; di[6] = rec.fam; di[7] = w.view;
                n++; WorldDraws++;
            }
            return true;
        }

        bool UploadShared(int[][] ci)
        {
            int total = 0;
            var counts = new int[ci.Length];
            for (int i = 0; i < ci.Length; i++) total += counts[i] = ci[i]?.Length ?? 0;
            IntPtr buf = Marshal.AllocHGlobal(Math.Max(4, total * 4));
            int o = 0;
            for (int i = 0; i < ci.Length; i++) if (counts[i] > 0) { Marshal.Copy(ci[i], 0, buf + o * 4, counts[i]); o += counts[i]; }
            int ok;
            fixed (int* pc = counts) ok = setShared(buf, (IntPtr)pc, ci.Length);
            Marshal.FreeHGlobal(buf);
            if (ok == 0) { ResetNative(); return false; }
            sharedSrc = ci;
            return true;
        }

        void Promote(Quad qd)
        {
            if (qd.next < 0) return;
            int st = meshState[qd.next];
            if (st == RsReady) { if (qd.id >= 0) releaseMesh(qd.id); qd.id = qd.next; qd.next = -1; }
            else if (st == RsFailed) { releaseMesh(qd.next); qd.next = -1; }
        }

        void UploadQuad(Quad qd)
        {
            var m = qd.q.mesh;
            if (m == null) return;
            m.GetVertices(lv); m.GetNormals(ln); m.GetTangents(lt); m.GetColors(lc); m.GetUVs(0, lu0); m.GetUVs(1, lu1);
            int nv = lv.Count;
            if (nv == 0) return;
            Bounds b = m.bounds;
            int id;
            fixed (Vector3* pv = Arr(lv), pn = Arr(ln))
            fixed (Vector4* pt = Arr(lt))
            fixed (Color* pc = Arr(lc))
            fixed (Vector2* p0 = Arr(lu0), p1 = Arr(lu1))
            {
                var c = (IntPtr*)meshCpu;
                c[0] = (IntPtr)pv; c[1] = ln.Count == nv ? (IntPtr)pn : IntPtr.Zero; c[2] = lt.Count == nv ? (IntPtr)pt : IntPtr.Zero;
                c[3] = lc.Count == nv ? (IntPtr)pc : IntPtr.Zero; c[4] = lu0.Count == nv ? (IntPtr)p0 : IntPtr.Zero; c[5] = lu1.Count == nv ? (IntPtr)p1 : IntPtr.Zero;
                c[6] = IntPtr.Zero;
                int* ni = (int*)(meshCpu + 56); ni[0] = nv; ni[1] = 0;
                float* bf = (float*)(meshCpu + 64);
                bf[0] = b.center.x; bf[1] = b.center.y; bf[2] = b.center.z; bf[3] = b.extents.x; bf[4] = b.extents.y; bf[5] = b.extents.z;
                id = registerCpu(meshCpu);
            }
            if (id < 0) { ResetNative(); return; }
            if (qd.next >= 0) releaseMesh(qd.next);
            qd.next = id;
        }

        void FillViews(float* f, Camera cam)
        {
            bool on = WorldDraws > 0;
            View(f + 140, on ? ScaledCam : null);
            View(f + 168, on ? GalaxyCam : null);
            f[196] = cam.farClipPlane; f[197] = Time.timeSinceLevelLoad; f[198] = f[199] = 0;
        }

        static void View(float* o, Camera c)
        {
            for (int i = 0; i < 28; i++) o[i] = 0;
            if (c == null || !c.isActiveAndEnabled) return;
            Matrix4x4 v = c.worldToCameraMatrix, p = c.projectionMatrix;
            for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) o[r * 4 + k] = v[r, k];
            o[16] = p.m00; o[17] = p.m01; o[18] = p.m02; o[19] = p.m10; o[20] = p.m11; o[21] = p.m12;
            o[22] = c.nearClipPlane; o[23] = c.farClipPlane;
            Vector3 cp = c.transform.position; o[24] = cp.x; o[25] = cp.y; o[26] = cp.z; o[27] = 1;
        }

        // ---------- materials ----------
        int Tx(Material m, string n, ref bool ready)
        {
            int pid = P(n);
            if (!m.HasProperty(pid)) return -1;
            int id = TexId(m.GetTexture(pid), -1);
            if (id < 0) return -1;
            if (texState[id] == RsReady) return id;
            if (texState[id] != RsFailed) ready = false;
            return -1;
        }
        static void V4(float* o, Vector4 v) { o[0] = v.x; o[1] = v.y; o[2] = v.z; o[3] = v.w; }
        static void V4(float* o, float x, float y, float z, float w) { o[0] = x; o[1] = y; o[2] = z; o[3] = w; }
        static void Ids(float* o, int a, int b, int c, int d) { int* i = (int*)o; i[0] = a; i[1] = b; i[2] = c; i[3] = d; }

        void WriteWorldMat(MatRec rec, float* f)
        {
            for (int i = 0; i < MatF; i++) f[i] = 0;
            Material m = rec.r != null ? rec.r.sharedMaterial : rec.m;
            if (m == null || (rec.r == null && rec.c != null)) { rec.ready = false; return; }
            bool ok = true;
            switch (rec.fam)
            {
                case FamPqs: WritePqs(rec, m, f, ref ok); break;
                case FamOcean:
                    Col(f, m, "_Color", Color.white); Col(f + 4, m, "_ColorFromSpace", Color.white); Col(f + 8, m, "_SpecColor", Color.gray); Col(f + 12, m, "_fogColor", Color.white);
                    V4(f + 16, Flt(m, "_displacement", 0), Flt(m, "_texDisplacement", 0), Flt(m, "_tiling", 1000), Flt(m, "_dispFreq", 0));
                    V4(f + 20, Flt(m, "_Shininess", 0.08f), Flt(m, "_Gloss", 0), Flt(m, "_oceanOpacity", 1), Flt(m, "_PlanetOpacity", 0));
                    V4(f + 24, Flt(m, "_falloffPower", 1), Flt(m, "_falloffExp", 1), Flt(m, "_fadeStart", 0), Flt(m, "_fadeEnd", 1));
                    V4(f + 28, Flt(m, "_globalDensity", 0), Shader.GetGlobalFloat("_heightDensityAtViewer"), 0, 0);
                    V4(f + 32, Shader.GetGlobalVector("_sunLightDirection"));
                    Ids(f + 36, Tx(m, "_WaterTex", ref ok), Tx(m, "_WaterTex1", ref ok), Tx(m, "_fogColorRamp", ref ok), -1);
                    break;
                case FamScaled:
                {
                    if (m.HasProperty(P("_MainTex")) && m.GetTexture(P("_MainTex")) is Texture mt) hiResTex.Add(mt);
                    Col(f, m, "_Color", Color.white); Col(f + 4, m, "_SpecColor", Color.gray);
                    V4(f + 8, Flt(m, "_Shininess", 0.078125f), Flt(m, "_Opacity", 1), Flt(m, "_rimPower", 1), Flt(m, "_rimBlend", 0));
                    ST(f + 12, m, "_MainTex"); ST(f + 16, m, "_BumpMap"); ST(f + 20, m, "_ResourceMap");
                    Vector4 ld = Vector4.zero;
                    if (rec.r.HasPropertyBlock()) { rec.r.GetPropertyBlock(mpb); ld = mpb.GetVector("_localLightDirection"); }
                    if (ld == Vector4.zero && Sun.Instance != null) ld = rec.r.transform.InverseTransformDirection(Sun.Instance.transform.forward).normalized;
                    V4(f + 24, ld);
                    int main = Tx(m, "_MainTex", ref ok);
                    Ids(f + 28, main < 0 ? TexWhite : main, Tx(m, "_BumpMap", ref ok), Tx(m, "_rimColorRamp", ref ok), Tx(m, "_ResourceMap", ref ok));
                    f[32] = rec.flags;
                    break;
                }
                case FamSun:
                    Col(f, m, "_EmitColor0", Color.white); Col(f + 4, m, "_EmitColor1", Color.white); Col(f + 8, m, "_SunspotColor", Color.black); Col(f + 12, m, "_RimColor", Color.white);
                    V4(f + 16, Flt(m, "_Offset0", 0), Flt(m, "_Offset1", 0), Flt(m, "_Offset2", 0), Flt(m, "_Offset3", 0));
                    V4(f + 20, Flt(m, "_SunspotPower", 1), Flt(m, "_RimPower", 1), Flt(m, "_RimBlend", 0), 0);
                    ST(f + 24, m, "_NoiseMap"); ST(f + 28, m, "_SunspotTex");
                    Ids(f + 32, Tx(m, "_NoiseMap", ref ok), Tx(m, "_RampMap", ref ok), Tx(m, "_SunspotTex", ref ok), -1);
                    break;
                case FamAtmo:
                {
                    var a = rec.c as AtmosphereFromGround;
                    if (a == null) { ok = false; break; }
                    Vector3 o = a.transform.position;
                    V4(f, o.x, o.y, o.z, 1); V4(f + 4, a.cameraPos.x, a.cameraPos.y, a.cameraPos.z, 0);
                    V4(f + 8, a.sunLightDirection.x, a.sunLightDirection.y, a.sunLightDirection.z, 0);
                    V4(f + 12, a.invWaveLength.r, a.invWaveLength.g, a.invWaveLength.b, a.invWaveLength.a);
                    V4(f + 16, a.cameraHeight, a.innerRadius, a.KrESun, a.KmESun);
                    V4(f + 20, a.Kr4PI, a.Km4PI, a.scale, a.scaleDepth);
                    V4(f + 24, a.scaleOverScaleDepth, a.samples, a.g, a.g2);
                    V4(f + 28, a.exposure, a.camHeightUnderwater, a.underwaterOpacityAltBase, a.underwaterOpacityAltMult);
                    V4(f + 32, a.underwaterColorStart.r, a.underwaterColorStart.g, a.underwaterColorStart.b, a.underwaterColorStart.a); V4(f + 36, a.underwaterColorEnd.r, a.underwaterColorEnd.g, a.underwaterColorEnd.b, a.underwaterColorEnd.a); f[40] = a.lightDot;
                    AtmoParams(a.planet, f);
                    break;
                }
                case FamGalaxy:
                {
                    Color c = m.HasProperty(P("_Color")) ? m.GetColor(P("_Color")) : Color.white;
                    if (rec.r.HasPropertyBlock()) { rec.r.GetPropertyBlock(mpb); if (!mpb.isEmpty) c = mpb.GetColor("_Color"); }
                    V4(f, c.r, c.g, c.b, c.a); ST(f + 4, m, "_MainTex");
                    Ids(f + 8, Tx(m, "_MainTex", ref ok), 0, 0, 0);
                    break;
                }
            }
            rec.ready = ok;
        }

        // Enhanced atmosphere (8d), metres: 44 ground / 45 top radius, 46 / 47 Rayleigh / Mie scale height, 48 / 49 Rayleigh / Mie density (x Earth sea level),
        // 50 valid, 51 Mie g, 52..54 planet centre (local world). Scale heights from the pressure curve (1/e), cached per body; KSPVK_ATMO overrides.
        static readonly Dictionary<string, float> scaleH = new Dictionary<string, float>();
        static void AtmoParams(CelestialBody b, float* f)
        {
            if (b == null || !b.atmosphere || b.atmosphereDepth <= 0) return;
            var o = Compat.Atmo(b.bodyName);
            double p0 = b.GetPressure(0);
            if (!scaleH.TryGetValue(b.bodyName, out float h))
            {
                double lo = 0, hi = b.atmosphereDepth;
                for (int i = 0; i < 40 && p0 > 0; i++) { double mid = (lo + hi) * 0.5; if (b.GetPressure(mid) > p0 / Math.E) lo = mid; else hi = mid; }
                scaleH[b.bodyName] = h = p0 > 0 ? (float)Math.Max(lo, 500) : (float)b.atmosphereDepth / 10;
            }
            float dens = Mathf.Clamp((float)(p0 / 101.325), 0.01f, 6) * o.density;
            f[44] = (float)b.Radius; f[45] = (float)(b.Radius + b.atmosphereDepth);
            f[46] = o.scaleHeight > 0 ? o.scaleHeight : h; f[47] = o.mieScaleHeight > 0 ? o.mieScaleHeight : f[46] * 0.15f;
            f[48] = dens; f[49] = dens * o.mie; f[50] = 1; f[51] = o.mieG;
            Vector3 c = b.position; f[52] = c.x; f[53] = c.y; f[54] = c.z;
        }

        // PQS material -> body (detail textures per body): the sphere whose surface material it is, else the current main body
        readonly Dictionary<int, string> pqsBody = new Dictionary<int, string>();
        string PqsBody(Material m)
        {
            int k = m.GetInstanceID();
            if (pqsBody.TryGetValue(k, out string n)) return n;
            if (FlightGlobals.Bodies != null)
                foreach (var b in FlightGlobals.Bodies)
                {
                    var p = b != null ? b.pqsController : null;
                    if (p != null && (ReferenceEquals(p.surfaceMaterial, m) || ReferenceEquals(p.lowQualitySurfaceMaterial, m) || ReferenceEquals(p.mediumQualitySurfaceMaterial, m) ||
                                      ReferenceEquals(p.highQualitySurfaceMaterial, m) || ReferenceEquals(p.ultraQualitySurfaceMaterial, m)))
                        return pqsBody[k] = b.bodyName;
                }
            return FlightGlobals.currentMainBody != null ? FlightGlobals.currentMainBody.bodyName : "";
        }
        int TxT(Texture t, ref bool ready)
        {
            int id = t != null ? TexId(t, -1) : -1;
            if (id < 0) return -1;
            if (texState[id] == RsReady) return id;
            if (texState[id] != RsFailed) ready = false;
            return -1;
        }

        void WritePqs(MatRec rec, Material m, float* f, ref bool ok)
        {
            int kind = rec.flags, nb = 0;
            {   // Enhanced terrain (8e): optional detail albedo / normal per band from KSPVK_TERRAIN (88..91, 92..95)
                var dt = Compat.Terrain(PqsBody(m));
                for (int i = 0; i < 4; i++) { ((int*)f)[88 + i] = dt != null ? TxT(dt[i], ref ok) : -1; ((int*)f)[92 + i] = dt != null ? TxT(dt[4 + i], ref ok) : -1; }
            }
            if (kind < 2)
            {
                bool opt = kind == 1;
                var kw = m.shaderKeywords;
                bool lowOff = Array.IndexOf(kw, "LOW_TEXTURING_OFF") >= 0, highOff = Array.IndexOf(kw, "HIGH_TEXTURING_OFF") >= 0;
                bool steepOff = Array.IndexOf(kw, "STEEP_TEXTURING_OFF") >= 0, fog = Array.IndexOf(kw, "AERIAL_OFF") < 0;
                float ls = Flt(m, "_lowStart", 0), le = Flt(m, "_lowEnd", 0), hs = Flt(m, "_highStart", 1), he = Flt(m, "_highEnd", 1);
                if (!lowOff) MainBand(m, f, nb++, "_low", -Inf, -Inf, ls, le, opt, ref ok);
                MainBand(m, f, nb++, "_mid", lowOff ? -Inf : ls, lowOff ? -Inf : le, highOff ? Inf : hs, highOff ? Inf : he, opt, ref ok);
                if (!highOff) MainBand(m, f, nb++, "_high", hs, he, Inf, Inf, opt, ref ok);
                V4(f + 48, Flt(m, "_steepNearTiling", 1), Flt(m, "_steepTiling", 1), 0, 0);
                Ids(f + 52, steepOff ? -1 : Tx(m, "_steepTex", ref ok), steepOff ? -1 : Tx(m, "_steepBumpMap", ref ok), 0, 0);
                V4(f + 60, Flt(m, "_powerNear", 1), Flt(m, "_powerFar", 1), Flt(m, "_steepPower", 1), Flt(m, "_PlanetOpacity", 0));
                V4(f + 64, Flt(m, "_contrast", 1), Flt(m, "_saturation", 1), fog ? 1 : 0, nb);
                f[75] = opt ? 0.75f : 0.5f;
                if (opt) { ((int*)f)[84] = Tx(m, "_midBumpMap", ref ok); f[85] = Flt(m, "_midBumpNearTiling", 1); } else ((int*)f)[84] = -1;
                f[86] = Flt(m, "_oceanFogDistance", 1000); f[87] = Shader.GetGlobalFloat("_cameraAltitude");
            }
            else
            {
                float tt = Flt(m, "_texTiling", 1);
                SpBand(m, f, nb++, "_deep", -Inf, -Inf, Flt(m, "_deepStart", 0), Flt(m, "_deepEnd", 0), tt, ref ok);
                SpBand(m, f, nb++, "_main", Flt(m, "_mainLoStart", 0), Flt(m, "_mainLoEnd", 0), Flt(m, "_mainHiStart", 1), Flt(m, "_mainHiEnd", 1), tt, ref ok);
                SpBand(m, f, nb++, "_high", Flt(m, "_hiLoStart", 1), Flt(m, "_hiLoEnd", 1), Flt(m, "_hiHiStart", 1), Flt(m, "_hiHiEnd", 1), tt, ref ok);
                SpBand(m, f, nb++, "_snow", Flt(m, "_snowStart", 1), Flt(m, "_snowEnd", 1), Inf, Inf, tt, ref ok);
                float st = Flt(m, "_steepTiling", 1);
                V4(f + 48, st, st, 1, 0);   // z = 1: single steep blend (no near/far split)
                Ids(f + 52, Tx(m, "_steepTex", ref ok), -1, 0, 0);
                V4(f + 60, Flt(m, "_texPower", 1), Flt(m, "_multiPower", 1), Flt(m, "_steepPower", 1), Flt(m, "_PlanetOpacity", 0));
                V4(f + 64, Flt(m, "_contrast", 1), Flt(m, "_saturation", 1), kind == 3 ? 1 : 0, nb);
                f[75] = 0.5f;
                ((int*)f)[84] = -1; f[86] = f[87] = 1;
            }
            V4(f + 56, Flt(m, "_groundTexStart", 0), Flt(m, "_groundTexEnd", 1), Flt(m, "_steepTexStart", 0), Flt(m, "_steepTexEnd", 1));
            Col(f + 68, m, "_tintColor", new Color(1, 1, 1, 0));
            f[72] = Flt(m, "_globalDensity", 0); f[73] = Shader.GetGlobalFloat("_heightDensityAtViewer");
            ((int*)f)[74] = Tx(m, "_fogColorRamp", ref ok);
            V4(f + 76, Shader.GetGlobalVector("_sunLightDirection"));
            V4(f + 80, Shader.GetGlobalVector("_floatingOriginOffset"));
            if ((FamMask >> FamScaled & 1) == 0) f[63] = 0;   // the scaled planet is Unity's, so there is nothing to fade into: opaque terrain
        }

        // band i: v[i] = (lo, hi) ranges, v[4+i] tilings (near, far, bumpNear, bumpFar), v[8+i] ids (near, far, bumpNear, bumpFar)
        void MainBand(Material m, float* f, int i, string p, float l0, float l1, float h0, float h1, bool opt, ref bool ok)
        {
            V4(f + i * 4, l0, l1, h0, h1);
            V4(f + 16 + i * 4, Flt(m, p + "NearTiling", 1), Flt(m, p + "MultiFactor", 1), Flt(m, p + "BumpNearTiling", 1), Flt(m, p + "BumpFarTiling", 1));
            int t = Tx(m, p + "Tex", ref ok), bm = opt ? -1 : Tx(m, p + "BumpMap", ref ok);
            Ids(f + 32 + i * 4, t, t, bm, bm);
        }

        void SpBand(Material m, float* f, int i, string p, float l0, float l1, float h0, float h1, float tiling, ref bool ok)
        {
            V4(f + i * 4, l0, l1, h0, h1);
            V4(f + 16 + i * 4, tiling, Flt(m, p + "MultiFactor", 1), 0, 0);
            Ids(f + 32 + i * 4, Tx(m, p + "Tex", ref ok), Tx(m, p + "MultiTex", ref ok), -1, -1);
        }

        // ---------- restore ----------
        void RestoreWorld()
        {
            foreach (var qd in quadList)
            {
                if (qd.hidden && qd.q != null && qd.q.meshRenderer != null) qd.q.meshRenderer.forceRenderingOff = false;
                if (releaseMesh != null) { if (qd.id >= 0) releaseMesh(qd.id); if (qd.next >= 0) releaseMesh(qd.next); }
                qd.dead = true;
            }
            foreach (var w in wobjs) if (w.hidden && w.r != null) w.r.forceRenderingOff = false;
            quadList.Clear(); quads.Clear(); dirty.Clear(); hooked.Clear(); wobjs.Clear(); wByR.Clear(); worldSlots.Clear(); worldSlotList.Clear();
            WorldHidden = WorldDraws = QuadPending = 0;
        }

        /// Diagnostics: the packed world material records.
        public string WorldMatDump()
        {
            var sb = new System.Text.StringBuilder();
            foreach (int s in worldSlotList)
            {
                var rec = matRecs[s]; float* f = (float*)mats + s * MatF;
                sb.Append($"slot {s} fam {rec.fam} kind {rec.flags} ready {rec.ready} {(rec.r != null ? rec.r.name : rec.m != null ? rec.m.name : "?")}:");
                if (rec.fam == FamPqs) sb.Append(" [" + (rec.r != null ? rec.r.sharedMaterial?.shader?.name : rec.m != null ? rec.m.shader.name : "?") + "]");
                if (rec.fam == FamScaled && rec.r != null && rec.r.sharedMaterial != null)
                    foreach (var tn in new[] { "_MainTex", "_BumpMap", "_ResourceMap" })
                        if (rec.r.sharedMaterial.HasProperty(tn) && rec.r.sharedMaterial.GetTexture(tn) is Texture t)
                            sb.Append($" [{tn} {t.name} {t.width}x{t.height} {t.graphicsFormat} mips {t.mipmapCount} bias {t.mipMapBias} aniso {t.anisoLevel} filter {t.filterMode} readable {t.isReadable} id {TexId(t, -1)}]");
                for (int i = 0; i < (rec.fam == FamPqs ? MatF : 40); i++) sb.Append(' ').Append(f[i].ToString("G5"));
                sb.AppendLine();
            }
            return sb.ToString();
        }

        void ForgetWorldMeshes() { sharedSrc = null; resets++; }

        /// World objects still drawn by Unity, by reason (diagnostics).
        public void WorldResiduals(Dictionary<string, int> res, ref int residual)
        {
            if (Cam != null)
                foreach (var qd in quadList)
                {
                    var mr = qd.q != null ? qd.q.meshRenderer : null;
                    if (qd.hidden || mr == null || !mr.enabled || !qd.q.gameObject.activeInHierarchy) continue;
                    // a freshly built quad stays with Unity until its first upload lands (1-2 frames): transient, not a residual
                    string k = qd.slot >= 0 && (FamMask >> matRecs[qd.slot].fam & 1) == 0 ? "by design PQS family drawn by Unity (settings or mod)"
                             : qd.id < 0 && qd.next >= 0 ? "by design PQS quad (first upload in flight)"
                             : "RESIDUAL PQS quad (" + (qd.id < 0 ? "mesh not uploaded" : qd.slot < 0 ? "shader " + (mr.sharedMaterial ? mr.sharedMaterial.shader.name : "-") : "material textures pending") + ")";
                    res[k] = res.TryGetValue(k, out int c) ? c + 1 : 1;
                    if (k[0] == 'R') residual++;
                }
            foreach (var w in wobjs)
            {
                if (w.r == null || w.hidden || !w.r.enabled || !w.r.gameObject.activeInHierarchy) continue;
                bool off = w.slot >= 0 && (FamMask >> matRecs[w.slot].fam & 1) == 0;
                string k = off ? "by design world family drawn by Unity (settings or mod)" : "RESIDUAL world " + (w.reason ?? "pending upload") + " (" + w.r.name + ")";
                res[k] = res.TryGetValue(k, out int c) ? c + 1 : 1; if (!off) residual++;
            }
            if (Cam != null) foreach (var b in FlightGlobals.Bodies)   // no local view: quads are seen by no camera
                if (b.pqsController != null)
                    foreach (var pqs in b.pqsController.GetComponentsInChildren<PQS>(false))
                        if (pqs.isActive && pqs.quads != null) foreach (var q in pqs.quads) CountUnknown(q, res, ref residual);
        }

        void CountUnknown(PQ q, Dictionary<string, int> res, ref int residual)
        {
            if (q == null) return;
            if (q.meshRenderer != null && q.meshRenderer.enabled && q.gameObject.activeInHierarchy && q.mesh != null && q.mesh.vertexCount > 0 && !q.meshRenderer.forceRenderingOff && !quads.ContainsKey(q.GetInstanceID()))
            {
                string k = "RESIDUAL PQS quad not tracked (" + (q.meshRenderer.sharedMaterial?.shader?.name ?? "null") + ")";
                res[k] = res.TryGetValue(k, out int c) ? c + 1 : 1; residual++;
            }
            if (q.subNodes != null) foreach (var s in q.subNodes) CountUnknown(s, res, ref residual);
        }
    }

    static class DictExt
    {
        public static K FirstKey<K, V>(this Dictionary<K, V> d, V v) where V : class { foreach (var kv in d) if (kv.Value == v) return kv.Key; return default(K); }
    }
}
