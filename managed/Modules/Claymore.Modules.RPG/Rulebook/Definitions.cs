using System.Collections.Generic;

namespace Claymore.Modules.RPG.Rulebook
{
	public sealed class StatDef { public string Id { get; init; } = string.Empty; }
	public sealed class SkillDef { public string Id { get; init; } = string.Empty; }

	public sealed class CheckDef
	{
		public string DiceExpression { get; init; } = "1d20";
		public string? StatId { get; init; }
		public string? SkillId { get; init; }
		public int DC { get; init; } = 10;
	}

	public abstract class EffectDef { }

	public sealed class DamageEffectDef : EffectDef
	{
		public string DamageDice { get; init; } = "1d6";
		public string? StatModId { get; init; }
		public string DamageType { get; init; } = "physical";
	}

	public sealed class SequenceEffectDef : EffectDef
	{
		public List<EffectDef> Steps { get; init; } = new();
	}

	public sealed class BranchOnSuccessEffectDef : EffectDef
	{
		public EffectDef? OnFail { get; init; }
		public EffectDef? OnSuccess { get; init; }
		public EffectDef? OnGreat { get; init; }
		public EffectDef? OnDisaster { get; init; }
	}

	public sealed class ActionDef
	{
		public string Id { get; init; } = string.Empty;
		public CheckDef Check { get; init; } = new();
		public EffectDef Effect { get; init; } = new SequenceEffectDef();
	}

	public sealed class ConditionDef
	{
		public string Id { get; init; } = string.Empty;
		public int DefaultDuration { get; init; } = 1;
	}
}


