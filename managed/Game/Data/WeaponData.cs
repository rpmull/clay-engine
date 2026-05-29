using ClaymoreEngine.Scripting.Scriptable;

namespace Game.Data
{
    [CreateAssetMenu("Scriptable/Items/Weapon", fileName:"NewWeaponData", order:10)]
    public sealed class WeaponData : ClayScriptableObject
    {
        public string Name = string.Empty;
        public int Damage = 0;
        public float CritChance = 0.0f;
        public string Tags = string.Empty;
    }
}


