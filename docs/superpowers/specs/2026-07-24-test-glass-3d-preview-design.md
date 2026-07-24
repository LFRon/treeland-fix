# test_glass 3D Preview Design

## Goal

Add an independent 3D preview mode to `examples/test_glass` so developers can inspect the Liquid Glass geometry and material parameters in a rotatable view while keeping the existing 2D `GlassEffect` demo unchanged.

## Scope

Included:
- Add a `3D Preview` mode beside the existing 2D Liquid Glass mode.
- Generate a rounded-rectangle glass mesh from the existing demo parameters.
- Reuse the current parameter panel for both 2D and 3D modes.
- Support mouse drag rotation, double-click front reset, and yaw/pitch display.
- Keep `refractionMaxTan` UI maximum at 20.

Excluded:
- Replacing Treeland's runtime `GlassEffect` implementation.
- Porting `liquidglass.frag` verbatim into Qt Quick 3D materials.
- Matching HTML texture refraction pixel-for-pixel in the first Qt demo version.

## Architecture

`Main.qml` keeps the current `RenderBufferBlitter + GlassEffect` path as the canonical 2D runtime preview. A new preview mode switches `effectPanel` to a `View3D` scene. The 3D scene uses a C++ geometry type, `GlassGeometry`, exposed through the existing `GlassExample` QML module.

`GlassGeometry` inherits `QQuick3DGeometry`. It rebuilds vertex/index buffers when any shape parameter changes. The generated mesh contains:
- top bevel surface,
- rounded-rectangle outline,
- side wall,
- bottom rim.

CMake adds `Qt6::Quick3D` to `examples/test_glass` and compiles the new geometry source only for the demo target.

## Parameters

The 3D mesh consumes:
- `effectWidth`
- `effectHeight`
- `effectRadius`
- `glassBezelWidth`
- `glassThickness`
- `glassProfilePower`

The material consumes:
- `glassSpecular`
- `glassTint`
- `glassInnerShadow`

The 2D shader-only parameters remain visible because the panel is shared:
- `glassIor`
- `glassRefractionMaxTan`
- `glassContentEdgePull`
- `glassContentRampEnd`
- blur/color controls

Those values still affect the 2D preview. If a parameter has no direct 3D equivalent in the first implementation, the UI remains shared and the 3D preview ignores it explicitly rather than faking behavior.

## Interaction

`3D Preview` mode provides local interaction inside the preview area:
- pointer drag changes yaw and pitch,
- pitch clamps to avoid flipping through the back side,
- double-click resets yaw and pitch to zero,
- a small HUD shows the current yaw/pitch angles.

The existing 2D panel drag behavior remains active only in 2D mode. In 3D mode, dragging rotates the model instead of moving the overlay.

## Rendering Approach

Use Qt Quick 3D built-ins:
- `View3D` for the scene,
- `PerspectiveCamera` aimed at the model center,
- one or two lights for highlights,
- `Model` using `GlassGeometry`,
- `PrincipledMaterial` for translucent tinted glass.

The first implementation approximates glass material visually. It does not claim to be the same optical path as `liquidglass.frag`; the existing 2D mode remains the source-of-truth for shader behavior.

## Error Handling

Build-time failure is preferred over silent fallback if `Qt6::Quick3D` is unavailable. The demo target should fail clearly at CMake configure time. Runtime QML should avoid undefined optional types or dynamic imports.

## Verification

Required checks:
- `cmake --build --preset default --target test_glass` succeeds.
- A static QML/source check confirms `refractionMaxTan` max is 20 in both HTML and `test_glass`.
- The demo starts without QML import/type errors.
- Switching to 3D mode shows a model.
- Dragging changes yaw/pitch HUD values.
- Double-click resets yaw/pitch to zero.

## Risks

Qt Quick 3D availability is the main dependency risk. The repository's nix inputs include `qtquick3d`, but non-nix developer machines may need the Qt Quick 3D development package installed.

Pixel-level refraction parity with HTML is intentionally out of scope for this step. Trying to force shader parity through Qt Quick 3D custom materials would mix two rendering APIs and make the demo harder to maintain.
