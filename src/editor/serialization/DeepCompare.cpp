#include "DeepCompare.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/managed/ScriptReflection.h"
#include "core/serialization/Serializer.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace cm { namespace editor {

static void AddDiff(std::vector<Diff>& out, const std::string& path, Diff::Kind kind = Diff::Kind::Change, float delta = 0.0f) {
	Diff d; d.kind = kind; d.path = path; d.delta = delta; out.push_back(d);
}

static std::string NormalizePath(const std::string& p) {
	std::string out = p;
	for (char& c : out) if (c == '\\') c = '/';
	return out;
}

static void CompareBool(bool a, bool b, const std::string& path, std::vector<Diff>& out) {
	if (a != b) AddDiff(out, path);
}

static void CompareInt(int a, int b, const std::string& path, std::vector<Diff>& out) {
	if (a != b) AddDiff(out, path);
}

static void CompareUInt(uint32_t a, uint32_t b, const std::string& path, std::vector<Diff>& out) {
	if (a != b) AddDiff(out, path);
}

static void CompareFloat(float a, float b, float eps, const std::string& path, std::vector<Diff>& out) {
	if (std::fabs(a - b) > eps) AddDiff(out, path, Diff::Kind::Change, std::fabs(a - b));
}

static void CompareVec2(const glm::vec2& a, const glm::vec2& b, float eps, const std::string& path, std::vector<Diff>& out) {
	float d = glm::distance(a, b);
	if (d > eps) AddDiff(out, path, Diff::Kind::Change, d);
}

static void CompareVec3(const glm::vec3& a, const glm::vec3& b, float eps, const std::string& path, std::vector<Diff>& out) {
	float d = glm::distance(a, b);
	if (d > eps) AddDiff(out, path, Diff::Kind::Change, d);
}

static void CompareQuat(const glm::quat& a, const glm::quat& b, float eps, const std::string& path, std::vector<Diff>& out) {
	float dot = std::fabs(glm::dot(a, b));
	if ((1.0f - dot) > eps) AddDiff(out, path, Diff::Kind::Change, 1.0f - dot);
}

static void CompareString(const std::string& a, const std::string& b, const std::string& path, std::vector<Diff>& out) {
	if (a != b) AddDiff(out, path);
}

static void CompareGuid(const ClaymoreGUID& a, const ClaymoreGUID& b, const std::string& path, std::vector<Diff>& out) {
	if (a != b) AddDiff(out, path);
}

static void CompareAssetRef(const AssetReference& a, const AssetReference& b, const std::string& path, std::vector<Diff>& out) {
	CompareGuid(a.guid, b.guid, path + "/guid", out);
	CompareInt(a.fileID, b.fileID, path + "/fileId", out);
	CompareInt(a.type, b.type, path + "/type", out);
}

static void CompareStringVec(const std::vector<std::string>& a, const std::vector<std::string>& b, const std::string& path, std::vector<Diff>& out) {
	if (a.size() != b.size()) { AddDiff(out, path + "/size"); return; }
	for (size_t i = 0; i < a.size(); ++i) {
		if (NormalizePath(a[i]) != NormalizePath(b[i])) AddDiff(out, path + "/" + std::to_string(i));
	}
}

static void CompareInlineMaterials(const std::vector<binary::InlineMaterialData>& a,
								   const std::vector<binary::InlineMaterialData>& b,
								   float eps, const std::string& path, std::vector<Diff>& out) {
	if (a.size() != b.size()) { AddDiff(out, path + "/size"); return; }
	for (size_t i = 0; i < a.size(); ++i) {
		const auto& ia = a[i];
		const auto& ib = b[i];
		if (ia.materialType != ib.materialType) AddDiff(out, path + "/" + std::to_string(i) + "/type");
		CompareString(NormalizePath(ia.albedoPath), NormalizePath(ib.albedoPath), path + "/" + std::to_string(i) + "/albedo", out);
		CompareString(NormalizePath(ia.normalPath), NormalizePath(ib.normalPath), path + "/" + std::to_string(i) + "/normal", out);
		CompareString(NormalizePath(ia.metallicRoughnessPath), NormalizePath(ib.metallicRoughnessPath), path + "/" + std::to_string(i) + "/metallicRoughness", out);
		CompareString(NormalizePath(ia.emissionPath), NormalizePath(ib.emissionPath), path + "/" + std::to_string(i) + "/emission", out);
		CompareString(NormalizePath(ia.shaderGraphPath), NormalizePath(ib.shaderGraphPath), path + "/" + std::to_string(i) + "/shaderGraphPath", out);
		CompareFloat(ia.metallic, ib.metallic, eps, path + "/" + std::to_string(i) + "/metallic", out);
		CompareFloat(ia.roughness, ib.roughness, eps, path + "/" + std::to_string(i) + "/roughness", out);
		CompareBool(ia.receiveShadowsOverride, ib.receiveShadowsOverride, path + "/" + std::to_string(i) + "/receiveShadowsOverride", out);
		CompareBool(ia.receiveShadows, ib.receiveShadows, path + "/" + std::to_string(i) + "/receiveShadows", out);
	}
}

static std::string SerializeInstancerForComparison(const cm::instancer::InstancerComponent& instancer) {
	nlohmann::json data = Serializer::SerializeInstancer(instancer);
	if (data.contains("meshPath") && data["meshPath"].is_string()) {
		data["meshPath"] = NormalizePath(data["meshPath"].get<std::string>());
	}
	if (data.contains("prefabPath") && data["prefabPath"].is_string()) {
		data["prefabPath"] = NormalizePath(data["prefabPath"].get<std::string>());
	}
	return data.dump();
}

static void CompareTerrainInstancerLayer(const TerrainInstancerLayerDesc& a,
										 const TerrainInstancerLayerDesc& b,
										 float eps,
										 const std::string& path,
										 std::vector<Diff>& out) {
	CompareGuid(a.Guid, b.Guid, path + "/guid", out);
	CompareString(a.Name, b.Name, path + "/name", out);
	CompareBool(a.Enabled, b.Enabled, path + "/enabled", out);
	CompareInt(static_cast<int>(a.Mask), static_cast<int>(b.Mask), path + "/mask", out);
	CompareFloat(a.SplatThreshold, b.SplatThreshold, eps, path + "/splatThreshold", out);
	CompareBool(a.Collision.Enabled, b.Collision.Enabled, path + "/collision/enabled", out);
	CompareFloat(a.Collision.ActivationDistance, b.Collision.ActivationDistance, eps, path + "/collision/activationDistance", out);
	CompareUInt(a.Collision.MaxActiveBodies, b.Collision.MaxActiveBodies, path + "/collision/maxActiveBodies", out);
	CompareUInt(a.Collision.PhysicsLayer, b.Collision.PhysicsLayer, path + "/collision/physicsLayer", out);
	CompareString(a.Collision.PhysicsLayerName, b.Collision.PhysicsLayerName, path + "/collision/physicsLayerName", out);
	CompareBool(a.Collision.UseSharedMeshShape, b.Collision.UseSharedMeshShape, path + "/collision/useSharedMeshShape", out);
	CompareString(SerializeInstancerForComparison(a.Instancer), SerializeInstancerForComparison(b.Instancer), path + "/instancer", out);
}

static void CompareScripts(const std::vector<ScriptInstance>& a,
						   const std::vector<ScriptInstance>& b,
						   float eps, const std::string& path, std::vector<Diff>& out) {
	(void)eps;
	if (a.size() != b.size()) { AddDiff(out, path + "/size"); return; }
	for (size_t i = 0; i < a.size(); ++i) {
		const auto& sa = a[i];
		const auto& sb = b[i];
		CompareString(sa.ClassName, sb.ClassName, path + "/" + std::to_string(i) + "/class", out);
		if (sa.Values.size() != sb.Values.size()) {
			AddDiff(out, path + "/" + std::to_string(i) + "/values/size");
		}
		for (const auto& [key, val] : sa.Values) {
			auto it = sb.Values.find(key);
			if (it == sb.Values.end()) { AddDiff(out, path + "/" + std::to_string(i) + "/values/" + key); continue; }
			std::string aval = ScriptReflection::PropertyValueToString(val);
			std::string bval = ScriptReflection::PropertyValueToString(it->second);
			if (aval != bval) AddDiff(out, path + "/" + std::to_string(i) + "/values/" + key);
		}
	}
}

static void CompareTransform(const TransformComponent& ta, const TransformComponent& tb, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareVec3(ta.Position, tb.Position, eps, basePath + "/components/Transform/position", out);
	CompareVec3(ta.Scale, tb.Scale, eps, basePath + "/components/Transform/scale", out);
	CompareBool(ta.UseQuatRotation, tb.UseQuatRotation, basePath + "/components/Transform/useQuat", out);
	CompareVec3(ta.Rotation, tb.Rotation, eps, basePath + "/components/Transform/rotation", out);
	CompareQuat(ta.RotationQ, tb.RotationQ, eps, basePath + "/components/Transform/rotationQ", out);
}

static void CompareMesh(const MeshComponent& a, const MeshComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareAssetRef(a.meshReference, b.meshReference, basePath + "/components/Mesh/ref", out);
	CompareStringVec(a.MaterialAssetPaths, b.MaterialAssetPaths, basePath + "/components/Mesh/materialPaths", out);
	CompareBool(a.RenderOnTop, b.RenderOnTop, basePath + "/components/Mesh/renderOnTop", out);
	CompareBool(a.ShowBackfaces, b.ShowBackfaces, basePath + "/components/Mesh/showBackfaces", out);
	CompareBool(a.SkipFrustumCulling, b.SkipFrustumCulling, basePath + "/components/Mesh/skipFrustumCulling", out);
	CompareFloat(a.BoundsPadding, b.BoundsPadding, eps, basePath + "/components/Mesh/boundsPadding", out);
	CompareBool(a.UniqueMaterial, b.UniqueMaterial, basePath + "/components/Mesh/uniqueMaterial", out);
	CompareBool(a.EnableInstancing, b.EnableInstancing, basePath + "/components/Mesh/enableInstancing", out);
	CompareInlineMaterials(a.InlineMaterials, b.InlineMaterials, eps, basePath + "/components/Mesh/inlineMaterials", out);
}

static void CompareLight(const LightComponent& a, const LightComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareInt(static_cast<int>(a.Type), static_cast<int>(b.Type), basePath + "/components/Light/type", out);
	CompareVec3(a.Color, b.Color, eps, basePath + "/components/Light/color", out);
	CompareFloat(a.Intensity, b.Intensity, eps, basePath + "/components/Light/intensity", out);
}

static void CompareCamera(const CameraComponent& a, const CameraComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareBool(a.Active, b.Active, basePath + "/components/Camera/active", out);
	CompareInt(a.priority, b.priority, basePath + "/components/Camera/priority", out);
	CompareUInt(a.LayerMask, b.LayerMask, basePath + "/components/Camera/layerMask", out);
	CompareFloat(a.FieldOfView, b.FieldOfView, eps, basePath + "/components/Camera/fov", out);
	CompareFloat(a.NearClip, b.NearClip, eps, basePath + "/components/Camera/near", out);
	CompareFloat(a.FarClip, b.FarClip, eps, basePath + "/components/Camera/far", out);
	CompareBool(a.IsPerspective, b.IsPerspective, basePath + "/components/Camera/perspective", out);
}

static void CompareCollider(const ColliderComponent& a, const ColliderComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareInt(static_cast<int>(a.ShapeType), static_cast<int>(b.ShapeType), basePath + "/components/Collider/shape", out);
	CompareVec3(a.Offset, b.Offset, eps, basePath + "/components/Collider/offset", out);
	CompareVec3(a.Size, b.Size, eps, basePath + "/components/Collider/size", out);
	CompareFloat(a.Radius, b.Radius, eps, basePath + "/components/Collider/radius", out);
	CompareFloat(a.Height, b.Height, eps, basePath + "/components/Collider/height", out);
	CompareString(NormalizePath(a.MeshPath), NormalizePath(b.MeshPath), basePath + "/components/Collider/meshPath", out);
	CompareBool(a.IsTrigger, b.IsTrigger, basePath + "/components/Collider/trigger", out);
	CompareUInt(a.PhysicsLayer, b.PhysicsLayer, basePath + "/components/Collider/layer", out);
}

static void CompareRigidBody(const RigidBodyComponent& a, const RigidBodyComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareFloat(a.Mass, b.Mass, eps, basePath + "/components/RigidBody/mass", out);
	CompareFloat(a.Friction, b.Friction, eps, basePath + "/components/RigidBody/friction", out);
	CompareFloat(a.Restitution, b.Restitution, eps, basePath + "/components/RigidBody/restitution", out);
	CompareBool(a.UseGravity, b.UseGravity, basePath + "/components/RigidBody/useGravity", out);
	CompareBool(a.IsKinematic, b.IsKinematic, basePath + "/components/RigidBody/isKinematic", out);
	CompareUInt(a.PhysicsLayer, b.PhysicsLayer, basePath + "/components/RigidBody/layer", out);
	CompareUInt(a.CollisionMask, b.CollisionMask, basePath + "/components/RigidBody/collisionMask", out);
}

static void CompareStaticBody(const StaticBodyComponent& a, const StaticBodyComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareFloat(a.Friction, b.Friction, eps, basePath + "/components/StaticBody/friction", out);
	CompareFloat(a.Restitution, b.Restitution, eps, basePath + "/components/StaticBody/restitution", out);
	CompareUInt(a.PhysicsLayer, b.PhysicsLayer, basePath + "/components/StaticBody/layer", out);
}

static void CompareAnimationPlayer(const cm::animation::AnimationPlayerComponent& a,
								   const cm::animation::AnimationPlayerComponent& b,
								   float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareString(NormalizePath(a.ControllerPath), NormalizePath(b.ControllerPath), basePath + "/components/Animation/controller", out);
	CompareString(NormalizePath(a.ControllerOverridePath), NormalizePath(b.ControllerOverridePath), basePath + "/components/Animation/controllerOverride", out);
	CompareString(NormalizePath(a.SingleClipPath), NormalizePath(b.SingleClipPath), basePath + "/components/Animation/singleClip", out);
	CompareFloat(a.PlaybackSpeed, b.PlaybackSpeed, eps, basePath + "/components/Animation/playbackSpeed", out);
	CompareBool(a.Enabled, b.Enabled, basePath + "/components/Animation/enabled", out);
	CompareBool(a.PlayOnStart, b.PlayOnStart, basePath + "/components/Animation/playOnStart", out);
	CompareInt(static_cast<int>(a.MotionTarget), static_cast<int>(b.MotionTarget), basePath + "/components/Animation/motionTarget", out);
	CompareInt(static_cast<int>(a.ExplicitTargetEntityId), static_cast<int>(b.ExplicitTargetEntityId), basePath + "/components/Animation/explicitTarget", out);
}

static void ComparePrefabInstance(const PrefabInstanceComponent& a, const PrefabInstanceComponent& b, const std::string& basePath, std::vector<Diff>& out) {
	CompareGuid(a.PrefabAssetGuid, b.PrefabAssetGuid, basePath + "/components/Prefab/guid", out);
	CompareString(NormalizePath(a.PrefabPath), NormalizePath(b.PrefabPath), basePath + "/components/Prefab/path", out);
	CompareGuid(a.ModelAssetGuid, b.ModelAssetGuid, basePath + "/components/Prefab/modelGuid", out);
	if (a.OwnedEntityGuids.size() != b.OwnedEntityGuids.size()) {
		AddDiff(out, basePath + "/components/Prefab/ownedCount");
	}
}

static void CompareNavAgent(const nav::NavAgentComponent& a, const nav::NavAgentComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareBool(a.Enabled, b.Enabled, basePath + "/components/NavAgent/enabled", out);
	CompareInt(static_cast<int>(a.NavMeshEntity), static_cast<int>(b.NavMeshEntity), basePath + "/components/NavAgent/navMeshEntity", out);
	CompareFloat(a.Params.radius, b.Params.radius, eps, basePath + "/components/NavAgent/params/radius", out);
	CompareFloat(a.Params.height, b.Params.height, eps, basePath + "/components/NavAgent/params/height", out);
	CompareFloat(a.Params.maxSlopeDeg, b.Params.maxSlopeDeg, eps, basePath + "/components/NavAgent/params/maxSlope", out);
	CompareFloat(a.Params.maxStep, b.Params.maxStep, eps, basePath + "/components/NavAgent/params/maxStep", out);
	CompareFloat(a.Params.maxSpeed, b.Params.maxSpeed, eps, basePath + "/components/NavAgent/params/maxSpeed", out);
	CompareFloat(a.Params.maxAccel, b.Params.maxAccel, eps, basePath + "/components/NavAgent/params/maxAccel", out);
	CompareFloat(a.ArriveThreshold, b.ArriveThreshold, eps, basePath + "/components/NavAgent/arriveThreshold", out);
	CompareBool(a.AutoRepath, b.AutoRepath, basePath + "/components/NavAgent/autoRepath", out);
	CompareFloat(a.SteeringSmoothness, b.SteeringSmoothness, eps, basePath + "/components/NavAgent/steeringSmoothness", out);
	CompareFloat(a.ArrivalSlowdownDist, b.ArrivalSlowdownDist, eps, basePath + "/components/NavAgent/arrivalSlowdown", out);
}

static void CompareNavLink(const nav::NavLinkComponent& a, const nav::NavLinkComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareBool(a.Enabled, b.Enabled, basePath + "/components/NavLink/enabled", out);
	CompareVec3(a.Start, b.Start, eps, basePath + "/components/NavLink/start", out);
	CompareVec3(a.End, b.End, eps, basePath + "/components/NavLink/end", out);
	CompareFloat(a.Radius, b.Radius, eps, basePath + "/components/NavLink/radius", out);
	CompareFloat(a.Cost, b.Cost, eps, basePath + "/components/NavLink/cost", out);
	CompareUInt(a.Flags, b.Flags, basePath + "/components/NavLink/flags", out);
	CompareBool(a.Bidirectional, b.Bidirectional, basePath + "/components/NavLink/bidirectional", out);
	CompareBool(a.UseWorldSpace, b.UseWorldSpace, basePath + "/components/NavLink/useWorldSpace", out);
}

static void CompareNavMesh(const nav::NavMeshComponent& a, const nav::NavMeshComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareBool(a.Enabled, b.Enabled, basePath + "/components/NavMesh/enabled", out);
	CompareGuid(a.NavMeshDataGuid, b.NavMeshDataGuid, basePath + "/components/NavMesh/guid", out);
	CompareString(NormalizePath(a.AssetPath), NormalizePath(b.AssetPath), basePath + "/components/NavMesh/assetPath", out);
	CompareUInt(a.TerrainSampleStep, b.TerrainSampleStep, basePath + "/components/NavMesh/terrainSampleStep", out);
	CompareBool(a.GeometryIncludeRegexEnabled, b.GeometryIncludeRegexEnabled, basePath + "/components/NavMesh/geometryIncludeRegexEnabled", out);
	CompareString(a.GeometryIncludeRegexPattern, b.GeometryIncludeRegexPattern, basePath + "/components/NavMesh/geometryIncludeRegexPattern", out);
	CompareBool(a.BakeVisibleChunksOnly, b.BakeVisibleChunksOnly, basePath + "/components/NavMesh/bakeVisibleChunksOnly", out);
	CompareUInt(a.BakeVisibleChunkPadding, b.BakeVisibleChunkPadding, basePath + "/components/NavMesh/bakeVisibleChunkPadding", out);
	CompareBool(a.BakeMissingChunksOnly, b.BakeMissingChunksOnly, basePath + "/components/NavMesh/bakeMissingChunksOnly", out);
	CompareBool(a.ChunkedNavEnabled, b.ChunkedNavEnabled, basePath + "/components/NavMesh/chunkedNavEnabled", out);
	CompareInt(static_cast<int>(a.ChunkingMode), static_cast<int>(b.ChunkingMode), basePath + "/components/NavMesh/chunkingMode", out);
	CompareFloat(a.ChunkWorldSize, b.ChunkWorldSize, eps, basePath + "/components/NavMesh/chunkWorldSize", out);
	CompareUInt(a.ChunkBakePadding, b.ChunkBakePadding, basePath + "/components/NavMesh/chunkBakePadding", out);
	CompareFloat(a.ChunkStreamRadius, b.ChunkStreamRadius, eps, basePath + "/components/NavMesh/chunkStreamRadius", out);
	CompareString(NormalizePath(a.NavPackPath), NormalizePath(b.NavPackPath), basePath + "/components/NavMesh/navPackPath", out);
	CompareFloat(a.AgentPlacementOffset, b.AgentPlacementOffset, eps, basePath + "/components/NavMesh/agentPlacementOffset", out);
	CompareBool(a.CostAwareSmoothing, b.CostAwareSmoothing, basePath + "/components/NavMesh/costAwareSmoothing", out);
	CompareBool(a.EnableStitching, b.EnableStitching, basePath + "/components/NavMesh/enableStitching", out);
	CompareFloat(a.StitchEpsilon, b.StitchEpsilon, eps, basePath + "/components/NavMesh/stitchEpsilon", out);
	CompareFloat(a.StitchMaxNormalAngleDeg, b.StitchMaxNormalAngleDeg, eps, basePath + "/components/NavMesh/stitchMaxNormalAngle", out);
	CompareFloat(a.StitchMaxHeight, b.StitchMaxHeight, eps, basePath + "/components/NavMesh/stitchMaxHeight", out);
	CompareFloat(a.StitchMaxXZ, b.StitchMaxXZ, eps, basePath + "/components/NavMesh/stitchMaxXZ", out);
	CompareInt(a.DomainId, b.DomainId, basePath + "/components/NavMesh/domainId", out);
	CompareInt(a.DomainPriority, b.DomainPriority, basePath + "/components/NavMesh/domainPriority", out);
	CompareBool(a.AutoPortalEnabled, b.AutoPortalEnabled, basePath + "/components/NavMesh/autoPortalEnabled", out);
	CompareFloat(a.AutoPortalMaxXZ, b.AutoPortalMaxXZ, eps, basePath + "/components/NavMesh/autoPortalMaxXZ", out);
	CompareFloat(a.AutoPortalMaxHeight, b.AutoPortalMaxHeight, eps, basePath + "/components/NavMesh/autoPortalMaxHeight", out);
}

static void ComparePortal(const PortalComponent& a, const PortalComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareBool(a.Enabled, b.Enabled, basePath + "/components/Portal/enabled", out);
	CompareString(NormalizePath(a.TargetScenePath), NormalizePath(b.TargetScenePath), basePath + "/components/Portal/targetScene", out);
	CompareGuid(a.TargetPortalGuid, b.TargetPortalGuid, basePath + "/components/Portal/targetPortalGuid", out);
	CompareString(a.TargetPortalPath, b.TargetPortalPath, basePath + "/components/Portal/targetPortalPath", out);
	CompareVec3(a.EntryOffset, b.EntryOffset, eps, basePath + "/components/Portal/entryOffset", out);
	CompareVec3(a.ExitOffset, b.ExitOffset, eps, basePath + "/components/Portal/exitOffset", out);
	CompareBool(a.AutoDetect, b.AutoDetect, basePath + "/components/Portal/autoDetect", out);
	CompareFloat(a.TriggerRadius, b.TriggerRadius, eps, basePath + "/components/Portal/triggerRadius", out);
	CompareBool(a.FireExitEvents, b.FireExitEvents, basePath + "/components/Portal/fireExitEvents", out);
}

static void CompareTerrain(const TerrainComponent& a, const TerrainComponent& b, float eps, const std::string& basePath, std::vector<Diff>& out) {
	CompareGuid(a.TerrainDataGuid, b.TerrainDataGuid, basePath + "/components/Terrain/guid", out);
	CompareString(NormalizePath(a.AssetPath), NormalizePath(b.AssetPath), basePath + "/components/Terrain/assetPath", out);
	CompareUInt(a.GridResolution, b.GridResolution, basePath + "/components/Terrain/gridRes", out);
	CompareVec2(a.WorldSize, b.WorldSize, eps, basePath + "/components/Terrain/worldSize", out);
	CompareFloat(a.MaxHeight, b.MaxHeight, eps, basePath + "/components/Terrain/maxHeight", out);
	if (a.HeightMap.size() != b.HeightMap.size()) AddDiff(out, basePath + "/components/Terrain/heightMapSize");
	if (a.SplatMap.size() != b.SplatMap.size()) AddDiff(out, basePath + "/components/Terrain/splatMapSize");
	if (a.Layers.size() != b.Layers.size()) AddDiff(out, basePath + "/components/Terrain/layerCount");
	if (a.GrassLayers.size() != b.GrassLayers.size()) AddDiff(out, basePath + "/components/Terrain/grassLayerCount");
	if (a.InstancerLayers.size() != b.InstancerLayers.size()) AddDiff(out, basePath + "/components/Terrain/instancerLayerCount");
	const size_t instancerCount = std::min(a.InstancerLayers.size(), b.InstancerLayers.size());
	for (size_t i = 0; i < instancerCount; ++i) {
		CompareTerrainInstancerLayer(
			a.InstancerLayers[i],
			b.InstancerLayers[i],
			eps,
			basePath + "/components/Terrain/instancerLayers/" + std::to_string(i),
			out);
	}
}

std::vector<Diff> DeepCompare(const Scene& a, const Scene& b, float floatEps) {
	std::vector<Diff> diffs;
	const auto& ea = a.GetEntities();
	const auto& eb = b.GetEntities();
	if (ea.size() != eb.size()) {
		Diff d; d.kind = (ea.size()<eb.size())?Diff::Kind::UnexpectedAdd:Diff::Kind::UnexpectedRemove; d.path = "/entities"; diffs.push_back(d);
	}
	const size_t n = std::min(ea.size(), eb.size());
	for (size_t i = 0; i < n; ++i) {
		EntityID ida = ea[i].GetID(); EntityID idb = eb[i].GetID();
		EntityData* da = const_cast<Scene&>(a).GetEntityData(ida); EntityData* db = const_cast<Scene&>(b).GetEntityData(idb);
		if ((!da) != (!db)) { Diff d; d.kind = Diff::Kind::TypeMismatch; d.path = "/entities/" + std::to_string(i); diffs.push_back(d); continue; }
		std::string base = std::string("/entities/") + std::to_string(i) + "/" + da->Name;

		CompareString(da->Name, db->Name, base + "/name", diffs);
		CompareInt(da->Layer, db->Layer, base + "/layer", diffs);
		CompareString(da->Tag, db->Tag, base + "/tag", diffs);
		CompareBool(da->Visible, db->Visible, base + "/visible", diffs);
		CompareBool(da->Active, db->Active, base + "/active", diffs);
		if (da->Parent != db->Parent) AddDiff(diffs, base + "/parent");

		CompareTransform(da->Transform, db->Transform, floatEps, base, diffs);

		// Mesh
		if ((da->Mesh != nullptr) != (db->Mesh != nullptr)) {
			AddDiff(diffs, base + "/components/Mesh", Diff::Kind::TypeMismatch);
		} else if (da->Mesh && db->Mesh) {
			CompareMesh(*da->Mesh, *db->Mesh, floatEps, base, diffs);
		}

		// Light
		if ((da->Light != nullptr) != (db->Light != nullptr)) {
			AddDiff(diffs, base + "/components/Light", Diff::Kind::TypeMismatch);
		} else if (da->Light && db->Light) {
			CompareLight(*da->Light, *db->Light, floatEps, base, diffs);
		}

		// Camera
		if ((da->Camera != nullptr) != (db->Camera != nullptr)) {
			AddDiff(diffs, base + "/components/Camera", Diff::Kind::TypeMismatch);
		} else if (da->Camera && db->Camera) {
			CompareCamera(*da->Camera, *db->Camera, floatEps, base, diffs);
		}

		// Collider
		if ((da->Collider != nullptr) != (db->Collider != nullptr)) {
			AddDiff(diffs, base + "/components/Collider", Diff::Kind::TypeMismatch);
		} else if (da->Collider && db->Collider) {
			CompareCollider(*da->Collider, *db->Collider, floatEps, base, diffs);
		}

		// RigidBody
		if ((da->RigidBody != nullptr) != (db->RigidBody != nullptr)) {
			AddDiff(diffs, base + "/components/RigidBody", Diff::Kind::TypeMismatch);
		} else if (da->RigidBody && db->RigidBody) {
			CompareRigidBody(*da->RigidBody, *db->RigidBody, floatEps, base, diffs);
		}

		// StaticBody
		if ((da->StaticBody != nullptr) != (db->StaticBody != nullptr)) {
			AddDiff(diffs, base + "/components/StaticBody", Diff::Kind::TypeMismatch);
		} else if (da->StaticBody && db->StaticBody) {
			CompareStaticBody(*da->StaticBody, *db->StaticBody, floatEps, base, diffs);
		}

		// Animation player
		if ((da->AnimationPlayer != nullptr) != (db->AnimationPlayer != nullptr)) {
			AddDiff(diffs, base + "/components/Animation", Diff::Kind::TypeMismatch);
		} else if (da->AnimationPlayer && db->AnimationPlayer) {
			CompareAnimationPlayer(*da->AnimationPlayer, *db->AnimationPlayer, floatEps, base, diffs);
		}

		// Prefab instance
		if ((da->PrefabInstance != nullptr) != (db->PrefabInstance != nullptr)) {
			AddDiff(diffs, base + "/components/Prefab", Diff::Kind::TypeMismatch);
		} else if (da->PrefabInstance && db->PrefabInstance) {
			ComparePrefabInstance(*da->PrefabInstance, *db->PrefabInstance, base, diffs);
		}

		// Navigation
		if ((da->NavAgent != nullptr) != (db->NavAgent != nullptr)) {
			AddDiff(diffs, base + "/components/NavAgent", Diff::Kind::TypeMismatch);
		} else if (da->NavAgent && db->NavAgent) {
			CompareNavAgent(*da->NavAgent, *db->NavAgent, floatEps, base, diffs);
		}
		if ((da->Navigation != nullptr) != (db->Navigation != nullptr)) {
			AddDiff(diffs, base + "/components/NavMesh", Diff::Kind::TypeMismatch);
		} else if (da->Navigation && db->Navigation) {
			CompareNavMesh(*da->Navigation, *db->Navigation, floatEps, base, diffs);
		}
		if ((da->NavLink != nullptr) != (db->NavLink != nullptr)) {
			AddDiff(diffs, base + "/components/NavLink", Diff::Kind::TypeMismatch);
		} else if (da->NavLink && db->NavLink) {
			CompareNavLink(*da->NavLink, *db->NavLink, floatEps, base, diffs);
		}
		if ((da->Portal != nullptr) != (db->Portal != nullptr)) {
			AddDiff(diffs, base + "/components/Portal", Diff::Kind::TypeMismatch);
		} else if (da->Portal && db->Portal) {
			ComparePortal(*da->Portal, *db->Portal, floatEps, base, diffs);
		}

		// Terrain
		if ((da->Terrain != nullptr) != (db->Terrain != nullptr)) {
			AddDiff(diffs, base + "/components/Terrain", Diff::Kind::TypeMismatch);
		} else if (da->Terrain && db->Terrain) {
			CompareTerrain(*da->Terrain, *db->Terrain, floatEps, base, diffs);
		}

		// Scripts
		CompareScripts(da->Scripts, db->Scripts, floatEps, base + "/components/Scripts", diffs);
	}
	return diffs;
}

}} // namespace cm::editor


