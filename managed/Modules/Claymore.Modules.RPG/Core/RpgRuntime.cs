using System;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Singleton-style access to the RPG runtime owned by the module.
	/// </summary>
	public static class RpgRuntime
	{
		private static RPGState? _state;
		public static RPGState State => _state ??= new RPGState(new DefaultRng(12345));

		public static void Reset(IRng rng)
		{
			_state = new RPGState(rng);
		}
	}
}


