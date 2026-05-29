using ActorId = System.UInt64;
using Seed = System.String;
using RuleId = System.UInt64;
using System.Diagnostics;
using System.Collections.Generic;

interface IRng
{
    int NextInt(int min, int max);
    float NextFloat(float min, float max);
    IRng Fork(string label);
}

struct RollResult
{
    int total;
    int[] dice;
    (string, int)[] mods;
}

struct StatDef
{
    string id;
    int baseVal;
    int min;
    int max;

}

struct Stat
{
    int baseVal;
    int bonuses;
    int temp;
    int min;
    int max;

    int value => Clamp(baseVal + bonuses + temp, min, max);

    static int Clamp(int value, int min, int max)
    {
        if (value < min) return min;
        if (value > max) return max;
        return value;
    }
}

struct Rulebook
{
    Dictionary<string, StatDef> Stats;
}
