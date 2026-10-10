using System;
using System.IO;
using System.Linq;
using UnityEngine;

namespace KSPVk
{
    /// KSPVk.log in PluginData (the previous session is kept as KSPVk.prev.log) plus the Unity log. The header carries what a bug report needs.
    /// session.lock tracks unclean exits: it is written at startup and deleted on a clean quit.
    public static class Log
    {
        public const string Version = "0.7.0-beta";
        static StreamWriter w;
        static string LockPath => Path.Combine(Core.PluginData, "session.lock");

        public static void Open()
        {
            try
            {
                string p = Path.Combine(Core.PluginData, "KSPVk.log"), prev = Path.Combine(Core.PluginData, "KSPVk.prev.log");
                Directory.CreateDirectory(Core.PluginData);
                if (File.Exists(p)) { File.Delete(prev); File.Move(p, prev); }
                w = new StreamWriter(p) { AutoFlush = true };
            }
            catch (Exception e) { Debug.LogWarning("[KSPVk] log file unavailable: " + e.Message); }
            Line($"KSPVk {Version} | KSP {Versioning.version_major}.{Versioning.version_minor}.{Versioning.Revision} | Unity {Application.unityVersion} | {SystemInfo.operatingSystem}");
            Line($"GPU {SystemInfo.graphicsDeviceName} ({SystemInfo.graphicsDeviceVersion}, {SystemInfo.graphicsMemorySize} MB) | CPU {SystemInfo.processorType} x{SystemInfo.processorCount} | RAM {SystemInfo.systemMemorySize} MB");
            Line($"game: {Screen.width}x{Screen.height} TEXTURE_QUALITY {GameSettings.TEXTURE_QUALITY} TERRAIN_SHADER_QUALITY {GameSettings.TERRAIN_SHADER_QUALITY} SHADOWS_QUALITY {GameSettings.SHADOWS_QUALITY} ANTI_ALIASING {GameSettings.ANTI_ALIASING}");
            Line("mods: " + string.Join(", ", AssemblyLoader.loadedAssemblies.Select(a => $"{a.name} {a.assembly.GetName().Version}")));
        }

        public static void Line(string s, bool warn = false)
        {
            if (warn) Debug.LogWarning("[KSPVk] " + s); else Debug.Log("[KSPVk] " + s);
            try { w?.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff ") + s); } catch { }
        }

        /// Native lines go to the file only with their own prefix (DrainLog also mirrors them to the Unity log).
        public static void Native(string s) { Debug.Log("[KSPVk.native] " + s); try { w?.WriteLine(DateTime.Now.ToString("HH:mm:ss.fff ") + "native: " + s); } catch { } }

        /// Number of consecutive sessions that ended without a clean quit (before this one), then marks this session as running.
        public static int BeginSession()
        {
            int n = 0;
            try
            {
                if (File.Exists(LockPath)) n = (int.TryParse(File.ReadAllText(LockPath).Trim(), out int prev) ? Math.Max(prev, 0) : 0) + 1;
                File.WriteAllText(LockPath, n.ToString());
            }
            catch { }
            return n;
        }

        public static void EndSession() { try { File.Delete(LockPath); } catch { } w?.Flush(); }
    }
}
