using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Asset reference to an Animator Controller Override (.animoverride).
    /// Stored as a VFS-relative path (e.g., "assets/animators/LocomotionOverrides.animoverride").
    /// </summary>
    [Serializable]
    public struct AnimationControllerOverride
    {
        public string? Path;

        /// <summary>
        /// Returns true if this override reference has a non-empty path.
        /// </summary>
        public bool IsValid => !string.IsNullOrWhiteSpace(Path);

        public override string ToString() => Path ?? string.Empty;
    }
}
