using System.Numerics;
using Claymore.Modules.RPG.Core;

namespace Claymore.Modules.RPG.Integration
{
	public interface IWorldAdapter
	{
		Vector3 GetPosition(int ecsEntityId);
		float Distance(int ecsEntityA, int ecsEntityB);
		bool HasLineOfSight(int ecsEntityA, int ecsEntityB);
	}
}


