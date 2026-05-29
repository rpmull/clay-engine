using System.Collections.Generic;
using System.Collections.ObjectModel;

namespace Claymore.Modules.RPG.Rulebook
{
	public sealed class Rulebook
	{
		public IReadOnlyDictionary<string, StatDef> Stats { get; init; } = new ReadOnlyDictionary<string, StatDef>(new Dictionary<string, StatDef>());
		public IReadOnlyDictionary<string, SkillDef> Skills { get; init; } = new ReadOnlyDictionary<string, SkillDef>(new Dictionary<string, SkillDef>());
		public IReadOnlyDictionary<string, ActionDef> Actions { get; init; } = new ReadOnlyDictionary<string, ActionDef>(new Dictionary<string, ActionDef>());
		public IReadOnlyDictionary<string, ConditionDef> Conditions { get; init; } = new ReadOnlyDictionary<string, ConditionDef>(new Dictionary<string, ConditionDef>());
	}
}


