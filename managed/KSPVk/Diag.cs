using System.Diagnostics;
using UnityEngine;

namespace KSPVk
{
    public unsafe partial class Scene
    {
        /// Diagnostics: cost of each per-object Unity call class the extractor makes per frame (ms for one full pass).
        public string MicroProf()
        {
            var sw = new Stopwatch();
            double Ms(System.Action a) { sw.Restart(); a(); return sw.Elapsed.TotalMilliseconds; }
            int ne = 0, nsk = 0, nq = 0, nb = 0; bool sink = false; Matrix4x4 ms = default; Material mm = null;
            foreach (var e in entries) if (e.hidden && !e.iva && e.r) { ne++; if (e.smr) { nsk++; nb += e.bones.Length; } }
            foreach (var q in quadList) if (q.q) nq++;
            double flags = Ms(() => { foreach (var e in entries) if (e.hidden && e.r) sink ^= e.r.enabled && e.r.gameObject.activeInHierarchy; });
            double chg = Ms(() => { foreach (var e in entries) if (e.hidden && e.r && !e.smr) sink ^= e.t.hasChanged; });
            double ltw = Ms(() => { foreach (var e in entries) if (e.hidden && e.r && !e.smr) ms = e.t.localToWorldMatrix; });
            double skin = Ms(() => { foreach (var e in entries) if (e.hidden && e.smr) { ms.m00 += e.smr.bounds.center.x; foreach (var b in e.bones) if (b) ms = b.localToWorldMatrix; } });
            double qmat = Ms(() => { foreach (var q in quadList) if (q.q) mm = q.q.meshRenderer.sharedMaterial; });
            double qflag = Ms(() => { foreach (var q in quadList) if (q.q) sink ^= q.q.meshRenderer.enabled && q.q.gameObject.activeInHierarchy; });
            double qltw = Ms(() => { foreach (var q in quadList) if (q.q) { sink ^= q.q.transform.hasChanged; ms = q.q.meshRenderer.localToWorldMatrix; } });
            double mpbs = Ms(() => { for (int i = 0; i < System.Math.Min(MpbPerFrame, entries.Count); i++) ReadMpb(entries[i]); });
            double matw = Ms(() => { for (int i = 0; i < System.Math.Min(MatPerFrame, matRecs.Count); i++) WriteMat(i); });
            double worst = RescanMaxMs; RescanMaxMs = 0;
            int qmis = 0;
            foreach (var q in quadList) if (q.q && q.q.meshRenderer && QuadOn(q.q) != (q.q.meshRenderer.enabled && q.q.gameObject.activeInHierarchy)) qmis++;
            return $"quad visibility mismatches {qmis} | worst rescan slice {worst:F2} | " + $"objects {ne} (skinned {nsk}, bones {nb}), quads {nq}, mats {matRecs.Count} | enabled+active {flags:F2} | hasChanged {chg:F2} | localToWorld {ltw:F2} | skin bounds+bones {skin:F2} | " +
                   $"quad material {qmat:F2} | quad enabled+active {qflag:F2} | quad hasChanged+ltw {qltw:F2} | {MpbPerFrame} MPBs {mpbs:F2} | {MatPerFrame} mats {matw:F2} ms {(sink ? "" : " ")}{(mm ? "" : " ")}";
        }
    }
}
