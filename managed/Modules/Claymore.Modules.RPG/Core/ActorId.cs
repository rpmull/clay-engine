using System;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Strongly-typed identifier for RPG actors.
	/// </summary>
	public readonly record struct ActorId(int Value)
	{
		public static readonly ActorId Invalid = new ActorId(-1);
		public override string ToString() => Value.ToString();
	}
}


