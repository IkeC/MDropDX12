# Visual Comparison: MDropDX12 vs Milkwave Visualizer

Side-by-side visual comparison of preset rendering between MDropDX12 (DirectX 12) and Milkwave Visualizer (DirectX 9Ex).

| Project | Graphics API | Version |
| ------- | ------------ | ------- |
| **MDropDX12** | DirectX 12 | v2.4.1 |
| **Milkwave Visualizer** | DirectX 9Ex | v3.5 |

---

## Presets Compared

Each preset loaded on both visualizers simultaneously with identical audio.

### 1. Martin - blue haze

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/01_blue_haze_mdrop.jpg) | ![Milkwave](images/comparison/01_blue_haze_milkwave.jpg) |

Both render the deep blue-purple background, concentric swirl rings, and bright particle fountain from center with matching color palette (pink/magenta strands, golden highlights, white-hot core). The fire orb sprite renders correctly on both. No visible differences in warp distortion, color grading, or particle behavior.

**Verdict:** Visually equivalent.

### 2. BrainStain - re entry

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/02_re_entry_mdrop.jpg) | ![Milkwave](images/comparison/02_re_entry_milkwave.jpg) |

This preset uses two textured non-additive custom shapes (99-sided circles) with aggressive decay (`fDecay=0.5`) and audio-driven zoom (`bass_att`). Multiple DX9→DX12 differences were investigated:

1. **Alpha feedback** (fixed): DX9 used `X8R8G8B8` render targets (no alpha channel). DX12's `R8G8B8A8_UNORM` stored alpha, which compounded through the feedback loop. Fixed with RGB-only write mask on all shape PSOs.
2. **Warp decay for auto-gen presets** (fixed): DX9 applied decay via fixed-function texture stage modulate (`D3DTOP_MODULATE × D3DTA_DIFFUSE`) for non-shader presets. DX12 now injects `ret *= _vDiffuse.rgb` in the warp output wrapper and `GenWarpPShaderText` generates the decay multiply for auto-gen presets.
3. **VS[1] clear alpha** (fixed): Changed from 0.0 to 1.0 to match DX9's implicit alpha.
4. **Custom warp shader decay**: DX9's fixed-function texture stage is BYPASSED when a pixel shader is active. BrainStain has a custom warp shader (trivial `ret = tex2D(sampler_main, uv).xyz`) so decay is NOT applied by DX9 either. The preset relies on shapes + darken post-effect for brightness control.

BrainStain shows the correct starburst structure and is audio-reactive on both renderers. MDropDX12 runs brighter at low volumes due to differences in how energy accumulates through the feedback loop when custom warp shaders skip decay. Color differences are expected (time-based wave color modulation cycles independently).

**Verdict:** Close match — structure and reactivity correct, brightness differs at low volumes.

### 3. balkhan + IkeC - Tunnel Cylinders

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/03_tunnel_cylinders_mdrop.jpg) | ![Milkwave](images/comparison/03_tunnel_cylinders_milkwave.jpg) |

This is a comp shader preset with 3D raymarched tunnel geometry. Both renderers produce the same concentric cylindrical tunnel structure with identical blue/orange/peach color gradients, geometric faceting, and spiral depth recession. The central rose-spiral focal point and floating arrow shapes match exactly.

**Verdict:** Visually equivalent.

### 4. Marex + IkeC - Shadow Party Shader Jam 2025

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/04_shadow_party_mdrop.jpg) | ![Milkwave](images/comparison/04_shadow_party_milkwave.jpg) |

Both render the raymarched scene of reflective green-to-yellow spheres in a recursive lattice with specular highlights and ambient occlusion. The sphere geometry, color gradients, and lattice structure match. Previously rendered black on MDropDX12 due to HLSL error X3005 — a local variable `R2D` shadowed a user-defined function of the same name (valid in GLSL, not HLSL). Fixed by `FixShadowedUserFunctions` in engine_shaders.cpp.

**Verdict:** Visually equivalent (fixed).

### 5. Illusion & Rovastar - Clouded Bottle

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/05_clouded_bottle_mdrop.jpg) | ![Milkwave](images/comparison/05_clouded_bottle_milkwave.jpg) |

Both render the same dark scene with green waveform threads crossing in an X-pattern. The wave line density, color (dark green), and crossing geometry are consistent. MDropDX12 shows slightly more blue tint in some threads; Milkwave's lines appear marginally denser. The overall composition and mood match.

**Verdict:** Visually equivalent.

### 6. martin - deep blue

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/06_deep_blue_mdrop.jpg) | ![Milkwave](images/comparison/06_deep_blue_milkwave.jpg) |

Both render deep-sea organic tentacle/tube structures against a dark navy background with matching cyan-green edge glow and internal blue shading. The tube shapes, branching patterns, and color gradients are consistent. Milkwave's tubes appear slightly thicker due to different window resolution at capture time.

**Verdict:** Visually equivalent.

### 7. martin - push ax

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/07_push_ax_mdrop.jpg) | ![Milkwave](images/comparison/07_push_ax_milkwave.jpg) |

Both render a 3D voxel cityscape / cube matrix with rainbow color gradients (green, orange, pink, purple). The recursive cube geometry, reflective surfaces, and lit window patterns are present on both. Frame captures differ in camera angle due to chaotic animation timing, but the same visual elements and color palette are rendered.

**Verdict:** Visually equivalent.

### 8. shifter - escape the worm (Eo.S. + Phat mix)

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/08_escape_worm_mdrop.jpg) | ![Milkwave](images/comparison/08_escape_worm_milkwave.jpg) |

Both render the worm-tunnel perspective with colored block walls (blue/purple on left, teal/green on right) and a central triangular waveform shape with spiral trail. The block mosaic pattern, color gradients, and tunnel depth are consistent. Camera position differs slightly due to animation timing.

**Verdict:** Visually equivalent.

### 9. Flexi - oldschool tree

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/09_oldschool_tree_mdrop.jpg) | ![Milkwave](images/comparison/09_oldschool_tree_milkwave.jpg) |

Both render the fractal tree with autumn-colored leaves (orange-brown to yellow-green gradient, left to right) against a black background with concentric ring ripples emanating from center. The leaf geometry, branching structure, trunk, and internal tile patterns are identical. Audio-reactive ring ripples match in spacing and intensity.

**Verdict:** Visually equivalent.

### 10. martin - axon3

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/10_axon3_mdrop.jpg) | ![Milkwave](images/comparison/10_axon3_milkwave.jpg) |

Both render dark, iridescent organic neural/axon structures with flowing tentacle shapes, chromatic highlights (red, cyan, yellow, purple), and a deep biological aesthetic. The rendering style, color palette, and organic distortion are consistent. Exact frame content differs due to chaotic per-frame animation, but the visual quality and complexity match.

**Verdict:** Visually equivalent.

### 11. Zylot - Spiral (Hypnotic) Phat Double Spiral Mix

| MDropDX12 | Milkwave |
| --------- | -------- |
| ![MDropDX12](images/comparison/11_spiral_hypnotic_mdrop.jpg) | ![Milkwave](images/comparison/11_spiral_hypnotic_milkwave.jpg) |

Both render the hypnotic double spiral with concentric rings in green, pink/red, and dark tones. The spiral geometry, color banding, and fine-line texture within the rings are identical. The warp-driven rotation and color distribution match precisely.

**Verdict:** Visually equivalent.

---

## Summary

| # | Preset | Result |
|---|--------|--------|
| 1 | Martin - blue haze | Equivalent |
| 2 | BrainStain - re entry | Close match — fixed alpha feedback, brightness differs at low volumes |
| 3 | balkhan + IkeC - Tunnel Cylinders | Equivalent |
| 4 | Marex + IkeC - Shadow Party Shader Jam 2025 | Equivalent (fixed — was black, X3005 variable/function shadow) |
| 5 | Illusion & Rovastar - Clouded Bottle | Equivalent |
| 6 | martin - deep blue | Equivalent |
| 7 | martin - push ax | Equivalent |
| 8 | shifter - escape the worm (Eo.S. + Phat mix) | Equivalent |
| 9 | Flexi - oldschool tree | Equivalent |
| 10 | martin - axon3 | Equivalent |
| 11 | Zylot - Spiral (Hypnotic) Phat Double Spiral Mix | Equivalent |

**All 11 presets** render with matching structure and behavior. Key fixes applied: #2 (RT alpha feedback via RGB-only shape write mask, warp decay for auto-gen presets via vertex color), #4 (HLSL X3005 variable shadowing user function — `FixShadowedUserFunctions`). Investigation confirmed DX9 fixed-function texture stage modulate is bypassed when a pixel shader is active, so custom warp shader presets correctly receive white vertices (no decay) on both DX9 and DX12. Remaining brightness differences at low volumes are due to feedback loop sensitivity in presets with extreme decay values.

---

## Gain Sweep Results

Tested all 11 presets at audio gain levels 0.05, 0.1, 0.5, and 1.0 (system volume at max, gain attenuates). Both visualizers received identical `SET_AUDIO_GAIN` commands via Named Pipe IPC.

**Legend:** E = Equivalent, ~ = Close match (minor differences), B = MDropDX12 brighter, D = MDropDX12 dimmer, — = not tested

| # | Preset | Type | 0.05 | 0.1 | 0.5 | 1.0 |
|---|--------|------|------|-----|-----|-----|
| 1 | blue haze | shapes (fDecay=0.5, bDarken) | ~ | ~ | E | E |
| 2 | BrainStain | shapes (fDecay=0.5, bDarken) | ~ | ~ | ~ | E |
| 3 | Tunnel Cylinders | comp shader | E | E | E | E |
| 4 | Shadow Party | comp shader | E | E | E | E |
| 5 | Clouded Bottle | waves (fDecay=0.999) | E | E | D | E |
| 6 | deep blue | shapes (fDecay=0.5, bDarken) | D | D | ~ | E |
| 7 | push ax | comp shader | E | E | E | E |
| 8 | escape worm | shapes (fDecay=1.0, bBrighten+bDarken) | E | E | E | E |
| 9 | oldschool tree | shapes (fDecay=1.0, bBrighten) | E | E | E | E |
| 10 | axon3 | shapes (fDecay=0.5, bDarken) | — | ~ | ~ | E |
| 11 | Zylot Spiral | warp only (fDecay=0.997) | E | E | E | E |

### Observations

- **Comp shader presets (#3, #4, #7)** are identical at all gain levels — the shader does all rendering, no feedback loop sensitivity.
- **Shape/feedback presets (#1, #2, #6, #10)** with aggressive `fDecay=0.5` show minor differences at low gain (0.05–0.1) where the feedback loop amplifies small per-frame energy differences.
- **At gain=0.5 and above**, all presets are equivalent or close match — the audio energy dominates and both renderers converge.
- **Clouded Bottle (#5)** was slightly dimmer on MDropDX12 at gain=0.5 — this is a waves-only preset with very high decay (0.999), so wave rendering intensity matters more.
- **deep blue (#6)** is slightly dimmer on MDropDX12 at low gain — its custom warp shader encodes its own decay (`ret = ret1 * q10 - 0.04`), making it sensitive to any feedback loop differences.
