using System;
using ClaymoreEngine;
using ClaymoreEngine.Modules;

namespace Claymore.Modules.RPG
{
    [ClayComponent("RPG/Inventory", 1, 1)]
    public class Inventory : ModuleComponentBase
    {
        [ClayField]
        public int Slots = 10;
    }
}
