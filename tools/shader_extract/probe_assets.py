"""Phase 0 asset probe: shader platforms, player/quality/graphics/tag settings."""
import sys, os, json, collections, UnityPy
KSP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Steam\steamapps\common\Kerbal Space Program"
D = os.path.join(KSP, "KSP_x64_Data")
FILES = ["globalgamemanagers", "globalgamemanagers.assets", "resources.assets"] + \
        sorted(f for f in os.listdir(D) if f.startswith("sharedassets") and f.endswith(".assets"))
# ShaderCompilerPlatform enum (Unity 2019.4)
PLAT = {0:"None",4:"D3D11",5:"GLES20",9:"GLES3x",14:"Metal",15:"OpenGLCore",16:"D3D11_9x",
        18:"Vulkan",19:"Switch",20:"XboxOneD3D12",21:"GameCoreXboxOne",22:"GameCoreScarlett",23:"PS5",24:"PS5NGGC"}
out = {"shaders": {}, "settings": {}}
for f in FILES:
    env = UnityPy.load(os.path.join(D, f))
    for obj in env.objects:
        t = obj.type.name
        try:
            if t == "Shader":
                tt = obj.read_typetree()
                pf = tt.get("m_ParsedForm", {})
                name = pf.get("m_Name") or tt.get("m_Name")
                subs = pf.get("m_SubShaders", [])
                passes = sum(len(s.get("m_Passes", [])) for s in subs)
                tags = [dict(s.get("m_Tags", {}).get("tags", [])) for s in subs]
                out["shaders"][name] = {"file": f, "platforms": [PLAT.get(p, p) for p in tt.get("platforms", [])],
                                         "subshaders": len(subs), "passes": passes, "tags": tags,
                                         "keywords": len(tt.get("m_ParsedForm", {}).get("m_KeywordNames", []))}
            elif t in ("PlayerSettings", "QualitySettings", "GraphicsSettings", "TagManager", "BuildSettings"):
                out["settings"][t] = obj.read_typetree()
        except Exception as e:
            out.setdefault("errors", []).append(f"{f}:{t}:{e}")
ps = out["settings"].get("PlayerSettings", {})
keep = {k: ps.get(k) for k in ("m_ActiveColorSpace", "m_StereoRenderingPath", "m_MTRendering", "gpuSkinning",
        "graphicsJobs", "graphicsJobMode", "m_GraphicsJobs", "useHDRDisplay", "bundleVersion", "productName") if k in ps}
keep["m_BuildTargetGraphicsAPIs"] = ps.get("m_BuildTargetGraphicsAPIs")
out["settings"]["PlayerSettings"] = keep
json.dump(out, open(sys.argv[2], "w"), indent=1, default=str)
plats = collections.Counter(tuple(v["platforms"]) for v in out["shaders"].values())
print("shaders:", len(out["shaders"])); print("platform sets:", dict(plats))
print("player:", json.dumps(keep, default=str)[:800])
