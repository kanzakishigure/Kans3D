using System;
using System.Text;

namespace Kans
{
    public static class SceneManager
    {
        // Switches the playing game to another scene, by its path relative to the project root
        // ("Scenes/Level2.kans"). The switch happens once the current frame ends, never in the
        // middle of it: every script finishes this Update first, and a later Load in the same frame
        // replaces an earlier one. The new scene starts like Play does (its scripts' OnCreate run),
        // while play mode carries on and static script state is kept. In the editor, stopping play
        // returns to the scene that was being edited, unchanged. Returns false outside play or for a
        // path that is absolute, leaves the project or is not a .kans file.
        public static unsafe bool Load(string path)
        {
            if (string.IsNullOrEmpty(path)) return false;
            var bytes = Encoding.UTF8.GetBytes(path + "\0");
            fixed (byte* text = bytes) return InternalCalls.Api.LoadScene(text) > 0;
        }
    }
}
