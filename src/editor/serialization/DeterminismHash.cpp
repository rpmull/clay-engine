#include "DeterminismHash.h"
#include "core/ecs/EntityData.h"
#include "core/managed/ScriptReflection.h"
#include "core/serialization/Serializer.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cstring>

namespace cm { namespace editor {

static void HashAppend(uint64_t& h, const void* data, size_t len) {
	// 64-bit FNV-1a
	const uint8_t* bytes = static_cast<const uint8_t*>(data);
	const uint64_t FNV_PRIME = 1099511628211ull;
	for (size_t i = 0; i < len; ++i) { h ^= bytes[i]; h *= FNV_PRIME; }
}

static void HashString(uint64_t& h, const std::string& s) {
	uint64_t len = static_cast<uint64_t>(s.size());
	HashAppend(h, &len, sizeof(len));
	if (!s.empty()) HashAppend(h, s.data(), s.size());
}

static void HashGuid(uint64_t& h, const ClaymoreGUID& g) {
	HashAppend(h, &g.high, sizeof(g.high));
	HashAppend(h, &g.low, sizeof(g.low));
}

static void HashAssetRef(uint64_t& h, const AssetReference& r) {
	HashGuid(h, r.guid);
	HashAppend(h, &r.fileID, sizeof(r.fileID));
	HashAppend(h, &r.type, sizeof(r.type));
}

static std::string NormalizePath(const std::string& p) {
	std::string out = p;
	for (char& c : out) if (c == '\\') c = '/';
	return out;
}

static void HashVec3(uint64_t& h, const glm::vec3& v) {
	HashAppend(h, &v.x, sizeof(v.x));
	HashAppend(h, &v.y, sizeof(v.y));
	HashAppend(h, &v.z, sizeof(v.z));
}

static void HashVec2(uint64_t& h, const glm::vec2& v) {
	HashAppend(h, &v.x, sizeof(v.x));
	HashAppend(h, &v.y, sizeof(v.y));
}

static void HashQuat(uint64_t& h, const glm::quat& q) {
	HashAppend(h, &q.x, sizeof(q.x));
	HashAppend(h, &q.y, sizeof(q.y));
	HashAppend(h, &q.z, sizeof(q.z));
	HashAppend(h, &q.w, sizeof(q.w));
}

static void HashInlineMaterials(uint64_t& h, const std::vector<binary::InlineMaterialData>& mats) {
	uint64_t count = static_cast<uint64_t>(mats.size());
	HashAppend(h, &count, sizeof(count));
	for (const auto& m : mats) {
		HashAppend(h, &m.materialType, sizeof(m.materialType));
		HashString(h, NormalizePath(m.albedoPath));
		HashString(h, NormalizePath(m.normalPath));
		HashString(h, NormalizePath(m.metallicRoughnessPath));
		HashString(h, NormalizePath(m.emissionPath));
		HashString(h, NormalizePath(m.shaderGraphPath));
		HashAppend(h, &m.metallic, sizeof(m.metallic));
		HashAppend(h, &m.roughness, sizeof(m.roughness));
		HashAppend(h, &m.receiveShadowsOverride, sizeof(m.receiveShadowsOverride));
		HashAppend(h, &m.receiveShadows, sizeof(m.receiveShadows));
	}
}

static std::string SerializeInstancerForHash(const cm::instancer::InstancerComponent& instancer) {
	nlohmann::json data = Serializer::SerializeInstancer(instancer);
	if (data.contains("meshPath") && data["meshPath"].is_string()) {
		data["meshPath"] = NormalizePath(data["meshPath"].get<std::string>());
	}
	if (data.contains("prefabPath") && data["prefabPath"].is_string()) {
		data["prefabPath"] = NormalizePath(data["prefabPath"].get<std::string>());
	}
	return data.dump();
}

static void HashTerrainInstancerLayer(uint64_t& h, const TerrainInstancerLayerDesc& layer) {
	HashGuid(h, layer.Guid);
	HashString(h, layer.Name);
	HashAppend(h, &layer.Enabled, sizeof(layer.Enabled));
	uint8_t mask = static_cast<uint8_t>(layer.Mask);
	HashAppend(h, &mask, sizeof(mask));
	HashAppend(h, &layer.SplatThreshold, sizeof(layer.SplatThreshold));
	HashAppend(h, &layer.Collision.Enabled, sizeof(layer.Collision.Enabled));
	HashAppend(h, &layer.Collision.ActivationDistance, sizeof(layer.Collision.ActivationDistance));
	HashAppend(h, &layer.Collision.MaxActiveBodies, sizeof(layer.Collision.MaxActiveBodies));
	HashAppend(h, &layer.Collision.PhysicsLayer, sizeof(layer.Collision.PhysicsLayer));
	HashString(h, layer.Collision.PhysicsLayerName);
	HashAppend(h, &layer.Collision.UseSharedMeshShape, sizeof(layer.Collision.UseSharedMeshShape));
	HashString(h, SerializeInstancerForHash(layer.Instancer));
}

static void HashScripts(uint64_t& h, const std::vector<ScriptInstance>& scripts) {
	uint64_t count = static_cast<uint64_t>(scripts.size());
	HashAppend(h, &count, sizeof(count));
	for (const auto& s : scripts) {
		HashString(h, s.ClassName);
		std::vector<std::string> keys;
		keys.reserve(s.Values.size());
		for (const auto& kv : s.Values) keys.push_back(kv.first);
		std::sort(keys.begin(), keys.end());
		uint64_t keyCount = static_cast<uint64_t>(keys.size());
		HashAppend(h, &keyCount, sizeof(keyCount));
		for (const auto& k : keys) {
			HashString(h, k);
			auto it = s.Values.find(k);
			if (it != s.Values.end()) {
				std::string v = ScriptReflection::PropertyValueToString(it->second);
				HashString(h, v);
			}
		}
	}
}

uint64_t HashSubtree(const Entity& e, const HashOptions& opts) {
	(void)opts.includeChildOrder;
	uint64_t h = 1469598103934665603ull; // FNV offset basis
	Scene& s = Scene::Get();
	const EntityData* d = s.GetEntityData(e.GetID());
	if (!d) return h;

	HashString(h, d->Name);
	HashAppend(h, &d->Layer, sizeof(d->Layer));
	HashString(h, d->Tag);
	HashAppend(h, &d->Visible, sizeof(d->Visible));
	HashAppend(h, &d->Active, sizeof(d->Active));
	HashAppend(h, &d->Parent, sizeof(d->Parent));

	HashVec3(h, d->Transform.Position);
	HashVec3(h, d->Transform.Scale);
	HashAppend(h, &d->Transform.UseQuatRotation, sizeof(d->Transform.UseQuatRotation));
	HashVec3(h, d->Transform.Rotation);
	HashQuat(h, d->Transform.RotationQ);

	bool hasMesh = d->Mesh != nullptr; HashAppend(h, &hasMesh, sizeof(hasMesh));
	if (d->Mesh) {
		HashAssetRef(h, d->Mesh->meshReference);
		uint64_t mpCount = static_cast<uint64_t>(d->Mesh->MaterialAssetPaths.size());
		HashAppend(h, &mpCount, sizeof(mpCount));
		for (const auto& p : d->Mesh->MaterialAssetPaths) HashString(h, NormalizePath(p));
		HashAppend(h, &d->Mesh->RenderOnTop, sizeof(d->Mesh->RenderOnTop));
		HashAppend(h, &d->Mesh->ShowBackfaces, sizeof(d->Mesh->ShowBackfaces));
		HashAppend(h, &d->Mesh->SkipFrustumCulling, sizeof(d->Mesh->SkipFrustumCulling));
		HashAppend(h, &d->Mesh->BoundsPadding, sizeof(d->Mesh->BoundsPadding));
		HashAppend(h, &d->Mesh->UniqueMaterial, sizeof(d->Mesh->UniqueMaterial));
		HashAppend(h, &d->Mesh->EnableInstancing, sizeof(d->Mesh->EnableInstancing));
		HashInlineMaterials(h, d->Mesh->InlineMaterials);
	}

	bool hasLight = d->Light != nullptr; HashAppend(h, &hasLight, sizeof(hasLight));
	if (d->Light) {
		HashAppend(h, &d->Light->Type, sizeof(d->Light->Type));
		HashVec3(h, d->Light->Color);
		HashAppend(h, &d->Light->Intensity, sizeof(d->Light->Intensity));
	}

	bool hasCamera = d->Camera != nullptr; HashAppend(h, &hasCamera, sizeof(hasCamera));
	if (d->Camera) {
		HashAppend(h, &d->Camera->Active, sizeof(d->Camera->Active));
		HashAppend(h, &d->Camera->priority, sizeof(d->Camera->priority));
		HashAppend(h, &d->Camera->LayerMask, sizeof(d->Camera->LayerMask));
		HashAppend(h, &d->Camera->FieldOfView, sizeof(d->Camera->FieldOfView));
		HashAppend(h, &d->Camera->NearClip, sizeof(d->Camera->NearClip));
		HashAppend(h, &d->Camera->FarClip, sizeof(d->Camera->FarClip));
		HashAppend(h, &d->Camera->IsPerspective, sizeof(d->Camera->IsPerspective));
	}

	bool hasCollider = d->Collider != nullptr; HashAppend(h, &hasCollider, sizeof(hasCollider));
	if (d->Collider) {
		HashAppend(h, &d->Collider->ShapeType, sizeof(d->Collider->ShapeType));
		HashVec3(h, d->Collider->Offset);
		HashVec3(h, d->Collider->Size);
		HashAppend(h, &d->Collider->Radius, sizeof(d->Collider->Radius));
		HashAppend(h, &d->Collider->Height, sizeof(d->Collider->Height));
		HashString(h, NormalizePath(d->Collider->MeshPath));
		HashAppend(h, &d->Collider->IsTrigger, sizeof(d->Collider->IsTrigger));
		HashAppend(h, &d->Collider->PhysicsLayer, sizeof(d->Collider->PhysicsLayer));
	}

	bool hasRigidBody = d->RigidBody != nullptr; HashAppend(h, &hasRigidBody, sizeof(hasRigidBody));
	if (d->RigidBody) {
		HashAppend(h, &d->RigidBody->Mass, sizeof(d->RigidBody->Mass));
		HashAppend(h, &d->RigidBody->Friction, sizeof(d->RigidBody->Friction));
		HashAppend(h, &d->RigidBody->Restitution, sizeof(d->RigidBody->Restitution));
		HashAppend(h, &d->RigidBody->UseGravity, sizeof(d->RigidBody->UseGravity));
		HashAppend(h, &d->RigidBody->IsKinematic, sizeof(d->RigidBody->IsKinematic));
		HashAppend(h, &d->RigidBody->PhysicsLayer, sizeof(d->RigidBody->PhysicsLayer));
		HashAppend(h, &d->RigidBody->CollisionMask, sizeof(d->RigidBody->CollisionMask));
	}

	bool hasStaticBody = d->StaticBody != nullptr; HashAppend(h, &hasStaticBody, sizeof(hasStaticBody));
	if (d->StaticBody) {
		HashAppend(h, &d->StaticBody->Friction, sizeof(d->StaticBody->Friction));
		HashAppend(h, &d->StaticBody->Restitution, sizeof(d->StaticBody->Restitution));
		HashAppend(h, &d->StaticBody->PhysicsLayer, sizeof(d->StaticBody->PhysicsLayer));
	}

	bool hasAnim = d->AnimationPlayer != nullptr; HashAppend(h, &hasAnim, sizeof(hasAnim));
	if (d->AnimationPlayer) {
		HashString(h, NormalizePath(d->AnimationPlayer->ControllerPath));
		HashString(h, NormalizePath(d->AnimationPlayer->ControllerOverridePath));
		HashString(h, NormalizePath(d->AnimationPlayer->SingleClipPath));
		HashAppend(h, &d->AnimationPlayer->PlaybackSpeed, sizeof(d->AnimationPlayer->PlaybackSpeed));
		HashAppend(h, &d->AnimationPlayer->Enabled, sizeof(d->AnimationPlayer->Enabled));
		HashAppend(h, &d->AnimationPlayer->PlayOnStart, sizeof(d->AnimationPlayer->PlayOnStart));
		HashAppend(h, &d->AnimationPlayer->MotionTarget, sizeof(d->AnimationPlayer->MotionTarget));
		HashAppend(h, &d->AnimationPlayer->ExplicitTargetEntityId, sizeof(d->AnimationPlayer->ExplicitTargetEntityId));
	}

	bool hasPrefab = d->PrefabInstance != nullptr; HashAppend(h, &hasPrefab, sizeof(hasPrefab));
	if (d->PrefabInstance) {
		HashGuid(h, d->PrefabInstance->PrefabAssetGuid);
		HashString(h, NormalizePath(d->PrefabInstance->PrefabPath));
		HashGuid(h, d->PrefabInstance->ModelAssetGuid);
		uint64_t owned = static_cast<uint64_t>(d->PrefabInstance->OwnedEntityGuids.size());
		HashAppend(h, &owned, sizeof(owned));
	}

	bool hasNavAgent = d->NavAgent != nullptr; HashAppend(h, &hasNavAgent, sizeof(hasNavAgent));
	if (d->NavAgent) {
		HashAppend(h, &d->NavAgent->Enabled, sizeof(d->NavAgent->Enabled));
		HashAppend(h, &d->NavAgent->NavMeshEntity, sizeof(d->NavAgent->NavMeshEntity));
		HashAppend(h, &d->NavAgent->Params.radius, sizeof(d->NavAgent->Params.radius));
		HashAppend(h, &d->NavAgent->Params.height, sizeof(d->NavAgent->Params.height));
		HashAppend(h, &d->NavAgent->Params.maxSlopeDeg, sizeof(d->NavAgent->Params.maxSlopeDeg));
		HashAppend(h, &d->NavAgent->Params.maxStep, sizeof(d->NavAgent->Params.maxStep));
		HashAppend(h, &d->NavAgent->Params.maxSpeed, sizeof(d->NavAgent->Params.maxSpeed));
		HashAppend(h, &d->NavAgent->Params.maxAccel, sizeof(d->NavAgent->Params.maxAccel));
		HashAppend(h, &d->NavAgent->ArriveThreshold, sizeof(d->NavAgent->ArriveThreshold));
		HashAppend(h, &d->NavAgent->AutoRepath, sizeof(d->NavAgent->AutoRepath));
		HashAppend(h, &d->NavAgent->SteeringSmoothness, sizeof(d->NavAgent->SteeringSmoothness));
		HashAppend(h, &d->NavAgent->ArrivalSlowdownDist, sizeof(d->NavAgent->ArrivalSlowdownDist));
	}

	bool hasNavMesh = d->Navigation != nullptr; HashAppend(h, &hasNavMesh, sizeof(hasNavMesh));
	if (d->Navigation) {
		HashAppend(h, &d->Navigation->Enabled, sizeof(d->Navigation->Enabled));
		HashGuid(h, d->Navigation->NavMeshDataGuid);
		HashString(h, NormalizePath(d->Navigation->AssetPath));
		HashAppend(h, &d->Navigation->TerrainSampleStep, sizeof(d->Navigation->TerrainSampleStep));
		HashAppend(h, &d->Navigation->GeometryIncludeRegexEnabled, sizeof(d->Navigation->GeometryIncludeRegexEnabled));
		HashString(h, d->Navigation->GeometryIncludeRegexPattern);
		HashAppend(h, &d->Navigation->BakeVisibleChunksOnly, sizeof(d->Navigation->BakeVisibleChunksOnly));
		HashAppend(h, &d->Navigation->BakeVisibleChunkPadding, sizeof(d->Navigation->BakeVisibleChunkPadding));
		HashAppend(h, &d->Navigation->BakeMissingChunksOnly, sizeof(d->Navigation->BakeMissingChunksOnly));
		HashAppend(h, &d->Navigation->ChunkedNavEnabled, sizeof(d->Navigation->ChunkedNavEnabled));
		HashAppend(h, &d->Navigation->ChunkingMode, sizeof(d->Navigation->ChunkingMode));
		HashAppend(h, &d->Navigation->ChunkWorldSize, sizeof(d->Navigation->ChunkWorldSize));
		HashAppend(h, &d->Navigation->ChunkBakePadding, sizeof(d->Navigation->ChunkBakePadding));
		HashAppend(h, &d->Navigation->ChunkStreamRadius, sizeof(d->Navigation->ChunkStreamRadius));
		HashString(h, NormalizePath(d->Navigation->NavPackPath));
		HashAppend(h, &d->Navigation->AgentPlacementOffset, sizeof(d->Navigation->AgentPlacementOffset));
	}

	bool hasNavLink = d->NavLink != nullptr; HashAppend(h, &hasNavLink, sizeof(hasNavLink));
	if (d->NavLink) {
		HashAppend(h, &d->NavLink->Enabled, sizeof(d->NavLink->Enabled));
		HashVec3(h, d->NavLink->Start);
		HashVec3(h, d->NavLink->End);
		HashAppend(h, &d->NavLink->Radius, sizeof(d->NavLink->Radius));
		HashAppend(h, &d->NavLink->Cost, sizeof(d->NavLink->Cost));
		HashAppend(h, &d->NavLink->Flags, sizeof(d->NavLink->Flags));
		HashAppend(h, &d->NavLink->Bidirectional, sizeof(d->NavLink->Bidirectional));
		HashAppend(h, &d->NavLink->UseWorldSpace, sizeof(d->NavLink->UseWorldSpace));
	}

	bool hasPortal = d->Portal != nullptr; HashAppend(h, &hasPortal, sizeof(hasPortal));
	if (d->Portal) {
		HashAppend(h, &d->Portal->Enabled, sizeof(d->Portal->Enabled));
		HashString(h, NormalizePath(d->Portal->TargetScenePath));
		HashGuid(h, d->Portal->TargetPortalGuid);
		HashString(h, d->Portal->TargetPortalPath);
		HashVec3(h, d->Portal->EntryOffset);
		HashVec3(h, d->Portal->ExitOffset);
		HashAppend(h, &d->Portal->AutoDetect, sizeof(d->Portal->AutoDetect));
		HashAppend(h, &d->Portal->TriggerRadius, sizeof(d->Portal->TriggerRadius));
		HashAppend(h, &d->Portal->FireExitEvents, sizeof(d->Portal->FireExitEvents));
	}

	bool hasTerrain = d->Terrain != nullptr; HashAppend(h, &hasTerrain, sizeof(hasTerrain));
	if (d->Terrain) {
		HashGuid(h, d->Terrain->TerrainDataGuid);
		HashString(h, NormalizePath(d->Terrain->AssetPath));
		HashAppend(h, &d->Terrain->GridResolution, sizeof(d->Terrain->GridResolution));
		HashVec2(h, d->Terrain->WorldSize);
		HashAppend(h, &d->Terrain->MaxHeight, sizeof(d->Terrain->MaxHeight));
		uint64_t hm = static_cast<uint64_t>(d->Terrain->HeightMap.size());
		uint64_t sm = static_cast<uint64_t>(d->Terrain->SplatMap.size());
		HashAppend(h, &hm, sizeof(hm));
		HashAppend(h, &sm, sizeof(sm));
		uint64_t layers = static_cast<uint64_t>(d->Terrain->Layers.size());
		uint64_t grass = static_cast<uint64_t>(d->Terrain->GrassLayers.size());
		uint64_t instancers = static_cast<uint64_t>(d->Terrain->InstancerLayers.size());
		HashAppend(h, &layers, sizeof(layers));
		HashAppend(h, &grass, sizeof(grass));
		HashAppend(h, &instancers, sizeof(instancers));
		for (const auto& layer : d->Terrain->InstancerLayers) {
			HashTerrainInstancerLayer(h, layer);
		}
	}

	HashScripts(h, d->Scripts);
	return h;
}

uint64_t HashScene(const Scene& s, const HashOptions& opts) {
	uint64_t h = 1469598103934665603ull;
	for (const auto& e : s.GetEntities()) {
		uint64_t eh = HashSubtree(e, opts);
		HashAppend(h, &eh, sizeof(eh));
	}
	return h;
}

}} // namespace cm::editor


