using System;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Default seedable RNG. Wraps System.Random in a deterministic fashion.
	/// </summary>
	public sealed class DefaultRng : IRng
	{
		private readonly Random _random;
		private readonly int _seed;

		public DefaultRng(int seed)
		{
			_seed = seed;
			_random = new Random(seed);
		}

		public int NextInt(int minInclusive, int maxExclusive)
		{
			return _random.Next(minInclusive, maxExclusive);
		}

		public double NextDouble()
		{
			return _random.NextDouble();
		}

		public IRng Fork(int salt)
		{
			// Derive a child seed deterministically from the parent seed and salt
			unchecked
			{
				int mixed = _seed;
				mixed ^= unchecked((int)0x9E3779B9u); // golden ratio constant for mixing
				mixed = (mixed << 6) + (mixed >> 2) + salt * 1664525 + 1013904223;
				return new DefaultRng(mixed);
			}
		}
	}
}


