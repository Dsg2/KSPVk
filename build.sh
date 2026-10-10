#!/usr/bin/env bash
# Builds the native core + tools and deploys them into GameData/KSPVk. Logs go to _scratch/build/.
set -e
cd "$(dirname "$0")"
KSP="$(cd .. && pwd)"
mkdir -p _scratch/build "$KSP/GameData/KSPVk/Plugins" "$KSP/GameData/KSPVk/PluginData"
export DOTNET_ROOT="$USERPROFILE/.dotnet" PATH="$USERPROFILE/.dotnet:$PATH"

gcc_build() {  # name, output, sources..., then extra flags after --
  local name=$1 out=$2; shift 2
  MSYS_NO_PATHCONV=1 cmd /c "set PATH=C:\\msys64\\mingw64\\bin;%PATH% && C:\\msys64\\mingw64\\bin\\g++.exe -O2 -std=c++17 -Inative\\third_party\\Vulkan-Headers\\include -I_scratch\build $* -o $out 1>_scratch\\build\\$name.txt 2>&1 && echo BUILT $name || echo FAILED $name"
}

gcc_build interop_bench _scratch\\build\\interop_bench.exe native\\interop_bench\\interop_bench.cpp -ld3d11 -ldxgi -static
# SPIR-V for the scene renderer -> C headers included by scene.inl
GLSL=/c/msys64/mingw64/bin/glslangValidator
V="part:vert:part: part_skin:vert:part:-DSKIN shadow:vert:shadow: shadow_skin:vert:shadow:-DSKIN part:frag:part: shadow:frag:shadow: world:vert:world:"
for f in pqs ocean scaled sun atmo galaxy; do V="$V $f:frag:$f:"; done
# Enhanced variants (G-buffer extras, motion, jitter); the stock SPIR-V above stays unchanged
V="$V part_enh:vert:part:-DENH part_skin_enh:vert:part:-DSKIN,-DENH world_enh:vert:world:-DENH shadow_enh:vert:shadow:-DENH shadow_skin_enh:vert:shadow:-DSKIN,-DENH"
for f in part pqs ocean scaled sun atmo galaxy; do V="$V ${f}_enh:frag:$f:-DENH"; done
for p in hiz taa hist expo bdown bup gtao ssgi den resolve zcopy ssr; do V="$V post_$p:comp:post:-D${p^^}"; done
for p in trans multi skyv ap; do V="$V atmo_$p:comp:atmo:-D${p^^}"; done
for p in spec fft nrm mip; do V="$V ocean_$p:comp:ocean:-D${p^^}"; done
for p in down pref sh; do V="$V ibl_$p:comp:ibl:-D${p^^}"; done
V="$V tonemap:frag:tonemap: fs:vert:fs: upscale:frag:upscale:"
for v in $V; do
  IFS=: read -r n st src def <<< "$v"
  $GLSL -V --target-env vulkan1.3 ${def//,/ } --vn ${n}_$st -o _scratch/build/${n}_$st.h native/shaders/$src.$st > _scratch/build/glsl_${n}_$st.txt 2>&1 || { echo "FAILED shader ${n}_$st"; cat _scratch/build/glsl_${n}_$st.txt; exit 1; }
done
echo "BUILT shaders"
gcc_build KSPVkNative _scratch\\build\\KSPVkNative.dll native\\core\\core.cpp native\\gpu_timer\\gpu_timer.cpp -shared -ld3d11 -ldxgi -static
cp _scratch/build/KSPVkNative.dll "$KSP/GameData/KSPVk/PluginData/"
cp gamedata/* "$KSP/GameData/KSPVk/"

dotnet build managed/KSPVk/KSPVk.csproj -c Release -nologo -v q > _scratch/build/KSPVk.txt 2>&1 && echo "BUILT KSPVk" || { echo "FAILED KSPVk"; grep -E "error|warn" _scratch/build/KSPVk.txt | head -30; exit 1; }
dotnet build managed/KSPVk.Probe/KSPVk.Probe.csproj -c Release -nologo -v q > _scratch/build/KSPVk.Probe.txt 2>&1 && echo "BUILT KSPVk.Probe" || { echo "FAILED KSPVk.Probe"; grep -E "error|warn" _scratch/build/KSPVk.Probe.txt | head -30; exit 1; }
