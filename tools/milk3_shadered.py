#!/usr/bin/env python3
"""
milk3_shadered.py — Convert MDropDX12 .milk3 <-> SHADERed project (folder / .zip / .sprj)

Python only (no PowerShell required).

Commands:
  to-shadered  PRESET.milk3 [-o out_dir|out.zip] [--zip] [--glsl|--hlsl]
  to-milk3     project.zip|.sprj|folder [-o out.milk3]
  info         path

Examples:
  python tools/milk3_shadered.py to-shadered resources/presets/BlueRedShiney.milk3 --zip
  python tools/milk3_shadered.py to-milk3 src/mDropDX12/Release_x64/resources/presets/LOzB7CqQGt.zip
  python tools/milk3_shadered.py info LOzB7CqQGt.zip

Notes:
  - Real SHADERed zips often use GLSL (#version 330) with .vk/.glsl shader files
    and ScreenQuadNDC (see LOzB7CqQGt.zip). That path is first-class here.
  - milk3 stores MDrop HLSL (shader_body). Export can emit GLSL (default) or HLSL.
  - Round-trip of milk3->shadered->milk3 keeps sources in milk3_raw/ when present.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import zipfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from xml.etree import ElementTree as ET

PASS_KEYS = ("common", "bufferA", "bufferB", "bufferC", "bufferD", "image")
PASS_ORDER = ("bufferA", "bufferB", "bufferC", "bufferD", "image")

MARKER_PREAMBLE_BEGIN = "// === MILK3_PREAMBLE_BEGIN ==="
MARKER_PREAMBLE_END = "// === MILK3_PREAMBLE_END ==="
MARKER_BODY_BEGIN = "// === MILK3_BODY_BEGIN ==="
MARKER_BODY_END = "// === MILK3_BODY_END ==="


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def die(msg: str, code: int = 1) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(code)


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def load_json(path: Path) -> Dict[str, Any]:
    return json.loads(read_text(path))


def safe_name(name: str) -> str:
    s = re.sub(r"[^\w\-]+", "_", name).strip("_")
    return s or "preset"


def xml_esc(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


# ---------------------------------------------------------------------------
# milk3 parse / split
# ---------------------------------------------------------------------------

def load_milk3(path: Path) -> Dict[str, Any]:
    data = load_json(path)
    if not isinstance(data, dict):
        die(f"invalid milk3: {path}")
    return data


def pass_source(milk: Dict[str, Any], key: str) -> str:
    v = milk.get(key)
    if not v:
        return ""
    if isinstance(v, dict):
        return str(v.get("code") or v.get("hlsl") or v.get("glsl") or "")
    return str(v)


def split_shader_body(src: str) -> Tuple[str, str]:
    if not src:
        return "", ""
    m = re.search(r"shader_body\s*\{", src)
    if not m:
        return "", src.strip()
    start = m.end()
    depth, i = 1, start
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    if depth:
        die("unbalanced braces in shader_body")
    return src[: m.start()].rstrip(), src[start : i - 1].strip("\n")


def join_shader_body(preamble: str, body: str) -> str:
    parts = []
    if preamble.strip():
        parts.append(preamble.rstrip())
        parts.append("")
    parts.append("shader_body {")
    parts.append(body.rstrip() if body.strip() else "  ret = float4(0,0,0,1);")
    parts.append("}")
    parts.append("")
    return "\n".join(parts)


def extract_markers(src: str) -> Optional[str]:
    if MARKER_PREAMBLE_BEGIN not in src or MARKER_BODY_BEGIN not in src:
        return None

    def bet(a: str, b: str) -> str:
        i, j = src.find(a), src.find(b)
        if i < 0 or j < 0 or j <= i:
            return ""
        return src[i + len(a) : j].strip("\n")

    pre = bet(MARKER_PREAMBLE_BEGIN, MARKER_PREAMBLE_END)
    body = bet(MARKER_BODY_BEGIN, MARKER_BODY_END)
    if pre.strip() == "// (empty preamble)":
        pre = ""
    return join_shader_body(pre, body)


# ---------------------------------------------------------------------------
# GLSL <-> milk3 HLSL (mechanical, good enough for simple fullscreen shaders)
# ---------------------------------------------------------------------------


def _extract_braced_body(s: str, open_brace_index: int):
    """Given index of '{', return (body_inside, index_after_closing_brace)."""
    depth, i = 1, open_brace_index + 1
    while i < len(s) and depth:
        if s[i] == "{":
            depth += 1
        elif s[i] == "}":
            depth -= 1
        i += 1
    if depth:
        die("unbalanced braces while parsing GLSL")
    return s[open_brace_index + 1 : i - 1], i


def glsl_to_milk3_hlsl(glsl: str, time_uniform: str = "u_time", res_uniform: str = "u_resolution") -> str:
    """Best-effort GLSL fragment -> MDrop milk3 HLSL (shader_body)."""
    s = glsl
    s = re.sub(r"^\s*#version[^\n]*\n", "", s, flags=re.M)
    s = re.sub(r"^\s*precision\s+\w+\s+float\s*;\s*\n", "", s, flags=re.M)
    s = re.sub(r"^\s*out\s+vec4\s+\w+\s*;\s*\n", "", s, flags=re.M)
    s = re.sub(r"^\s*layout\s*\([^)]*\)\s*", "", s, flags=re.M)
    s = re.sub(r"^\s*uniform\s+[^;]+;\s*(?://[^\n]*)?\n", "", s, flags=re.M)

    # Prefer mainImage(out vec4, in vec2) over void main() forwarder
    mi = re.search(
        r"void\s+mainImage\s*\(\s*out\s+vec4\s+(\w+)\s*,\s*in\s+vec2\s+(\w+)\s*\)\s*\{",
        s,
    )
    if mi:
        color_name, coord_name = mi.group(1), mi.group(2)
        body_raw, _end_i = _extract_braced_body(s, mi.end() - 1)
        preamble = s[: mi.start()].rstrip()
        # Drop trailing void main() / main(void) { mainImage(...); }
        preamble = re.sub(
            r"\nvoid\s+main\s*\(\s*(?:void)?\s*\)\s*\{[\s\S]*?\}\s*$",
            "",
            preamble,
        )
        body = body_raw
        body = re.sub(rf"\b{re.escape(color_name)}\b", "ret", body)
        body = re.sub(rf"\b{re.escape(coord_name)}\b", "(uv * texsize.xy)", body)
    else:
        # GLSL Sandbox / ES often use void main(void); desktop uses void main()
        main_m = re.search(r"void\s+main\s*\(\s*(?:void)?\s*\)\s*\{", s)
        if main_m:
            body_raw, _ = _extract_braced_body(s, main_m.end() - 1)
            preamble = s[: main_m.start()].rstrip()
            body = body_raw
        else:
            preamble, body = s, "  ret = float4(0,0,0,1);"

    def xlat_types(t: str) -> str:
        for a, b in (
            ("vec4", "float4"),
            ("vec3", "float3"),
            ("vec2", "float2"),
            ("mat4", "float4x4"),
            ("mat3", "float3x3"),
            ("mat2", "float2x2"),
            ("ivec2", "int2"),
            ("ivec3", "int3"),
            ("ivec4", "int4"),
        ):
            t = re.sub(rf"\b{a}\b", b, t)
        return t

    preamble, body = xlat_types(preamble), xlat_types(body)

    def xlat_builtins(t: str) -> str:
        t = re.sub(r"\bfract\b", "frac", t)
        t = re.sub(r"\bmix\b", "lerp", t)
        t = re.sub(r"\binversesqrt\b", "rsq", t)
        t = re.sub(r"\btexture2DLod\s*\(", "tex2Dlod(", t)
        t = re.sub(r"\btexture2D\s*\(", "tex2D(", t)
        t = re.sub(r"\btexture\s*\(", "tex2D(", t)
        t = re.sub(r"\btextureLod\s*\(", "tex2Dlod(", t)
        # Two-arg atan(y,x) -> atan2; leave single-arg atan(x) alone
        t = re.sub(r"\batan\s*\(([^,()]+),([^()]+)\)", r"atan2(\1,\2)", t)
        # GLSL mod is floor-based; HLSL fmod truncates toward zero — wrong for
        # negative UVs (common after surfacePosition - fragCoord math).
        t = re.sub(r"\bmod\s*\(", "mod_glsl(", t)
        t = re.sub(r"\bdFdx\b", "ddx", t)
        t = re.sub(r"\bdFdy\b", "ddy", t)
        t = re.sub(r"\bfwidth\b", "fwidth", t)
        return t

    preamble, body = xlat_builtins(preamble), xlat_builtins(body)

    # Inject GLSL-compatible mod overloads when used (float / float2 / float3 / float4)
    if re.search(r"\bmod_glsl\s*\(", preamble + body):
        mod_helpers = (
            "// GLSL-compatible mod (floor-based; HLSL fmod differs for negatives)\n"
            "float  mod_glsl(float  x, float  y) { return x - y * floor(x / y); }\n"
            "float2 mod_glsl(float2 x, float2 y) { return x - y * floor(x / y); }\n"
            "float3 mod_glsl(float3 x, float3 y) { return x - y * floor(x / y); }\n"
            "float4 mod_glsl(float4 x, float4 y) { return x - y * floor(x / y); }\n"
            "float2 mod_glsl(float2 x, float  y) { return x - y * floor(x / y); }\n"
            "float3 mod_glsl(float3 x, float  y) { return x - y * floor(x / y); }\n"
            "float4 mod_glsl(float4 x, float  y) { return x - y * floor(x / y); }\n"
        )
        preamble = mod_helpers + "\n" + preamble

    # Drop GLSL `#define saturate(...)` — HLSL already has saturate()
    preamble = re.sub(r"#define\s+saturate\s*\([^)]*\)\s*[^\n]*\n", "", preamble)
    body = re.sub(r"#define\s+saturate\s*\([^)]*\)\s*[^\n]*\n", "", body)

    # Mutable global `float time` collides with milk3 `#define time _c2.x`
    # (X3003 redefinition of _c2). Rename to gTime before iTime->time mapping.
    if re.search(r"\bfloat\s+time\s*;", preamble):
        preamble = re.sub(r"\bfloat\s+time\s*;", "static float gTime;", preamble)
        preamble = re.sub(r"(?<![.\w])time(?![\w])", "gTime", preamble)
        # Body still has iTime etc.; after mapping, fix gTime = gTime * k from time=iTime*k
        body_has_global_time = True
    else:
        body_has_global_time = False

    renames = {
        time_uniform: "time",
        res_uniform: "texsize",
        "iTime": "time",
        "iTimeDelta": "(1.0/60.0)",
        "iResolution": "texsize",
        "iMouse": "mouse",
        "iFrame": "frame",
    }
    for old, new in renames.items():
        preamble = re.sub(rf"\b{re.escape(old)}\b", new, preamble)
        body = re.sub(rf"\b{re.escape(old)}\b", new, body)

    if body_has_global_time:
        # Uses of the mutable clock in the body were `time`; iTime already became `time`.
        # Rename body `time` -> gTime, then restore milk3 clock for assigns from iTime.
        body = re.sub(r"(?<![.\w])time(?![\w])", "gTime", body)
        # gTime = gTime * 0.2  was  time = iTime * 0.2
        body = re.sub(
            r"\bgTime\s*=\s*gTime\s*(\*|/)\s*",
            r"gTime = time \1 ",
            body,
            count=1,
        )

    # HLSL reserved names used as functions (X3000 unexpected token 'line')
    for bad, good in (("line", "line2"), ("half", "half_"), ("short", "short_")):
        preamble = re.sub(rf"\b{bad}\s*\(", f"{good}(", preamble)
        body = re.sub(rf"\b{bad}\s*\(", f"{good}(", body)

    # milk3 injects `#define uv _uv.xy` — a formal param named `uv` becomes
    # `float2 _uv.xy` (X3000 unexpected '.'). Rename formals + their uses.
    def rename_uv_formals(src: str) -> str:
        # float line2(..., float2 uv)  or  float2 uv,  in param lists
        def repl_sig(m: re.Match) -> str:
            return m.group(0).replace(" uv", " puv").replace("(uv", "(puv")

        src = re.sub(
            r"\b(float2|float3|float4|vec2|vec3|vec4)\s+uv\b(?=\s*[,)])",
            r"\1 puv",
            src,
        )
        # Body of helpers: still need uv uses inside those functions renamed.
        # Only rewrite identifier `uv` outside of milk3 built-in usages that we
        # inject later as (uv * texsize.xy). Helpers never use that pattern.
        # Conservative: if preamble had a formal puv, rename bare `uv` in preamble
        # to puv except inside comments.
        if re.search(r"\bpuv\b", src):
            lines = src.split("\n")
            out = []
            for ln in lines:
                if re.search(r"\(\s*uv\s*\*\s*texsize", ln):
                    out.append(ln)
                else:
                    out.append(re.sub(r"\buv\b", "puv", ln))
            src = "\n".join(out)
        return src

    preamble = rename_uv_formals(preamble)

    def fix_matrix_muls(src: str) -> str:
        """GLSL allows v*=mat / v*mat / mat*v; HLSL needs mul()."""
        # Collect matrix variable names from declarations + common rot/mat ids
        mat_names = set(re.findall(r"\bfloat[2-4]x[2-4]\s+(\w+)", src))
        mat_names |= set(re.findall(r"\b(rot\d*|mrot|mat[A-Z]\w*|mat\d*)\b", src))
        # Single-letter matrix vars: `float3x3 m = ...` (very common)
        mat_names |= set(re.findall(r"\bfloat[2-4]x[2-4]\s+([a-zA-Z_]\w*)\s*=", src))
        # rotate2D(...) result used inline
        src = re.sub(
            r"(\w+(?:\.[xyzw]{1,4})?)\s*\*=\s*(rotate2D\s*\([^;]+\)|float2x2\s*\([^;]+\)|float3x3\s*\([^;]+\))\s*;",
            r"\1 = mul(\1, \2);",
            src,
        )
        _num = r"(?:[0-9]+\.?[0-9]*|\.[0-9]+)"
        for mn in sorted(mat_names, key=len, reverse=True):
            # dir.xz *= rot1;  /  dir *= rot;
            src = re.sub(
                rf"(\w+(?:\.[xyzw]{{1,4}})?)\s*\*=\s*{re.escape(mn)}\s*\*\s*({_num})\s*;",
                rf"\1 = mul(\1, {mn} * \2);",
                src,
            )
            src = re.sub(
                rf"(\w+(?:\.[xyzw]{{1,4}})?)\s*\*=\s*{re.escape(mn)}\s*;",
                rf"\1 = mul(\1, {mn});",
                src,
            )
            # R.Dir = float3(xx,yy,-1) * rot;
            src = re.sub(
                rf"(\w+(?:\.\w+)?)\s*=\s*([^;]*?)\s*\*\s*{re.escape(mn)}\s*;",
                rf"\1 = mul(\2, {mn});",
                src,
            )
            # return m * p;  /  return mul(m, p);
            src = re.sub(
                rf"\breturn\s+{re.escape(mn)}\s*\*\s*(\w+)\s*;",
                rf"return mul({mn}, \1);",
                src,
            )
            src = re.sub(
                rf"\breturn\s+(\w+)\s*\*\s*{re.escape(mn)}\s*;",
                rf"return mul(\1, {mn});",
                src,
            )
        return src

    preamble, body = fix_matrix_muls(preamble), fix_matrix_muls(body)

    # milk3 include.fx: `#define rad _rad_ang.x` / `#define ang _rad_ang.y`
    # Params/locals named rad/ang become `float2 _rad_ang.x` -> X3000 unexpected '.'.
    def rename_rad_ang_shadows(src: str) -> str:
        if not re.search(r"\b(float[2-4]?|int)\s+(rad|ang)\b", src):
            return src
        for old, new in (("rad", "_mw_rad"), ("ang", "_mw_ang")):
            if not re.search(rf"\b(float[2-4]?|int)\s+{old}\b", src):
                continue
            # Rename all identifier uses (not inside other words)
            src = re.sub(rf"\b{old}\b", new, src)
        return src

    preamble = rename_rad_ang_shadows(preamble)
    body = rename_rad_ang_shadows(body)

    # gl_FragCoord components before whole-identifier replace
    body = body.replace("gl_FragCoord.xy", "(uv * texsize.xy)")
    body = re.sub(r"\bgl_FragCoord\.x\b", "(uv * texsize.xy).x", body)
    body = re.sub(r"\bgl_FragCoord\.y\b", "(uv * texsize.xy).y", body)
    body = body.replace("gl_FragCoord", "(float4(uv * texsize.xy, 0, 1))")
    preamble = preamble.replace("gl_FragCoord.xy", "(uv * texsize.xy)")
    preamble = re.sub(r"\bgl_FragCoord\.x\b", "(uv * texsize.xy).x", preamble)
    preamble = re.sub(r"\bgl_FragCoord\.y\b", "(uv * texsize.xy).y", preamble)

    # Strip GLSL `in` only — HLSL accepts `out`/`inout` and needs them for writeback
    # (e.g. bool Scene(..., out float resT, out float type)).
    # Bare `in float2` is invalid in some HLSL profiles; default is already `in`.
    preamble = re.sub(r"\bin\s+(float[2-4]?|int[2-4]?|bool)\b", r"\1", preamble)
    body = re.sub(r"\bin\s+(float[2-4]?|int[2-4]?|bool)\b", r"\1", body)
    # clamp is valid HLSL
    preamble = re.sub(r"\bclamp\b", "clamp", preamble)

    # HLSL intrinsics cannot be used as variable names (compile error -> black screen)
    def rename_hlsl_reserved_vars(src: str) -> str:
        # Common GLSL locals that collide with HLSL built-ins
        reserved = {
            "distance": "hitDist",
            "asfloat": "asfloat_",
            "asint": "asint_",
            "asuint": "asuint_",
            "radians": "radians_",
            "degrees": "degrees_",
            "saturate": "saturate_",
            "frac": "frac_",
            "lerp": "lerp_",
            "step": "step_",
            "reflect": "reflect_",
            "refract": "refract_",
            "faceforward": "faceforward_",
            "normalize": "normalize_",  # rare as var
            "length": "length_",  # rare as var — skip, too common as... actually length is often a var!
        }
        # Only rename high-confidence collisions (frequent black-screen causes).
        # `length`/`normalize` as var names are rare enough to skip (too many false positives).
        reserved = {
            "distance": "hitDist",
            "asfloat": "asfloat_",
            "asint": "asint_",
            "asuint": "asuint_",
            "EvaluateAttributeSnapped": "EvaluateAttributeSnapped_",
        }
        for old, new in reserved.items():
            # Declarations: float distance / out float distance
            src = re.sub(
                rf"\b((?:out|inout)\s+)?(float[2-4]?|int[2-4]?|bool)\s+{old}\b",
                rf"\1\2 {new}",
                src,
            )
            # Uses that are not function calls: distance * x  but not distance(a,b)
            src = re.sub(rf"\b{old}\b(?!\s*\()", new, src)
        return src

    preamble = rename_hlsl_reserved_vars(preamble)
    body = rename_hlsl_reserved_vars(body)

    def rename_local_uv(src: str) -> str:
        """Local `float2 uv = ...` / `float2 uv, p = ...` shadows milk3 `#define uv`.

        Rename each local uv decl to uvn/uvn2/... and subsequent uses.
        Built-in pixel UV expressions `(uv * texsize.xy)` are left alone.
        """
        if not re.search(r"\bfloat2\s+uv\s*[=,;]", src):
            return src
        lines = src.split("\n")
        out_lines: List[str] = []
        active: Optional[str] = None
        n = 0
        # Protect both plain and Y-flipped fragCoord forms
        protect = (
            r"(\(\s*uv\s*\*\s*texsize\.xy(?:\s*\.\s*[xy])?\s*\)|"
            r"\(\s*float2\s*\(\s*uv\.x\s*,\s*1\.0\s*-\s*uv\.y\s*\)\s*\*\s*texsize\.xy(?:\s*\.\s*[xy])?\s*\))"
        )

        def rewrite_uses(text: str, name: str) -> str:
            parts = re.split(protect, text)
            rebuilt: List[str] = []
            for part in parts:
                if part and re.match(protect, part):
                    rebuilt.append(part)
                else:
                    rebuilt.append(re.sub(r"\buv\b", name, part or ""))
            return "".join(rebuilt)

        for ln in lines:
            m = re.search(r"\bfloat2\s+uv\s*([=,;])", ln)
            if m:
                n += 1
                active = "uvn" if n == 1 else f"uvn{n}"
                # float2 uv =  / float2 uv,  / float2 uv;
                ln = re.sub(r"\bfloat2\s+uv\b", f"float2 {active}", ln, count=1)
                ln = rewrite_uses(ln, active)
                out_lines.append(ln)
                continue
            if active:
                ln = rewrite_uses(ln, active)
            out_lines.append(ln)
        return "\n".join(out_lines)

    preamble = rename_local_uv(preamble)
    body = rename_local_uv(body)

    def rename_shadowing_time(src: str) -> str:
        """Local `float time = ...` becomes `float _c2.x = ...` via milk3 #define time.

        Rename the local to gTime; keep RHS `time` as the milk3 clock on the
        declaration line, then rename later uses to gTime.
        """
        if not re.search(r"\bfloat\s+time\s*[=;]", src):
            return src
        lines = src.split("\n")
        out: List[str] = []
        active = False
        for ln in lines:
            if re.search(r"\bfloat\s+time\s*=", ln):
                # decl only -> gTime; RHS keeps milk3 `time` clock
                ln = re.sub(r"\bfloat\s+time\s*=", "float gTime =", ln, count=1)
                out.append(ln)
                active = True
                continue
            if re.search(r"\bfloat\s+time\s*;", ln):
                ln = re.sub(r"\bfloat\s+time\s*;", "float gTime = 0.0;", ln, count=1)
                out.append(ln)
                active = True
                continue
            if active:
                ln = re.sub(r"\btime\b", "gTime", ln)
            out.append(ln)
        return "\n".join(out)

    preamble = rename_shadowing_time(preamble)
    body = rename_shadowing_time(body)

    # GLSL allows v[i] = x on vectors; HLSL does not (X3500). Expand simple cases.
    # Use .xyz only by default — float3 has no .w (X3018). float4[i] with i==3 is rare
    # in Sandbox loops (usually for(i=0;i<3;i++) on vec3).
    def fix_dynamic_vec_index_assign(src: str) -> str:
        def repl(m: re.Match) -> str:
            var, idx, expr = m.group(1), m.group(2), m.group(3).strip()
            return (
                f"if(({idx})==0) {var}.x = ({expr}); "
                f"else if(({idx})==1) {var}.y = ({expr}); "
                f"else {var}.z = ({expr});"
            )

        # var[i] = expr;  but not var[i].member = expr
        return re.sub(
            r"\b([A-Za-z_]\w*)\[([^\]\n]+)\]\s*=\s*([^;]+);",
            repl,
            src,
        )

    preamble = fix_dynamic_vec_index_assign(preamble)
    body = fix_dynamic_vec_index_assign(body)

    for a in ("gl_FragColor", "outColor", "fragColor", "shadertoy_outcolor"):
        body = re.sub(rf"\b{a}\b", "ret", body)

    if "ret" not in body and "return" in body:
        body = re.sub(r"\breturn\s+", "ret = ", body)

    # Early `return;` in main/shader_body skips the engine's trailing
    # `_return_value = float4(ret.xyz, 1);` -> uninitialized out -> snow/garbage
    # letterbox bars (e.g. GLSL Sandbox cine-crop). Same fix as engine_shader_import_ui.
    body = re.sub(
        r"\breturn\s*;",
        "_return_value = float4(ret.xyz, 1.0); return;",
        body,
    )

    body = body.replace("texsize.xy.xy", "texsize.xy").replace("texsize.xy.y", "texsize.y")
    preamble = preamble.replace("texsize.xy.xy", "texsize.xy").replace("texsize.xy.y", "texsize.y")

    def splat_ctor(src: str) -> str:
        """GLSL vecN(x) scalar broadcast + vecN(vectorExpr) cast.

        HLSL rejects bare float3(scalar) and also rejects float3(v,v,v) when v is
        already a float3 (common bug from naive splat of vec3(v*.01)).

        Strategy:
          - single-arg floatN(expr) -> ((floatN)(expr))  (works for scalar AND vector)
          - N identical args floatN(a,a,...,a) -> ((floatN)(a))
          - otherwise leave multi-arg constructors alone
        """

        def split_top_args(inner: str) -> List[str]:
            parts: List[str] = []
            d = 0
            start = 0
            for i, ch in enumerate(inner):
                if ch == "(":
                    d += 1
                elif ch == ")":
                    d -= 1
                elif ch == "," and d == 0:
                    parts.append(inner[start:i].strip())
                    start = i + 1
            parts.append(inner[start:].strip())
            return parts

        def expand_floatn(text: str, n: int) -> str:
            token = f"float{n}"
            out: List[str] = []
            i = 0
            while i < len(text):
                j = text.find(token, i)
                if j < 0:
                    out.append(text[i:])
                    break
                # avoid matching identifiers that end with floatN (e.g. myfloat3)
                if j > 0 and (text[j - 1].isalnum() or text[j - 1] == "_"):
                    out.append(text[i : j + len(token)])
                    i = j + len(token)
                    continue
                k = j + len(token)
                while k < len(text) and text[k] in " \t":
                    k += 1
                if k >= len(text) or text[k] != "(":
                    out.append(text[i : j + len(token)])
                    i = j + len(token)
                    continue
                # balanced paren match
                depth = 0
                p = k
                while p < len(text):
                    if text[p] == "(":
                        depth += 1
                    elif text[p] == ")":
                        depth -= 1
                        if depth == 0:
                            break
                    p += 1
                if p >= len(text):
                    out.append(text[i:])
                    break
                inner = text[k + 1 : p]
                out.append(text[i:j])
                args = split_top_args(inner)
                if len(args) == 1 and args[0]:
                    arg = args[0].strip()
                    # Nested same-type ctor: float4(float4(0.0)) / vec4(vec4(0))
                    # Leave only one cast — bare float4(0.0) is X3014 in HLSL.
                    m_nest = re.match(
                        rf"^{token}\s*\((.*)\)$",
                        arg,
                        flags=re.S,
                    )
                    if m_nest:
                        arg = m_nest.group(1).strip()
                    # scalar broadcast OR vector cast — both OK as HLSL cast
                    out.append(f"(({token})({arg}))")
                elif len(args) == n and args[0] and all(a == args[0] for a in args):
                    # float3(v*.01, v*.01, v*.01) from older splat — collapse
                    out.append(f"(({token})({args[0]}))")
                else:
                    out.append(text[j : p + 1])
                i = p + 1
            return "".join(out)

        # Multi-pass so nested float4(float4(0.0)) becomes ((float4)(0.0))
        for _ in range(4):
            prev = src
            for n in (2, 3, 4):
                src = expand_floatn(src, n)
            if src == prev:
                break
        # Final safety: any remaining floatN(scalar_literal) -> cast form
        # (HLSL X3014: float4(0.0) is not a valid constructor in some profiles)
        def fix_scalar_ctor(text: str, n: int) -> str:
            token = f"float{n}"
            return re.sub(
                rf"\b{token}\s*\(\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?f?)\s*\)",
                rf"(({token})(\1))",
                text,
            )

        for n in (2, 3, 4):
            src = fix_scalar_ctor(src, n)
        return src

    # Known mutable globals from Shadertoy ports (HLSL PS needs static)
    for gname in ("lightPos", "LightPos", "light", "ro", "rd", "camPos", "cameraPos"):
        preamble = re.sub(
            rf"(?<!static )float3\s+{gname}\s*;",
            f"static float3 {gname};",
            preamble,
        )
        preamble = re.sub(
            rf"(?<!static )float3\s+{gname}\s*=",
            f"static float3 {gname} =",
            preamble,
        )

    # Top-level (column-0) globals -> static. Locals in functions are indented.
    # Skip initializers that reference runtime uniforms (texsize/time/mouse) —
    # HLSL static init must be constant; convert those to macros instead.
    def make_column0_globals_static(src: str) -> str:
        lines_out: List[str] = []
        for ln in src.split("\n"):
            if not ln or ln[0].isspace() or ln.startswith("//") or ln.startswith("#"):
                lines_out.append(ln)
                continue
            if ln.startswith("static ") or ln.startswith("struct ") or ln.startswith("typedef "):
                lines_out.append(ln)
                continue
            # Skip function definitions: Type name(
            if re.match(
                r"^(?:const\s+)?(?:void|bool|float[2-4]?|float[2-4]x[2-4]|int[2-4]?|half|double|"
                r"[A-Z]\w*)\s+\w+\s*\(",
                ln,
            ):
                lines_out.append(ln)
                continue
            # Global var / const / array at column 0
            mglob = re.match(
                r"^(?:const\s+)?(?:float[2-4]?|float[2-4]x[2-4]|int[2-4]?|bool|double)\s+(\w+)\s*=\s*(.+);\s*$",
                ln,
            )
            if mglob:
                name, expr = mglob.group(1), mglob.group(2)
                if re.search(r"\b(texsize|time|mouse|_c\d+)\b", expr):
                    # Runtime-dependent -> #define so it re-evaluates each use
                    lines_out.append(f"#define {name} ({expr})")
                    continue
                ln = "static " + ln
                lines_out.append(ln)
                continue
            if re.match(
                r"^(?:const\s+)?(?:float[2-4]?|float[2-4]x[2-4]|int[2-4]?|bool|double|[A-Z]\w*)\s+\w+\s*[=;[\[]",
                ln,
            ):
                ln = "static " + ln
            lines_out.append(ln)
        return "\n".join(lines_out)

    preamble = make_column0_globals_static(preamble)

    # Global arrays of structs / values (GLSL Sandbox raytracers etc.)
    preamble = re.sub(r"(?m)^(?!static\s)(const\s+int\s+)", r"static \1", preamble)
    preamble = re.sub(
        r"(?m)^(?!static\s)([A-Za-z_]\w*)\s+(\w+)\s*\[",
        r"static \1 \2[",
        preamble,
    )

    # GLSL: if(cond) a *= -1.0, b = true;  — HLSL needs a block.
    # Only assignment pairs; balanced parens so nested call conditions work.
    def fix_comma_if(src: str) -> str:
        out: List[str] = []
        i = 0
        while True:
            m = re.search(r"\bif\s*\(", src[i:])
            if not m:
                out.append(src[i:])
                break
            start = i + m.start()
            out.append(src[i:start])
            paren_start = i + m.end() - 1  # index of '('
            depth = 0
            p = paren_start
            while p < len(src):
                if src[p] == "(":
                    depth += 1
                elif src[p] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                p += 1
            if p >= len(src):
                out.append(src[start:])
                break
            cond = src[paren_start + 1 : p]
            rest_start = p + 1
            while rest_start < len(src) and src[rest_start] in " \t":
                rest_start += 1
            rest = src[rest_start:]
            m2 = re.match(
                r"(\w+(?:\.\w+)?\s*(?:\*=|\+=|\-=|\/=|=)\s*[^,;]+)\s*,\s*"
                r"(\w+(?:\.\w+)?\s*(?:\*=|\+=|\-=|\/=|=)\s*[^;]+)\s*;",
                rest,
            )
            if m2:
                a = m2.group(1).strip()
                b = m2.group(2).strip()
                out.append(f"if({cond}) {{ {a}; {b}; }}")
                i = rest_start + m2.end()
            else:
                out.append(src[start:rest_start])
                i = rest_start
        return "".join(out)

    preamble, body = fix_comma_if(preamble), fix_comma_if(body)

    # Uninitialized locals used as accumulators (GLSL zero-fills; HLSL does not)
    preamble = re.sub(
        r"\bfloat\s+dS\s*,\s*dO\s*;",
        "float dS = 0.0, dO = 0.0;",
        preamble,
    )
    body = re.sub(
        r"\bfloat\s+dS\s*,\s*dO\s*;",
        "float dS = 0.0, dO = 0.0;",
        body,
    )

    preamble, body = splat_ctor(preamble), splat_ctor(body)
    return join_shader_body(preamble, body)


def milk3_hlsl_to_glsl(preamble: str, body: str, time_u: str = "u_time", res_u: str = "u_resolution") -> str:
    """Rough HLSL milk3 -> GLSL fragment for SHADERed preview (lossy)."""
    s = (preamble + "\n" + body) if preamble else body

    for a, b in (
        ("float4", "vec4"),
        ("float3", "vec3"),
        ("float2", "vec2"),
        ("float4x4", "mat4"),
        ("float3x3", "mat3"),
        ("float2x2", "mat2"),
        ("int2", "ivec2"),
        ("int3", "ivec3"),
        ("int4", "ivec4"),
    ):
        s = re.sub(rf"\b{a}\b", b, s)

    s = re.sub(r"\bfrac\b", "fract", s)
    s = re.sub(r"\blerp\b", "mix", s)
    s = re.sub(r"\brsq\b", "inversesqrt", s)
    s = re.sub(r"\btex2D\s*\(\s*(\w+)\s*,", r"texture(\1,", s)
    s = re.sub(r"\batan2\s*\(([^,]+),([^)]+)\)", r"atan(\1,\2)", s)
    s = re.sub(r"\bfmod\s*\(", "mod(", s)

    # mul(A,B) -> A*B (GLSL)
    s = re.sub(r"\bmul\s*\(\s*([^,]+)\s*,\s*([^)]+)\)", r"(\1 * \2)", s)

    s = re.sub(r"\btime\b", time_u, s)
    s = re.sub(r"\btexsize\.xy\b", res_u, s)
    s = re.sub(r"\btexsize\b", res_u, s)

    # ret -> gl_FragColor
    s = re.sub(r"\bret\b", "gl_FragColor", s)

    # uv: milk3 is 0..1; many GLSL shaders use centered UV — leave uv as-is and
    # define uv from gl_FragCoord at top of main
    # Body may use `uv` — provide it

    # Strip HLSL register annotations / semantics if any
    s = re.sub(r"\s*:\s*register\s*\([^)]*\)", "", s)
    s = re.sub(r"\s*:\s*SV_\w+", "", s)

    main_body = s
    # If still has leftover shader_body text, strip
    main_body = re.sub(r"shader_body\s*", "", main_body)

    return f"""#version 330
precision mediump float;

uniform float {time_u};
uniform vec2 {res_u};

{MARKER_PREAMBLE_BEGIN}
// (helpers inlined with body for milk3->glsl export)
{MARKER_PREAMBLE_END}

void main()
{{
    vec2 uv = gl_FragCoord.xy / {res_u};
    // milk3-style 0..1 uv; shaders that expect centered UV should compute it themselves
{MARKER_BODY_BEGIN}
{main_body}
{MARKER_BODY_END}
}}
"""


# ---------------------------------------------------------------------------
# SHADERed project I/O
# ---------------------------------------------------------------------------

def load_tree(path: Path) -> Dict[str, bytes]:
    """Load folder or zip into {posix_relpath: bytes}."""
    if path.is_file() and path.suffix.lower() == ".zip":
        tree: Dict[str, bytes] = {}
        with zipfile.ZipFile(path, "r") as zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                tree[info.filename.replace("\\", "/")] = zf.read(info)
        # If zip has a single top-level folder, strip it for convenience
        tops = {k.split("/")[0] for k in tree}
        if len(tops) == 1:
            top = next(iter(tops))
            if not any(k == top for k in tree):  # it's a directory prefix
                # only strip if all paths share prefix and sprj is inside
                pref = top + "/"
                if all(k.startswith(pref) for k in tree) and any(
                    k.lower().endswith(".sprj") for k in tree
                ):
                    tree = {k[len(pref) :]: v for k, v in tree.items()}
        return tree

    if path.is_file() and path.suffix.lower() == ".sprj":
        root = path.parent
        tree = {}
        for f in root.rglob("*"):
            if f.is_file():
                tree[f.relative_to(root).as_posix()] = f.read_bytes()
        return tree

    if path.is_dir():
        tree = {}
        for f in path.rglob("*"):
            if f.is_file():
                tree[f.relative_to(path).as_posix()] = f.read_bytes()
        return tree

    die(f"not a zip/sprj/folder: {path}")


def tree_get_text(tree: Dict[str, bytes], rel: str) -> str:
    rel = rel.replace("\\", "/").lstrip("/")
    if rel in tree:
        return tree[rel].decode("utf-8", errors="replace")
    low = {k.lower(): k for k in tree}
    if rel.lower() in low:
        return tree[low[rel.lower()]].decode("utf-8", errors="replace")
    base = Path(rel).name.lower()
    for k in tree:
        if Path(k).name.lower() == base:
            return tree[k].decode("utf-8", errors="replace")
    die(f"missing in project: {rel}")


def find_sprj_key(tree: Dict[str, bytes]) -> str:
    keys = [k for k in tree if k.lower().endswith(".sprj")]
    if not keys:
        die("no .sprj in project")
    keys.sort(key=lambda k: (0 if Path(k).name.lower() == "project.sprj" else 1, len(k)))
    return keys[0]


def parse_sprj_passes(xml: str) -> List[Tuple[str, str, str]]:
    """[(pass_name, ps_path, vs_path)]"""
    root = ET.fromstring(xml)
    pipe = root.find("pipeline")
    if pipe is None:
        die("sprj missing pipeline")
    out = []
    for pe in pipe.findall("pass"):
        name = pe.get("name") or "pass"
        ps = vs = ""
        for sh in pe.findall("shader"):
            t = (sh.get("type") or "").lower()
            p = (sh.get("path") or "").replace("\\", "/")
            if t == "ps":
                ps = p
            elif t == "vs":
                vs = p
        if ps:
            out.append((name, ps, vs))
    if not out:
        die("no pixel shader passes in sprj")
    return out


def detect_uniforms(ps_src: str) -> Tuple[str, str]:
    """Return (time_name, resolution_name) from GLSL/HLSL uniforms."""
    time_u, res_u = "u_time", "u_resolution"
    for m in re.finditer(r"uniform\s+\w+\s+(\w+)\s*;", ps_src):
        n = m.group(1)
        nl = n.lower()
        if "time" in nl:
            time_u = n
        if "res" in nl or "resolution" in nl or "viewport" in nl:
            res_u = n
    # also iTime / iResolution without uniform keyword (Shadertoy paste)
    if re.search(r"\biTime\b", ps_src):
        time_u = "iTime"
    if re.search(r"\biResolution\b", ps_src):
        res_u = "iResolution"
    return time_u, res_u


def is_rust_spirv(src: str, path: str = "") -> bool:
    """SHADERed rust-gpu / spirv-std fragment shaders (.rs), not GLSL/HLSL."""
    if path.lower().endswith(".rs"):
        return True
    return bool(
        re.search(r"#!\[cfg_attr\s*\(\s*target_arch\s*=\s*\"spirv\"", src)
        or re.search(r"\bspirv_std\b", src)
        or re.search(r"#\[spirv\s*\(\s*fragment\s*\)\]", src)
        or re.search(r"\bfn\s+main_fs\s*\(", src)
    )


def is_glsl(src: str) -> bool:
    if is_rust_spirv(src):
        return False
    return bool(
        re.search(r"#version\s+\d+", src)
        or re.search(r"\bgl_FragColor\b", src)
        or re.search(r"\bgl_FragCoord\b", src)
        or re.search(r"\bvoid\s+main\s*\(\s*\)", src)
        or (
            re.search(r"\bvec[234]\b", src)
            and not re.search(r"\buse\s+spirv_std", src)
        )
    )


def extract_shadertoy_id(src: str) -> Optional[str]:
    m = re.search(r"shadertoy\.com/view/([A-Za-z0-9]+)", src, re.I)
    return m.group(1) if m else None


def rust_creation_xsxxdn_to_milk3() -> str:
    """Danilo Guanabara 'Creation' (Shadertoy XsXXDn / pouet 57245) as milk3 HLSL.

    Matches the rust-gpu port logic (rem_euclid, sin(l*9 - z*2)).
    """
    return r'''// Creation by Danilo Guanabara — Shadertoy XsXXDn / pouet 57245
// Converted from SHADERed rust-gpu port for MDropDX12

shader_body {
  float3 c = float3(0, 0, 0);
  float l = 0.0;
  float z = time;
  float2 frag = uv; // milk3 uv is 0..1

  [unroll] for (int i = 0; i < 3; i++) {
    float2 p = frag;
    float2 q = p;
    p -= 0.5;
    p.x *= texsize.x / max(texsize.y, 1.0);
    z += 0.07;
    l = length(p);
    // avoid div0 at exact center
    float invl = 1.0 / max(l, 1e-4);
    q += p * invl * (sin(z) + 1.0) * abs(sin(l * 9.0 - z * 2.0));
    // GLSL/Rust rem_euclid into [0,1), then distance to tile center
    float2 t = q - floor(q);
    float ci = 0.01 / max(length(abs(t - 0.5)), 1e-4);
    if (i == 0) c.x = ci;
    else if (i == 1) c.y = ci;
    else c.z = ci;
  }

  ret = float4(c / max(l, 1e-3), 1.0);
}
'''


def convert_rust_spirv_to_milk3(src: str, path: str = "") -> str:
    """Best-effort: known Shadertoy ports; otherwise raise with a clear error."""
    st_id = extract_shadertoy_id(src)
    # XsXXDn / Guanabara creation — common SHADERed rust-gpu sample
    if st_id and st_id.lower() == "xsxxdn":
        return rust_creation_xsxxdn_to_milk3()
    if "rem_euclid" in src and "0.01 /" in src and "z += 0.07" in src.replace(" ", ""):
        return rust_creation_xsxxdn_to_milk3()
    if "z += 0.07" in src or "z+=0.07" in src.replace(" ", ""):
        # same family
        if "l * 9.0" in src or "l*9" in src.replace(" ", ""):
            return rust_creation_xsxxdn_to_milk3()

    hint = f" (Shadertoy {st_id})" if st_id else ""
    die(
        f"unsupported SHADERed shader language: rust-gpu / SPIR-V Rust{hint} in {path or 'pixel shader'}.\n"
        "  This converter supports GLSL (#version / gl_FragColor) and HLSL (shader_body),\n"
        "  not .rs spirv-std shaders.\n"
        "  Fix: in SHADERed export/save as GLSL, or paste the original Shadertoy GLSL into a .glsl pass.\n"
        f"  If this is XsXXDn-style, re-run after ensuring the source cites shadertoy.com/view/XsXXDn."
    )
    return ""  # unreachable


def guess_pass_key(name: str, path: str, index: int, n: int) -> str:
    blob = f"{name} {path}".lower()
    for key in ("buffera", "bufferb", "bufferc", "bufferd", "image", "common", "main"):
        if key in blob.replace(" ", ""):
            if key == "buffera":
                return "bufferA"
            if key == "bufferb":
                return "bufferB"
            if key == "bufferc":
                return "bufferC"
            if key == "bufferd":
                return "bufferD"
            if key == "main":
                return "image"
            return key
    return "image" if index == n - 1 or n == 1 else f"buffer{chr(ord('A') + index)}"


# ---------------------------------------------------------------------------
# to-shadered
# ---------------------------------------------------------------------------

SIMPLE_VS_GLSL = """#version 330 core
layout (location = 0) in vec3 aPos;

void main()
{
    gl_Position = vec4(aPos, 1.0);
}
"""

SIMPLE_VS_HLSL = """cbuffer cbPerFrame : register(b0)
{
    float4x4 matVP;
    float4x4 matGeo;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 UV       : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 UV       : TEXCOORD0;
};

VSOutput main(VSInput vin)
{
    VSOutput o;
    o.Position = mul(mul(float4(vin.Position, 1.0), matGeo), matVP);
    o.UV = vin.UV;
    o.UV.y = 1.0 - o.UV.y;
    return o;
}
"""


def wrap_milk3_as_glsl_ps(pass_name: str, milk3_src: str) -> str:
    """Export milk3 HLSL as a GLSL fragment-ish file with markers for round-trip."""
    pre, body = split_shader_body(milk3_src)
    # Keep original HLSL in markers; add a tiny GLSL shell so SHADERed can open it.
    # Full automatic HLSL->GLSL of complex milk3 is lossy; markers preserve fidelity.
    return f"""#version 330
precision mediump float;

uniform float u_time;
uniform vec2 u_resolution;

// Pass: {pass_name}
// Original milk3 HLSL is preserved in markers for to-milk3 round-trip.
// SHADERed may not fully compile complex milk3 HLSL-as-GLSL; use milk3_raw/ for truth.

{MARKER_PREAMBLE_BEGIN}
{pre if pre.strip() else "// (empty preamble)"}
{MARKER_PREAMBLE_END}

void main()
{{
    // Placeholder preview (magenta) — replace body with real GLSL or convert externally.
    // Round-trip uses markers + milk3_raw/, not this placeholder.
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;
{MARKER_BODY_BEGIN}
{body if body.strip() else "  // empty"}
{MARKER_BODY_END}
    gl_FragColor = vec4(uv, 0.5 + 0.5 * sin(u_time), 1.0);
}}
"""


def wrap_milk3_as_hlsl_ps(pass_name: str, milk3_src: str, common: str = "") -> str:
    pre, body = split_shader_body(milk3_src)
    if common.strip():
        pre = (common.strip() + "\n\n" + pre).strip() if pre else common.strip()

    return f"""// SHADERed HLSL wrapper — pass {pass_name}
// Edit between MILK3_* markers for round-trip.

cbuffer cbPerFrame : register(b0)
{{
    float4x4 matVP;
    float4x4 matGeo;
    float2   iResolution;
    float    iTime;
    float2   iMouse;
    float    iFrame;
}};

static float4 _c2;
static float4 texsize;
static float time, frame;
static float bass, mid, treb, vol;
static float2 uv, uv_orig;
static float4 ret;

#define shader_body void milk3_shader_body()

struct PSInput
{{
    float4 Position : SV_POSITION;
    float2 UV       : TEXCOORD0;
}};

{MARKER_PREAMBLE_BEGIN}
{pre if pre.strip() else "// (empty preamble)"}
{MARKER_PREAMBLE_END}

shader_body {{
{MARKER_BODY_BEGIN}
{body if body.strip() else "  ret = float4(0,0,0,1);"}
{MARKER_BODY_END}
}}

float4 main(PSInput pin) : SV_TARGET
{{
    _c2 = float4(iTime, 60.0, iFrame, 0.0);
    time = iTime;
    frame = iFrame;
    texsize = float4(iResolution, 1.0 / max(iResolution.x, 1.0), 1.0 / max(iResolution.y, 1.0));
    uv = pin.UV;
    uv_orig = pin.UV;
    bass = mid = treb = vol = 0.5;
    ret = float4(0, 0, 0, 1);
    milk3_shader_body();
    return ret;
}}
"""


def build_sprj_simple(
    pass_name: str,
    vs_path: str,
    ps_path: str,
    lang: str,
    time_u: str = "u_time",
    res_u: str = "u_resolution",
) -> str:
    """Match real SHADERed simple fullscreen project (LOzB7CqQGt style)."""
    if lang == "glsl":
        geo = "ScreenQuadNDC"
        variables = f"""\t\t\t<variables>
\t\t\t\t<variable type="float" name="{xml_esc(time_u)}" system="Time" />
\t\t\t\t<variable type="float2" name="{xml_esc(res_u)}" system="ViewportSize" />
\t\t\t</variables>"""
    else:
        geo = "ScreenQuad"
        variables = f"""\t\t\t<variables>
\t\t\t\t<variable type="float4x4" name="matVP" system="Orthographic" />
\t\t\t\t<variable type="float4x4" name="matGeo" system="GeometryTransform" />
\t\t\t\t<variable type="float2" name="iResolution" system="ViewportSize" />
\t\t\t\t<variable type="float" name="iTime" system="Time" />
\t\t\t\t<variable type="float2" name="iMouse" system="MousePosition" />
\t\t\t\t<variable type="float" name="iFrame" system="FrameIndex" />
\t\t\t</variables>"""

    return f"""<?xml version="1.0"?>
<!-- Generated by tools/milk3_shadered.py -->
<project version="2">
\t<pipeline>
\t\t<pass name="{xml_esc(pass_name)}" type="shader" active="true">
\t\t\t<shader type="vs" path="{xml_esc(vs_path)}" entry="main" />
\t\t\t<shader type="ps" path="{xml_esc(ps_path)}" entry="main" />
\t\t\t<inputlayout>
\t\t\t\t<item value="Position" semantic="POSITION" />
\t\t\t\t<item value="Normal" semantic="NORMAL" />
\t\t\t\t<item value="Texcoord" semantic="TEXCOORD0" />
\t\t\t</inputlayout>
\t\t\t<rendertexture />
\t\t\t<items>
\t\t\t\t<item name="Quad" type="geometry">
\t\t\t\t\t<type>{geo}</type>
\t\t\t\t\t<width>1</width>
\t\t\t\t\t<height>1</height>
\t\t\t\t\t<depth>1</depth>
\t\t\t\t\t<topology>TriangleList</topology>
\t\t\t\t</item>
\t\t\t</items>
\t\t\t<itemvalues />
{variables}
\t\t\t<macros />
\t\t</pass>
\t</pipeline>
\t<objects />
\t<cameras />
\t<settings>
\t\t<entry type="property" name="{xml_esc(pass_name)}" item="pipe" />
\t\t<entry type="file" name="{xml_esc(pass_name)}" shader="ps" />
\t\t<entry type="file" name="{xml_esc(pass_name)}" shader="vs" />
\t\t<entry type="camera" fp="false">
\t\t\t<distance>7</distance>
\t\t\t<pitch>0</pitch>
\t\t\t<yaw>0</yaw>
\t\t\t<roll>0</roll>
\t\t</entry>
\t\t<entry type="clearcolor" r="0" g="0" b="0" a="0" />
\t\t<entry type="usealpha" val="false" />
\t</settings>
\t<plugindata />
</project>
"""


def write_tree(files: Dict[str, str], out: Path, as_zip: bool) -> Path:
    if as_zip or str(out).lower().endswith(".zip"):
        if not str(out).lower().endswith(".zip"):
            out = Path(str(out) + ".zip")
        out.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            for rel, content in files.items():
                zf.writestr(rel.replace("\\", "/"), content)
        print(f"wrote {out}")
        return out

    out.mkdir(parents=True, exist_ok=True)
    for rel, content in files.items():
        write_text(out / rel.replace("\\", "/"), content)
    print(f"wrote {out / 'project.sprj'}")
    return out


def cmd_to_shadered(milk3_path: Path, out: Optional[Path], as_zip: bool, lang: str) -> Path:
    milk = load_milk3(milk3_path)
    name = str(milk.get("name") or milk3_path.stem)
    safe = safe_name(name)

    if out is None:
        out = milk3_path.parent / f"{safe}_shadered"
        if as_zip:
            out = Path(str(out) + ".zip")

    common = pass_source(milk, "common")
    passes: List[Tuple[str, str]] = []
    for key in PASS_ORDER:
        src = pass_source(milk, key)
        if src.strip():
            passes.append((key, src))
    if not passes:
        die("no image/buffer* passes in milk3")

    # For multi-pass milk3, export each PS; for single image, match LOzB7CqQGt layout (Main)
    files: Dict[str, str] = {}
    if lang == "glsl":
        vs_name = "shaders/MainVS.vk"
        files[vs_name] = SIMPLE_VS_GLSL
        if len(passes) == 1:
            key, src = passes[0]
            ps_name = "shaders/MainPS.vk"
            files[ps_name] = wrap_milk3_as_glsl_ps(key, src if not common else common + "\n" + src)
            files["project.sprj"] = build_sprj_simple("Main", vs_name, ps_name, "glsl")
        else:
            # multi-pass: still use .vk names
            for key, src in passes:
                files[f"shaders/{key}.vk"] = wrap_milk3_as_glsl_ps(
                    key, (common + "\n" + src) if common else src
                )
            # simple sprj with last pass only as main for preview (full multipass RTs later)
            last = passes[-1][0]
            files["project.sprj"] = build_sprj_simple(
                last, vs_name, f"shaders/{last}.vk", "glsl"
            )
    else:
        files["shaders/MainVS.hlsl"] = SIMPLE_VS_HLSL
        if len(passes) == 1:
            key, src = passes[0]
            files["shaders/MainPS.hlsl"] = wrap_milk3_as_hlsl_ps(key, src, common)
            files["project.sprj"] = build_sprj_simple(
                "Main", "shaders/MainVS.hlsl", "shaders/MainPS.hlsl", "hlsl"
            )
        else:
            for key, src in passes:
                files[f"shaders/{key}.hlsl"] = wrap_milk3_as_hlsl_ps(key, src, common)
            last = passes[-1][0]
            files["project.sprj"] = build_sprj_simple(
                last, "shaders/MainVS.hlsl", f"shaders/{last}.hlsl", "hlsl"
            )

    # Always store raw milk3 for perfect round-trip
    meta = {
        "name": name,
        "version": milk.get("version", 1),
        "shadertoy": True,
        "channels": milk.get("channels", {}),
        "notes": milk.get("notes", {}),
        "source_milk3": milk3_path.name,
        "passes": [k for k, _ in passes],
        "export_lang": lang,
    }
    files["milk3_raw/meta.json"] = json.dumps(meta, indent=2)
    if common:
        files["milk3_raw/common.hlsl"] = common
    for key, src in passes:
        files[f"milk3_raw/{key}.hlsl"] = src

    files["README.txt"] = (
        f"SHADERed project from {milk3_path.name}\n"
        f"Open project.sprj in SHADERed.\n"
        f"Raw milk3 sources: milk3_raw/\n"
        f"Back to milk3:\n"
        f"  python tools/milk3_shadered.py to-milk3 <this> -o {safe}.milk3\n"
    )

    return write_tree(files, Path(out), as_zip or str(out).lower().endswith(".zip"))


# ---------------------------------------------------------------------------
# to-milk3
# ---------------------------------------------------------------------------

def cmd_to_milk3(src: Path, out: Optional[Path]) -> Path:
    tree = load_tree(src)

    # High-fidelity path
    meta_keys = [k for k in tree if k.replace("\\", "/").endswith("milk3_raw/meta.json")]
    if meta_keys:
        meta = json.loads(tree[meta_keys[0]].decode("utf-8"))
        prefix = str(Path(meta_keys[0]).parent).replace("\\", "/") + "/"
        milk: Dict[str, Any] = {
            "version": meta.get("version", 1),
            "shadertoy": True,
            "name": meta.get("name") or src.stem,
        }
        if meta.get("notes"):
            milk["notes"] = meta["notes"]
        if meta.get("channels"):
            milk["channels"] = meta["channels"]
        for key in PASS_KEYS:
            for cand in (f"milk3_raw/{key}.hlsl", f"{prefix}{key}.hlsl"):
                cand = cand.replace("\\", "/")
                if cand in tree or any(k.lower() == cand.lower() for k in tree):
                    milk[key] = tree_get_text(tree, cand)
                    break
        if out is None:
            out = src.parent / f"{milk['name']}.milk3"
        write_text(Path(out), json.dumps(milk, indent=2, ensure_ascii=False) + "\n")
        print(f"wrote {out} (from milk3_raw)")
        return Path(out)

    sprj_key = find_sprj_key(tree)
    sprj_xml = tree[sprj_key].decode("utf-8", errors="replace")
    passes = parse_sprj_passes(sprj_xml)

    milk = {
        "version": 1,
        "shadertoy": True,
        "name": (out.stem if out else src.stem),
        "channels": {},
    }
    channels: Dict[str, Any] = {}

    for idx, (pname, ps_path, _vs) in enumerate(passes):
        key = guess_pass_key(pname, ps_path, idx, len(passes))
        text = tree_get_text(tree, ps_path)

        rebuilt = extract_markers(text)
        if rebuilt is not None and "placeholder" not in text.lower()[:500]:
            # markers with real body
            milk[key] = rebuilt
        elif is_rust_spirv(text, ps_path):
            milk[key] = convert_rust_spirv_to_milk3(text, ps_path)
            print(f"note: converted rust-gpu pass '{pname}' via known-port handler")
        elif is_glsl(text):
            time_u, res_u = detect_uniforms(text)
            milk[key] = glsl_to_milk3_hlsl(text, time_u, res_u)
        else:
            # assume HLSL; try shader_body or wrap whole file
            if "shader_body" in text:
                milk[key] = text if text.strip().endswith("}") else text
                # if wrapper, extract
                ext = extract_markers(text)
                milk[key] = ext if ext else text
            else:
                milk[key] = join_shader_body(
                    "// Imported from SHADERed HLSL",
                    text,
                )

        if key.startswith("buffer"):
            channels[key] = {"iChannel0": "self"}
        elif key == "image":
            prev = next((b for b in ("bufferD", "bufferC", "bufferB", "bufferA") if b in milk), None)
            channels[key] = {"iChannel0": prev or "self"}

    if "image" not in milk:
        # promote last content key
        for k in list(milk.keys()):
            if k in PASS_ORDER:
                milk["image"] = milk.pop(k)
                break

    if channels:
        milk["channels"] = channels

    if out is None:
        out = src.parent / f"{safe_name(str(milk['name']))}.milk3"
    out = Path(out)
    write_text(out, json.dumps(milk, indent=2, ensure_ascii=False) + "\n")
    print(f"wrote {out}")
    return out


def cmd_info(path: Path) -> None:
    if path.suffix.lower() == ".milk3":
        m = load_milk3(path)
        print(f"milk3: {path}")
        print(f"  name: {m.get('name')}")
        print(f"  passes: {[k for k in PASS_KEYS if pass_source(m, k).strip()]}")
        print(f"  channels: {m.get('channels')}")
        return
    tree = load_tree(path)
    sprj = find_sprj_key(tree)
    print(f"shadered: {path}")
    print(f"  sprj: {sprj}")
    xml = tree[sprj].decode("utf-8", errors="replace")
    for name, ps, vs in parse_sprj_passes(xml):
        text = tree_get_text(tree, ps)
        if is_rust_spirv(text, ps):
            kind = "rust-gpu/spirv"
            st = extract_shadertoy_id(text)
            extra = f" shadertoy={st}" if st else ""
        elif is_glsl(text):
            kind = "glsl"
            extra = ""
        else:
            kind = "hlsl/other"
            extra = ""
        tu, ru = detect_uniforms(text)
        print(f"  pass {name}: ps={ps} vs={vs} ({kind}{extra}) uniforms time={tu} res={ru}")
    if any("milk3_raw" in k for k in tree):
        print("  milk3_raw: yes")


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="MDropDX12 milk3 <-> SHADERed project converter")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p1 = sub.add_parser("to-shadered", help="milk3 -> SHADERed project")
    p1.add_argument("milk3", type=Path)
    p1.add_argument("-o", "--output", type=Path, default=None)
    p1.add_argument("--zip", action="store_true")
    g = p1.add_mutually_exclusive_group()
    g.add_argument("--glsl", action="store_true", default=True, help="Export GLSL-style project (default, matches SHADERed ScreenQuadNDC)")
    g.add_argument("--hlsl", action="store_true", help="Export HLSL-style project")

    p2 = sub.add_parser("to-milk3", help="SHADERed project -> milk3")
    p2.add_argument("project", type=Path)
    p2.add_argument("-o", "--output", type=Path, default=None)

    p3 = sub.add_parser("info", help="Inspect milk3 or SHADERed project")
    p3.add_argument("path", type=Path)

    args = ap.parse_args(argv)

    if args.cmd == "to-shadered":
        if not args.milk3.is_file():
            die(f"not found: {args.milk3}")
        lang = "hlsl" if args.hlsl else "glsl"
        cmd_to_shadered(args.milk3, args.output, args.zip, lang)
    elif args.cmd == "to-milk3":
        if not args.project.exists():
            die(f"not found: {args.project}")
        cmd_to_milk3(args.project, args.output)
    elif args.cmd == "info":
        if not args.path.exists():
            die(f"not found: {args.path}")
        cmd_info(args.path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
