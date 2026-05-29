using System.Collections.Generic;
using Claymore.Modules.RPG.Actions;

namespace Claymore.Modules.RPG.Effects
{
	public sealed class SequenceEffect : Effect
	{
		private readonly IReadOnlyList<Effect> _steps;
		public SequenceEffect(IReadOnlyList<Effect> steps) { _steps = steps; }
		public override IEnumerable<Events.DomainEvent> Execute(EffectContext ctx)
		{
			foreach (var step in _steps)
			{
				foreach (var e in step.Execute(ctx)) yield return e;
			}
		}
	}

	public sealed class BranchOnSuccessEffect : Effect
	{
		private readonly Effect _onFail;
		private readonly Effect _onSuccess;
		private readonly Effect _onGreat;
		private readonly Effect _onDisaster;

		public BranchOnSuccessEffect(Effect onFail, Effect onSuccess, Effect onGreat, Effect onDisaster)
		{
			_onFail = onFail;
			_onSuccess = onSuccess;
			_onGreat = onGreat;
			_onDisaster = onDisaster;
		}

		public override IEnumerable<Events.DomainEvent> Execute(EffectContext ctx)
		{
			if (ctx.Outcome == SuccessLevel.Disaster) return _onDisaster.Execute(ctx);
			if (ctx.Outcome == SuccessLevel.Fail) return _onFail.Execute(ctx);
			if (ctx.Outcome == SuccessLevel.Great) return _onGreat.Execute(ctx);
			return _onSuccess.Execute(ctx);
		}
	}
}


