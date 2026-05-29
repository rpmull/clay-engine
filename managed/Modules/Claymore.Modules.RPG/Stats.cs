using System;
using ClaymoreEngine;
using ClaymoreEngine.Modules;

namespace Claymore.Modules.RPG
{
    [ClayComponent("RPG/Stats", 0, 1)]
    [Obsolete("Use RPG/Actor with Actor.Stats in the new system.")]
    public class Stats : ModuleComponentBase
    {
        [ClayField]
        public int Level = 1;

        [ClayField]
        public int HPMax = 100;

        [ClayField]
        public int Faction = 0;
    }
}