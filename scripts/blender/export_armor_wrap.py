"""
Claymore Armor Wrap Export Script for Blender
==============================================

This script exports armor-to-body surface binding data for the Claymore engine's
armor wrap deformation system.

Usage:
1. Open this script in Blender's Text Editor
2. Select the armor object in the 3D viewport
3. Set BODY_OBJECT_NAME below to match your body mesh name
4. Run the script (Alt+P in Text Editor)
5. The .wrap.json file will be saved next to the armor mesh file

The exported JSON is then imported by Claymore's editor pipeline and converted
to a runtime-ready .wrapbin file.

Requirements:
- Both armor and body meshes must be in the scene
- Meshes should be in their final posed state (or use rest pose)
- Body mesh should be triangulated (or will be auto-triangulated)
"""

import bpy
import json
import os
from mathutils import Vector
from mathutils.bvhtree import BVHTree

# ============================================================================
# Configuration
# ============================================================================

# Name of the body mesh object in the scene
BODY_OBJECT_NAME = "Body"

# Default wrap weight for all vertices (0.0 - 1.0)
DEFAULT_WRAP_WEIGHT = 1.0

# Output filename suffix (appended to armor object name)
OUTPUT_SUFFIX = ".wrap.json"


# ============================================================================
# Barycentric Coordinate Computation
# ============================================================================

def compute_barycentric(point, tri_verts):
    """
    Compute barycentric coordinates of a point with respect to a triangle.
    
    Args:
        point: Vector3 - The point to compute coords for
        tri_verts: tuple of 3 Vector3 - Triangle vertices (a, b, c)
    
    Returns:
        tuple (u, v, w) - Barycentric coordinates where point = u*a + v*b + w*c
    """
    a, b, c = tri_verts
    
    v0 = c - a
    v1 = b - a
    v2 = point - a
    
    dot00 = v0.dot(v0)
    dot01 = v0.dot(v1)
    dot02 = v0.dot(v2)
    dot11 = v1.dot(v1)
    dot12 = v1.dot(v2)
    
    denom = dot00 * dot11 - dot01 * dot01
    if abs(denom) < 1e-10:
        # Degenerate triangle
        return (1.0, 0.0, 0.0)
    
    inv_denom = 1.0 / denom
    w = (dot00 * dot12 - dot01 * dot02) * inv_denom
    v = (dot11 * dot02 - dot01 * dot12) * inv_denom
    u = 1.0 - v - w
    
    return (u, v, w)


# ============================================================================
# Main Export Function
# ============================================================================

def export_armor_wrap(armor_obj, body_obj, output_path):
    """
    Export armor wrap binding data to JSON.
    
    Args:
        armor_obj: Blender mesh object (armor)
        body_obj: Blender mesh object (body)
        output_path: Output .wrap.json file path
    
    Returns:
        tuple (success: bool, message: str)
    """
    
    # Get evaluated meshes (with modifiers applied)
    depsgraph = bpy.context.evaluated_depsgraph_get()
    
    armor_eval = armor_obj.evaluated_get(depsgraph)
    armor_mesh = armor_eval.to_mesh()
    
    body_eval = body_obj.evaluated_get(depsgraph)
    body_mesh = body_eval.to_mesh()
    
    # Ensure body mesh is triangulated
    # BVHTree works on triangles, so we triangulate temporarily
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(body_mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(body_mesh)
    bm.free()
    
    # Build BVH tree from body mesh
    body_bvh = BVHTree.FromObject(body_obj, depsgraph)
    
    # Get body mesh data
    body_verts = [v.co.copy() for v in body_mesh.vertices]
    body_polys = body_mesh.polygons
    
    # Get armor mesh data
    armor_verts = armor_mesh.vertices
    
    # Transform matrices (armor -> body space if needed)
    armor_mat = armor_obj.matrix_world
    body_mat_inv = body_obj.matrix_world.inverted()
    
    # Compute bindings
    wrap_data = []
    errors = 0
    
    for i, vert in enumerate(armor_verts):
        # Transform armor vertex to world space, then to body local space
        world_pos = armor_mat @ vert.co
        local_pos = body_mat_inv @ world_pos
        
        # Find nearest point on body surface
        location, normal, face_idx, distance = body_bvh.find_nearest(local_pos)
        
        if location is None or face_idx is None:
            print(f"Warning: No nearest point found for vertex {i}")
            # Use fallback (first triangle)
            face_idx = 0
            w0, w1, w2 = 1.0, 0.0, 0.0
            errors += 1
        else:
            # Get triangle vertices
            poly = body_polys[face_idx]
            if len(poly.vertices) != 3:
                print(f"Warning: Non-triangle face {face_idx} (has {len(poly.vertices)} verts)")
                errors += 1
            
            tri_verts = tuple(body_verts[vi] for vi in poly.vertices[:3])
            
            # Compute barycentric coordinates
            w0, w1, w2 = compute_barycentric(location, tri_verts)
        
        # Clamp weights to valid range
        w0 = max(0.0, min(1.0, w0))
        w1 = max(0.0, min(1.0, w1))
        w2 = max(0.0, min(1.0, w2))
        
        # Normalize (in case of small errors)
        total = w0 + w1 + w2
        if total > 0:
            w0 /= total
            w1 /= total
            w2 /= total
        
        entry = {
            "tri": face_idx,
            "w0": round(w0, 6),
            "w1": round(w1, 6),
            "w2": round(w2, 6),
            "weight": DEFAULT_WRAP_WEIGHT,
            "flags": 0
        }
        wrap_data.append(entry)
    
    # Clean up temporary meshes
    armor_eval.to_mesh_clear()
    body_eval.to_mesh_clear()
    
    # Build output JSON
    output = {
        "version": 1,
        "armor_object": armor_obj.name,
        "body_object": body_obj.name,
        "vertex_count": len(wrap_data),
        "wrap": wrap_data
    }
    
    # Write JSON
    with open(output_path, 'w') as f:
        json.dump(output, f, indent=2)
    
    message = f"Exported {len(wrap_data)} wrap bindings to: {output_path}"
    if errors > 0:
        message += f" ({errors} warnings)"
    
    return True, message


# ============================================================================
# Blender Operator
# ============================================================================

class EXPORT_OT_armor_wrap(bpy.types.Operator):
    """Export armor wrap binding data for Claymore engine"""
    bl_idname = "export.armor_wrap"
    bl_label = "Export Armor Wrap"
    bl_options = {'REGISTER', 'UNDO'}
    
    body_object: bpy.props.StringProperty(
        name="Body Object",
        description="Name of the body mesh object",
        default=BODY_OBJECT_NAME
    )
    
    wrap_weight: bpy.props.FloatProperty(
        name="Wrap Weight",
        description="Default wrap weight for all vertices",
        default=DEFAULT_WRAP_WEIGHT,
        min=0.0,
        max=1.0
    )
    
    def execute(self, context):
        global DEFAULT_WRAP_WEIGHT
        DEFAULT_WRAP_WEIGHT = self.wrap_weight
        
        # Get selected armor object
        armor_obj = context.active_object
        if armor_obj is None or armor_obj.type != 'MESH':
            self.report({'ERROR'}, "Select an armor mesh object")
            return {'CANCELLED'}
        
        # Get body object
        body_obj = bpy.data.objects.get(self.body_object)
        if body_obj is None:
            self.report({'ERROR'}, f"Body object '{self.body_object}' not found")
            return {'CANCELLED'}
        
        if body_obj.type != 'MESH':
            self.report({'ERROR'}, f"Body object '{self.body_object}' is not a mesh")
            return {'CANCELLED'}
        
        # Determine output path
        if armor_obj.data.library:
            # Linked mesh - use blend file directory
            output_dir = os.path.dirname(bpy.data.filepath)
        else:
            output_dir = os.path.dirname(bpy.data.filepath)
        
        output_path = os.path.join(output_dir, armor_obj.name + OUTPUT_SUFFIX)
        
        # Export
        success, message = export_armor_wrap(armor_obj, body_obj, output_path)
        
        if success:
            self.report({'INFO'}, message)
            return {'FINISHED'}
        else:
            self.report({'ERROR'}, message)
            return {'CANCELLED'}
    
    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)
    
    def draw(self, context):
        layout = self.layout
        layout.prop(self, "body_object")
        layout.prop(self, "wrap_weight")


# ============================================================================
# Panel UI
# ============================================================================

class VIEW3D_PT_armor_wrap(bpy.types.Panel):
    """Armor Wrap Export Panel"""
    bl_label = "Claymore Armor Wrap"
    bl_idname = "VIEW3D_PT_armor_wrap"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Claymore"
    
    def draw(self, context):
        layout = self.layout
        
        obj = context.active_object
        
        if obj and obj.type == 'MESH':
            layout.label(text=f"Armor: {obj.name}")
            layout.operator("export.armor_wrap")
        else:
            layout.label(text="Select an armor mesh")


# ============================================================================
# Registration
# ============================================================================

classes = [
    EXPORT_OT_armor_wrap,
    VIEW3D_PT_armor_wrap,
]

def register():
    for cls in classes:
        bpy.utils.register_class(cls)

def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)

if __name__ == "__main__":
    register()
    
    # If running directly, try to export active selection
    if bpy.context.active_object:
        bpy.ops.export.armor_wrap('INVOKE_DEFAULT')

