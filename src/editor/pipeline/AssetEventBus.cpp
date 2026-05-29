#include "AssetEventBus.h"
#include <algorithm>
#include <iostream>

AssetEventBus& AssetEventBus::Instance() {
    static AssetEventBus instance;
    return instance;
}

int AssetEventBus::Subscribe(AssetType type, AssetEventCallback callback) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    int handle = m_NextHandle++;
    m_Subscriptions.push_back({handle, type, std::move(callback)});
    return handle;
}

int AssetEventBus::SubscribeAll(AssetEventCallback callback) {
    // Use Unknown as sentinel for "all types"
    return Subscribe(AssetType::Unknown, std::move(callback));
}

void AssetEventBus::Unsubscribe(int handle) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = std::remove_if(m_Subscriptions.begin(), m_Subscriptions.end(),
        [handle](const Subscription& s) { return s.handle == handle; });
    m_Subscriptions.erase(it, m_Subscriptions.end());
}

void AssetEventBus::Emit(AssetEvent event, AssetType type, const std::string& path, ClaymoreGUID guid) {
    // Copy callbacks under lock, then invoke outside lock to avoid deadlocks
    std::vector<AssetEventCallback> toCall;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const auto& sub : m_Subscriptions) {
            // Match if subscriber wants this type or subscribed to all (Unknown)
            if (sub.type == type || sub.type == AssetType::Unknown) {
                toCall.push_back(sub.callback);
            }
        }
    }

    // Invoke callbacks outside the lock
    for (const auto& cb : toCall) {
        try {
            cb(event, path, guid);
        } catch (const std::exception& e) {
            std::cerr << "[AssetEventBus] Callback threw: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[AssetEventBus] Callback threw unknown exception" << std::endl;
        }
    }
}

bool AssetEventBus::HasSubscribers(AssetType type) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& sub : m_Subscriptions) {
        if (sub.type == type || sub.type == AssetType::Unknown) {
            return true;
        }
    }
    return false;
}

void AssetEventBus::ClearAll() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Subscriptions.clear();
}

