#include "Camera.h"
#include <algorithm>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>


Camera::Camera(float fov, float aspect, float nearPlane, float farPlane)
    : m_FOV(fov), m_Aspect(aspect), m_Near(nearPlane), m_Far(farPlane) {
    UpdateProjection();
    RecalculateView();
}

void Camera::SetViewportSize(float width, float height) {
    if (height <= 0.0f) height = 1.0f; // Prevent divide by zero
    m_Aspect = width / height;
    UpdateProjection();
}

void Camera::UpdateProjection() {
    m_ProjectionMatrix = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
}

void Camera::SetPosition(const glm::vec3& pos) {
    m_Position = pos;
    RecalculateView();
}

void Camera::SetRotation(const glm::vec3& rot) {
    m_Rotation = rot;
    m_UseQuatRotation = false;
    RecalculateView();
}

void Camera::SetRotationQuat(const glm::quat& rotQ) {
    m_RotationQuat = glm::normalize(rotQ);
    m_UseQuatRotation = true;
    RecalculateView();
}

void Camera::LookAt(const glm::vec3& target) {
    m_ViewMatrix = glm::lookAt(m_Position, target, glm::vec3(0, 1, 0));
}

void Camera::LookAt(const glm::vec3& target, const glm::vec3& up) {
    m_ViewMatrix = glm::lookAt(m_Position, target, up);
}

void Camera::RecalculateView() {
    glm::mat4 rotation;
    if (m_UseQuatRotation) {
        rotation = glm::toMat4(m_RotationQuat);
    } else {
        rotation = glm::yawPitchRoll(glm::radians(m_Rotation.y), glm::radians(m_Rotation.x), glm::radians(m_Rotation.z));
    }
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), -m_Position);
    // View is inverse of camera transform (T * R). Since rotation is orthonormal, inverse(R) = transpose(R).
    // So view = R^T * T^{-1} = transpose(rotation) * translate(-position)
    m_ViewMatrix = glm::transpose(rotation) * translation;
}

float* Camera::GetViewArray() {
    return glm::value_ptr(m_ViewMatrix);
}

float* Camera::GetProjectionArray() {
    return glm::value_ptr(m_ProjectionMatrix);
}

void Camera::SetPerspective(float fovDegrees, float aspect, float nearClip, float farClip) {
    m_FOV = fovDegrees;
    m_Aspect = aspect;
    m_Near = nearClip;
    m_Far = farClip;

    m_ProjectionMatrix = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
}

void Camera::SetOrthographic(float size, float aspect, float nearClip, float farClip) {
    // Interpret size as vertical world-space size
    m_Aspect = aspect;
    m_Near = nearClip;
    m_Far = farClip;

    float halfHeight = size * 0.5f;
    float halfWidth = halfHeight * aspect;
    m_ProjectionMatrix = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, m_Near, m_Far);
}

void Camera::SetFieldOfView(float fovDegrees) {
    constexpr float kMinFov = 1.0f;
    constexpr float kMaxFov = 179.0f;
    m_FOV = std::clamp(fovDegrees, kMinFov, kMaxFov);
    UpdateProjection();
}

void Camera::SetNearClip(float nearClip) {
    constexpr float kMinNear = 0.001f;
    m_Near = std::max(kMinNear, nearClip);
    if (m_Near >= m_Far) {
        m_Far = m_Near + kMinNear;
    }
    UpdateProjection();
}

void Camera::SetFarClip(float farClip) {
    constexpr float kMinDelta = 0.001f;
    m_Far = std::max(farClip, m_Near + kMinDelta);
    UpdateProjection();
}

