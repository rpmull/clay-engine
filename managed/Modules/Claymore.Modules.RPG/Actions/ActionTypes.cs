using System;
using System.Collections.Generic;
using Claymore.Modules.RPG.Core;

namespace Claymore.Modules.RPG.Actions
{
	public sealed class ActionIntent
	{
		public ActorId Actor { get; init; }
		public string ActionId { get; init; } = string.Empty;

		public IReadOnlyList<ActorId> Targets { get; init; } = Array.Empty<ActorId>();
		public Dictionary<string, object> Params { get; init; } = new();
	}

	public sealed class CheckPlan
	{
		public string DiceExpression { get; init; } = "1d20";
		public string? StatId { get; init; }
		public string? SkillId { get; init; }
		public int DC { get; init; } = 10;
	}

	public sealed class RollResult
	{
		public string Expression { get; init; } = string.Empty;
		public List<int> Rolls { get; init; } = new();
		public int Modifiers { get; init; }
		public int Total { get; init; }
	}

	public sealed class ValidatedAction
	{
		public ActionIntent Intent { get; init; }
		public IReadOnlyList<object> Costs { get; init; } = Array.Empty<object>();
		public CheckPlan Plan { get; init; } = new();
		public Effects.Effect EffectGraph { get; init; }
	}
}


