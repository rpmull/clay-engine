using System.Collections.Generic;
using Claymore.Modules.RPG.Core;

namespace Claymore.Modules.RPG.Events
{
	public sealed record ActionStarted(long Tick, ActorId Actor, string ActionId) : DomainEvent(Tick);
	public sealed record RollPerformed(long Tick, ActorId Actor, string Expression, IReadOnlyList<int> Rolls, int Modifiers, int Total) : DomainEvent(Tick);
	public sealed record DamageApplied(long Tick, ActorId Source, ActorId Target, int Amount, string DamageType) : DomainEvent(Tick);
	public sealed record ConditionApplied(long Tick, ActorId Target, string ConditionId, int Stacks, int Duration) : DomainEvent(Tick);
	public sealed record StatChanged(long Tick, ActorId Target, string StatId, int OldValue, int NewValue) : DomainEvent(Tick);
	public sealed record TurnStarted(long Tick, ActorId Actor) : DomainEvent(Tick);
	public sealed record TurnEnded(long Tick, ActorId Actor) : DomainEvent(Tick);
}


