using System.Collections.Generic;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Global RPG runtime state for the module.
	/// </summary>
	public sealed class RPGState
	{
		public long Tick { get; private set; }
		public Dictionary<ActorId, Actor> Actors { get; } = new();
		public TurnOrder Timeline { get; } = new();
		public List<Events.DomainEvent> Events { get; } = new();
		public IRng Rng { get; }

		public RPGState(IRng rng)
		{
			Rng = rng;
		}

		public void AdvanceTick() => Tick++;
	}
}


