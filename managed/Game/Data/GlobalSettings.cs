using ClaymoreEngine.Scripting.Scriptable;

namespace Game.Data
{
    public enum Difficulty { Easy=0, Normal=1, Hard=2 }

    [CreateAssetMenu("Scriptable/Settings/Global", fileName:"GlobalSettings", order:1)]
    public sealed class GlobalSettings : ClayScriptableObject
    {
        public float MasterVolume = 1.0f;
        public Difficulty Difficulty = Difficulty.Normal;
        public string Language = "en";
    }
}


