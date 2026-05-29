using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Asset reference to an Animator Controller (.animctrl).
    /// Stored as a VFS-relative path (e.g., "assets/animators/Locomotion.animctrl").
    /// </summary>
    [Serializable]
    public struct AnimationController
    {
        public string? Path;

        /// <summary>
        /// Returns true if this controller reference has a non-empty path.
        /// </summary>
        public bool IsValid => !string.IsNullOrWhiteSpace(Path);

        public override string ToString() => Path ?? string.Empty;
    }
}

