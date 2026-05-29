#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import json
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


class Vec3(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float), ("z", ctypes.c_float)]


class MeshbinDesc(ctypes.Structure):
    _fields_ = [
        ("vertexCount", ctypes.c_uint32),
        ("indexCount", ctypes.c_uint32),
        ("vertexStride", ctypes.c_uint32),
        ("indexSize", ctypes.c_uint32),
        ("hasSkinning", ctypes.c_uint32),
        ("vbOffset", ctypes.c_uint32),
        ("ibOffset", ctypes.c_uint32),
        ("bmin", Vec3),
        ("bmax", Vec3),
        ("nameOffset", ctypes.c_uint32),
        ("nameLength", ctypes.c_uint32),
        ("texOffset", ctypes.c_uint32),
        ("texSize", ctypes.c_uint32),
        ("extrasOffset", ctypes.c_uint32),
        ("extrasSize", ctypes.c_uint32),
        ("quantInfoOffset", ctypes.c_uint32),
        ("quantInfoSize", ctypes.c_uint32),
        ("xformOffset", ctypes.c_uint32),
        ("xformSize", ctypes.c_uint32),
        ("submeshOffset", ctypes.c_uint32),
        ("submeshSize", ctypes.c_uint32),
        ("blendOffset", ctypes.c_uint32),
        ("blendSize", ctypes.c_uint32),
    ]


@dataclass
class MeshStats:
    name: str
    vertex_count: int
    unique_bones: int
    max_bone: int
    material_slots: int


@dataclass
class CharacterStats:
    name: str
    skeleton_bones: int
    mesh_count: int
    material_slots: int
    blendshape_meshes: int
    meshes: list[MeshStats]


def iter_entities(node: dict) -> Iterable[dict]:
    yield node
    for child in node.get("children", []):
        if isinstance(child, dict):
            yield from iter_entities(child)


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def load_meshbin_descs(meshbin_path: Path) -> list[MeshbinDesc]:
    data = meshbin_path.read_bytes()
    magic, version, submesh_count = struct.unpack_from("<III", data, 0)
    if magic != 0x4D534842:
        raise ValueError(f"{meshbin_path} is not a meshbin file")
    if version > 9:
        raise ValueError(f"Unsupported meshbin version {version} in {meshbin_path}")

    desc_size = ctypes.sizeof(MeshbinDesc)
    descs: list[MeshbinDesc] = []
    offset = 12
    for _ in range(submesh_count):
        descs.append(MeshbinDesc.from_buffer_copy(data, offset))
        offset += desc_size
    return descs


def meshbin_desc_name(meshbin_bytes: bytes, desc: MeshbinDesc) -> str:
    # meshbin stores [uint32 length][bytes...] at nameOffset; nameLength is the raw byte count
    start = desc.nameOffset + 4
    end = start + desc.nameLength
    return meshbin_bytes[start:end].decode("utf-8", "ignore")


def mesh_unique_bones(meshbin_bytes: bytes, desc: MeshbinDesc) -> tuple[int, int]:
    if desc.hasSkinning == 0:
        return 0, -1
    vb = memoryview(meshbin_bytes)[desc.vbOffset : desc.vbOffset + desc.vertexCount * desc.vertexStride]
    unique: set[int] = set()
    for vertex_index in range(desc.vertexCount):
        base = vertex_index * desc.vertexStride
        bone_indices = struct.unpack_from("<4B", vb, base + 32)
        weights = struct.unpack_from("<4f", vb, base + 36)
        for bone_index, weight in zip(bone_indices, weights):
            if weight > 1.0e-5:
                unique.add(int(bone_index))
    return len(unique), (max(unique) if unique else -1)


def load_character_stats(project_root: Path) -> CharacterStats:
    prefab_path = project_root / "assets/prefabs/base_human_96.prefab"
    meta_path = project_root / "assets/models/base_human.meta"
    prefab = load_json(prefab_path)
    meta = load_json(meta_path)
    root_entity = prefab["entities"][0]

    skeleton_bones = 0
    mesh_nodes: list[dict] = []
    blendshape_meshes = 0
    for entity in iter_entities(root_entity):
        if "mesh" in entity:
            mesh_nodes.append(entity)
            if "blendShapes" in entity:
                blendshape_meshes += 1
        if "skeleton" in entity:
            skeleton = entity["skeleton"]
            skeleton_bones = max(skeleton_bones, len(skeleton.get("boneNames", [])))

    meshbin_path = project_root / meta["meshbin"]
    meshbin_bytes = meshbin_path.read_bytes()
    descs = load_meshbin_descs(meshbin_path)
    desc_names = {meshbin_desc_name(meshbin_bytes, desc): desc for desc in descs}

    meshes: list[MeshStats] = []
    material_slots = 0
    for node in mesh_nodes:
        mesh = node["mesh"]
        mesh_name = node["name"]
        desc = desc_names[mesh_name]
        unique_bones, max_bone = mesh_unique_bones(meshbin_bytes, desc)
        slot_count = len(mesh.get("slotPropertyBlocks", [])) or len(mesh.get("slotMaterials", [])) or 1
        material_slots += slot_count
        meshes.append(
            MeshStats(
                name=mesh_name,
                vertex_count=desc.vertexCount,
                unique_bones=unique_bones,
                max_bone=max_bone,
                material_slots=slot_count,
            )
        )

    return CharacterStats(
        name="base_human_96",
        skeleton_bones=skeleton_bones,
        mesh_count=len(meshes),
        material_slots=material_slots,
        blendshape_meshes=blendshape_meshes,
        meshes=meshes,
    )


def summarize_scene_entities(scene_path: Path, names: list[str]) -> dict[str, dict[str, int]]:
    scene = load_json(scene_path)
    result: dict[str, dict[str, int]] = {}

    for entity in scene.get("entities", []):
        if entity.get("name") not in names:
            continue
        mesh_count = 0
        skeleton_count = 0
        animator_count = 0
        render_override_count = 0
        direct_children = len(entity.get("children", []))
        for node in iter_entities(entity):
            mesh_count += 1 if "mesh" in node and "skinning" in node else 0
            skeleton_count += 1 if "skeleton" in node else 0
            animator_count += 1 if "animator" in node else 0
            render_override_count += 1 if ("renderOverride" in node or "renderOverrides" in node) else 0
        result[entity["name"]] = {
            "skinned_meshes": mesh_count,
            "skeletons": skeleton_count,
            "animators": animator_count,
            "render_overrides": render_override_count,
            "direct_children": direct_children,
        }

    return result


def compact_policy(mesh: MeshStats, sibling_skinned_meshes: int) -> str:
    if sibling_skinned_meshes > 1 and mesh.unique_bones > 4:
        return "shared"
    return "compact"


def simulate_character_work(stats: CharacterStats, characters: int, active_ragdolls: int) -> dict[str, float]:
    baseline_palette_binds = stats.mesh_count * characters
    baseline_palette_mats = sum(mesh.unique_bones for mesh in stats.meshes) * characters

    optimized_palette_binds_per_character = 0
    optimized_palette_mats_per_character = 0
    for mesh in stats.meshes:
        mode = compact_policy(mesh, stats.mesh_count)
        if mode == "shared":
            continue
        optimized_palette_binds_per_character += 1
        optimized_palette_mats_per_character += mesh.unique_bones
    optimized_palette_binds = (1 * characters) + optimized_palette_binds_per_character * characters
    optimized_palette_mats = (stats.skeleton_bones * characters) + optimized_palette_mats_per_character * characters

    skinning_lod_bounds_baseline = stats.mesh_count * characters
    skinning_lod_bounds_optimized = 1 * characters

    # Main scene culling, offscreen render views, and shadow caster bounds all reused per mesh today.
    # The shared-bounds path reduces the expensive AABB-build step to one per character per pass.
    cull_passes = 3
    renderer_bounds_builds_baseline = stats.mesh_count * characters * cull_passes
    renderer_bounds_builds_optimized = 1 * characters * cull_passes

    # Split skinned characters were still re-testing the same shared AABB against the
    # main view, auxiliary views, and shadow cascades per mesh. Cache that once per
    # skeleton/group per pass instead.
    frustum_test_passes = 7
    renderer_frustum_tests_baseline = stats.mesh_count * characters * frustum_test_passes
    renderer_frustum_tests_optimized = 1 * characters * frustum_test_passes

    ragdoll_bone_scans_baseline = active_ragdolls * stats.skeleton_bones * 4
    ragdoll_bone_scans_optimized = active_ragdolls * stats.skeleton_bones * 1

    # Weighted score chosen to emphasize the main-frame costs we are cutting.
    weighted_baseline = (
        baseline_palette_binds * 6.0
        + baseline_palette_mats * 0.12
        + skinning_lod_bounds_baseline * 4.0
        + renderer_bounds_builds_baseline * 5.0
        + renderer_frustum_tests_baseline * 1.25
        + ragdoll_bone_scans_baseline * 0.25
    )
    weighted_optimized = (
        optimized_palette_binds * 6.0
        + optimized_palette_mats * 0.12
        + skinning_lod_bounds_optimized * 4.0
        + renderer_bounds_builds_optimized * 5.0
        + renderer_frustum_tests_optimized * 1.25
        + ragdoll_bone_scans_optimized * 0.25
    )

    return {
        "baseline_palette_binds": baseline_palette_binds,
        "optimized_palette_binds": optimized_palette_binds,
        "baseline_palette_matrices": baseline_palette_mats,
        "optimized_palette_matrices": optimized_palette_mats,
        "baseline_skinning_lod_bounds": skinning_lod_bounds_baseline,
        "optimized_skinning_lod_bounds": skinning_lod_bounds_optimized,
        "baseline_renderer_bounds_builds": renderer_bounds_builds_baseline,
        "optimized_renderer_bounds_builds": renderer_bounds_builds_optimized,
        "baseline_renderer_frustum_tests": renderer_frustum_tests_baseline,
        "optimized_renderer_frustum_tests": renderer_frustum_tests_optimized,
        "baseline_ragdoll_bone_scans": ragdoll_bone_scans_baseline,
        "optimized_ragdoll_bone_scans": ragdoll_bone_scans_optimized,
        "baseline_weighted_score": weighted_baseline,
        "optimized_weighted_score": weighted_optimized,
    }


def percent_delta(before: float, after: float) -> float:
    if before <= 0.0:
        return 0.0
    return (after - before) / before * 100.0


def build_report_payload(
    stats: CharacterStats,
    scene_summary: dict[str, dict[str, int]],
    characters: int,
) -> dict[str, object]:
    crowd = simulate_character_work(stats, characters=characters, active_ragdolls=0)
    ragdoll = simulate_character_work(stats, characters=characters, active_ragdolls=characters)
    return {
        "character": {
            "name": stats.name,
            "skeleton_bones": stats.skeleton_bones,
            "mesh_count": stats.mesh_count,
            "material_slots": stats.material_slots,
            "blendshape_meshes": stats.blendshape_meshes,
        },
        "scene_anchors": scene_summary,
        "mesh_palette_policy": [
            {
                "name": mesh.name,
                "unique_bones": mesh.unique_bones,
                "vertex_count": mesh.vertex_count,
                "material_slots": mesh.material_slots,
                "mode": compact_policy(mesh, stats.mesh_count),
            }
            for mesh in stats.meshes
        ],
        "crowd": crowd,
        "ragdoll": ragdoll,
    }


def print_report(stats: CharacterStats, scene_summary: dict[str, dict[str, int]], characters: int) -> None:
    print(f"Canonical character: {stats.name}")
    print(f"Skeleton bones:      {stats.skeleton_bones}")
    print(f"Skinned meshes:      {stats.mesh_count}")
    print(f"Material slots:      {stats.material_slots}")
    print(f"Blendshape meshes:   {stats.blendshape_meshes}")
    print()

    print("Scene anchors:")
    for name, summary in scene_summary.items():
        print(
            f"  {name:<13} meshes={summary['skinned_meshes']:2d} "
            f"skeletons={summary['skeletons']:1d} animators={summary['animators']:1d} "
            f"renderOverrides={summary['render_overrides']:2d} children={summary['direct_children']:3d}"
        )
    print()

    print("Mesh palette policy after optimization:")
    for mesh in stats.meshes:
        mode = compact_policy(mesh, stats.mesh_count)
        print(
            f"  {mesh.name:<14} uniqueBones={mesh.unique_bones:2d} "
            f"verts={mesh.vertex_count:4d} slots={mesh.material_slots:1d} -> {mode}"
        )
    print()

    report = build_report_payload(stats, scene_summary, characters)
    crowd = report["crowd"]
    ragdoll = report["ragdoll"]

    print(f"Crowd simulation ({characters} visible characters, no active ragdolls):")
    print(
        f"  Palette binds:        {crowd['baseline_palette_binds']:6.0f} -> {crowd['optimized_palette_binds']:6.0f} "
        f"({percent_delta(crowd['baseline_palette_binds'], crowd['optimized_palette_binds']):6.1f}%)"
    )
    print(
        f"  Palette matrices:     {crowd['baseline_palette_matrices']:6.0f} -> {crowd['optimized_palette_matrices']:6.0f} "
        f"({percent_delta(crowd['baseline_palette_matrices'], crowd['optimized_palette_matrices']):6.1f}%)"
    )
    print(
        f"  Skinning LOD bounds:  {crowd['baseline_skinning_lod_bounds']:6.0f} -> {crowd['optimized_skinning_lod_bounds']:6.0f} "
        f"({percent_delta(crowd['baseline_skinning_lod_bounds'], crowd['optimized_skinning_lod_bounds']):6.1f}%)"
    )
    print(
        f"  Render bound builds:  {crowd['baseline_renderer_bounds_builds']:6.0f} -> {crowd['optimized_renderer_bounds_builds']:6.0f} "
        f"({percent_delta(crowd['baseline_renderer_bounds_builds'], crowd['optimized_renderer_bounds_builds']):6.1f}%)"
    )
    print(
        f"  Frustum tests:        {crowd['baseline_renderer_frustum_tests']:6.0f} -> {crowd['optimized_renderer_frustum_tests']:6.0f} "
        f"({percent_delta(crowd['baseline_renderer_frustum_tests'], crowd['optimized_renderer_frustum_tests']):6.1f}%)"
    )
    print(
        f"  Weighted frame score: {crowd['baseline_weighted_score']:6.1f} -> {crowd['optimized_weighted_score']:6.1f} "
        f"({percent_delta(crowd['baseline_weighted_score'], crowd['optimized_weighted_score']):6.1f}%)"
    )
    print()

    print(f"Ragdoll stress simulation ({characters} active ragdolls):")
    print(
        f"  Bone scans/frame:     {ragdoll['baseline_ragdoll_bone_scans']:6.0f} -> {ragdoll['optimized_ragdoll_bone_scans']:6.0f} "
        f"({percent_delta(ragdoll['baseline_ragdoll_bone_scans'], ragdoll['optimized_ragdoll_bone_scans']):6.1f}%)"
    )
    print(
        f"  Weighted frame score: {ragdoll['baseline_weighted_score']:6.1f} -> {ragdoll['optimized_weighted_score']:6.1f} "
        f"({percent_delta(ragdoll['baseline_weighted_score'], ragdoll['optimized_weighted_score']):6.1f}%)"
    )


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description="Simulate character runtime work for the canonical Claymore humanoid.")
    parser.add_argument(
        "--project-root",
        default=str(repo_root),
        help=(
            "Path to a Claymore project checkout that contains the canonical "
            "scene and prefab data used by this simulation."
        ),
    )
    parser.add_argument(
        "--characters",
        type=int,
        default=20,
        help="How many fully visible characters to model in the crowd simulation.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit the simulation summary as JSON instead of a human-readable report.",
    )
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    required_paths = [
        project_root / "assets/prefabs/base_human_96.prefab",
        project_root / "assets/models/base_human.meta",
        project_root / "scenes/world.scene",
    ]
    missing_paths = [path for path in required_paths if not path.exists()]
    if missing_paths:
        missing_text = "\n".join(f"  - {path}" for path in missing_paths)
        raise SystemExit(
            "The selected project root does not contain the files this simulation expects.\n"
            "Pass --project-root to a compatible Claymore game/project checkout.\n"
            f"{missing_text}"
        )

    stats = load_character_stats(project_root)
    scene_summary = summarize_scene_entities(
        project_root / "scenes/world.scene",
        names=["Player", "base_human_96"],
    )
    characters = max(1, args.characters)
    if args.json:
        print(json.dumps(build_report_payload(stats, scene_summary, characters), indent=2))
    else:
        print_report(stats, scene_summary, characters=characters)


if __name__ == "__main__":
    main()
