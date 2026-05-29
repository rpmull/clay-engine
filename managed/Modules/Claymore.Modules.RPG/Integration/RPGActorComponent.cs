using Claymore.Modules.RPG.Core;
using ClaymoreEngine;

namespace Claymore.Modules.RPG.Integration
{
	[ClaymoreEngine.Modules.ClayComponent("RPG/Actor", 0, 1)]
	public sealed class RPGActorComponent : ModuleComponentBase
	{
		[ClaymoreEngine.Modules.ClayField]
		public int ActorIdValue;

		protected override void OnInitialize()
		{
			if (ActorIdValue <= 0)
			{
				var id = AllocateActorId();
				ActorIdValue = id.Value;
				SyncToNative();
			}

			var actorId = new ActorId(ActorIdValue);
			if (!RpgRuntime.State.Actors.ContainsKey(actorId))
			{
				int entityId = GetEntityId();
				var actor = new Actor(actorId, entityId);
				RpgRuntime.State.Actors[actorId] = actor;
			}
		}

		private int GetEntityId()
		{
			// ModuleComponentBase.entity is internal; use reflection safely
			var baseType = typeof(ModuleComponentBase);
			var field = baseType.GetField("entity", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
			if (field?.GetValue(this) is Entity e)
				return e.EntityID;
			return -1;
		}

		private static int s_NextId = 1;
		private static ActorId AllocateActorId()
		{
			return new ActorId(System.Threading.Interlocked.Increment(ref s_NextId));
		}
	}
}


