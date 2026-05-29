using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace Claymore.Modules.RPG
{
    /// <summary>
    /// Comprehensive tabletop-style dice rolling system supporting standard RPG notation.
    /// Supports expressions like "3d6+2", "1d20", "2d10+1d4-1", "d100", etc.
    /// </summary>
    public static class DiceSystem
    {
        private static readonly Random _random = new Random();
        private static readonly Regex _diceRegex = new Regex(
            @"(?<count>\d*)d(?<sides>\d+)(?<modifier>[+-]\d+)?",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

        /// <summary>
        /// Roll dice using standard RPG notation (e.g., "3d6+2", "1d20", "2d10+1d4-1")
        /// </summary>
        public static DiceResult Roll(string expression)
        {
            if (string.IsNullOrWhiteSpace(expression))
                throw new ArgumentException("Dice expression cannot be null or empty");

            var result = new DiceResult { Expression = expression.Trim() };
            var totalValue = 0;

            // Handle simple modifiers at the end (e.g., "3d6+2")
            var parts = expression.Split(new[] { '+', '-' }, StringSplitOptions.RemoveEmptyEntries);
            var operators = GetOperators(expression);

            for (int i = 0; i < parts.Length; i++)
            {
                var part = parts[i].Trim();
                var isNegative = i > 0 && operators[i - 1] == '-';

                if (_diceRegex.IsMatch(part))
                {
                    var diceRoll = RollDice(part);
                    result.Rolls.AddRange(diceRoll.Rolls);
                    totalValue += isNegative ? -diceRoll.Total : diceRoll.Total;
                }
                else if (int.TryParse(part, out int modifier))
                {
                    totalValue += isNegative ? -modifier : modifier;
                    result.Modifiers += isNegative ? -modifier : modifier;
                }
            }

            result.Total = totalValue;
            return result;
        }

        /// <summary>
        /// Roll a single die with specified number of sides
        /// </summary>
        public static int RollSingle(int sides)
        {
            if (sides <= 0)
                throw new ArgumentException("Number of sides must be positive");
            
            return _random.Next(1, sides + 1);
        }

        public static int RollSingle(int sides, Core.IRng rng)
        {
            if (sides <= 0)
                throw new ArgumentException("Number of sides must be positive");

            return rng.NextInt(1, sides + 1);
        }

        /// <summary>
        /// Roll multiple dice of the same type
        /// </summary>
        public static List<int> RollMultiple(int count, int sides)
        {
            if (count <= 0)
                throw new ArgumentException("Number of dice must be positive");
            if (sides <= 0)
                throw new ArgumentException("Number of sides must be positive");

            var results = new List<int>();
            for (int i = 0; i < count; i++)
            {
                results.Add(RollSingle(sides));
            }
            return results;
        }

        public static List<int> RollMultiple(int count, int sides, Core.IRng rng)
        {
            if (count <= 0)
                throw new ArgumentException("Number of dice must be positive");
            if (sides <= 0)
                throw new ArgumentException("Number of sides must be positive");

            var results = new List<int>();
            for (int i = 0; i < count; i++)
            {
                results.Add(RollSingle(sides, rng));
            }
            return results;
        }

        /// <summary>
        /// Roll with advantage (roll twice, take higher)
        /// </summary>
        public static DiceResult RollAdvantage(string expression)
        {
            var roll1 = Roll(expression);
            var roll2 = Roll(expression);
            
            var result = roll1.Total >= roll2.Total ? roll1 : roll2;
            result.HasAdvantage = true;
            result.AlternateRoll = roll1.Total >= roll2.Total ? roll2 : roll1;
            return result;
        }

        public static DiceResult RollAdvantage(string expression, Core.IRng rng)
        {
            var roll1 = Roll(expression, rng);
            var roll2 = Roll(expression, rng);
            var result = roll1.Total >= roll2.Total ? roll1 : roll2;
            result.HasAdvantage = true;
            result.AlternateRoll = roll1.Total >= roll2.Total ? roll2 : roll1;
            return result;
        }

        /// <summary>
        /// Roll with disadvantage (roll twice, take lower)
        /// </summary>
        public static DiceResult RollDisadvantage(string expression)
        {
            var roll1 = Roll(expression);
            var roll2 = Roll(expression);
            
            var result = roll1.Total <= roll2.Total ? roll1 : roll2;
            result.HasDisadvantage = true;
            result.AlternateRoll = roll1.Total <= roll2.Total ? roll2 : roll1;
            return result;
        }

        public static DiceResult RollDisadvantage(string expression, Core.IRng rng)
        {
            var roll1 = Roll(expression, rng);
            var roll2 = Roll(expression, rng);
            var result = roll1.Total <= roll2.Total ? roll1 : roll2;
            result.HasDisadvantage = true;
            result.AlternateRoll = roll1.Total <= roll2.Total ? roll2 : roll1;
            return result;
        }

        /// <summary>
        /// Roll and keep highest N dice (e.g., roll 4d6, keep highest 3)
        /// </summary>
        public static DiceResult RollKeepHighest(int count, int sides, int keep)
        {
            if (keep > count)
                throw new ArgumentException("Cannot keep more dice than rolled");

            var rolls = RollMultiple(count, sides);
            var keptRolls = rolls.OrderByDescending(x => x).Take(keep).ToList();
            var droppedRolls = rolls.Except(keptRolls).ToList();

            return new DiceResult
            {
                Expression = $"{count}d{sides} keep highest {keep}",
                Rolls = keptRolls,
                DroppedRolls = droppedRolls,
                Total = keptRolls.Sum()
            };
        }

        public static DiceResult RollKeepHighest(int count, int sides, int keep, Core.IRng rng)
        {
            if (keep > count)
                throw new ArgumentException("Cannot keep more dice than rolled");

            var rolls = RollMultiple(count, sides, rng);
            var keptRolls = rolls.OrderByDescending(x => x).Take(keep).ToList();
            var droppedRolls = rolls.Except(keptRolls).ToList();

            return new DiceResult
            {
                Expression = $"{count}d{sides} keep highest {keep}",
                Rolls = keptRolls,
                DroppedRolls = droppedRolls,
                Total = keptRolls.Sum()
            };
        }

        /// <summary>
        /// Roll and keep lowest N dice
        /// </summary>
        public static DiceResult RollKeepLowest(int count, int sides, int keep)
        {
            if (keep > count)
                throw new ArgumentException("Cannot keep more dice than rolled");

            var rolls = RollMultiple(count, sides);
            var keptRolls = rolls.OrderBy(x => x).Take(keep).ToList();
            var droppedRolls = rolls.Except(keptRolls).ToList();

            return new DiceResult
            {
                Expression = $"{count}d{sides} keep lowest {keep}",
                Rolls = keptRolls,
                DroppedRolls = droppedRolls,
                Total = keptRolls.Sum()
            };
        }

        public static DiceResult RollKeepLowest(int count, int sides, int keep, Core.IRng rng)
        {
            if (keep > count)
                throw new ArgumentException("Cannot keep more dice than rolled");

            var rolls = RollMultiple(count, sides, rng);
            var keptRolls = rolls.OrderBy(x => x).Take(keep).ToList();
            var droppedRolls = rolls.Except(keptRolls).ToList();

            return new DiceResult
            {
                Expression = $"{count}d{sides} keep lowest {keep}",
                Rolls = keptRolls,
                DroppedRolls = droppedRolls,
                Total = keptRolls.Sum()
            };
        }

        /// <summary>
        /// Roll with exploding dice (roll again on max value)
        /// </summary>
        public static DiceResult RollExploding(int count, int sides)
        {
            var allRolls = new List<int>();
            
            for (int i = 0; i < count; i++)
            {
                var roll = RollSingle(sides);
                allRolls.Add(roll);
                
                // Keep rolling while we get max value
                while (roll == sides)
                {
                    roll = RollSingle(sides);
                    allRolls.Add(roll);
                }
            }

            return new DiceResult
            {
                Expression = $"{count}d{sides}!",
                Rolls = allRolls,
                Total = allRolls.Sum(),
                IsExploding = true
            };
        }

        public static DiceResult RollExploding(int count, int sides, Core.IRng rng)
        {
            var allRolls = new List<int>();

            for (int i = 0; i < count; i++)
            {
                var roll = RollSingle(sides, rng);
                allRolls.Add(roll);

                while (roll == sides)
                {
                    roll = RollSingle(sides, rng);
                    allRolls.Add(roll);
                }
            }

            return new DiceResult
            {
                Expression = $"{count}d{sides}!",
                Rolls = allRolls,
                Total = allRolls.Sum(),
                IsExploding = true
            };
        }

        private static DiceRollResult RollDice(string diceExpression)
        {
            var match = _diceRegex.Match(diceExpression);
            if (!match.Success)
                throw new ArgumentException($"Invalid dice expression: {diceExpression}");

            var countStr = match.Groups["count"].Value;
            var sidesStr = match.Groups["sides"].Value;
            var modifierStr = match.Groups["modifier"].Value;

            int count = string.IsNullOrEmpty(countStr) ? 1 : int.Parse(countStr);
            int sides = int.Parse(sidesStr);
            int modifier = string.IsNullOrEmpty(modifierStr) ? 0 : int.Parse(modifierStr);

            var rolls = RollMultiple(count, sides);
            return new DiceRollResult
            {
                Rolls = rolls,
                Total = rolls.Sum() + modifier,
                Modifier = modifier
            };
        }

        public static DiceResult Roll(string expression, Core.IRng rng)
        {
            if (string.IsNullOrWhiteSpace(expression))
                throw new ArgumentException("Dice expression cannot be null or empty");

            var result = new DiceResult { Expression = expression.Trim() };
            var totalValue = 0;

            var parts = expression.Split(new[] { '+', '-' }, StringSplitOptions.RemoveEmptyEntries);
            var operators = GetOperators(expression);

            for (int i = 0; i < parts.Length; i++)
            {
                var part = parts[i].Trim();
                var isNegative = i > 0 && operators[i - 1] == '-';

                if (_diceRegex.IsMatch(part))
                {
                    var diceRoll = RollDice(part, rng);
                    result.Rolls.AddRange(diceRoll.Rolls);
                    totalValue += isNegative ? -diceRoll.Total : diceRoll.Total;
                }
                else if (int.TryParse(part, out int modifier))
                {
                    totalValue += isNegative ? -modifier : modifier;
                    result.Modifiers += isNegative ? -modifier : modifier;
                }
            }

            result.Total = totalValue;
            return result;
        }

        private static DiceRollResult RollDice(string diceExpression, Core.IRng rng)
        {
            var match = _diceRegex.Match(diceExpression);
            if (!match.Success)
                throw new ArgumentException($"Invalid dice expression: {diceExpression}");

            var countStr = match.Groups["count"].Value;
            var sidesStr = match.Groups["sides"].Value;
            var modifierStr = match.Groups["modifier"].Value;

            int count = string.IsNullOrEmpty(countStr) ? 1 : int.Parse(countStr);
            int sides = int.Parse(sidesStr);
            int modifier = string.IsNullOrEmpty(modifierStr) ? 0 : int.Parse(modifierStr);

            var rolls = RollMultiple(count, sides, rng);
            return new DiceRollResult
            {
                Rolls = rolls,
                Total = rolls.Sum() + modifier,
                Modifier = modifier
            };
        }

        private static List<char> GetOperators(string expression)
        {
            var operators = new List<char>();
            for (int i = 0; i < expression.Length; i++)
            {
                if (expression[i] == '+' || expression[i] == '-')
                {
                    operators.Add(expression[i]);
                }
            }
            return operators;
        }

        private class DiceRollResult
        {
            public List<int> Rolls { get; set; } = new List<int>();
            public int Total { get; set; }
            public int Modifier { get; set; }
        }
    }

    /// <summary>
    /// Result of a dice roll operation
    /// </summary>
    public class DiceResult
    {
        public string Expression { get; set; } = "";
        public List<int> Rolls { get; set; } = new List<int>();
        public List<int> DroppedRolls { get; set; } = new List<int>();
        public int Total { get; set; }
        public int Modifiers { get; set; }
        public bool HasAdvantage { get; set; }
        public bool HasDisadvantage { get; set; }
        public bool IsExploding { get; set; }
        public DiceResult? AlternateRoll { get; set; }

        public override string ToString()
        {
            var result = $"{Expression} = {Total}";
            if (Rolls.Any())
            {
                result += $" [{string.Join(", ", Rolls)}]";
            }
            if (DroppedRolls.Any())
            {
                result += $" (dropped: [{string.Join(", ", DroppedRolls)}])";
            }
            if (HasAdvantage)
            {
                result += " [ADVANTAGE]";
            }
            if (HasDisadvantage)
            {
                result += " [DISADVANTAGE]";
            }
            return result;
        }
    }
}

