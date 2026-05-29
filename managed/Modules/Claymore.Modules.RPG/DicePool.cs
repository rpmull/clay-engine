using System;
using System.Collections.Generic;
using ClaymoreEngine;
using ClaymoreEngine.Modules;

namespace Claymore.Modules.RPG
{
public enum Dice : uint
   {
   D2 = 2,
   D4 = 4,
   D6 = 6,
   D8 = 8,
   D10 = 10,
   D12 = 12,
   D20 = 20,
   D100 = 100
   }

[ClayComponent("RPG/DicePool", 0, 1)]
[Obsolete("Use DiceSystem with IRng via RpgRuntime.State.Rng.")]
public class DicePool : ModuleComponentBase
   {
   private Random _rng;

   [ClayField]
   public int LastRoll = 0;

   public DicePool()
      {
      _rng = new Random();
      }

   public List<int> RollQuantity(Dice d, uint times)
      {
      List<int> rolls = new List<int>();
      for (int i = 0; i < times; i++)
         {
         rolls.Add(InternalRoll((uint)d));
         }
      return rolls;
      }

   public int RollWithModifier(Dice dice, uint modifier)
      {
      int result = InternalRoll((uint)dice) + (int)modifier;
      LastRoll = result;
      return result;
      }

   public int Roll(Dice d)
      {
      int result = InternalRoll((uint)d);
      LastRoll = result;
      return result;
      }

   private int InternalRoll(uint dice)
      {
      return 1 + _rng.Next((int)dice);
      }

   }
}