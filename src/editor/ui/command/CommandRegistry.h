#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Lightweight command registry for the in-viewport play-mode console.
// Usage:
//   CommandRegistry::Instance().Register("spawn", "spawn <primitive|prefab> [pos:(x,y,z)]", handler);

class CommandRegistry {
public:
    struct Command {
        std::string name;
        std::string help;
        std::function<void(const std::vector<std::string>&)> handler;
    };

    static CommandRegistry& Instance() {
        static CommandRegistry reg; return reg;
    }

    void Register(const std::string& name,
                  const std::string& help,
                  std::function<void(const std::vector<std::string>&)> handler) {
        Command c{ name, help, std::move(handler) };
        m_Commands[name] = std::move(c);
    }

    bool Execute(const std::string& line);

    // List of "name - help" strings for help output
    std::vector<std::string> GetHelp() const;

    // Install built-in commands
    void RegisterBuiltins();

private:
    CommandRegistry() = default;
    std::unordered_map<std::string, Command> m_Commands;
};



