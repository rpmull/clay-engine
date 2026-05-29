using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace Claymore.Modules.RPG.RulebookLoading
{
	public static class RulebookLoader
	{
		public static Rulebook.Rulebook CreateDefault()
		{
			var stats = new Dictionary<string, Rulebook.StatDef>
			{
				{"STR", new Rulebook.StatDef{Id="STR"}},
				{"DEX", new Rulebook.StatDef{Id="DEX"}}
			};

			var attack = new Rulebook.ActionDef
			{
				Id = "Attack",
				Check = new Rulebook.CheckDef { DiceExpression = "1d20", StatId = "DEX", DC = 12 },
				Effect = new Rulebook.BranchOnSuccessEffectDef
				{
					OnSuccess = new Rulebook.DamageEffectDef { DamageDice = "1d8", StatModId = "STR", DamageType = "physical" }
				}
			};

			var actions = new Dictionary<string, Rulebook.ActionDef> { { attack.Id, attack } };
			return new Rulebook.Rulebook
			{
				Stats = new ReadOnlyDictionary<string, Rulebook.StatDef>(stats),
				Actions = new ReadOnlyDictionary<string, Rulebook.ActionDef>(actions)
			};
		}
	}
}


