#include "ResourceManifest.h"
#include "core/vfs/FileSystem.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

static uint64_t Fnv1a64(const std::string& data, uint64_t seed) {
    uint64_t hash = 14695981039346656037ULL ^ seed;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static ClaymoreGUID DeterministicGuidFromPath(const fs::path& path) {
    std::string normalized = path.lexically_normal().generic_string();
    if (normalized.empty()) {
        normalized = path.generic_string();
    }

    std::string lower;
    lower.reserve(normalized.size());
    for (char c : normalized) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    uint64_t high = Fnv1a64(lower, 0);
    uint64_t low = Fnv1a64(lower, 0x9e3779b97f4a7c15ULL);
    return ClaymoreGUID(high, low);
}

ClaymoreGUID ResourceManifest::DeterministicGuidFromPath(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty()) return ClaymoreGUID();

    std::string lower;
    lower.reserve(normalized.size());
    for (char c : normalized) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    uint64_t high = Fnv1a64(lower, 0);
    uint64_t low = Fnv1a64(lower, 0x9e3779b97f4a7c15ULL);
    return ClaymoreGUID(high, low);
}

// Case-insensitive string comparison helper
static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != 
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::filesystem::path ResourceManifest::FindResourcesFolder(const fs::path& projectRoot) const {
    // Case-insensitive search for "resources" folder
    std::error_code ec;
    if (!fs::exists(projectRoot, ec)) return {};
    
    for (auto& entry : fs::directory_iterator(projectRoot, ec)) {
        if (!entry.is_directory()) continue;
        
        std::string folderName = entry.path().filename().string();
        if (EqualsIgnoreCase(folderName, "resources")) {
            return entry.path();
        }
    }
    
    return {};
}

bool ResourceManifest::Initialize(const fs::path& projectRoot) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    m_ProjectRoot = projectRoot;
    m_Initialized = true;
    
    // Try to find resources folder (case-insensitive)
    m_ResourcesPath = FindResourcesFolder(projectRoot);
    m_ResourcesFolderExists = !m_ResourcesPath.empty();
    
    if (m_ResourcesFolderExists) {
        std::cout << "[ResourceManifest] Found resources folder: " << m_ResourcesPath << std::endl;
    } else {
        std::cout << "[ResourceManifest] No resources folder found in: " << projectRoot << std::endl;
        std::cout << "[ResourceManifest] Resources system will activate when 'resources' folder is created." << std::endl;
    }
    
    return m_ResourcesFolderExists;
}

bool ResourceManifest::TryDiscoverResourcesFolder(const fs::path& projectRoot) {
    if (m_ResourcesFolderExists) return true; // Already discovered
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    m_ProjectRoot = projectRoot;
    m_ResourcesPath = FindResourcesFolder(projectRoot);
    m_ResourcesFolderExists = !m_ResourcesPath.empty();
    
    if (m_ResourcesFolderExists) {
        m_Initialized = true;
        std::cout << "[ResourceManifest] Discovered resources folder: " << m_ResourcesPath << std::endl;
        return true;
    }
    
    return false;
}

int ResourceManifest::Scan() {
    if (!m_Initialized) {
        std::cerr << "[ResourceManifest] Not initialized!" << std::endl;
        return 0;
    }
    
    // Check if resources folder exists now (might have been created)
    if (!m_ResourcesFolderExists) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_ResourcesPath = FindResourcesFolder(m_ProjectRoot);
        m_ResourcesFolderExists = !m_ResourcesPath.empty();
        
        if (!m_ResourcesFolderExists) {
            return 0; // No resources folder yet
        }
        std::cout << "[ResourceManifest] Resources folder now exists: " << m_ResourcesPath << std::endl;
    }
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Track old resources for change detection
    std::unordered_set<ClaymoreGUID> oldGuids;
    for (const auto& [guid, entry] : m_Resources) {
        oldGuids.insert(guid);
    }
    
    // Clear existing data
    m_Resources.clear();
    m_TypeIndex.clear();
    m_NameIndex.clear();
    m_PathIndex.clear();
    
    std::error_code ec;
    if (!fs::exists(m_ResourcesPath, ec)) {
        m_ResourcesFolderExists = false;
        std::cout << "[ResourceManifest] Resources folder no longer exists." << std::endl;
        return 0;
    }
    
    int count = 0;
    std::vector<ResourceChangeEvent> changes;
    
    for (auto& entry : fs::recursive_directory_iterator(m_ResourcesPath, ec)) {
        if (!entry.is_regular_file()) continue;
        
        // Only process .clayobj files
        if (entry.path().extension() != ".clayobj") continue;
        
        ResourceEntry resource;
        if (ParseClayObject(entry.path(), resource)) {
            // Store in primary map
            m_Resources[resource.guid] = resource;
            
            // Build indices
            m_TypeIndex[resource.typeName].push_back(resource.guid);
            m_NameIndex[resource.typeName + ":" + resource.name] = resource.guid;
            
            // Normalize path for lookup
            std::string normalizedPath = fs::weakly_canonical(entry.path(), ec).string();
            std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
            m_PathIndex[normalizedPath] = resource.guid;
            
            // Check if this is a new resource
            if (oldGuids.find(resource.guid) == oldGuids.end()) {
                // New resource - queue add event if type is subscribed
                if (m_SubscribedTypes.find(resource.typeName) != m_SubscribedTypes.end()) {
                    ResourceChangeEvent evt;
                    evt.type = ResourceChangeType::Added;
                    evt.typeName = resource.typeName;
                    evt.name = resource.name;
                    evt.guid = resource.guid;
                    changes.push_back(evt);
                }
            } else {
                oldGuids.erase(resource.guid);
            }
            
            count++;
        }
    }
    
    // Any remaining oldGuids were removed
    for (const auto& removedGuid : oldGuids) {
        // We don't have the entry anymore, but we can check subscribed types
        // This is a limitation - we'd need to track the old entries to emit proper remove events
    }
    
    // Queue the changes
    if (!changes.empty()) {
        std::lock_guard<std::mutex> changeLock(m_ChangeMutex);
        m_PendingChanges.insert(m_PendingChanges.end(), changes.begin(), changes.end());
    }
    
    std::cout << "[ResourceManifest] Scan complete. Found " << count << " resources." << std::endl;
    return count;
}

bool ResourceManifest::RefreshFile(const std::string& path) {
    fs::path filePath(path);
    if (filePath.extension() != ".clayobj") return false;
    
    // Check if this is in the resources folder
    if (!IsResourcePath(path)) return false;
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Normalize path
    std::error_code ec;
    std::string normalizedPath = fs::weakly_canonical(filePath, ec).string();
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
    
    bool wasExisting = false;
    ResourceEntry oldEntry;
    
    // Check if this resource already exists
    auto pathIt = m_PathIndex.find(normalizedPath);
    if (pathIt != m_PathIndex.end()) {
        wasExisting = true;
        ClaymoreGUID oldGuid = pathIt->second;
        auto resIt = m_Resources.find(oldGuid);
        if (resIt != m_Resources.end()) {
            oldEntry = resIt->second;
            
            // Remove from type index
            auto& typeVec = m_TypeIndex[resIt->second.typeName];
            typeVec.erase(std::remove(typeVec.begin(), typeVec.end(), oldGuid), typeVec.end());
            
            // Remove from name index
            m_NameIndex.erase(resIt->second.typeName + ":" + resIt->second.name);
            
            // Remove from primary storage
            m_Resources.erase(resIt);
        }
        m_PathIndex.erase(pathIt);
    }
    
    // Re-parse if file still exists
    if (!fs::exists(filePath, ec)) {
        // File was deleted
        if (wasExisting && m_SubscribedTypes.find(oldEntry.typeName) != m_SubscribedTypes.end()) {
            ResourceChangeEvent evt;
            evt.type = ResourceChangeType::Removed;
            evt.typeName = oldEntry.typeName;
            evt.name = oldEntry.name;
            evt.guid = oldEntry.guid;
            
            std::lock_guard<std::mutex> changeLock(m_ChangeMutex);
            m_PendingChanges.push_back(evt);
        }
        return wasExisting;
    }
    
    ResourceEntry resource;
    if (ParseClayObject(filePath, resource)) {
        m_Resources[resource.guid] = resource;
        m_TypeIndex[resource.typeName].push_back(resource.guid);
        m_NameIndex[resource.typeName + ":" + resource.name] = resource.guid;
        m_PathIndex[normalizedPath] = resource.guid;
        
        // Emit change event
        if (m_SubscribedTypes.find(resource.typeName) != m_SubscribedTypes.end()) {
            ResourceChangeEvent evt;
            evt.type = wasExisting ? ResourceChangeType::Modified : ResourceChangeType::Added;
            evt.typeName = resource.typeName;
            evt.name = resource.name;
            evt.guid = resource.guid;
            
            std::lock_guard<std::mutex> changeLock(m_ChangeMutex);
            m_PendingChanges.push_back(evt);
        }
        
        std::cout << "[ResourceManifest] " << (wasExisting ? "Updated" : "Added") 
                  << " resource: " << resource.name << std::endl;
        return true;
    }
    
    return false;
}

bool ResourceManifest::RemoveFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Normalize path
    std::error_code ec;
    std::string normalizedPath = fs::weakly_canonical(fs::path(path), ec).string();
    std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
    
    auto pathIt = m_PathIndex.find(normalizedPath);
    if (pathIt == m_PathIndex.end()) return false;
    
    ClaymoreGUID guid = pathIt->second;
    auto resIt = m_Resources.find(guid);
    if (resIt != m_Resources.end()) {
        const ResourceEntry& entry = resIt->second;
        
        // Emit remove event if type is subscribed
        if (m_SubscribedTypes.find(entry.typeName) != m_SubscribedTypes.end()) {
            ResourceChangeEvent evt;
            evt.type = ResourceChangeType::Removed;
            evt.typeName = entry.typeName;
            evt.name = entry.name;
            evt.guid = entry.guid;
            
            std::lock_guard<std::mutex> changeLock(m_ChangeMutex);
            m_PendingChanges.push_back(evt);
        }
        
        // Remove from type index
        auto& typeVec = m_TypeIndex[entry.typeName];
        typeVec.erase(std::remove(typeVec.begin(), typeVec.end(), guid), typeVec.end());
        
        // Remove from name index
        m_NameIndex.erase(entry.typeName + ":" + entry.name);
        
        std::cout << "[ResourceManifest] Removed resource: " << entry.name << std::endl;
        
        // Remove from primary storage
        m_Resources.erase(resIt);
    }
    m_PathIndex.erase(pathIt);
    return true;
}

std::vector<const ResourceEntry*> ResourceManifest::GetResourcesByType(const std::string& typeName) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::vector<const ResourceEntry*> result;
    
    auto it = m_TypeIndex.find(typeName);
    if (it == m_TypeIndex.end()) return result;
    
    for (const auto& guid : it->second) {
        auto resIt = m_Resources.find(guid);
        if (resIt != m_Resources.end()) {
            result.push_back(&resIt->second);
        }
    }
    
    return result;
}

const ResourceEntry* ResourceManifest::GetResource(const std::string& typeName, const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_NameIndex.find(typeName + ":" + name);
    if (it == m_NameIndex.end()) return nullptr;
    
    auto resIt = m_Resources.find(it->second);
    if (resIt == m_Resources.end()) return nullptr;
    
    return &resIt->second;
}

const ResourceEntry* ResourceManifest::GetResourceByGUID(const ClaymoreGUID& guid) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_Resources.find(guid);
    if (it == m_Resources.end()) return nullptr;
    
    return &it->second;
}

std::vector<const ResourceEntry*> ResourceManifest::GetAllResources() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::vector<const ResourceEntry*> result;
    result.reserve(m_Resources.size());
    
    for (const auto& [guid, entry] : m_Resources) {
        result.push_back(&entry);
    }
    
    return result;
}

std::vector<ClaymoreGUID> ResourceManifest::GetAllDependencies() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::unordered_set<ClaymoreGUID> uniqueDeps;
    
    for (const auto& [guid, entry] : m_Resources) {
        for (const auto& dep : entry.dependencies) {
            uniqueDeps.insert(dep);
        }
    }
    
    return std::vector<ClaymoreGUID>(uniqueDeps.begin(), uniqueDeps.end());
}

bool ResourceManifest::IsResourcePath(const std::string& path) const {
    if (!m_Initialized || !m_ResourcesFolderExists) return false;
    
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::path(path), ec);
    fs::path resourcesCanonical = fs::weakly_canonical(m_ResourcesPath, ec);
    
    // Check if path starts with resources path
    auto [resEnd, pathEnd] = std::mismatch(
        resourcesCanonical.begin(), resourcesCanonical.end(),
        canonical.begin(), canonical.end());
    
    return resEnd == resourcesCanonical.end();
}

bool ResourceManifest::SaveToFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    try {
        json j;
        j["version"] = 1;
        j["resources"] = json::array();
        
        for (const auto& [guid, entry] : m_Resources) {
            json res;
            res["guid"] = guid.ToString();
            res["path"] = entry.path;
            res["name"] = entry.name;
            res["typeName"] = entry.typeName;
            
            json deps = json::array();
            for (const auto& dep : entry.dependencies) {
                deps.push_back(dep.ToString());
            }
            res["dependencies"] = deps;
            
            j["resources"].push_back(res);
        }
        
        std::ofstream out(path);
        if (!out.is_open()) return false;
        out << j.dump(2);
        
        std::cout << "[ResourceManifest] Saved manifest to: " << path << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ResourceManifest] Failed to save manifest: " << e.what() << std::endl;
        return false;
    }
}

bool ResourceManifest::LoadFromFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    try {
        std::string text;
        if (!FileSystem::Instance().ReadTextFile(path, text)) {
            if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
                return false;
            }
            std::ifstream in(path);
            if (!in.is_open()) return false;
            std::stringstream ss;
            ss << in.rdbuf();
            text = ss.str();
        }
        
        json j = json::parse(text);
        
        // Clear existing data
        m_Resources.clear();
        m_TypeIndex.clear();
        m_NameIndex.clear();
        m_PathIndex.clear();
        
        if (!j.contains("resources")) return false;
        
        for (const auto& res : j["resources"]) {
            ResourceEntry entry;
            entry.guid = ClaymoreGUID::FromString(res["guid"].get<std::string>());
            entry.path = res["path"].get<std::string>();
            entry.name = res["name"].get<std::string>();
            entry.typeName = res["typeName"].get<std::string>();
            
            if (res.contains("dependencies")) {
                for (const auto& dep : res["dependencies"]) {
                    entry.dependencies.push_back(ClaymoreGUID::FromString(dep.get<std::string>()));
                }
            }
            
            m_Resources[entry.guid] = entry;
            m_TypeIndex[entry.typeName].push_back(entry.guid);
            m_NameIndex[entry.typeName + ":" + entry.name] = entry.guid;
            
            // Normalize path for lookup
            std::string normalizedPath = entry.path;
            std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
            m_PathIndex[normalizedPath] = entry.guid;
        }
        
        m_Initialized = true;
        m_ResourcesFolderExists = true; // If loading from file, assume resources exist
        std::cout << "[ResourceManifest] Loaded " << m_Resources.size() << " resources from manifest." << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ResourceManifest] Failed to load manifest: " << e.what() << std::endl;
        return false;
    }
}

bool ResourceManifest::LoadFromJson(const std::string& jsonString) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    try {
        json j = json::parse(jsonString);
        
        // Clear existing data
        m_Resources.clear();
        m_TypeIndex.clear();
        m_NameIndex.clear();
        m_PathIndex.clear();
        
        if (!j.contains("resources")) {
            m_Initialized = true;
            m_ResourcesFolderExists = true;
            return true; // Not an error, just no resources
        }
        
        for (const auto& res : j["resources"]) {
            if (!res.contains("guid") || !res.contains("name") || !res.contains("typeName") || !res.contains("path")) {
                continue;
            }
            
            ResourceEntry entry;
            entry.guid = ClaymoreGUID::FromString(res["guid"].get<std::string>());
            entry.path = res["path"].get<std::string>();
            entry.name = res["name"].get<std::string>();
            entry.typeName = res["typeName"].get<std::string>();
            
            if (res.contains("dependencies")) {
                for (const auto& dep : res["dependencies"]) {
                    entry.dependencies.push_back(ClaymoreGUID::FromString(dep.get<std::string>()));
                }
            }
            
            m_Resources[entry.guid] = entry;
            m_TypeIndex[entry.typeName].push_back(entry.guid);
            m_NameIndex[entry.typeName + ":" + entry.name] = entry.guid;
            
            // Normalize path for lookup
            std::string normalizedPath = entry.path;
            std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
            m_PathIndex[normalizedPath] = entry.guid;
        }
        
        m_Initialized = true;
        m_ResourcesFolderExists = true;
        std::cout << "[ResourceManifest] Loaded " << m_Resources.size() << " resources from JSON." << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ResourceManifest] Failed to parse JSON: " << e.what() << std::endl;
        return false;
    }
}

void ResourceManifest::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    m_Resources.clear();
    m_TypeIndex.clear();
    m_NameIndex.clear();
    m_PathIndex.clear();
}

// ========== Change Notification System ==========

uint32_t ResourceManifest::RegisterChangeCallback(ResourceChangeCallback callback) {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    uint32_t handle = m_NextCallbackHandle++;
    m_ChangeCallbacks[handle] = std::move(callback);
    return handle;
}

void ResourceManifest::UnregisterChangeCallback(uint32_t handle) {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    m_ChangeCallbacks.erase(handle);
}

std::vector<std::string> ResourceManifest::GetSubscribedTypes() const {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    return std::vector<std::string>(m_SubscribedTypes.begin(), m_SubscribedTypes.end());
}

void ResourceManifest::SubscribeToType(const std::string& typeName) {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    m_SubscribedTypes.insert(typeName);
    std::cout << "[ResourceManifest] Subscribed to type: " << typeName << std::endl;
}

void ResourceManifest::UnsubscribeFromType(const std::string& typeName) {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    m_SubscribedTypes.erase(typeName);
    std::cout << "[ResourceManifest] Unsubscribed from type: " << typeName << std::endl;
}

bool ResourceManifest::HasSubscribers(const std::string& typeName) const {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    return m_SubscribedTypes.find(typeName) != m_SubscribedTypes.end();
}

void ResourceManifest::FlushPendingChanges() {
    std::vector<ResourceChangeEvent> changes;
    
    {
        std::lock_guard<std::mutex> lock(m_ChangeMutex);
        if (m_PendingChanges.empty()) return;
        changes = std::move(m_PendingChanges);
        m_PendingChanges.clear();
    }
    
    NotifyChanges(changes);
}

void ResourceManifest::QueueChangeEvent(ResourceChangeType type, const ResourceEntry& entry) {
    std::lock_guard<std::mutex> lock(m_ChangeMutex);
    
    // Only queue if type is subscribed
    if (m_SubscribedTypes.find(entry.typeName) == m_SubscribedTypes.end()) return;
    
    ResourceChangeEvent evt;
    evt.type = type;
    evt.typeName = entry.typeName;
    evt.name = entry.name;
    evt.guid = entry.guid;
    m_PendingChanges.push_back(evt);
}

void ResourceManifest::NotifyChanges(const std::vector<ResourceChangeEvent>& events) {
    if (events.empty()) return;
    
    std::vector<ResourceChangeCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_ChangeMutex);
        if (m_ChangeCallbacks.empty()) return;
        callbacks.reserve(m_ChangeCallbacks.size());
        for (const auto& [handle, callback] : m_ChangeCallbacks) {
            callbacks.push_back(callback);
        }
    }

    for (const auto& callback : callbacks) {
        try {
            callback(events);
        } catch (const std::exception& e) {
            std::cerr << "[ResourceManifest] Callback error: " << e.what() << std::endl;
        }
    }
}

bool ResourceManifest::ParseClayObject(const fs::path& path, ResourceEntry& entry) {
    try {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        
        json j = json::parse(in);
        
        // Extract GUID from companion .meta file or generate from path
        fs::path metaPath = path;
        metaPath += ".meta";
        
        if (fs::exists(metaPath)) {
            std::ifstream metaIn(metaPath);
            if (metaIn.is_open()) {
                json meta = json::parse(metaIn);
                if (meta.contains("guid")) {
                    entry.guid = ClaymoreGUID::FromString(meta["guid"].get<std::string>());
                }
            }
        }
        
        // If no GUID from meta, check clayobj itself
        if (entry.guid == ClaymoreGUID()) {
            if (j.contains("guid")) {
                entry.guid = ClaymoreGUID::FromString(j["guid"].get<std::string>());
            } else {
                // Generate GUID from path (deterministic for consistency)
                entry.guid = ::DeterministicGuidFromPath(path);
                std::cout << "[ResourceManifest] Warning: Generated GUID for " << path << std::endl;
            }
        }
        
        entry.path = path.string();
        entry.name = path.stem().string();
        
        // Extract type name
        if (j.contains("typeName")) {
            entry.typeName = j["typeName"].get<std::string>();
        } else {
            std::cerr << "[ResourceManifest] Missing typeName in: " << path << std::endl;
            return false;
        }
        
        // Extract dependencies from fields
        if (j.contains("fields")) {
            ExtractDependencies(j["fields"], entry.dependencies);
        }
        
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[ResourceManifest] Failed to parse: " << path << " - " << e.what() << std::endl;
        return false;
    }
}

void ResourceManifest::ExtractDependencies(const json& fields, std::vector<ClaymoreGUID>& deps) {
    for (auto& [key, value] : fields.items()) {
        if (value.is_string()) {
            // Check if it looks like a GUID (32 hex chars or with dashes)
            std::string str = value.get<std::string>();
            if (str.length() >= 32 && str.length() <= 36) {
                bool isGuid = true;
                for (char c : str) {
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-')) {
                        isGuid = false;
                        break;
                    }
                }
                if (isGuid) {
                    ClaymoreGUID guid = ClaymoreGUID::FromString(str);
                    if (guid != ClaymoreGUID()) {
                        deps.push_back(guid);
                    }
                }
            }
            // Check for "guid:fileID" format (mesh references)
            size_t colonPos = str.find(':');
            if (colonPos != std::string::npos && colonPos >= 32) {
                std::string guidPart = str.substr(0, colonPos);
                ClaymoreGUID guid = ClaymoreGUID::FromString(guidPart);
                if (guid != ClaymoreGUID()) {
                    deps.push_back(guid);
                }
            }
        }
        else if (value.is_object()) {
            // Check for mesh object format: { "guid": "...", "fileID": 0 }
            if (value.contains("guid")) {
                std::string guidStr = value["guid"].get<std::string>();
                ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
                if (guid != ClaymoreGUID()) {
                    deps.push_back(guid);
                }
            }
            // Recurse into nested objects
            ExtractDependencies(value, deps);
        }
        else if (value.is_array()) {
            // Recurse into arrays
            for (const auto& item : value) {
                if (item.is_string()) {
                    std::string str = item.get<std::string>();
                    if (str.length() >= 32 && str.length() <= 36) {
                        ClaymoreGUID guid = ClaymoreGUID::FromString(str);
                        if (guid != ClaymoreGUID()) {
                            deps.push_back(guid);
                        }
                    }
                }
                else if (item.is_object()) {
                    if (item.contains("guid")) {
                        std::string guidStr = item["guid"].get<std::string>();
                        ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
                        if (guid != ClaymoreGUID()) {
                            deps.push_back(guid);
                        }
                    }
                    ExtractDependencies(item, deps);
                }
            }
        }
    }
}
