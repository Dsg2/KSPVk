# KSPVk 0.7.0-beta

A Vulkan scene renderer for Kerbal Space Program 1.12.5 (Windows, Direct3D 11).

*(Click on the images and zoom in to see full resolution)*
<img width="3846" height="752" alt="flight" src="https://github.com/user-attachments/assets/7bb8843c-5f4a-4135-b209-13dfb37731de" />

<img width="3846" height="752" alt="runway" src="https://github.com/user-attachments/assets/757f6481-ebd9-45ca-af26-1bb4317bb5c1" />

<img width="2564" height="752" alt="ksc" src="https://github.com/user-attachments/assets/75493478-ce94-456f-bd23-6ca24ca83db1" />

<img width="3846" height="752" alt="cockpit" src="https://github.com/user-attachments/assets/89da1021-c77f-4670-aed4-763afaa38502" />

<img width="2564" height="752" alt="mainmenu" src="https://github.com/user-attachments/assets/c6e238b6-3b10-42b8-a7af-3b50bccd621b" />

KSPVk takes over drawing the 3D world from Unity: parts, scenery, interiors, PQS terrain, ocean, scaled planets, sun, atmosphere and the galaxy skybox. It draws them in Vulkan and composites the result into KSP's cameras. Unity still runs the game and draws the UI, particles, transparent effects and anything KSPVk doesn't understand.

What you get is lower main-thread render cost, mainly in flight and in large craft scenes. The image is meant to match stock closely, not to improve on it.

## Requirements
- KSP 1.12.5, 64-bit, Windows, running on its default Direct3D 11 renderer. Do **not** use `-force-vulkan`, `-force-opengl` or `-force-d3d12`.
- A GPU and driver that support Vulkan 1.3 and the `VK_KHR_external_memory_win32` / `VK_KHR_external_semaphore_win32` interop. Recent Intel, AMD and NVIDIA drivers all qualify.
- About 200–450 MB of extra RAM or VRAM for KSPVk's copies of meshes and textures. On integrated GPUs this comes from system RAM.

## Install
Download the mod on the release page.
Extract and copy `KSPVk` into KSP's `GameData` folder. Nothing else is needed. KSPVk never edits KSP's own `settings.cfg` or your saves.

To uninstall, delete `GameData/KSPVk`.

## Using it
- **Settings window:** open it from the toolbar button (an orange "V") or with **Ctrl+Alt+F9**. Settings are saved to `GameData/KSPVk/PluginData/settings.cfg`.
- **Turn KSPVk on or off:** **O**. When it is off, KSP renders exactly as stock.
- **Scenes:** choose where KSPVk is used. By default it is on in flight, the Space Center, the VAB and menus. It is off in the SPH and the Tracking Station, which were measured slower with KSPVk on.
- **Families:** for each kind of object, you can choose whether Vulkan or Unity draws it.
- **Texture size cap** (default 2048): KSPVk's texture copies are limited to this size, which saves memory. Unity's own textures are not affected.
- KSP's stock Settings screen has no API for mod options, so KSPVk's settings live in its own window.

## Safety
- **On errors:** if KSPVk throws errors three times in a session, it turns itself off and shows a message. If the Vulkan device fails, KSP switches back to its stock renderer for the rest of the session.
- **After crashes:** if KSP exits uncleanly twice in a row while KSPVk is installed, the next start runs with KSPVk off. You can turn it back on with Ctrl+Alt+F11 or in the window.
- **Logs:** `GameData/KSPVk/PluginData/KSPVk.log` holds the current session, and `KSPVk.prev.log` the previous one. The header lists your GPU, settings and mods. Please attach both to bug reports, along with `KSP.log`.

## Mod compatibility
`GameData/KSPVk/compat.cfg` lists mods that need parts of the scene to stay with Unity. Any mod can ship its own `KSPVK_COMPAT` or `KSPVK_EXCLUDE` node:

```
KSPVK_COMPAT
{
    assembly = MyMod          // assembly name as in KSP.log
    unity = terrain, ocean    // families Unity keeps: objects, terrain, ocean, scaled, sun, atmosphere, galaxy
    disable = false           // true: turn KSPVk off entirely while this mod is loaded
    note = why
}
KSPVK_EXCLUDE
{
    shader = MyMod/Special    // exact shader name, or a prefix ending in *
    shader = MyMod/Fx*
}
```

Shipped entries:

| Mod | What changes |
|---|---|
| Scatterer | Unity draws the ocean, atmosphere, sun and scaled planets |
| EVE | Unity draws the scaled planets |
| Parallax | Unity draws the terrain |
| Deferred | KSPVk is disabled |
| Singularity | Unity draws the galaxy |
| TUFX, Kopernicus, Waterfall, TexturesUnlimited | Notes only |

**These entries are untested.** Reports are welcome.

Renderers with shaders KSPVk doesn't know always stay with Unity, so most part mods work unchanged.

## API for other mods
`KSPVk.API`, in `KSPVk.dll`. Use it through reflection (`Type.GetType("KSPVk.API, KSPVk")`) to avoid a hard dependency.

| Member | Purpose |
|---|---|
| `int Version` | Currently 1 |
| `bool IsActive` | Vulkan is drawing this frame's scene |
| `bool IsDrawnByVulkan(Renderer r)` | KSPVk owns this renderer right now |
| `void KeepInUnity(Renderer r)` | Hand a renderer back to Unity immediately (e.g. one you re-material every frame) |
| `void AllowVulkan(Renderer r)` | Undo `KeepInUnity` |
| `event Action<bool> ActiveChanged` | Raised on the main thread when `IsActive` changes |

## Known issues
- **Frame time:** frame time improves only where rendering was the bottleneck. CPU-bound scenes (physics, scripts) gain little.
- **Shadows:** KSPVk shadows are slightly sharper and darker than stock's soft filtering.
- **Camera shake:** under heavy camera shake (e.g. IVA during launch), the Vulkan image can trail the Unity UI by a frame.
- **Not in Vulkan:** particles, transparent effects and the Reflective Water shader stay with Unity, by design.
- **Texture memory:** KSPVk's texture copies sit next to Unity's. With `TEXTURE_QUALITY` at full resolution and 16 GB of RAM, a long tour of many planets can run out of memory. Use half-resolution textures or a lower texture cap.

## Licence
KSPVk is MIT-licensed; see `LICENSE.txt`. It includes the Khronos Vulkan headers (Apache-2.0 OR MIT). No KSP or Squad assets are included; the toolbar icon is drawn in code.
