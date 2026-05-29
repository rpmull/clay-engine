using System.Collections.Generic;
using System.Linq;
using Claymore.Modules.RPG.Actions;
using Claymore.Modules.RPG.Core;
using Claymore.Modules.RPG.Events;
using Claymore.Modules.RPG.Rulebook;

namespace Claymore.Modules.RPG.Effects
{
	public sealed class DamageEffect : Effect
	{
		private readonly string _damageDice;
		private readonly string? _statModId;
		private readonly string _damageType;

		public DamageEffect(string damageDice, string? statModId, string damageType)
		{
			_damageDice = damageDice;
			_statModId = statModId;
			_damageType = damageType;
		}

		public override IEnumerable<DomainEvent> Execute(EffectContext ctx)
		{
			int mod = 0;
			var state = ctx.State;
			if (!string.IsNullOrEmpty(_statModId))
			{
				var actor = state.Actors[ctx.Intent.Actor];
				if (actor.Stats.TryGetValue(_statModId!, out var statVal)) mod = statVal;
			}

			// For now, use existing DiceSystem (will be refactored to IRng later)
			var roll = DiceSystem.Roll(_damageDice);
			int amount = roll.Total + mod;
			foreach (var targetId in ctx.Intent.Targets)
			{
				// Apply to stat if present
				if (state.Actors.TryGetValue(targetId, out var target))
				{
					if (target.Stats.TryGetValue("HP", out var hp))
					{
						int oldHp = hp;
						int newHp = System.Math.Max(0, hp - amount);
						target.Stats["HP"] = newHp;
						yield return new DamageApplied(state.Tick, ctx.Intent.Actor, targetId, amount, _damageType);
						yield return new StatChanged(state.Tick, targetId, "HP", oldHp, newHp);
					}
					else
					{
						yield return new DamageApplied(state.Tick, ctx.Intent.Actor, targetId, amount, _damageType);
					}
				}
			}
		}
	}
}


