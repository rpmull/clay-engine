using System.Collections.Generic;
using Claymore.Modules.RPG.Core;
using Claymore.Modules.RPG.Actions;

namespace Claymore.Modules.RPG.Effects
{
	public sealed class EffectContext
	{
		public RPGState State { get; }
		public ActionIntent Intent { get; }
		public SuccessLevel Outcome { get; }

		public EffectContext(RPGState state, ActionIntent intent, SuccessLevel outcome)
		{
			State = state;
			Intent = intent;
			Outcome = outcome;
		}
	}

	public abstract class Effect
	{
		public abstract IEnumerable<Events.DomainEvent> Execute(EffectContext ctx);
	}
}


