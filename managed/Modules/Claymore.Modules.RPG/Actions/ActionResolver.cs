using System;
using System.Collections.Generic;
using System.Linq;
using Claymore.Modules.RPG.Core;
using Claymore.Modules.RPG.Effects;
using Claymore.Modules.RPG.Events;
using Claymore.Modules.RPG.Rulebook;

namespace Claymore.Modules.RPG.Actions
{
	public enum SuccessLevel { Disaster, Fail, Success, Great }

	public static class ActionResolver
	{
		public static ValidatedAction Validate(ActionIntent intent, Rulebook.Rulebook rules)
		{
			if (!rules.Actions.TryGetValue(intent.ActionId, out var actionDef))
				throw new InvalidOperationException($"Unknown action: {intent.ActionId}");

			var plan = new CheckPlan
			{
				DiceExpression = actionDef.Check.DiceExpression,
				StatId = actionDef.Check.StatId,
				SkillId = actionDef.Check.SkillId,
				DC = actionDef.Check.DC
			};

			var effect = BuildEffect(actionDef.Effect);
			return new ValidatedAction
			{
				Intent = intent,
				Plan = plan,
				EffectGraph = effect
			};
		}

		private static Effect BuildEffect(EffectDef def)
		{
			switch (def)
			{
				case DamageEffectDef dmg:
					return new DamageEffect(dmg.DamageDice, dmg.StatModId, dmg.DamageType);
				case SequenceEffectDef seq:
					return new SequenceEffect(seq.Steps.Select(BuildEffect).ToList());
				case BranchOnSuccessEffectDef br:
					var onFail = br.OnFail != null ? BuildEffect(br.OnFail) : new SequenceEffect(System.Array.Empty<Effect>());
					var onSuccess = br.OnSuccess != null ? BuildEffect(br.OnSuccess) : new SequenceEffect(System.Array.Empty<Effect>());
					var onGreat = br.OnGreat != null ? BuildEffect(br.OnGreat) : onSuccess;
					var onDisaster = br.OnDisaster != null ? BuildEffect(br.OnDisaster) : onFail;
					return new BranchOnSuccessEffect(onFail, onSuccess, onGreat, onDisaster);
				default:
					throw new NotSupportedException($"Unknown EffectDef type: {def?.GetType().Name}");
			}
		}

		public static void Resolve(ValidatedAction validated, Rulebook.Rulebook rules)
		{
			var state = RpgRuntime.State;
			RpgEventBus.Publish(new ActionStarted(state.Tick, validated.Intent.Actor, validated.Intent.ActionId));

			// Compute roll and modifiers
			int mods = 0;
			if (!string.IsNullOrEmpty(validated.Plan.StatId))
			{
				var actor = state.Actors[validated.Intent.Actor];
				if (actor.Stats.TryGetValue(validated.Plan.StatId!, out var statVal)) mods += statVal;
			}

			var res = DiceSystem.Roll(validated.Plan.DiceExpression);
			int total = res.Total + mods;
			RpgEventBus.Publish(new RollPerformed(state.Tick, validated.Intent.Actor, validated.Plan.DiceExpression, res.Rolls, mods, total));

			var outcome = ComputeOutcome(total, validated.Plan.DC);
			var ctx = new EffectContext(state, validated.Intent, outcome);
			foreach (var e in validated.EffectGraph.Execute(ctx))
			{
				RpgEventBus.Publish(e);
			}
		}

		private static SuccessLevel ComputeOutcome(int total, int dc)
		{
			if (total >= dc + 10) return SuccessLevel.Great;
			if (total >= dc) return SuccessLevel.Success;
			if (total <= dc - 10) return SuccessLevel.Disaster;
			return SuccessLevel.Fail;
		}
	}
}


