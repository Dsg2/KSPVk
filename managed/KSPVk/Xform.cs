using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;
using Unity.Jobs;
using UnityEngine;
using UnityEngine.Jobs;

namespace KSPVk
{
    /// Phase 6: every transform the draw loops need (hidden objects, bones with their bind poses, root bones, PQS quads) is read by one
    /// persistent IJobParallelForTransform per frame instead of one localToWorldMatrix icall each on the main thread.
    /// It is scheduled right after UpdateAllRenderers: earlier, Unity's renderer update waits for it (+1.3 ms in the VAB).
    public unsafe partial class Scene
    {
        struct R34 { public Vector4 a, b, c; }   // the 3 rows of a 3x4 matrix, as the draw records store it

        struct XJob : IJobParallelForTransform
        {
            [NativeDisableUnsafePtrRestriction] public Matrix4x4* bind;   // m33 == 0: none; bind poses are affine
            [NativeDisableUnsafePtrRestriction] public float* o;
            [NativeDisableUnsafePtrRestriction] public byte* on;   // 0: skip (bones of a skeleton that was off-screen last frame)
            public void Execute(int i, TransformAccess t)
            {
                if (on[i] == 0) return;
                Matrix4x4 m = t.localToWorldMatrix; Matrix4x4* b = bind + i; float* r = o + i * 12;
                if (b->m33 == 0)
                {
                    r[0] = m.m00; r[1] = m.m01; r[2] = m.m02; r[3] = m.m03; r[4] = m.m10; r[5] = m.m11; r[6] = m.m12; r[7] = m.m13;
                    r[8] = m.m20; r[9] = m.m21; r[10] = m.m22; r[11] = m.m23;
                    return;
                }
                float b00 = b->m00, b01 = b->m01, b02 = b->m02, b03 = b->m03, b10 = b->m10, b11 = b->m11, b12 = b->m12, b13 = b->m13, b20 = b->m20, b21 = b->m21, b22 = b->m22, b23 = b->m23;
                r[0] = m.m00 * b00 + m.m01 * b10 + m.m02 * b20; r[1] = m.m00 * b01 + m.m01 * b11 + m.m02 * b21; r[2] = m.m00 * b02 + m.m01 * b12 + m.m02 * b22; r[3] = m.m00 * b03 + m.m01 * b13 + m.m02 * b23 + m.m03;
                r[4] = m.m10 * b00 + m.m11 * b10 + m.m12 * b20; r[5] = m.m10 * b01 + m.m11 * b11 + m.m12 * b21; r[6] = m.m10 * b02 + m.m11 * b12 + m.m12 * b22; r[7] = m.m10 * b03 + m.m11 * b13 + m.m12 * b23 + m.m13;
                r[8] = m.m20 * b00 + m.m21 * b10 + m.m22 * b20; r[9] = m.m20 * b01 + m.m21 * b11 + m.m22 * b21; r[10] = m.m20 * b02 + m.m21 * b12 + m.m22 * b22; r[11] = m.m20 * b03 + m.m21 * b13 + m.m22 * b23 + m.m23;
            }
        }

        // owner of slot i: an Entry (sub -1 its transform, -2 its root bone, >= 0 bone index) or a Quad
        TransformAccessArray xTaa;
        NativeArray<Matrix4x4> xBind; NativeArray<R34> xOut; NativeArray<byte> xOn;
        object[] xOwn = new object[0]; int[] xSub = new int[0]; bool[] xFresh = new bool[0];
        int xN; JobHandle xJob; bool xRun;
        R34* X;

        int XAdd(Transform t, object own, int sub, Matrix4x4 bind)
        {
            if (t == null) return -1;
            XComplete();
            if (!xTaa.isCreated) xTaa = new TransformAccessArray(1024);
            if (xN == xOwn.Length)
            {
                int cap = System.Math.Max(1024, xN * 2);
                var nb = new NativeArray<Matrix4x4>(cap, Allocator.Persistent); var no = new NativeArray<R34>(cap, Allocator.Persistent); var nn = new NativeArray<byte>(cap, Allocator.Persistent);
                if (xBind.IsCreated) { NativeArray<Matrix4x4>.Copy(xBind, nb, xN); NativeArray<R34>.Copy(xOut, no, xN); NativeArray<byte>.Copy(xOn, nn, xN); xBind.Dispose(); xOut.Dispose(); xOn.Dispose(); }
                xBind = nb; xOut = no; xOn = nn; X = (R34*)NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(xOut);
                System.Array.Resize(ref xOwn, cap); System.Array.Resize(ref xSub, cap); System.Array.Resize(ref xFresh, cap);
            }
            xTaa.Add(t); xBind[xN] = bind; xOn[xN] = 1; xOwn[xN] = own; xSub[xN] = sub; xFresh[xN] = false;
            return xN++;
        }

        void XRemove(int i)
        {
            if (i < 0 || i >= xN) return;
            XComplete();
            int l = --xN;
            xTaa.RemoveAtSwapBack(i);
            if (i != l)
            {
                xBind[i] = xBind[l]; xOut[i] = xOut[l]; xOn[i] = xOn[l]; xFresh[i] = xFresh[l]; xOwn[i] = xOwn[l]; xSub[i] = xSub[l];
                if (xOwn[i] is Quad q) q.xi = i;
                else { var e = (Entry)xOwn[i]; int s = xSub[i]; if (s == -1) e.xi = i; else if (s == -2) e.rbx = i; else e.bx[s] = i; }
            }
            xOwn[l] = null;
        }

        void XTrack(Entry e)
        {
            if (e.smr == null) { e.xi = XAdd(e.t, e, -1, default); return; }
            e.bx = new int[e.bones.Length]; e.bonesOn = true;
            for (int b = 0; b < e.bones.Length; b++) e.bx[b] = XAdd(e.bones[b], e, b, e.bind[b]);
            e.rb = e.smr.rootBone ? e.smr.rootBone : e.t;
            e.rbx = XAdd(e.rb, e, -2, default);
            e.lb = e.smr.localBounds;
        }

        /// Next frame's job computes this skeleton's bones only if it is on screen now.
        void XBonesOn(Entry e, bool vis)
        {
            if (e.bonesOn == vis || e.bx == null) return;
            e.bonesOn = vis;
            byte* on = (byte*)xOn.GetUnsafePtr();
            foreach (int k in e.bx) if (k >= 0) on[k] = (byte)(vis ? 1 : 0);
        }

        void XUntrack(Entry e)
        {
            XRemove(e.xi); e.xi = -1;
            XRemove(e.rbx); e.rbx = -1;
            if (e.bx != null) for (int b = 0; b < e.bx.Length; b++) { XRemove(e.bx[b]); e.bx[b] = -1; }
        }

        void XSchedule()
        {
            XComplete();
            if (xN > 0) { xJob = new XJob { bind = (Matrix4x4*)xBind.GetUnsafePtr(), o = (float*)xOut.GetUnsafePtr(), on = (byte*)xOn.GetUnsafePtr() }.Schedule(xTaa); xRun = true; }
        }

        void XComplete()
        {
            if (!xRun) return;
            long t0 = Tick();
            xJob.Complete(); xRun = false;
            Prof[6] += Tick() - t0;
            byte* on = (byte*)xOn.GetUnsafeReadOnlyPtr();
            for (int i = 0; i < xN; i++) xFresh[i] = on[i] != 0;
            X = (R34*)NativeArrayUnsafeUtility.GetUnsafeReadOnlyPtr(xOut);
        }

        void XReset()
        {
            XComplete();
            if (xTaa.isCreated) xTaa.Dispose();
            System.Array.Clear(xOwn, 0, xOwn.Length);
            xN = 0;
        }

        /// World matrix rows of slot i (this frame's job result), or read directly when the slot was added after the job ran.
        void XRows(int i, Transform t, float* o)
        {
            if (i >= 0 && xFresh[i]) *(R34*)o = X[i];
            else Rows(t ? t.localToWorldMatrix : Matrix4x4.zero, o);
        }

        static Matrix4x4 Mat(float* r)
        {
            var m = Matrix4x4.identity;
            m.SetRow(0, new Vector4(r[0], r[1], r[2], r[3])); m.SetRow(1, new Vector4(r[4], r[5], r[6], r[7])); m.SetRow(2, new Vector4(r[8], r[9], r[10], r[11]));
            return m;
        }
    }
}
