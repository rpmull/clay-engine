#pragma once

#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <cstdint>
#include <bgfx/bgfx.h>
#include "core/assets/AssetReference.h"

// Runtime UI components: Canvas, Panel, Button, Slider, ProgressBar, Toggle, ScrollView, LayoutGroup
// All components support both screen-space and world-space rendering (e.g., health bars over characters)
// World-space is determined by the parent Canvas's RenderSpace setting

struct CanvasComponent {
    // If zero, canvas size is derived from framebuffer/window size
    int Width = 0;
    int Height = 0;

    // Global UI scale factor for DPI or user preference
    float DPIScale = 1.0f;

    enum class RenderSpace {
        ScreenSpace,
        WorldSpace
    };
    RenderSpace Space = RenderSpace::ScreenSpace;

    // Sorting order relative to other canvases (lower renders first)
    int SortOrder = 0;

    // Overall opacity for this canvas (multiplies child panels/text alpha)
    float Opacity = 1.0f;

    // If true, UI interactions on this canvas can block scene input
    bool BlockSceneInput = true;

    // When true, world-space canvases face the active camera.
    // Screen-space canvases ignore this flag.
    bool Billboard = true;
    
    // Reference Resolution - UI is designed at this resolution and scales to actual screen
    // When both are 0, no scaling is applied (legacy behavior with absolute pixels)
    int ReferenceWidth = 0;
    int ReferenceHeight = 0;
    
    // How to scale UI when aspect ratios differ
    enum class ScaleMode {
        ConstantPixelSize = 0,  // No scaling (legacy behavior)
        ScaleWithWidth,         // Scale based on width ratio
        ScaleWithHeight,        // Scale based on height ratio  
        ScaleWithSmallest,      // Use the smaller scale factor (UI never overflows)
        ScaleWithLargest,       // Use the larger scale factor (UI always fills)
        Expand                  // Match width and expand/contract height (or vice versa)
    };
    ScaleMode ReferenceScaleMode = ScaleMode::ConstantPixelSize;
};

// Common UI anchoring presets used by panels and text
enum class UIAnchorPreset : int {
    TopLeft = 0,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight
};

struct PanelComponent {
    // Top-left anchored position in canvas pixels
    glm::vec2 Position = {0.0f, 0.0f};
    // Size in pixels
    glm::vec2 Size = {100.0f, 100.0f};
    // Additional scaling factor (applied after Size)
    glm::vec2 Scale = {1.0f, 1.0f};
    // Pivot inside the panel rect (0..1)
    glm::vec2 Pivot = {0.5f, 0.5f};
    // Rotation in degrees (around pivot)
    float Rotation = 0.0f;

    // Anchor-based placement (optional)
    bool AnchorEnabled = false;
    bool AnchorToParentUI = false;
    UIAnchorPreset Anchor = UIAnchorPreset::TopLeft;
    glm::vec2 AnchorOffset = {0.0f, 0.0f};

    // Visuals
    AssetReference Texture; // type should be texture (e.g. type = 2)
    glm::vec4 UVRect = {0.0f, 0.0f, 1.0f, 1.0f}; // {u0, v0, u1, v1}
    glm::vec4 TintColor = {1.0f, 1.0f, 1.0f, 1.0f};
    float Opacity = 1.0f;
    bool DriveChildrenOpacity = false;

    // Fill mode
    enum class FillMode { Stretch = 0, Tile = 1, NineSlice = 2 };
    FillMode Mode = FillMode::Stretch;
    // For Tile mode: how many repeats over the panel area
    glm::vec2 TileRepeat = {1.0f, 1.0f};
    // For NineSlice: normalized margins in UV (left, top, right, bottom)
    glm::vec4 SliceUV = {0.1f, 0.1f, 0.1f, 0.1f};
    // For NineSlice: fixed pixel border sizes (left, top, right, bottom)
    // When non-zero, these override the UV-based calculation for fixed-size borders
    glm::vec4 SliceBorder = {0.0f, 0.0f, 0.0f, 0.0f};
    bool Visible = true;
    int ZOrder = 0; // sorting within a canvas (lower renders first)

    // Drag & drop (runtime UI interaction)
    bool AllowDrag = false;
    bool AllowDrop = false;
    bool Hovered = false;
    bool Pressed = false;
    bool Dragging = false;
    bool DragStarted = false; // one-frame pulse when drag begins
    bool DragEnded = false;   // one-frame pulse when drag ends
    bool Dropped = false;     // one-frame pulse when another panel dropped here
    int DropSourceEntity = -1;
    int DropTargetEntity = -1;

    // External texture override (e.g., render-to-texture)
    bool UseExternalTexture = false;
    bgfx::TextureHandle ExternalTextureHandle = BGFX_INVALID_HANDLE;
    
    // Runtime texture caching (not serialized)
    mutable bgfx::TextureHandle CachedTextureHandle = BGFX_INVALID_HANDLE;
};

struct ButtonComponent {
    // Interaction state (runtime)
    bool Interactable = true;
    bool Hovered = false;
    bool Pressed = false;
    bool Clicked = false; // true for one frame when released after press

    // Toggle button behavior
    bool Toggle = false;
    bool Toggled = false;

    // Visual overrides by state (multiplied with panel tint)
    glm::vec4 NormalTint = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 HoverTint = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 PressedTint = {0.9f, 0.9f, 0.9f, 1.0f};
    glm::vec4 DisabledTint = {0.5f, 0.5f, 0.5f, 1.0f};

    // Optional feedback
    AssetReference HoverSound;  // e.g. type for audio asset
    AssetReference ClickSound;  // e.g. type for audio asset
};

// -----------------------------------------------------------------------------
// Slider Component - Draggable value selector
// Requires a sibling PanelComponent for the track visual
// -----------------------------------------------------------------------------
struct SliderComponent {
    // Value range and current value
    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    float Value = 0.5f;
    float Step = 0.0f;  // 0 = continuous, otherwise snaps to step increments
    
    // Orientation
    enum class Direction { Horizontal = 0, Vertical = 1 };
    Direction SliderDirection = Direction::Horizontal;
    
    // Handle visuals (the draggable part)
    glm::vec2 HandleSize = {20.0f, 20.0f};
    AssetReference HandleTexture;
    glm::vec4 HandleNormalTint = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 HandleHoverTint = {1.2f, 1.2f, 1.2f, 1.0f};
    glm::vec4 HandlePressedTint = {0.9f, 0.9f, 0.9f, 1.0f};
    glm::vec4 HandleDisabledTint = {0.5f, 0.5f, 0.5f, 1.0f};
    
    // Fill bar (optional, shows progress from min to current value)
    bool ShowFill = true;
    AssetReference FillTexture;
    glm::vec4 FillColor = {0.3f, 0.6f, 1.0f, 1.0f};
    
    // Interaction state
    bool Interactable = true;
    bool Hovered = false;
    bool Dragging = false;
    bool ValueChanged = false;  // True for one frame when value changes
    
    // Whole-number only mode
    bool WholeNumbers = false;
    
    // Visibility and opacity (for IUIOpacity support)
    float Opacity = 1.0f;
    bool Visible = true;
};

// -----------------------------------------------------------------------------
// Progress Bar Component - Visual fill indicator (health, loading, etc.)
// Requires a sibling PanelComponent for the background
// -----------------------------------------------------------------------------
struct ProgressBarComponent {
    // Current value (0 to 1 normalized, or use MinValue/MaxValue for custom range)
    float Value = 0.5f;
    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    
    // Fill direction
    enum class FillDirection { LeftToRight = 0, RightToLeft, BottomToTop, TopToBottom };
    FillDirection Direction = FillDirection::LeftToRight;
    
    // Fill visuals
    AssetReference FillTexture;
    glm::vec4 FillColor = {0.2f, 0.8f, 0.2f, 1.0f};
    
    // Optional gradient based on value (e.g., green to red for health)
    bool UseGradient = false;
    glm::vec4 GradientLowColor = {1.0f, 0.2f, 0.2f, 1.0f};   // Value near min
    glm::vec4 GradientHighColor = {0.2f, 1.0f, 0.2f, 1.0f};  // Value near max
    
    // Fill padding (inset from panel edges)
    glm::vec4 Padding = {2.0f, 2.0f, 2.0f, 2.0f};  // left, top, right, bottom
    // When true, use the sibling Panel's SliceBorder as padding (overrides manual Padding)
    bool UsePanelBorderAsPadding = false;
    
    // Animation
    bool Animate = false;
    float AnimationSpeed = 5.0f;  // Units per second for smooth transitions
    float _DisplayValue = 0.5f;   // Runtime: current displayed value (for animation)
    
    // Visibility and opacity (for IUIOpacity support)
    float Opacity = 1.0f;
    bool Visible = true;
};

// -----------------------------------------------------------------------------
// Toggle Component - On/off switch with checkbox-style visuals
// Requires a sibling PanelComponent for the background
// -----------------------------------------------------------------------------
struct ToggleComponent {
    // Current state
    bool IsOn = false;
    bool ValueChanged = false;  // True for one frame when toggled
    
    // Interaction
    bool Interactable = true;
    bool Hovered = false;
    bool Pressed = false;
    
    // Checkmark/toggle visuals
    AssetReference CheckmarkTexture;
    glm::vec4 CheckmarkTint = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec2 CheckmarkSize = {16.0f, 16.0f};
    glm::vec2 CheckmarkOffset = {0.0f, 0.0f};  // Offset from center
    
    // Background tints by state
    glm::vec4 OffTint = {0.3f, 0.3f, 0.3f, 1.0f};
    glm::vec4 OnTint = {0.3f, 0.6f, 1.0f, 1.0f};
    glm::vec4 HoverTint = {1.1f, 1.1f, 1.1f, 1.0f};  // Multiplied with On/Off tint
    glm::vec4 DisabledTint = {0.5f, 0.5f, 0.5f, 0.5f};
    
    // Toggle group (optional - for radio button behavior)
    // Entities in the same group with the same GroupID will be mutually exclusive
    int GroupID = 0;  // 0 = no group
    
    // Visibility and opacity (for IUIOpacity support)
    float Opacity = 1.0f;
    bool Visible = true;
};

// -----------------------------------------------------------------------------
// Scroll View Component - Scrollable container with optional scrollbars
// Requires a sibling PanelComponent for the viewport mask
// -----------------------------------------------------------------------------
struct ScrollViewComponent {
    // Content offset (runtime, updated by scrolling)
    glm::vec2 ContentOffset = {0.0f, 0.0f};
    
    // Content size (set by LayoutGroup or manually)
    glm::vec2 ContentSize = {200.0f, 400.0f};
    
    // Scroll settings
    bool HorizontalScroll = false;
    bool VerticalScroll = true;
    float ScrollSensitivity = 30.0f;  // Pixels per scroll tick
    
    // Scrollbar visuals
    bool ShowScrollbars = true;
    float ScrollbarWidth = 12.0f;
    AssetReference ScrollbarTrackTexture;
    AssetReference ScrollbarThumbTexture;
    glm::vec4 ScrollbarTrackColor = {0.2f, 0.2f, 0.2f, 0.5f};
    glm::vec4 ScrollbarThumbColor = {0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 ScrollbarThumbHoverColor = {0.7f, 0.7f, 0.7f, 1.0f};
    
    // Inertia/momentum scrolling
    bool UseInertia = true;
    float InertiaDeceleration = 500.0f;  // Pixels per second squared
    glm::vec2 _Velocity = {0.0f, 0.0f};  // Runtime velocity for inertia
    
    // Elastic bounds (bounce back when over-scrolling)
    bool Elastic = true;
    float ElasticAmount = 50.0f;  // Max overscroll pixels
    
    // Runtime state
    bool IsDragging = false;
    bool IsScrollbarDragging = false;
    glm::vec2 _DragStart = {0.0f, 0.0f};
    
    // Runtime: resolved scroll metrics in screen space.
    // When a sibling LayoutGroup exists, its calculated content bounds drive
    // these values and override manual ContentSize for scrolling purposes.
    glm::vec2 _ResolvedContentSizeScreen = {0.0f, 0.0f};
    glm::vec2 _ResolvedViewportSizeScreen = {0.0f, 0.0f};
    bool _HasHorizontalOverflow = false;
    bool _HasVerticalOverflow = false;
    bool _LayoutGroupDrivesContentSize = false;
    
    // Visibility and opacity (for IUIOpacity support)
    float Opacity = 1.0f;
    bool Visible = true;
};

// -----------------------------------------------------------------------------
// Layout Group Component - Automatic child positioning
// Arranges child panels horizontally or vertically
// -----------------------------------------------------------------------------
struct LayoutGroupComponent {
    // Layout direction
    enum class LayoutDirection { Horizontal = 0, Vertical = 1 };
    LayoutDirection Direction = LayoutDirection::Vertical;
    
    // Padding around the layout area
    glm::vec4 Padding = {10.0f, 10.0f, 10.0f, 10.0f};  // left, top, right, bottom
    
    // Spacing between child elements
    float Spacing = 5.0f;
    
    // Child alignment within the layout
    enum class Alignment { Start = 0, Center = 1, End = 2 };
    Alignment ChildAlignment = Alignment::Start;
    
    // Cross-axis alignment (perpendicular to layout direction)
    Alignment CrossAlignment = Alignment::Start;
    
    // Size control
    bool ControlChildWidth = false;   // Force all children to same width
    bool ControlChildHeight = false;  // Force all children to same height
    bool ChildForceExpandWidth = false;   // Expand children to fill available width
    bool ChildForceExpandHeight = false;  // Expand children to fill available height
    
    // Reverse layout order
    bool ReverseOrder = false;
    
    // Grid layout (when both are > 0, layout becomes a grid)
    int Columns = 0;  // 0 = not a grid, use Direction
    int Rows = 0;     // 0 = auto-calculate based on children count
    glm::vec2 CellSize = {100.0f, 100.0f};  // Size of each grid cell
    
    // Runtime: calculated content size (for ScrollView integration)
    glm::vec2 _CalculatedContentSize = {0.0f, 0.0f};
};

// -----------------------------------------------------------------------------
// Input Field Component - Text input with cursor and selection
// Requires a sibling PanelComponent for the background
// Requires a sibling TextRendererComponent for display
// -----------------------------------------------------------------------------
struct InputFieldComponent {
    // Content
    std::string Text;
    std::string PlaceholderText = "Enter text...";
    
    // Input settings
    int MaxLength = 0;  // 0 = unlimited
    bool Multiline = false;
    bool ReadOnly = false;
    
    // Character restriction
    enum class ContentType { Standard = 0, Integer, Decimal, Alphanumeric, Password };
    ContentType Type = ContentType::Standard;
    char PasswordChar = '*';
    
    // Visual settings
    glm::vec4 TextColor = {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 PlaceholderColor = {0.5f, 0.5f, 0.5f, 1.0f};
    glm::vec4 SelectionColor = {0.3f, 0.5f, 0.8f, 0.5f};
    glm::vec4 CursorColor = {1.0f, 1.0f, 1.0f, 1.0f};
    float CursorWidth = 2.0f;
    
    // Interaction state
    bool Interactable = true;
    bool IsFocused = false;
    bool TextChanged = false;  // True for one frame when text changes
    bool Submitted = false;    // True for one frame when Enter pressed
    
    // Runtime cursor state
    int _CursorPosition = 0;
    int _SelectionStart = 0;
    int _SelectionEnd = 0;
    float _CursorBlinkTimer = 0.0f;
    bool _CursorVisible = true;
    
    // Visibility and opacity (for IUIOpacity support)
    float Opacity = 1.0f;
    bool Visible = true;
};

// -----------------------------------------------------------------------------
// Dropdown Component - Selection popup with options list
// Requires a sibling PanelComponent for the closed-state display
// Requires a sibling TextRendererComponent for the selected item text
// -----------------------------------------------------------------------------
struct DropdownComponent {
    // Options
    std::vector<std::string> Options;
    int SelectedIndex = 0;
    bool ValueChanged = false;  // True for one frame when selection changes
    
    // Interaction
    bool Interactable = true;
    bool IsOpen = false;
    bool Hovered = false;
    int HoveredOptionIndex = -1;
    
    // Dropdown list visuals
    float OptionHeight = 30.0f;
    int MaxVisibleOptions = 5;  // Scroll if more options
    glm::vec4 OptionNormalColor = {0.2f, 0.2f, 0.2f, 1.0f};
    glm::vec4 OptionHoverColor = {0.3f, 0.5f, 0.8f, 1.0f};
    glm::vec4 OptionSelectedColor = {0.2f, 0.4f, 0.7f, 1.0f};
    
    // Arrow indicator
    bool ShowArrow = true;
    AssetReference ArrowTexture;
    glm::vec2 ArrowSize = {12.0f, 12.0f};
    glm::vec4 ArrowTint = {1.0f, 1.0f, 1.0f, 1.0f};
    
    // Caption (placeholder when nothing selected or for labeling)
    std::string Caption;
    
    // Runtime state
    float _ScrollOffset = 0.0f;  // For scrolling long option lists
    
    // Visibility and opacity (for IUIOpacity support)
    float Opacity = 1.0f;
    bool Visible = true;
};

// -----------------------------------------------------------------------------
// UI Rect Component - Enables parent-relative positioning for UI elements
// Add this to Panel, Text, or other UI elements to anchor them relative to
// their parent UI element instead of the screen.
// -----------------------------------------------------------------------------
struct UIRectComponent {
    // If true, this element's position is relative to its parent UI element
    // (parent must have Panel or UIRect). If false, uses screen-space anchoring.
    bool AnchorToParent = false;
    
    // Horizontal alignment within parent rect (0=left, 0.5=center, 1=right)
    float HorizontalAnchor = 0.5f;
    // Vertical alignment within parent rect (0=top, 0.5=center, 1=bottom)
    float VerticalAnchor = 0.5f;
    
    // Pivot point for this element (where the anchor point connects)
    // (0,0)=top-left, (0.5,0.5)=center, (1,1)=bottom-right
    glm::vec2 Pivot = {0.5f, 0.5f};
    
    // Offset from the anchored position (in pixels)
    glm::vec2 Offset = {0.0f, 0.0f};
    
    // Size in pixels (used for elements without explicit Size, like Text)
    // When both are 0, size is computed from content (e.g., text bounds)
    glm::vec2 Size = {0.0f, 0.0f};
    
    // Runtime: computed screen-space rect (filled by UI layout system)
    // Format: (x, y, width, height) where (x,y) is top-left
    glm::vec4 _ComputedRect = {0.0f, 0.0f, 0.0f, 0.0f};
    
    // Runtime: whether the computed rect is valid/dirty
    bool _RectDirty = true;
};

// -----------------------------------------------------------------------------
// Fit To Content Component - Makes a Panel auto-size to fit its children
// Attach to an entity with a PanelComponent to automatically adjust the
// panel's Size based on the bounding box of its children.
// -----------------------------------------------------------------------------
struct FitToContentComponent {
    // Enable/disable auto-sizing (useful for temporarily disabling)
    bool Enabled = true;
    
    // Which axes to auto-fit
    bool FitWidth = true;
    bool FitHeight = true;
    
    // Padding around children content (left, top, right, bottom)
    glm::vec4 Padding = {10.0f, 10.0f, 10.0f, 10.0f};
    
    // Minimum size constraints (0 = no minimum)
    glm::vec2 MinSize = {0.0f, 0.0f};
    
    // Maximum size constraints (0 = no maximum)
    glm::vec2 MaxSize = {0.0f, 0.0f};
    
    // If true, include only direct children. If false, include all descendants.
    bool DirectChildrenOnly = true;
    
    // Runtime: cached children bounds (for dirty checking)
    glm::vec4 _CachedChildrenBounds = {0.0f, 0.0f, 0.0f, 0.0f};
    bool _BoundsDirty = true;
};

// -----------------------------------------------------------------------------
// UI Scene Capture Component - Renders a scene view into a UI panel
// Attach to an entity with a PanelComponent to display a render texture.
// -----------------------------------------------------------------------------
struct UISceneCaptureComponent {
    bool Enabled = true;
    bool AutoFrame = true;           // Center/zoom to target AABB
    bool IncludeChildren = true;     // Include child meshes in bounds
    float BoundsPadding = 1.15f;     // Extra padding around bounds when framing
    float FieldOfView = 60.0f;       // Perspective FOV for capture camera
    float NearClip = 0.1f;
    float FarClip = 500.0f;

    // Camera orientation and focus offset
    glm::vec3 ViewDirection = {0.0f, 0.0f, 1.0f}; // Direction from target to camera
    glm::vec3 UpDirection = {0.0f, 1.0f, 0.0f};
    glm::vec3 FocusOffset = {0.0f, 0.0f, 0.0f};
    // When true, ViewDirection/UpDirection are treated as local directions
    // on the target entity and rotated with its world transform.
    bool LockViewToTarget = false;

    // Target entity reference
    int TargetEntity = -1;
    uint64_t TargetGuidHigh = 0;
    uint64_t TargetGuidLow = 0;

    // Output render target size (0 uses panel size)
    int RenderWidth = 0;
    int RenderHeight = 0;
    uint32_t ClearColor = 0x00000000; // RGBA
    bool ShowGrid = false;

    // Runtime
    uint16_t _ViewIdBase = 0;
};
