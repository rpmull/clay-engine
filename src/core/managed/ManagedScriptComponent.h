#pragma once
#include "ScriptComponent.h"
#include "ScriptInterop.h"
#include <iostream>

// ============================================================================
// ManagedScriptComponent: Wrapper for .NET script instances
// ============================================================================
// This class wraps a managed script instance (GCHandle) and delegates
// lifecycle calls (OnCreate, OnUpdate) to the managed side.
//
// Uses the unified cm::script interface which works in both:
// - Editor mode (function pointers set by DotNetHost)
// - Runtime mode (function pointers set by RuntimeHost)
// ============================================================================

class ManagedScriptComponent : public ScriptComponent {
public:
    ManagedScriptComponent(const std::string& className) : m_ClassName(className) {
        m_Handle = cm::script::CreateInstance(className);
        if (!m_Handle) {
            std::cerr << "[ManagedScriptComponent] WARNING: CreateInstance returned null for '" 
                      << className << "' (g_CreateInstance=" << (void*)cm::script::g_CreateInstance << ")\n";
        }
    }

    ~ManagedScriptComponent() override {
        // Safety: only call into .NET if handle is valid and runtime is ready
        if (m_Handle && cm::script::IsReady()) {
            // Call OnDestroy before releasing the managed GCHandle
            cm::script::CallOnDestroy(m_Handle);
            // Release the managed GCHandle associated with this instance
            cm::script::DestroyInstance(m_Handle);
        }
        m_Handle = nullptr;
    }

    // OnBind: Registers this script in ScriptRegistry before OnCreate is called
    // This enables GetScript<T>() to work for cross-script dependencies during OnCreate
    void OnBind(Entity e) override {
        m_Entity = e;
        cm::script::CallOnBind(m_Handle, e.GetID());
    }

    void OnCreate(Entity e) override {
        cm::script::CallOnCreate(m_Handle, e.GetID(), m_ClassName.c_str());
    }

    void OnUpdate(float dt) override {
        cm::script::CallOnUpdate(m_Handle, dt, m_ClassName.c_str());
    }

    std::shared_ptr<ScriptComponent> Clone() const override {
        // Create a NEW managed instance rather than copying the handle
        return std::make_shared<ManagedScriptComponent>(m_ClassName);
    }

    ScriptBackend GetBackend() const override { return ScriptBackend::Managed; }

public:
    void* GetHandle() const { return m_Handle; }
    const std::string& GetClassName() const { return m_ClassName; }

private:
    void* m_Handle = nullptr;
    std::string m_ClassName;
};

