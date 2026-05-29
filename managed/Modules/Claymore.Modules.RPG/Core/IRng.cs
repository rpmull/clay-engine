using System;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Seedable deterministic random number generator abstraction.
	/// </summary>
	public interface IRng
	{
		int NextInt(int minInclusive, int maxExclusive);
		double NextDouble();
		IRng Fork(int salt);
	}
}


