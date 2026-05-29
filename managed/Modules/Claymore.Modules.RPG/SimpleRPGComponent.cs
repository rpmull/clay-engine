using ClaymoreEngine;
using System.Numerics;

namespace Claymore.Modules.RPG
{
    /// <summary>
    /// Simple RPG component to test the GetModuleComponent<T>() system
    /// </summary>
    [System.Obsolete("Use RPG/Actor component and the new Actor-centric RPG system.")]
    public class SimpleRPGComponent : ModuleComponentBase
    {
        // Basic RPG stats
        public int Health { get; set; } = 100;
        public int Mana { get; set; } = 50;
        public int Level { get; set; } = 1;
        public string CharacterName { get; set; } = "Hero";
        public Vector3 Position { get; set; } = Vector3.Zero;
        
        // Simple methods to test functionality
        public void TakeDamage(int damage)
        {
            Health = Math.Max(0, Health - damage);
            SyncToNative();
            
            if (Health <= 0)
            {
                Console.WriteLine($"{CharacterName} has been defeated!");
            }
        }
        
        public void Heal(int amount)
        {
            Health = Math.Min(100, Health + amount);
            SyncToNative();
            Console.WriteLine($"{CharacterName} healed for {amount}. Health: {Health}");
        }
        
        public void LevelUp()
        {
            Level++;
            Health = 100; // Full heal on level up
            Mana += 10;   // More mana per level
            SyncToNative();
            Console.WriteLine($"{CharacterName} reached level {Level}!");
        }
        
        protected override void OnInitialize()
        {
            Console.WriteLine($"[SimpleRPGComponent] {CharacterName} initialized - Level {Level}, Health {Health}");
        }
    }
    
    /// <summary>
    /// Test usage of the RPG component system
    /// </summary>
    public static class RPGTest
    {
        public static void RunTest()
        {
            Console.WriteLine("=== RPG Module Component Test ===");
            
            // Create entity and add RPG component
            var hero = Entity.Create("TestHero");
            var rpgComp = hero.AddModuleComponent<SimpleRPGComponent>();
            
            // Set some initial values
            rpgComp.CharacterName = "Sir Testington";
            rpgComp.Health = 80;
            rpgComp.Level = 5;
            
            Console.WriteLine($"Created {rpgComp.CharacterName} - Level {rpgComp.Level}, Health {rpgComp.Health}");
            
            // Test GetModuleComponent<T>() - should return same instance
            var sameComp = hero.GetModuleComponent<SimpleRPGComponent>();
            Console.WriteLine($"Retrieved component: Same instance? {ReferenceEquals(rpgComp, sameComp)}");
            Console.WriteLine($"Values preserved? Health={sameComp.Health}, Level={sameComp.Level}");
            
            // Test methods
            sameComp.TakeDamage(30);
            sameComp.Heal(15);
            sameComp.LevelUp();
            
            Console.WriteLine("✅ RPG Component test completed successfully!");
        }
    }
}

