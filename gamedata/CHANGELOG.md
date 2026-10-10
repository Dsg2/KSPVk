# Changelog

## Unreleased (Phase 8a–8f, render scale)
- **Render scale** (all modes; Settings > Resolution, default 100 %): renders the scene at 50–100 % of the screen and upscales it, with TAA upsampling in Enhanced and a sharp bicubic filter otherwise. The UI and Unity's particles stay at full resolution. At 67 % KSPVk's GPU time drops by 15–50 % (Kerbin views on Iris Xe).
- **Enhanced mode** (opt-in; Off keeps the stock look and cost):
  - HDR target with auto-exposure, bloom, and Neutral / AgX / ACES tonemapping.
  - TAA with per-object motion vectors.
  - G-buffer normals, roughness/metal and a Hi-Z pyramid, for later Enhanced features.
  - Tiers Off / Medium / High, per-feature toggles, live switching, per-pass GPU times in the settings window.
  - Physically based parts (GGX + Burley in linear light). Roughness and metalness are estimated from stock materials; per-part overrides via `KSPVK_PBR` nodes (see `pbr.cfg`).
  - Image-based lighting from a 128² environment cube that KSPVk renders itself (sky, planets, terrain, KSC), with prefiltered reflections, SH ambient and planetshine. Sliders for IBL intensity and the stock ambient kept under it.
  - Soft sun shadows (PCSS), contact shadows, GTAO, screen-space GI (High) and shadows for the 4 strongest spot lights (High). Sliders for sun softness, AO and GI strength.
  - Physically based atmosphere (Hillaire LUTs + aerial perspective) for the nearest body: sky, haze on terrain, parts and water, sunlight through the atmosphere, planet shadow and eclipses. The sky fades out at the atmosphere top. Per-body overrides via `KSPVK_ATMO` nodes; a sky brightness slider.
  - Ocean with FFT waves (Gerstner on Medium), Fresnel reflections, sun glint, depth-based water colour and shoreline / crest foam.
  - Screen-space reflections (High) on water and glossy parts, replacing the environment-cube reflection wherever the reflected point is on screen.
  - Terrain detail: height-blended bands, triplanar slopes, detail normals, macro variation, parallax occlusion close up (High). Optional CC0 detail textures via `KSPVK_TERRAIN` nodes (see `atmo_terrain.cfg`).
- **Fixes (all modes):**
  - Terrain seen from above about 90 km (KSP's Fast Blend PQS shaders) was left to Unity and hidden, so only the blurry scaled planet showed. It is now drawn.
  - Textures follow KSP's forced anisotropic filtering, so planets are sharp at grazing angles.
  - Scaled-space planet surface maps may use twice the texture size cap.
  - Map view and tracking station: planets and moons were drawn unlit (KSP turns off the local sun there); they now take the scaled-space sun. In Enhanced, the main body's atmosphere no longer draws as a black disc and ring in map view.
- **Enable / disable key** can be changed in the settings window (Keys; default Ctrl+Alt+F11, Backspace for none). Saved as `toggleKey`.

## 0.7.0-beta (first public beta)
- **Crash safety:**
  - KSPVk turns itself off after 3 errors in a session.
  - KSPVk starts disabled after 2 unclean exits in a row.
  - If Vulkan fails, KSP falls back to its stock renderer.
- **Logging:** `PluginData/KSPVk.log` with a system and mod header, plus `KSPVk.prev.log`.
- **Mod compatibility:**
  - `KSPVK_COMPAT` and `KSPVK_EXCLUDE` config nodes.
  - Shipped `compat.cfg`.
  - `KSPVk.API` for other mods.
- **Fixes:**
  - Scaled-space planets (and other meshes from KSP's asset bundles) were blurred with no bump relief. Their legacy vertex layout (tangent after texcoords) is now detected.
  - With scaled planets handed to Unity (e.g. EVE), terrain no longer turns dark and see-through during the fade from orbit.
- **Settings:**
  - Texture size cap (default 2048) to save memory.
  - Per-scene on/off (SPH and Tracking Station off by default).
  - Installed-mod notes in the window.

## 0.6 (internal)
- Transform job, culling and upload work: main-thread cost cut in the VAB, KSC and flight.

## 0.1–0.5 (internal)
- Device interop, part renderer, shadows, world families (terrain, ocean, scaled planets, sun, atmosphere, galaxy), interiors and editor scenes.
