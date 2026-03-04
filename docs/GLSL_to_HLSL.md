# GLSL to HLSL Conversion — Challenges and Solutions

This document covers the challenges of converting Shadertoy GLSL shaders to MDropDX12's HLSL/DX12 environment. The converter lives in `engine_shader_import_ui.cpp`, function `ConvertGLSLtoHLSL()`.

## Conversion Pipeline Overview

The converter runs in four phases:

1. **Phase 1** — Full-text global replacements (types, functions, uniforms, matrices)
2. **Phase 1b** — Matrix variable detection and `mul()` insertion
3. **Phase 2** — Extract `mainImage()`, build `shader_body` wrapper
4. **Phase 3** — Per-line processing (matrix ops, float expansion, atan fix, break replacement)

Post-processing: backslash continuation joining, return value insertion, code formatting.

---

## 1. Matrix Storage: Column-Major vs Row-Major

**The single hardest problem in the entire converter.**

GLSL and HLSL store matrices differently:

| | GLSL | HLSL |
|---|---|---|
| Storage | Column-major | Row-major |
| `mat3(a,b,c)` | a,b,c are **columns** | — |
| `float3x3(a,b,c)` | — | a,b,c are **rows** |
| `M[i]` | Returns column i | Returns row i |
| `M * v` | Column-major multiply | — |
| `mul(M, v)` | — | Row-major multiply |

A naive `mat3(` → `float3x3(` replacement produces a **transposed** matrix. Every multiplication gives wrong results and every `M[i]` access returns the wrong vector.

### Solution: Dual Strategy

The converter uses **different strategies for square vs non-square matrices** because they have different tradeoffs:

#### Square matrices (mat2, mat3, mat4): Mul-Swap

No `transpose()` on constructors. Instead, swap `mul()` argument order.

```
GLSL: mat3 ca = mat3(cu, cv, cw);     → HLSL: float3x3 ca = float3x3(cu, cv, cw);
GLSL: vec3 rd = ca * normalize(p);    → HLSL: float3 rd = mul(normalize(p), ca);  // SWAPPED
GLSL: vec3 col = v * ca;              → HLSL: float3 col = mul(ca, v);            // SWAPPED
```

**Why this works**: `float3x3(a,b,c)` stores a,b,c as rows. GLSL's `mat3(a,b,c) * v` computes `result[i] = a[i]*v[0] + b[i]*v[1] + c[i]*v[2]`. HLSL's `mul(v, float3x3(a,b,c))` computes the same thing — each result component is the dot product of `v` with a column of M, which equals `(a[i], b[i], c[i])`.

**Why not transpose**: `M[i]` indexing is common for square matrices (e.g., extracting camera basis vectors). With mul-swap, `ca[0]` = row 0 = `cu`, which matches GLSL's `ca[0]` = column 0 = `cu`. Wrapping with `transpose()` would break this — `ca[0]` would return `(cu[0], cv[0], cw[0])` instead.

#### Non-square matrices (mat3x4, mat4x3, etc.): Transpose

Wrap constructors with `transpose()`, use standard `mul()` order.

```
GLSL: mat3x4 M = mat3x4(a, b, c);    → HLSL: float4x3 M = transpose(float3x4(a, b, c));
GLSL: vec4 r = M * v;                 → HLSL: float4 r = mul(M, v);              // STANDARD
GLSL: vec3 r = v * M;                 → HLSL: float3 r = mul(v, M);              // STANDARD
```

**Why transpose is required**: A simple dimension-swap replacement (`mat3x4` → `float4x3`) produces a valid type but the **constructor fills elements completely wrong**. `float4x3` expects 4 rows of 3 elements; passing 3 `float4` vectors causes element interleaving:

```
GLSL mat3x4(a, b, c):
  col0 = a, col1 = b, col2 = c       ← clean column fill

HLSL float4x3(a, b, c) with 3 float4 args (12 scalars → 4×3):
  row0 = (a[0], a[1], a[2])          ← WRONG: elements from 'a' only
  row1 = (a[3], b[0], b[1])          ← WRONG: mixed elements
  row2 = (b[2], b[3], c[0])          ← WRONG: mixed elements
  row3 = (c[1], c[2], c[3])          ← WRONG: elements from 'c' only
```

With `transpose(float3x4(a, b, c))`: `float3x4` is 3 rows × 4 cols, so rows = a, b, c (clean). Transpose flips to 4 rows × 3 cols with columns = a, b, c — matching GLSL.

**Why standard mul order**: The matrix value is now correct (transpose fixed the layout), so standard multiplication order works directly.

#### Why the two strategies are incompatible

Square mul-swap: `matVar * expr` → `mul(expr, matVar)` (swapped)
Non-square standard: `matVar * expr` → `mul(matVar, expr)` (standard)

Phase 1b tracks which variables are square vs non-square and applies the correct `mul()` order for each.

### Multi-Line Constructors

Constant matrices often span multiple lines:

```glsl
const mat3 m3 = mat3(0.00, 0.80, 0.60,
                     -0.80, 0.36, -0.48,
                     -0.60, -0.48, 0.64);
```

Phase 1 runs on the full text with `FindClosingBracket()` for bracket matching, so multi-line constructors are handled correctly. Phase 3's per-line `FixMatrixMultiplication` handles `*=mat2(...)` patterns which are typically single-line (inline rotation matrices).

### Dimension Naming Convention

GLSL and HLSL use opposite conventions for `matAxB` / `floatAxB`:

| GLSL | Meaning | HLSL | Meaning |
|------|---------|------|---------|
| `mat3x4` | 3 cols × 4 rows | `float3x4` | 3 rows × 4 cols |

So `mat3x4` (a 4×3 math matrix) corresponds to `float4x3` in HLSL. **Dimensions are swapped** for type declarations but **kept as GLSL** inside `transpose()` constructors:

```
Type declaration:  mat3x4 → float4x3   (swapped)
Constructor:       mat3x4(args) → transpose(float3x4(args))  (GLSL dims, transpose produces float4x3)
```

---

## 2. Matrix Multiplication Operator

HLSL does not support `*` for matrix-vector multiplication. The compiler emits error X3020 (type mismatch). All matrix multiplications must use `mul()`.

### Named variables (Phase 1b)

The converter scans for matrix type declarations, collects variable names, then replaces `matVar * expr` and `expr * matVar` with appropriate `mul()` calls.

### Inline constructors (Phase 3)

Patterns like `uv *= mat2(cos(a), -sin(a), sin(a), cos(a))` are handled per-line by `FixMatrixMultiplication()`:

```
v *= matN(args)  →  v = mul(floatNxN(args), v)      // swap: constructor on left
x * matN(args)   →  mul(floatNxN(args), x)           // swap: constructor on left
```

---

## 3. Type and Function Mappings

### Types

| GLSL | HLSL |
|------|------|
| `vec2/3/4` | `float2/3/4` |
| `ivec2/3/4` | `int2/3/4` |
| `bvec2/3/4` | `bool2/3/4` |
| `mat2/3/4` | `float2x2/3x3/4x4` |

Integer/boolean types must be replaced **before** `vec` → `float` to avoid `ivec2` → `ifloat2`.

### Functions

| GLSL | HLSL | Notes |
|------|------|-------|
| `fract()` | `frac()` | |
| `mix()` | `lerp()` | |
| `mod()` | `mod_conv()` | Custom helper; HLSL `fmod` has different sign behavior |
| `atan(y,x)` | `atan2(y,x)` | Only 2-arg form; 1-arg `atan()` unchanged |
| `texture()` | `tex2D()` | |
| `textureLod()` | `tex2Dlod_conv()` | Custom wrapper adapting arg format |
| `texelFetch()` | `texelFetch_conv()` | Integer coords → UV conversion with `texsize.zw` |

### Precision qualifiers

`highp`, `mediump`, `lowp` are stripped (HLSL has no equivalents; all computation is full precision).

---

## 4. Shadertoy Uniform Mapping

| Shadertoy | MDropDX12 | Constant Buffer |
|-----------|-----------|-----------------|
| `iResolution` | `float3(texsize.x, texsize.y, 1.0)` | `_c7` |
| `iTime` | `time` | `_c2.x` |
| `iFrame` | `frame` | `_c2.z` |
| `iMouse` | `mouse` | (zeroed) |
| `iChannel0` | `sampler_feedback` | `register(s14)` |
| `iChannel1` | `sampler_noise_lq` | `register(s8)` |

`iResolution` is inline-expanded (not `#define`d) because `texsize` is a `float4` and `iResolution.z` (pixel aspect ratio, always 1.0) would conflict with `texsize.z` (1/width).

---

## 5. Variable Name Collisions

MDropDX12 presets use `#define` macros for audio data:

```hlsl
#define bass _c3.x
#define mid  _c3.y
#define treb _c3.z
```

Common GLSL variable names like `mid` (midpoint), `bass`, `treb`, `vol` get macro-expanded into constant buffer swizzles, breaking declarations like `float mid = 0.5;` → `float _c3.y = 0.5;`.

**Fix**: Whole-word rename before macro expansion: `mid` → `_st_mid`, `bass` → `_st_bass`, etc.

Similarly, `time` (a common GLSL variable) collides with the MDropDX12 `time` uniform, so it's renamed to `time_conv`.

---

## 6. Float Constructor Expansion

GLSL allows single-argument vector constructors: `float3(1.0)` expands to `float3(1.0, 1.0, 1.0)`. HLSL requires all arguments explicitly.

The converter detects single-argument constructors (no commas at top level) and expands them when the argument is:
- A numeric literal
- A function call
- A known `float` variable

This runs per-line in Phase 3 via `FixFloatNumberOfArguments()`.

---

## 7. Break Statement Replacement

HLSL Shader Model 3.0 (used by MDropDX12 for compatibility) does not support `break` in loops. The converter replaces `break` with an assignment that makes the loop condition false:

```glsl
for (int i = 0; i < N; i++) {
    if (hit) break;
}
```

becomes:

```hlsl
for (int i = 0; i < N; i++) {
    if (hit) i = N;
}
```

The condition is extracted from the most recent `for` loop header and the comparison operator is replaced with `=`.

---

## 8. Shadertoy Render Pipeline

Shadertoy shaders run in a completely separate render path from MilkDrop presets — no warp, blur, shapes, or waves.

### Pipeline

```
Frame N:
  Buffer A: reads feedback[read] → writes feedback[write]    (FLOAT32)
  Image:    reads feedback[write] → writes backbuffer         (UNORM)
  Swap feedback indices
```

### fragCoord Convention

The converter uses DX convention (y=0 at top) for `fragCoord`:

```hlsl
float2 fragCoord = uv * texsize.xy;  // uv.y=0 at top
```

This differs from Shadertoy's OpenGL convention (y=0 at bottom), but the feedback loop is **self-consistent**: what Buffer A writes at `uv.y = F/H` is read back at the same `uv.y = F/H`. The Image pass flips quad UVs to display right-side-up.

### Shader Output

Both Buffer A and Image passes output `ret` unchanged — no gamma correction, no `shiftHSV()`. FLOAT32 feedback buffers preserve full precision for temporal accumulation.

---

## 9. Known Limitations

| Feature | Status |
|---------|--------|
| `iDate` | Not supported (commented out with warning) |
| `iTimeDelta` | Not supported (commented out with warning) |
| `iChannelResolution` | Not mapped |
| `iChannel1-3` | Mapped to noise textures, not configurable per-shader |
| Buffer B/C/D | Not implemented (only Buffer A + Image) |
| Cubemap inputs | Not supported |
| `inversesqrt()` | Not converted (compiles if unused; `rsqrt()` is HLSL equivalent) |
| Non-square matrix `M[i]` indexing | Returns wrong vector (transpose breaks indexing) |
| `dFdx`/`dFdy` | Require `ddx`/`ddy` in HLSL (not currently converted) |

---

## 10. Debugging Converted Shaders

The engine dumps compiled shader text to diagnostic files in the working directory:

- `diag_comp_shader.txt` — Buffer A shader (compiled as comp type)
- `diag_comp_shader.txt` — Image/comp shader (overwrites Buffer A dump)

These show the final HLSL after all conversion phases and the engine's wrapper code (`shader_body`, return value injection, constant buffer declarations).

To inspect conversion results without running the visualizer, paste GLSL into the import dialog and click Convert — the HLSL output appears in the right panel.
