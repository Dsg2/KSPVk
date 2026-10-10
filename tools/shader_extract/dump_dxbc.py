"""Extract D3D11 (DXBC) subprograms of KSP shaders and disassemble them with d3dcompiler_47.

Dev-only tool: output stays local (never redistribute). Used to recover each KSP surface shader's
surf() logic so it can be re-implemented in HLSL for KSPVk.

usage: python dump_dxbc.py <KSP root> <out dir> [shader name ...]
"""
import ctypes, os, re, struct, sys, collections
import UnityPy
from UnityPy.helpers import CompressionHelper

KSP = sys.argv[1]
OUT = sys.argv[2]
NAMES = sys.argv[3:] or ["KSP/Bumped Specular", "KSP/Specular", "KSP/Diffuse", "KSP/Emissive/Bumped Specular", "KSP/Bumped",
                         "KSP/Emissive/Specular", "KSP/Bumped Specular (Mapped)", "KSP/Alpha/Cutoff", "KSP/UnlitColor",
                         "KSP/Alpha/Translucent Additive", "KSP/Alpha/Unlit Transparent", "KSP/Alpha/Translucent"]
D = os.path.join(KSP, "KSP_x64_Data")
FILES = ["resources.assets", "globalgamemanagers.assets"] + sorted(f for f in os.listdir(D) if f.startswith("sharedassets") and f.endswith(".assets"))
PLAT_D3D11 = 4
GPU_TYPE = {9: "vs_4_0_level_9", 11: "vs_4_0", 12: "vs_5_0", 15: "vs_5_0", 16: "ps_4_0", 17: "ps_5_0", 18: "gs_5_0"}

_d3dc = ctypes.WinDLL("d3dcompiler_47.dll")
_d3dc.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]

def disassemble(dxbc: bytes) -> str:
    blob = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(dxbc, len(dxbc))
    if _d3dc.D3DDisassemble(buf, len(dxbc), 0, None, ctypes.byref(blob)) != 0:
        return "<disassembly failed>"
    vt = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(blob)
    text = ctypes.string_at(ptr, size).decode("latin1")
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(blob)
    return text

def subprogram_dxbc(blob: bytes, index: int) -> bytes:
    """Blob (Unity 2019.3+) = int count, (offset, len, segment)*count, then entries; each entry embeds one DXBC container."""
    off, ln, _ = struct.unpack_from("<iii", blob, 4 + 12 * index)
    entry = blob[off:off + ln]
    p = entry.find(b"DXBC")
    if p < 0:
        return b""
    size = struct.unpack_from("<I", entry, p + 24)[0]
    return entry[p:p + size]

def dxbc_stage(dxbc: bytes) -> str:
    """Program type from the SHDR/SHEX version token (upper 16 bits: 0 PS, 1 VS, 2 GS)."""
    for k in range(struct.unpack_from("<I", dxbc, 28)[0]):
        co = struct.unpack_from("<I", dxbc, 32 + 4 * k)[0]
        if dxbc[co:co + 4] in (b"SHDR", b"SHEX"):
            return {0: "ps", 1: "vs", 2: "gs"}.get(struct.unpack_from("<I", dxbc, co + 8)[0] >> 16, "?")
    return "?"

BLEND = "Zero One DstColor SrcColor OneMinusDstColor SrcAlpha OneMinusSrcColor DstAlpha OneMinusDstAlpha SrcAlphaSaturate OneMinusSrcAlpha".split()
def state(st):
    v = lambda x: st[x]["val"] if isinstance(st.get(x), dict) else st.get(x)
    nm = lambda x: st[x]["name"] if isinstance(st.get(x), dict) and st[x]["name"] != "<noninit>" else None
    b = st["rtBlend0"]
    bf = lambda k: b[k]["name"] if b[k]["name"] != "<noninit>" else BLEND[int(b[k]["val"])]
    tags = dict(st["m_Tags"]["tags"]) if st.get("m_Tags") else {}
    return (f"// state: blend {bf('srcBlend')} {bf('destBlend')} op={b['blendOp']['val']:g} mask={b['colMask']['val']:g} | ztest={nm('zTest') or v('zTest')} "
            f"zwrite={nm('zWrite') or v('zWrite')} cull={nm('culling') or v('culling')} offset={v('offsetFactor')}/{v('offsetUnits')} | tags {tags}" + chr(10))

def layout(sp, nm):
    """Constant buffer members (cbN[reg].comp = name), cbuffer/texture bindings: Unity strips RDEF from the DXBC."""
    n = lambda i: nm.get(i, str(i))
    out = []
    bind = {n(b["m_NameIndex"]): b["m_Index"] for b in sp.get("m_ConstantBufferBindings", [])}
    for cb in sp.get("m_ConstantBuffers", []):
        cn = n(cb["m_NameIndex"])
        out.append(f"// cbuffer {cn} -> cb{bind.get(cn, '?')} ({cb.get('m_Size', '?')} B)")
        ps = [(v["m_Index"], n(v["m_NameIndex"]), f"float{v['m_Dim']}" + (f"[{v['m_ArraySize']}]" if v["m_ArraySize"] else "")) for v in cb["m_VectorParams"]]
        ps += [(v["m_Index"], n(v["m_NameIndex"]), f"float{v['m_RowCount']}x4" + (f"[{v['m_ArraySize']}]" if v["m_ArraySize"] else "")) for v in cb["m_MatrixParams"]]
        for off, pn, ty in sorted(ps):
            out.append(f"//   cb{bind.get(cn, '?')}[{off // 16}].{'xyzw'[(off % 16) // 4]}  {ty:12s} {pn}")
    for t in sp.get("m_TextureParams", []):
        out.append(f"// texture t{t['m_Index']} s{t['m_SamplerIndex']} {n(t['m_NameIndex'])} dim={t.get('m_Dim')}")
    for b in sp.get("m_BufferParams", []):
        out.append(f"// buffer t{b['m_Index']} {n(b['m_NameIndex'])}")
    return "".join(x + "\n" for x in out)

def main():
    os.makedirs(OUT, exist_ok=True)
    found, index = {}, []
    for f in FILES:
        env = UnityPy.load(os.path.join(D, f))
        for o in env.objects:
            if o.type.name != "Shader":
                continue
            tt = o.read_typetree()
            name = tt["m_ParsedForm"]["m_Name"]
            if name in NAMES and name not in found:
                found[name] = (f, tt)
    for name in NAMES:
        if name not in found:
            print("missing:", name); continue
        f, tt = found[name]
        pi = tt["platforms"].index(PLAT_D3D11)
        raw = bytes(tt["compressedBlob"])
        o, cl, dl = tt["offsets"][pi][0], tt["compressedLengths"][pi][0], tt["decompressedLengths"][pi][0]
        blob = CompressionHelper.decompress_lz4(raw[o:o + cl], dl)
        kw = {i: n for n, i in tt["m_ParsedForm"]["m_KeywordNames"]} if "m_KeywordNames" in tt["m_ParsedForm"] else {}
        names_idx = {}
        sdir = os.path.join(OUT, re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_"))
        os.makedirs(sdir, exist_ok=True)
        stats = collections.Counter()
        for si, sub in enumerate(tt["m_ParsedForm"]["m_SubShaders"]):
            for pidx, p in enumerate(sub["m_Passes"]):
                names_idx = {i: n for n, i in p["m_NameIndices"]}
                lightmode = dict(p["m_State"]["m_Tags"]["tags"]).get("LightMode", "") if p["m_State"].get("m_Tags") else ""
                pname = p["m_State"]["m_Name"] or lightmode or f"pass{pidx}"
                for stage, key in (("vs", "progVertex"), ("ps", "progFragment")):
                    subs = p[key]["m_SubPrograms"]
                    stats[f"{pname}.{stage}"] += len(subs)
                    # pick the leanest DIRECTIONAL variant (+ fewest extra keywords) as the reference program
                    def kws(s): return sorted(names_idx.get(i, str(i)) for i in s["m_GlobalKeywordIndices"] + s.get("m_LocalKeywordIndices", []))
                    cands = [s for s in subs if s["m_GpuProgramType"] in GPU_TYPE and s["m_ShaderHardwareTier"] in (0, 1, 3)]
                    want = [k for k in os.environ.get("DXBC_KW", "").split(",") if k]   # optional: required keywords
                    cands = [s for s in cands if all(k in kws(s) for k in want)] or ([] if want else cands)
                    if not cands:
                        continue
                    pick = min(cands, key=lambda s: (("DIRECTIONAL" not in kws(s)) and pname == "FORWARD", len(kws(s))))
                    dxbc = subprogram_dxbc(blob, pick["m_BlobIndex"])
                    if dxbc and dxbc_stage(dxbc) != stage:
                        stats["stage_mismatch"] += 1
                    fn = os.path.join(sdir, f"s{si}_p{pidx}_{pname}_{stage}.asm")
                    with open(fn, "w") as fo:
                        fo.write(f"// {name} | subshader {si} pass {pidx} '{pname}' {stage} | keywords: {' '.join(kws(pick)) or '<none>'}\n")
                        fo.write(f"// variants in pass/stage: {len(subs)} | blob {pick['m_BlobIndex']} | dxbc {len(dxbc)} bytes\n")
                        fo.write(state(p["m_State"]))
                        fo.write(layout(pick, names_idx))
                        fo.write(disassemble(dxbc) if dxbc else "<no DXBC found>")
        n_inst = sum(len(re.findall(r"^\s*(?!//)[a-z_]+[\w.]*\s", open(os.path.join(sdir, x)).read(), re.M)) for x in os.listdir(sdir))
        index.append(f"{name:40s} {f:24s} passes/variants: " + ", ".join(f"{k}={v}" for k, v in stats.items()) + f" | asm lines {n_inst}")
        print(index[-1])
    with open(os.path.join(OUT, "INDEX.txt"), "w") as fo:
        fo.write("\n".join(index) + "\n")

main()
