#!/usr/bin/env bash
# Packs the release zip into _scratch/release/ from the deployed build. Never includes the probe, bench output, logs, settings or session state.
set -e
cd "$(dirname "$0")"
G="$(cd .. && pwd)/GameData/KSPVk"
VER=$(grep -o 'Version = "[^"]*"' managed/KSPVk/Log.cs | cut -d'"' -f2)
mkdir -p _scratch/release
python - "$G" "_scratch/release/KSPVk-$VER.zip" <<'EOF'
import sys, zipfile, os
g, out = sys.argv[1], sys.argv[2]
files = ["Plugins/KSPVk.dll", "PluginData/KSPVkNative.dll", "compat.cfg", "pbr.cfg", "README.md", "LICENSE.txt", "CHANGELOG.md"]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for f in files: z.write(os.path.join(g, f), "GameData/KSPVk/" + f)
print(out, os.path.getsize(out) // 1024, "KB")
EOF
