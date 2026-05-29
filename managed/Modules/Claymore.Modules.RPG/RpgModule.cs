using System;
using System.Collections.Generic;
using ClaymoreEngine.Modules;

namespace Claymore.Modules.RPG
{
    [ClayModule]
    public sealed class RpgModule : IClayModule
    {
        public string Name => "Claymore.Modules.RPG";
        public Version Version => new Version(1,0,0,0);

        public void Initialize(NativeAPIs native, ManagedAPIs managed)
        {
            // The new system automatically handles component enumeration
            // We don't need to set managed.EnumerateComponents anymore
            // Components are automatically discovered via [ClayComponent] attributes
        }

        public void Shutdown() { }
    }
}


