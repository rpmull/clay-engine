using System.Collections.Generic;
using Claymore.Modules.RPG.Core;
using Claymore.Modules.RPG.Events;

namespace Claymore.Modules.RPG.Conditions
{
	public sealed class AppliedCondition
	{
		public string Id { get; init; } = string.Empty;
		public int Stacks { get; set; } = 1;
		public int RemainingDuration { get; set; } = 1;
	}

	public static class ConditionSystem
	{
		public static void Apply(RPGState state, ActorId target, string conditionId, int stacks, int duration)
		{
			if (!state.Actors.TryGetValue(target, out var actor)) return;
			actor.Conditions.Add(new AppliedCondition { Id = conditionId, Stacks = stacks, RemainingDuration = duration });
			RpgEventBus.Publish(new ConditionApplied(state.Tick, target, conditionId, stacks, duration));
		}

		public static void Tick(RPGState state)
		{
			foreach (var actor in state.Actors.Values)
			{
				for (int i = actor.Conditions.Count - 1; i >= 0; i--)
				{
					var c = (AppliedCondition)actor.Conditions[i];
					c.RemainingDuration--;
					if (c.RemainingDuration <= 0)
						actor.Conditions.RemoveAt(i);
				}
			}
		}
	}
}


