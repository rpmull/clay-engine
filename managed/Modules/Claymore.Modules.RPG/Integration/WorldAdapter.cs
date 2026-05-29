using System.Numerics;
using Claymore.Modules.RPG.Core;
using ClaymoreEngine;

namespace Claymore.Modules.RPG.Integration
{
	/// <summary>
	/// Basic adapter querying ECS transforms. Extend as needed.
	/// </summary>
	public sealed class WorldAdapter : IWorldAdapter
	{
		public Vector3 GetPosition(int ecsEntityId)
		{
			var entity = new Entity(ecsEntityId);
			var t = entity.transform;
			return t.position;
		}

		public float Distance(int ecsEntityA, int ecsEntityB)
		{
			var a = GetPosition(ecsEntityA);
			var b = GetPosition(ecsEntityB);
			return Vector3.Distance(a, b);
		}

		public bool HasLineOfSight(int ecsEntityA, int ecsEntityB)
		{
			// TODO: integrate with physics/raycast if available
			return true;
		}
	}
}


