using System.Collections.Generic;

namespace Claymore.Modules.RPG.Core
{
	/// <summary>
	/// Simple initiative queue for actor turns.
	/// </summary>
	public sealed class TurnOrder
	{
		private readonly Queue<ActorId> _queue = new();

		public void Enqueue(ActorId actorId) => _queue.Enqueue(actorId);
		public bool TryDequeue(out ActorId actorId)
		{
			if (_queue.Count > 0)
			{
				actorId = _queue.Dequeue();
				return true;
			}
			actorId = ActorId.Invalid;
			return false;
		}

		public int Count => _queue.Count;
	}
}


