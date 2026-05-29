#include "ShaderManager.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_set>
#include "core/vfs/FileSystem.h"
#include "ShaderBundle.h"

namespace fs = std::filesystem;

namespace {

std::string BuildPathProgramCacheKey(const std::string& vsBinPath, const std::string& fsBinPath)
{
    auto describePath = [](const std::string& rawPath) {
        std::error_code ec;
        fs::path normalized = fs::path(rawPath).lexically_normal();
        if (fs::exists(normalized, ec)) {
            fs::path canonical = fs::weakly_canonical(normalized, ec);
            if (!ec) {
                normalized = canonical;
            }
        }

        std::string description = normalized.generic_string();

        ec.clear();
        const auto fileSize = fs::file_size(normalized, ec);
        if (!ec) {
            description += "|size=" + std::to_string(fileSize);
        }

        ec.clear();
        const auto modifiedTime = fs::last_write_time(normalized, ec);
        if (!ec) {
            description += "|mtime=" + std::to_string(modifiedTime.time_since_epoch().count());
        }

        return description;
    };

    return describePath(vsBinPath) + "+" + describePath(fsBinPath);
}

fs::path ResolveVaryingDef(const fs::path& shaderSourcePath, ShaderType type, const fs::path& shadersDir)
{
    const fs::path perShaderVarying = shaderSourcePath.parent_path() / (shaderSourcePath.stem().string() + ".varying.def.sc");
    if (fs::exists(perShaderVarying)) {
        return perShaderVarying;
    }

    if (type == ShaderType::Compute) {
        return {};
    }

    const fs::path sharedVarying = shadersDir / "varying.def.sc";
    if (fs::exists(sharedVarying)) {
        return sharedVarying;
    }

    return {};
}

std::string TrimShaderLine(const std::string& line)
{
    const size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
}

bool TryExtractShaderInclude(const std::string& line, std::string& outIncludePath)
{
    const std::string trimmed = TrimShaderLine(line);
    if (trimmed.rfind("#include", 0) != 0) {
        return false;
    }

    const size_t quoteBegin = trimmed.find('"');
    if (quoteBegin != std::string::npos) {
        const size_t quoteEnd = trimmed.find('"', quoteBegin + 1);
        if (quoteEnd != std::string::npos && quoteEnd > quoteBegin + 1) {
            outIncludePath = trimmed.substr(quoteBegin + 1, quoteEnd - quoteBegin - 1);
            return true;
        }
    }

    const size_t angleBegin = trimmed.find('<');
    if (angleBegin != std::string::npos) {
        const size_t angleEnd = trimmed.find('>', angleBegin + 1);
        if (angleEnd != std::string::npos && angleEnd > angleBegin + 1) {
            outIncludePath = trimmed.substr(angleBegin + 1, angleEnd - angleBegin - 1);
            return true;
        }
    }

    return false;
}

fs::path NormalizeExistingPath(const fs::path& path)
{
    std::error_code ec;
    if (fs::exists(path, ec)) {
        fs::path canonical = fs::weakly_canonical(path, ec);
        if (!ec) {
            return canonical;
        }
    }
    return path.lexically_normal();
}

bool TryResolveShaderIncludePath(const fs::path& includingFile,
                                 const std::string& includePath,
                                 const std::vector<fs::path>& includeDirs,
                                 fs::path& outResolvedPath)
{
    std::error_code ec;
    const fs::path includeRelative(includePath);

    const fs::path localCandidate = includingFile.parent_path() / includeRelative;
    if (fs::exists(localCandidate, ec)) {
        outResolvedPath = NormalizeExistingPath(localCandidate);
        return true;
    }

    for (const fs::path& includeDir : includeDirs) {
        ec.clear();
        const fs::path candidate = includeDir / includeRelative;
        if (fs::exists(candidate, ec)) {
            outResolvedPath = NormalizeExistingPath(candidate);
            return true;
        }
    }

    return false;
}

fs::file_time_type ResolveShaderDependencyTimestampRecursive(
    const fs::path& filePath,
    const std::vector<fs::path>& includeDirs,
    std::unordered_set<std::string>& visited)
{
    const fs::path normalizedPath = NormalizeExistingPath(filePath);
    const std::string visitedKey = normalizedPath.generic_string();
    if (!visited.insert(visitedKey).second) {
        return fs::file_time_type::min();
    }

    std::error_code ec;
    fs::file_time_type latestWriteTime = fs::last_write_time(normalizedPath, ec);
    if (ec) {
        return fs::file_time_type::min();
    }

    std::ifstream input(normalizedPath);
    if (!input.is_open()) {
        return latestWriteTime;
    }

    std::string line;
    while (std::getline(input, line)) {
        std::string includePath;
        if (!TryExtractShaderInclude(line, includePath)) {
            continue;
        }

        fs::path resolvedIncludePath;
        if (!TryResolveShaderIncludePath(normalizedPath, includePath, includeDirs, resolvedIncludePath)) {
            continue;
        }

        const fs::file_time_type includeWriteTime =
            ResolveShaderDependencyTimestampRecursive(resolvedIncludePath, includeDirs, visited);
        if (includeWriteTime > latestWriteTime) {
            latestWriteTime = includeWriteTime;
        }
    }

    return latestWriteTime;
}

fs::file_time_type ResolveShaderDependencyTimestamp(
    const fs::path& shaderSourcePath,
    const fs::path& varyingFile,
    const std::vector<fs::path>& includeDirs)
{
    std::unordered_set<std::string> visited;
    fs::file_time_type latestWriteTime =
        ResolveShaderDependencyTimestampRecursive(shaderSourcePath, includeDirs, visited);

    std::error_code ec;
    if (!varyingFile.empty() && fs::exists(varyingFile, ec)) {
        const fs::file_time_type varyingWriteTime =
            ResolveShaderDependencyTimestampRecursive(varyingFile, includeDirs, visited);
        if (varyingWriteTime > latestWriteTime) {
            latestWriteTime = varyingWriteTime;
        }
    }

    return latestWriteTime;
}

} // namespace

ShaderManager& ShaderManager::Instance()
   {
   static ShaderManager instance;
   return instance;
   }

static std::string GetBackendFolder(bgfx::RendererType::Enum renderer)
   {
   switch (renderer)
      {
         case bgfx::RendererType::Direct3D11:
         case bgfx::RendererType::Direct3D12: return "windows";
         case bgfx::RendererType::OpenGL: return "opengl";
         case bgfx::RendererType::Vulkan: return "vulkan";
         case bgfx::RendererType::Metal: return "metal";
         default: return "unknown";
      }
   }

bool ShaderManager::CompileShader(const std::string& name, ShaderType type)
   {
   // In packaged runtime, skip compilation; we rely on precompiled bins in the pak
   if (FileSystem::Instance().IsPakMounted()) {
       return true;
   }
   fs::path exeDir = fs::current_path();  // e.g., build/Release
   fs::path shadersDir = exeDir / "shaders";
   fs::path toolsDir   = exeDir / "tools";

   fs::path shaderSrc = shadersDir / (name + ".sc");
   fs::path outFolder = shadersDir / "compiled" / "windows"; // For now: windows
   fs::path shaderOut = outFolder / (name + ".bin");

   // Create all necessary folders
   fs::create_directories(shaderOut.parent_path());

   if (!fs::exists(shaderSrc)) {
      std::cerr << "[ShaderManager] Source not found: " << shaderSrc << std::endl;
      return false;
      }

   fs::path varyingFile = ResolveVaryingDef(shaderSrc, type, shadersDir);
   fs::path bgfxInc = exeDir;
   for (int i = 0; i < 12 && !fs::exists(bgfxInc / "external/bgfx/src/bgfx_shader.sh"); ++i) {
      bgfxInc = bgfxInc.parent_path();
   }
   bgfxInc /= "external/bgfx/src";

   std::vector<fs::path> includeDirs;
   includeDirs.push_back(shadersDir);
   includeDirs.push_back(shadersDir / "include");
   includeDirs.push_back(bgfxInc);

   if (fs::exists(shaderOut)) {
      std::error_code ec;
      const auto binTime = fs::last_write_time(shaderOut, ec);
      if (!ec) {
         const auto latestDependencyTime =
            ResolveShaderDependencyTimestamp(shaderSrc, varyingFile, includeDirs);
         if (latestDependencyTime != fs::file_time_type::min() &&
             binTime > latestDependencyTime) {
            return true; // Up-to-date, including shared include dependencies.
         }
      }
   }

   std::string shaderTypeStr = (type == ShaderType::Vertex) ? "vertex" :
      (type == ShaderType::Fragment) ? "fragment" : "compute";

   std::string profile = "s_5_0"; // DX11 default
   std::string cmd = "\"" + (toolsDir / "shaderc.exe").string() + "\""
      + " -f " + shaderSrc.string()
      + " -o " + shaderOut.string()
      + " --type " + shaderTypeStr
      + " --platform windows"
      + " --profile " + profile
      + " -i " + shadersDir.string()
      + " -i " + (shadersDir / "include").string();
   if (!varyingFile.empty()) {
      cmd += " --varyingdef " + varyingFile.string();
   }
   cmd += " -i " + bgfxInc.string();

   std::cout << "[ShaderManager] Compiling: " << cmd << std::endl;

   int result = system(cmd.c_str());
   if (result != 0) {
      std::cerr << "[ShaderManager] Failed to compile shader: " << shaderSrc << std::endl;
      return false;
      }
   return true;
   }

static bgfx::ShaderHandle CreateShaderFromFile(const fs::path& path)
   {
   std::vector<uint8_t> data;
   if (!FileSystem::Instance().ReadFile(path.string(), data)) {
       std::cerr << "[ShaderManager] Failed to read shader: \"" << path.string() << "\"" << std::endl;
       return BGFX_INVALID_HANDLE;
   }
   const bgfx::Memory* mem = bgfx::alloc(uint32_t(data.size() + 1));
   if (!data.empty()) memcpy(mem->data, data.data(), data.size());
   mem->data[data.size()] = '\0';

   return bgfx::createShader(mem);
   }

bgfx::ShaderHandle ShaderManager::LoadShader(const std::string& name, ShaderType type)
   {
   fs::path exeDir = fs::current_path();
   fs::path shaderOut;
   
   // Check if we're in packaged/runtime mode (PAK mounted via VFS or FileSystem)
   bool isRuntimeMode = FileSystem::Instance().IsPakMounted();
   
   if (isRuntimeMode) {
       // Packaged runtime mode: try multiple paths to find shader binaries
       std::vector<std::string> candidates = {
           // Virtual paths (for PAK files)
           "shaders/compiled/windows/" + name + ".bin",
           "shaders/" + name + ".bin",
           // Absolute paths (fallback)
           (exeDir / "shaders" / "compiled" / "windows" / (name + ".bin")).string(),
           (exeDir / "shaders" / (name + ".bin")).string()
       };
       
       for (const auto& c : candidates) {
           std::vector<uint8_t> data;
           if (FileSystem::Instance().ReadFile(c, data) && !data.empty()) {
               std::cout << "[ShaderManager] Found shader: " << c << " (" << data.size() << " bytes)" << std::endl;
               
               // Create shader directly from memory
               const bgfx::Memory* mem = bgfx::alloc(uint32_t(data.size() + 1));
               memcpy(mem->data, data.data(), data.size());
               mem->data[data.size()] = '\0';
               
               bgfx::ShaderHandle handle = bgfx::createShader(mem);
               if (bgfx::isValid(handle)) {
                   return handle;
               }
           }
       }
       
       std::cerr << "[ShaderManager] ERROR: Failed to find shader '" << name << "' in PAK!" << std::endl;
       std::cerr << "[ShaderManager] Tried paths:" << std::endl;
       for (const auto& c : candidates) {
           std::cerr << "[ShaderManager]   - " << c << std::endl;
       }
       return BGFX_INVALID_HANDLE;
   }
   
   // Editor mode: compile shader if needed
   shaderOut = exeDir / "shaders" / "compiled" / "windows" / (name + ".bin");
   if (!CompileShader(name, type)) {
       return BGFX_INVALID_HANDLE;
   }
   return CreateShaderFromFile(shaderOut);
   }

bgfx::ProgramHandle ShaderManager::LoadProgram(const std::string& vsName, const std::string& fsName)
   {
   // Check cache first
   std::string key = vsName + "+" + fsName;
   {
      std::lock_guard<std::mutex> lock(m_ProgramMutex);
      auto it = m_Programs.find(key);
      if (it != m_Programs.end() && bgfx::isValid(it->second)) {
         return it->second;
      }
   }
   
   // Not cached, load shaders
   bgfx::ShaderHandle vsh = LoadShader(vsName, ShaderType::Vertex);
   bgfx::ShaderHandle fsh = LoadShader(fsName, ShaderType::Fragment);

   if (!bgfx::isValid(vsh)) {
      printf("Vertex Shader Invalid - %s\n", vsName.c_str());
      }
   if (!bgfx::isValid(fsh)) {
      printf("Fragment Shader Invalid - %s\n", fsName.c_str());
      }

   if (!bgfx::isValid(vsh) || !bgfx::isValid(fsh)) {
      if (bgfx::isValid(vsh)) {
         bgfx::destroy(vsh);
      }
      if (bgfx::isValid(fsh)) {
         bgfx::destroy(fsh);
      }
      return BGFX_INVALID_HANDLE;
      }

   bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, false);
   bgfx::destroy(vsh);
   bgfx::destroy(fsh);
   if (!bgfx::isValid(program)) {
      std::cerr << "[ShaderManager] Failed to create program: " << key << std::endl;
      return BGFX_INVALID_HANDLE;
   }
   {
   std::lock_guard<std::mutex> lock(m_ProgramMutex);
   m_Programs[key] = program;
   }
   RegisterProgram(program, key);
   std::cout << "[ShaderManager] Loaded program: " << key << " (idx=" << program.idx << ")" << std::endl;
   return program;
   }

bgfx::ProgramHandle ShaderManager::LoadProgramFromBundle(const std::string& baseName)
{
    return ShaderBundle::Instance().Load(baseName);
}

bgfx::ProgramHandle ShaderManager::LoadProgramFromPaths(const std::string& vsBinPath, const std::string& fsBinPath)
{
    // Check cache first
    const std::string cacheKey = BuildPathProgramCacheKey(vsBinPath, fsBinPath);
    {
        std::lock_guard<std::mutex> lock(m_ProgramMutex);
        auto it = m_Programs.find(cacheKey);
        if (it != m_Programs.end() && bgfx::isValid(it->second)) {
            return it->second;
        }
    }
    
    // Load shaders from the specified paths
    bgfx::ShaderHandle vsh = CreateShaderFromFile(fs::path(vsBinPath));
    bgfx::ShaderHandle fsh = CreateShaderFromFile(fs::path(fsBinPath));
    
    if (!bgfx::isValid(vsh)) {
        std::cerr << "[ShaderManager] Failed to load vertex shader from: " << vsBinPath << std::endl;
        if (bgfx::isValid(fsh)) {
            bgfx::destroy(fsh);
        }
        return BGFX_INVALID_HANDLE;
    }
    if (!bgfx::isValid(fsh)) {
        std::cerr << "[ShaderManager] Failed to load fragment shader from: " << fsBinPath << std::endl;
        bgfx::destroy(vsh);
        return BGFX_INVALID_HANDLE;
    }
    
    bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh, false);
    bgfx::destroy(vsh);
    bgfx::destroy(fsh);
    if (!bgfx::isValid(program)) {
        std::cerr << "[ShaderManager] Failed to create program from paths: " << cacheKey << std::endl;
        return BGFX_INVALID_HANDLE;
    }
    {
        std::lock_guard<std::mutex> lock(m_ProgramMutex);
        m_Programs[cacheKey] = program;
    }
    RegisterProgram(program, cacheKey);
    std::cout << "[ShaderManager] Loaded program from paths: " << cacheKey << " (idx=" << program.idx << ")" << std::endl;
    return program;
}

void ShaderManager::InvalidateProgram(const std::string& key)
{
    // For legacy programs we keep m_Programs map; for bundles we forward to ShaderBundle
    {
        std::lock_guard<std::mutex> lock(m_ProgramMutex);
        auto it = m_Programs.find(key);
        if (it != m_Programs.end()) {
            if (bgfx::isValid(it->second)) {
                std::cout << "[ShaderManager] Invalidating program: " << key << " (idx=" << it->second.idx << ")" << std::endl;
                UnregisterProgram(it->second);
                bgfx::destroy(it->second);
            }
            m_Programs.erase(it);
        }
    }
    ShaderBundle::Instance().Invalidate(key);
}

// ------------------- New: CompileAllShaders -------------------
void ShaderManager::CompileAllShaders()
{
    if (FileSystem::Instance().IsPakMounted()) return;
    fs::path exeDir = fs::current_path();
    fs::path shadersDir = exeDir / "shaders";

    // Mirror source shaders into runtime dir if running from build output
    {
        fs::path src = exeDir;
        for (int i=0; i<5 && !fs::exists(src / "shaders"); ++i)
            src = src.parent_path();
        src = src / "shaders";
        if (fs::exists(src) && src != shadersDir) {
            for (auto& entry : fs::recursive_directory_iterator(src)) {
                if (!entry.is_regular_file()) continue;
                fs::path rel = fs::relative(entry.path(), src);
                fs::path dst = shadersDir / rel;
                fs::create_directories(dst.parent_path());
                try { fs::copy_file(entry.path(), dst, fs::copy_options::overwrite_existing); }
                catch(...){}
            }
        }
    }

    if (!fs::exists(shadersDir))
        return;

    // Ensure varying.def.sc has no UTF-8 BOM
    {
        fs::path varying = shadersDir / "varying.def.sc";
        if (fs::exists(varying)) {
            std::ifstream in(varying, std::ios::binary);
            std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (contents.size() >= 3 && (uint8_t)contents[0] == 0xEF && (uint8_t)contents[1] == 0xBB && (uint8_t)contents[2] == 0xBF) {
                std::cout << "[ShaderManager] Stripping BOM from varying.def.sc\n";
                contents.erase(0, 3);
                std::ofstream out(varying, std::ios::binary | std::ios::trunc);
                out.write(contents.data(), contents.size());
            }
        }
    }

    for (auto& entry : fs::recursive_directory_iterator(shadersDir))
    {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".sc") continue;
        if (entry.path().filename() == "varying.def.sc") continue;

        std::string stem = entry.path().stem().string();

        ShaderType type = ShaderType::Fragment; // default
        if (stem.rfind("vs_", 0) == 0) {
            type = ShaderType::Vertex;
        }
        else if (stem.rfind("fs_", 0) == 0 || stem.rfind("ps_", 0) == 0) {
            type = ShaderType::Fragment;
        }
        else if (stem.rfind("cs_", 0) == 0) {
            type = ShaderType::Compute;
        }

        CompileShader(stem, type);
    }
}

bgfx::ShaderHandle ShaderManager::CompileAndCache(const std::string& path, ShaderType type) {
    // existing implementation unchanged ... (placed here for brevity)
    std::lock_guard<std::mutex> lock(m_ShaderMutex);
    std::string shaderName = fs::path(path).stem().string();
    auto it = m_ShaderCache.find(shaderName);
    if (it != m_ShaderCache.end() && bgfx::isValid(it->second))
        return it->second;

    fs::path exeDir = fs::current_path();
    fs::path shadersDir = exeDir / "shaders";
    fs::path toolsDir   = exeDir / "tools";
    fs::path compiledDir= shadersDir / "compiled" / "windows";
    fs::path shaderOut  = compiledDir / (shaderName + ".bin");
    fs::create_directories(shaderOut.parent_path());

    bool needsCompile = true;
    fs::path varyingFile = ResolveVaryingDef(fs::path(path), type, shadersDir);
    if (fs::exists(shaderOut) && fs::exists(path)) {
        needsCompile = fs::last_write_time(shaderOut) < fs::last_write_time(path);
        if (!needsCompile && fs::exists(varyingFile)) {
            needsCompile = fs::last_write_time(shaderOut) < fs::last_write_time(varyingFile);
        }
    }

    if (needsCompile) {
        std::string shaderTypeStr = (type == ShaderType::Vertex) ? "vertex" :
                                    (type == ShaderType::Fragment) ? "fragment" : "compute";
        std::string profile = "s_5_0";
        std::string cmd = "\"" + (toolsDir / "shaderc.exe").string() + "\"";
        cmd += " -f " + path;
        cmd += " -o " + shaderOut.string();
        cmd += " --type " + shaderTypeStr;
        cmd += " --platform windows";
        cmd += " --profile " + profile;
        if (!varyingFile.empty()) {
            cmd += " --varyingdef " + varyingFile.string();
        }
        cmd += " -i " + shadersDir.string();
        cmd += " -i " + (shadersDir / "include").string();
    // Add bgfx built-in shader include path
    fs::path bgfxInc = exeDir;
    for(int i=0;i<12 && !fs::exists(bgfxInc / "external/bgfx/src/bgfx_shader.sh"); ++i)
        bgfxInc = bgfxInc.parent_path();
    bgfxInc /= "external/bgfx/src";
    cmd += " -i " + bgfxInc.string();
        std::cout << "[ShaderManager] Compiling shader: " << path << std::endl;
        if (system(cmd.c_str()) != 0) {
            std::cerr << "[ShaderManager] Failed to compile: " << path << std::endl;
            return BGFX_INVALID_HANDLE;
        }
    }

    bgfx::ShaderHandle handle = CreateShaderFromFile(shaderOut);
    if (bgfx::isValid(handle)) {
        m_ShaderCache[shaderName] = handle;
    }
    return handle;
}
