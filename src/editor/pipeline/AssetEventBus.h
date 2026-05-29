#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include "core/assets/AssetReference.h"
#include "AssetLibrary.h"

/// @brief Event types for asset changes
enum class AssetEvent {
    Imported,       ///< New asset first imported
    Reimported,     ///< Existing asset re-imported (source file changed)
    Deleted,        ///< Asset deleted
    Moved,          ///< Asset moved/renamed
    CacheInvalidated ///< Binary cache invalidated
};

/// @brief Callback signature for asset change notifications
/// @param event The type of event
/// @param path Virtual path to the asset (e.g., "assets/models/character.fbx")
/// @param guid GUID of the asset (may be zero for delete events)
using AssetEventCallback = std::function<void(AssetEvent event, const std::string& path, ClaymoreGUID guid)>;

/// @brief Central hub for asset change notifications
/// 
/// Allows any system to subscribe to asset changes without tight coupling.
/// Thread-safe for subscription/unsubscription and event emission.
/// 
/// Usage:
/// ```cpp
/// int handle = AssetEventBus::Instance().Subscribe(
///     AssetType::Mesh,
///     [](AssetEvent e, const std::string& path, ClaymoreGUID guid) {
///         if (e == AssetEvent::Reimported) {
///             // Handle model reimport
///         }
///     }
/// );
/// 
/// // Later, when done:
/// AssetEventBus::Instance().Unsubscribe(handle);
/// ```
class AssetEventBus {
public:
    static AssetEventBus& Instance();

    /// @brief Subscribe to events for a specific asset type
    /// @param type The asset type to listen for (use AssetType::Unknown for all types)
    /// @param callback Function to call when event occurs
    /// @return Handle for unsubscription
    int Subscribe(AssetType type, AssetEventCallback callback);

    /// @brief Subscribe to events for all asset types
    /// @param callback Function to call when event occurs
    /// @return Handle for unsubscription
    int SubscribeAll(AssetEventCallback callback);

    /// @brief Unsubscribe from events
    /// @param handle The handle returned by Subscribe
    void Unsubscribe(int handle);

    /// @brief Emit an asset event to all relevant subscribers
    /// @param event The type of event
    /// @param type The asset type
    /// @param path Virtual path to the asset
    /// @param guid GUID of the asset
    void Emit(AssetEvent event, AssetType type, const std::string& path, ClaymoreGUID guid);

    /// @brief Check if any subscribers exist for a given asset type
    bool HasSubscribers(AssetType type) const;

    /// @brief Clear all subscriptions (for shutdown)
    void ClearAll();

private:
    AssetEventBus() = default;
    ~AssetEventBus() = default;
    AssetEventBus(const AssetEventBus&) = delete;
    AssetEventBus& operator=(const AssetEventBus&) = delete;

    struct Subscription {
        int handle;
        AssetType type;
        AssetEventCallback callback;
    };

    mutable std::mutex m_Mutex;
    std::vector<Subscription> m_Subscriptions;
    std::atomic<int> m_NextHandle{1};
};

