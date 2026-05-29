#pragma once
#include <bgfx/bgfx.h>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <iostream>

enum class ShaderType
   {
   Vertex,
   Fragment,
   Compute
   };

class ShaderManager
   {
   public:
      static ShaderManager& Instance();

      bgfx::ShaderHandle LoadShader(const std::string& name, ShaderType type);
      bgfx::ProgramHandle LoadProgram(const std::string& vsName, const std::string& fsName);
      // Load a program from specific binary file paths (absolute or relative to project)
      bgfx::ProgramHandle LoadProgramFromPaths(const std::string& vsBinPath, const std::string& fsBinPath);
      // Load a unified program from shaders/meta/<Name>.json and compiled bins
      bgfx::ProgramHandle LoadProgramFromBundle(const std::string& baseName);
      void InvalidateProgram(const std::string& key);

      // Compile all shaders found in the executable's shaders directory if out-of-date or missing bin.
      void CompileAllShaders();

      bgfx::ShaderHandle CompileAndCache(const std::string& path, ShaderType type);

      // Check if a program handle is safe to use (valid and not destroyed)
      bool IsProgramSafe(bgfx::ProgramHandle program) const {
         if (!bgfx::isValid(program)) return false;
         std::lock_guard<std::mutex> lock(m_ProgramMutex);
         return m_ValidPrograms.count(program.idx) > 0;
      }
      
      // Register a program as valid (call after successful creation)
      void RegisterProgram(bgfx::ProgramHandle program, const std::string& name) {
         if (bgfx::isValid(program)) {
            std::lock_guard<std::mutex> lock(m_ProgramMutex);
            m_ValidPrograms.insert(program.idx);
            m_ProgramNames[program.idx] = name;
         }
      }
      
      // Get program name for debugging
      std::string GetProgramName(bgfx::ProgramHandle program) const {
         std::lock_guard<std::mutex> lock(m_ProgramMutex);
         auto it = m_ProgramNames.find(program.idx);
         return it != m_ProgramNames.end() ? it->second : "unknown";
      }
      
      // Unregister a program (call before destruction)
      void UnregisterProgram(bgfx::ProgramHandle program) {
         if (bgfx::isValid(program)) {
            std::lock_guard<std::mutex> lock(m_ProgramMutex);
            m_ValidPrograms.erase(program.idx);
            m_ProgramNames.erase(program.idx);
         }
      }

   private:
      ShaderManager() = default;

      bool CompileShader(const std::string& name, ShaderType type);

      std::atomic<bool> m_Watching{ false };
      std::thread m_WatchThread;

      mutable std::mutex m_ProgramMutex;
      std::unordered_map<std::string, bgfx::ProgramHandle> m_Programs;

      std::unordered_map<std::string, bgfx::ShaderHandle> m_ShaderCache; // name -> handle
      mutable std::mutex m_ShaderMutex;

      std::function<void(const std::string&)> m_ReloadCallback;
      
      // Track valid program indices to detect use-after-destroy
      mutable std::unordered_set<uint16_t> m_ValidPrograms;
      mutable std::unordered_map<uint16_t, std::string> m_ProgramNames;
   };
