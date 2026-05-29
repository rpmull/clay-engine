using System;
using Claymore.Modules.RPG.Core;

namespace Claymore.Modules.RPG.Events
{
	public static class RpgEventBus
	{
		public static event Action<DomainEvent>? OnEvent;

		public static void Publish(DomainEvent e)
		{
			RpgRuntime.State.Events.Add(e);
			OnEvent?.Invoke(e);
		}
	}
}


