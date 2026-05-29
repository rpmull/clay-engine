using System.Collections.Generic;
using System.Numerics;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Runtime actor state owned by the RPG module.
	/// </summary>
	public sealed class Actor
	{
		public ActorId Id { get; }
		public int EcsEntityId { get; }
		public HashSet<string> Tags { get; } = new();
		public Dictionary<string, int> Stats { get; } = new();
		public Dictionary<string, int> Skills { get; } = new();
		public List<Claymore.Modules.RPG.Conditions.AppliedCondition> Conditions { get; } = new();
		public HashSet<string> Abilities { get; } = new();
		public Inventory Inventory { get; } = new();
		public Vector3 Position { get; set; }

		public Actor(ActorId id, int ecsEntityId)
		{
			Id = id;
			EcsEntityId = ecsEntityId;
		}
	}

	/// <summary>
	/// Minimal inventory placeholder; extend as needed.
	/// </summary>
	public sealed class Inventory
	{
		public List<string> ItemIds { get; } = new();
	}
}


