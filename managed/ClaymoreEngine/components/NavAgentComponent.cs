using System;
using System.Collections.Generic;
using System.Numerics;

namespace ClaymoreEngine
{
    /// <summary>
    /// Managed wrapper for native NavAgent component.
    /// Provides pathfinding and navigation capabilities for NPCs.
    /// </summary>
    public sealed class NavAgentComponent : ComponentBase
    {
        private Action? _onDestinationReached;
        private bool _registered;
        private bool _wasMoving;
        private bool _hasActiveDestination;

        private static readonly HashSet<NavAgentComponent> s_Registry = new();
        private static readonly List<NavAgentComponent> s_Snapshot = new();

        /// <summary>
        /// Fired when this nav agent finishes traversing an active destination.
        /// </summary>
        public event Action DestinationReached
        {
            add
            {
                EnsureRegistered();
                _onDestinationReached += value;
            }
            remove
            {
                _onDestinationReached -= value;
                TryUnregisterIfIdle();
            }
        }

        #region Methods

        /// <summary>
        /// Sets the destination for the agent to navigate to.
        /// </summary>
        public void SetDestination(Vector3 destination)
        {
            if (_registered || _onDestinationReached != null)
            {
                EnsureRegistered();
                _hasActiveDestination = true;
            }
            NavigationInterop.AgentSetDestination?.Invoke(entity.EntityID, destination.X, destination.Y, destination.Z);
        }

        /// <summary>
        /// Stops the agent immediately and clears its path.
        /// </summary>
        public void Stop()
        {
            _hasActiveDestination = false;
            NavigationInterop.AgentStop?.Invoke(entity.EntityID);
            TryUnregisterIfIdle();
        }

        /// <summary>
        /// Immediately teleports the agent to the specified position.
        /// </summary>
        public void Warp(Vector3 position)
        {
            _hasActiveDestination = false;
            NavigationInterop.AgentWarp?.Invoke(entity.EntityID, position.X, position.Y, position.Z);
            TryUnregisterIfIdle();
        }

        /// <summary>
        /// Polls the native nav agent state and fires movement completion events.
        /// The engine pumps this each frame.
        /// </summary>
        public void Update()
        {
            bool isMoving = IsMoving;
            bool hasPath = HasPath;
            bool isStopped = IsStopped;

            if (isMoving)
            {
                _hasActiveDestination = true;
            }

            // Detect a movement -> stopped transition for an active destination.
            if (_wasMoving && !isMoving && _hasActiveDestination)
            {
                float remainingDistance = RemainingDistance;
                float stoppingDistance = StoppingDistance;
                bool reachedByDistance = remainingDistance <= stoppingDistance;
                bool reachedByState = !hasPath && isStopped;

                if (reachedByDistance || reachedByState)
                {
                    _hasActiveDestination = false;
                    _onDestinationReached?.Invoke();
                    TryUnregisterIfIdle();
                }
            }

            _wasMoving = isMoving;
        }

        internal static void Register(NavAgentComponent navAgent)
        {
            if (navAgent == null) return;
            s_Registry.Add(navAgent);
        }

        internal static void Unregister(NavAgentComponent navAgent)
        {
            if (navAgent == null) return;
            s_Registry.Remove(navAgent);
        }

        internal static void ClearRegistry()
        {
            s_Registry.Clear();
            s_Snapshot.Clear();
        }

        public static void UpdateAll()
        {
            if (s_Registry.Count == 0) return;

            s_Snapshot.Clear();
            s_Snapshot.AddRange(s_Registry);

            foreach (var navAgent in s_Snapshot)
            {
                if (navAgent == null || !navAgent.IsValid)
                {
                    if (navAgent != null) s_Registry.Remove(navAgent);
                    continue;
                }

                if (ComponentInterop.HasComponent != null &&
                    !ComponentInterop.HasComponent(navAgent.entity.EntityID, nameof(NavAgentComponent)))
                {
                    s_Registry.Remove(navAgent);
                    continue;
                }

                navAgent.Update();
            }
        }

        private void EnsureRegistered()
        {
            if (_registered) return;
            _registered = true;
            _wasMoving = IsMoving;
            if (_wasMoving || HasPath) _hasActiveDestination = true;
            Register(this);
        }

        private void TryUnregisterIfIdle()
        {
            if (!_registered) return;
            if (_onDestinationReached != null) return;
            if (_hasActiveDestination) return;
            _registered = false;
            Unregister(this);
        }

        #endregion

        #region State Properties

        /// <summary>
        /// Returns the remaining distance along the current path.
        /// Returns float.MaxValue if there is no path.
        /// </summary>
        public float RemainingDistance
        {
            get
            {
                return NavigationInterop.AgentRemainingDistance != null
                    ? NavigationInterop.AgentRemainingDistance(entity.EntityID)
                    : float.MaxValue;
            }
        }

        /// <summary>
        /// Returns true if the agent is stopped (no destination and no active path).
        /// </summary>
        public bool IsStopped
        {
            get
            {
                return NavigationInterop.AgentIsStopped != null
                    ? NavigationInterop.AgentIsStopped(entity.EntityID)
                    : true;
            }
        }

        /// <summary>
        /// Returns true if the agent is actively moving along a path.
        /// </summary>
        public bool IsMoving
        {
            get
            {
                return NavigationInterop.AgentIsMoving != null
                    ? NavigationInterop.AgentIsMoving(entity.EntityID)
                    : false;
            }
        }

        /// <summary>
        /// Returns true if the agent has a valid path calculated.
        /// </summary>
        public bool HasPath
        {
            get
            {
                return NavigationInterop.AgentHasPath != null
                    ? NavigationInterop.AgentHasPath(entity.EntityID)
                    : false;
            }
        }

        #endregion

        #region Movement Parameters

        /// <summary>
        /// Gets or sets the maximum movement speed of the agent.
        /// </summary>
        public float Speed
        {
            get
            {
                return NavigationInterop.AgentGetSpeed != null
                    ? NavigationInterop.AgentGetSpeed(entity.EntityID)
                    : 0f;
            }
            set
            {
                NavigationInterop.AgentSetSpeed?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Gets or sets the maximum acceleration of the agent.
        /// </summary>
        public float Acceleration
        {
            get
            {
                return NavigationInterop.AgentGetAcceleration != null
                    ? NavigationInterop.AgentGetAcceleration(entity.EntityID)
                    : 0f;
            }
            set
            {
                NavigationInterop.AgentSetAcceleration?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Gets or sets the collision radius of the agent.
        /// </summary>
        public float Radius
        {
            get
            {
                return NavigationInterop.AgentGetRadius != null
                    ? NavigationInterop.AgentGetRadius(entity.EntityID)
                    : 0f;
            }
            set
            {
                NavigationInterop.AgentSetRadius?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Gets or sets the height of the agent.
        /// </summary>
        public float Height
        {
            get
            {
                return NavigationInterop.AgentGetHeight != null
                    ? NavigationInterop.AgentGetHeight(entity.EntityID)
                    : 0f;
            }
            set
            {
                NavigationInterop.AgentSetHeight?.Invoke(entity.EntityID, value);
            }
        }

        /// <summary>
        /// Gets or sets the distance at which the agent considers itself arrived at the destination.
        /// </summary>
        public float StoppingDistance
        {
            get
            {
                return NavigationInterop.AgentGetStoppingDistance != null
                    ? NavigationInterop.AgentGetStoppingDistance(entity.EntityID)
                    : 0f;
            }
            set
            {
                NavigationInterop.AgentSetStoppingDistance?.Invoke(entity.EntityID, value);
            }
        }

        #endregion

        #region Velocity (Read-Only)

        /// <summary>
        /// Gets the current velocity of the agent as computed by the navigation system.
        /// This is the actual movement velocity being applied to the agent.
        /// Read-only - direction is determined by the path, speed can be adjusted via the Speed property.
        /// </summary>
        public Vector3 Velocity
        {
            get
            {
                float x = NavigationInterop.AgentGetVelocityX != null
                    ? NavigationInterop.AgentGetVelocityX(entity.EntityID)
                    : 0f;
                float y = NavigationInterop.AgentGetVelocityY != null
                    ? NavigationInterop.AgentGetVelocityY(entity.EntityID)
                    : 0f;
                float z = NavigationInterop.AgentGetVelocityZ != null
                    ? NavigationInterop.AgentGetVelocityZ(entity.EntityID)
                    : 0f;
                return new Vector3(x, y, z);
            }
        }

        #endregion
    }
}


