using ClaymoreEngine.Scripting.Scriptable;

namespace Game.Data
{
    [CreateAssetMenu("Scriptable/Enemies/Archetype", fileName:"NewEnemyArchetype", order:20)]
    public sealed class EnemyArchetype : ClayScriptableObject
    {
        public string DisplayName = string.Empty;
        public int Health = 100;
        public float Speed = 1.0f;
        public float AggroRadius = 5.0f;
    }
}


