#!/usr/bin/env python3
"""
glslsandbox_to_milk3.py — Fetch a GLSL Sandbox effect and convert it to a .milk3 preset.

GLSL Sandbox effects are single-pass fragment shaders (void main / main(void)) using
uniforms time, resolution, mouse, optional backbuffer / surfacePosition.

The converter reuses tools/milk3_shadered.glsl_to_milk3_hlsl (best-effort GLSL->HLSL)
and writes a milk3 JSON preset loadable by MDropDX12.

Usage:
  python tools/glslsandbox_to_milk3.py https://glslsandbox.com/e#109677.0
  python tools/glslsandbox_to_milk3.py 109677.0
  python tools/glslsandbox_to_milk3.py e#109677.0 -o resources/presets/gs_109677.milk3
  python tools/glslsandbox_to_milk3.py 109677 --keep-glsl   # also embed original GLSL

API:
  GET https://glslsandbox.com/item/{id}.{version}
  -> { "code": "...", "user": "...", "parent": "..." }

Notes:
  - Version suffix is required by the site (.0 if omitted).
  - backbuffer -> sampler_feedback + channels.image.iChannel0 = "self"
  - mouse is remapped to 0..1 (Sandbox convention); milk3 stores pixels in the
    same y-up orientation, so the remap is a divide with no Y flip
  - Complex multi-pass / WebGL-only extensions may need hand fixes in Shader Import
"""

from __future__ import annotations

import argparse
import json
import re
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

# Allow `python tools/glslsandbox_to_milk3.py` from repo root
_TOOLS = Path(__file__).resolve().parent
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from milk3_shadered import glsl_to_milk3_hlsl  # noqa: E402

ROOT = _TOOLS.parent
DEFAULT_OUT_DIR = ROOT / "resources" / "presets"

UA = "MDropDX12-glslsandbox-to-milk3/1.0 (+https://github.com/)"
ITEM_URL = "https://glslsandbox.com/item/{ident}"


# ---------------------------------------------------------------------------
# URL / ID parsing
# ---------------------------------------------------------------------------

def parse_effect_id(spec: str) -> str:
    """
    Accept:
      https://glslsandbox.com/e#109677.0
      https://glslsandbox.com/e#109677
      http://glslsandbox.com/e#109677.0
      e#109677.0
      #109677.0
      109677.0
      109677
    Return canonical 'id.version' (version defaults to 0).
    """
    s = (spec or "").strip()
    if not s:
        raise ValueError("empty effect id / URL")

    # Full URL: take fragment after #, or /item/ path
    m = re.search(r"glslsandbox\.com/item/([0-9]+(?:\.[0-9]+)?)", s, re.I)
    if m:
        return _ensure_version(m.group(1))

    m = re.search(r"glslsandbox\.com/e#?([0-9]+(?:\.[0-9]+)?)", s, re.I)
    if m:
        return _ensure_version(m.group(1))

    # Hash-only fragment in URL we already missed
    if "#" in s:
        frag = s.rsplit("#", 1)[-1].strip()
        frag = re.sub(r"^e", "", frag, flags=re.I).lstrip("#")
        if re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", frag):
            return _ensure_version(frag)

    s = re.sub(r"^e#?", "", s, flags=re.I).lstrip("#").strip()
    if re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", s):
        return _ensure_version(s)

    raise ValueError(f"unrecognized GLSL Sandbox id/URL: {spec!r}")


def _ensure_version(ident: str) -> str:
    if "." not in ident:
        return f"{ident}.0"
    return ident


def effect_page_url(ident: str) -> str:
    return f"https://glslsandbox.com/e#{ident}"


# ---------------------------------------------------------------------------
# Fetch
# ---------------------------------------------------------------------------

def fetch_item(ident: str, timeout: float = 30.0) -> Dict[str, Any]:
    url = ITEM_URL.format(ident=ident)
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": UA,
            "Accept": "application/json, text/plain, */*",
            "Referer": effect_page_url(ident),
        },
    )
    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"HTTP {e.code} fetching {url}: {e.reason}") from e
    except urllib.error.URLError as e:
        raise RuntimeError(f"network error fetching {url}: {e.reason}") from e

    try:
        data = json.loads(raw.decode("utf-8", errors="replace"))
    except json.JSONDecodeError as e:
        raise RuntimeError(f"invalid JSON from {url}: {e}") from e

    if not isinstance(data, dict) or "code" not in data:
        raise RuntimeError(f"unexpected response from {url}: keys={list(data)!r}")
    if not data.get("code"):
        raise RuntimeError(f"empty code for {ident} (does this effect exist?)")
    return data


# ---------------------------------------------------------------------------
# Sandbox-specific GLSL cleanup before shared converter
# ---------------------------------------------------------------------------

def preprocess_sandbox_glsl(code: str) -> Tuple[str, Dict[str, bool]]:
    """Normalize GLSL Sandbox source for glsl_to_milk3_hlsl.

    Returns (glsl, flags) where flags notes features *used* in the shader body.
    """
    flags = {
        "backbuffer": False,
        "surfacePosition": False,
        "surfaceSize": False,
        "mouse": False,
    }
    s = code.replace("\r\n", "\n").replace("\r", "\n")

    # #extension lines
    s = re.sub(r"^\s*#extension\s+[^\n]*\n", "", s, flags=re.M)

    # #ifdef GL_ES ... precision ... #endif  (or bare precision)
    s = re.sub(
        r"#ifdef\s+GL_ES\s*\n(?:[^\n]*\n)*?#endif\s*\n?",
        "",
        s,
        flags=re.M,
    )
    s = re.sub(r"^\s*precision\s+\w+\s+float\s*;\s*\n", "", s, flags=re.M)

    # varying / in surfacePosition (attribute-like)
    s = re.sub(r"^\s*(?:varying|in)\s+vec2\s+surfacePosition\s*;\s*\n", "", s, flags=re.M)
    s = re.sub(r"^\s*(?:varying|in)\s+[^;]+;\s*\n", "", s, flags=re.M)

    # Uniforms — allow trailing // comments on the same line
    s = re.sub(
        r"^\s*uniform\s+sampler2D\s+backbuffer\s*;\s*(?://[^\n]*)?\n",
        "",
        s,
        flags=re.M,
    )
    s = re.sub(r"^\s*uniform\s+[^;]+;\s*(?://[^\n]*)?\n", "", s, flags=re.M)

    # Common Shadertoy-emulation aliases used on Sandbox ports.
    # Strip the #defines so renames map iTime/iResolution/iMouse cleanly;
    # leaving them after resolution->texsize yields invalid `#define texsize.xy ...`.
    s = re.sub(
        r"^\s*#define\s+iTime\s+time\s*(?://[^\n]*)?\n",
        "",
        s,
        flags=re.M,
    )
    s = re.sub(
        r"^\s*#define\s+iResolution\s+resolution\s*(?://[^\n]*)?\n",
        "",
        s,
        flags=re.M,
    )
    s = re.sub(
        r"^\s*#define\s+iMouse\s+[^\n]+\n",
        "",
        s,
        flags=re.M,
    )
    # Drop empty "shadertoy emulation" comment blocks noise (optional)
    s = re.sub(r"^\s*//\s*shadertoy emulation\s*\n", "", s, flags=re.M | re.I)
    s = re.sub(r"^\s*//\s*glslsandbox uniforms\s*\n", "", s, flags=re.M | re.I)

    flags["backbuffer"] = bool(re.search(r"\bbackbuffer\b", s))
    flags["surfacePosition"] = bool(re.search(r"\bsurfacePosition\b", s))
    flags["surfaceSize"] = bool(re.search(r"\bsurfaceSize\b", s))
    flags["mouse"] = bool(re.search(r"\b(?:mouse|iMouse)\b", s))

    if flags["backbuffer"]:
        s = re.sub(r"\bbackbuffer\b", "sampler_feedback", s)

    # lastFrame is Sandbox feedback (previous front buffer)
    if re.search(r"\blastFrame\b", s):
        flags["backbuffer"] = True
        s = re.sub(r"\blastFrame\b", "sampler_feedback", s)

    # --- Self-referential / redefining macros (common on complex Sandbox ports) ---
    # Milk3 has `#define time _c2.x` only — no uniform named `time`.  When a shader
    # does `#define time rtime` with `#define now (...time...)`, the residual
    # unexpanded `time` is undeclared -> black screen (X3004).
    # Capture the clock as `_sb_clock` before any `#define time ...`.
    if re.search(r"^\s*#define\s+time\s+(?!time\b)", s, flags=re.M):
        s = (
            "// milk3 clock capture (user redefines #define time ...)\n"
            "#define _sb_clock _c2.x\n"
            + s
        )
        # In #define bodies BEFORE `#define time <new>`, replace time -> _sb_clock
        lines = s.split("\n")
        out_lines: list = []
        seen_time_redef = False
        for ln in lines:
            if re.match(r"^\s*#define\s+time\s+(?!time\b)", ln):
                seen_time_redef = True
            if (
                not seen_time_redef
                and re.match(r"^\s*#define\s+", ln)
                and not re.match(r"^\s*#define\s+time\b", ln)
                and not re.match(r"^\s*#define\s+_sb_clock\b", ln)
            ):
                m = re.match(r"^(\s*#define\s+\S+\s+)(.*)$", ln)
                if m:
                    ln = m.group(1) + re.sub(r"\btime\b", "_sb_clock", m.group(2))
            out_lines.append(ln)
        s = "\n".join(out_lines)

    # `#define surfacePosition <expr involving surfacePosition>` — the RHS refers
    # to the varying; rename varying to _sb_surfPos so the macro is well-founded.
    if re.search(
        r"^\s*#define\s+surfacePosition\b.*\bsurfacePosition\b",
        s,
        flags=re.M,
    ):
        flags["surfacePosition"] = True
        # First rename varying uses in define RHS: only the embedded identifier
        def _fix_sp_define(m: re.Match) -> str:
            body = m.group(1)
            # Replace surfacePosition in RHS with _sb_surfPos (not the define name)
            body2 = re.sub(r"\bsurfacePosition\b", "_sb_surfPos", body)
            return f"#define surfacePosition {body2}"

        s = re.sub(
            r"^\s*#define\s+surfacePosition\s+(.+)$",
            _fix_sp_define,
            s,
            flags=re.M,
        )
        # varying already stripped; inject will declare _sb_surfPos

    return s, flags


# OpenGL / GLSL Sandbox: y=0 at bottom — and so is the milk3 Image pass.
#
# This file used to bake a y-flip into every gl_FragCoord substitute, because
# when it was written (2026-08-07) the Image quad carried DX UVs (uv.y=0 at
# top). a3a65f41 (2026-08-17) flipped that quad so fragCoord matches Shadertoy
# (uv.y=0 at bottom), and the two flips cancelled into an upside-down picture on
# every sandbox import (Shane, 2026-08-23: glslsandbox_109644.0 rendered its sky
# below the terrain). `uv` is now already the Sandbox convention: do not flip it
# here, and do not flip `mouse` either — the engine hands milk3 shaders an
# iMouse that is already y-up (engine.cpp: targetH-1 - sy).
_SB_FRAG = "(uv * texsize.xy)"
# Normalized Sandbox-style UV (0,0 bottom-left):
_SB_UV = "uv"


def sandbox_to_hlsl(code: str) -> Tuple[str, Dict[str, bool]]:
    cleaned, flags = preprocess_sandbox_glsl(code)

    # Sandbox uniforms: time, resolution, mouse (not iTime / iResolution)
    hlsl = glsl_to_milk3_hlsl(
        cleaned,
        time_uniform="time",
        res_uniform="resolution",
    )

    # Drop any remaining broken/no-op #defines after rename (e.g. #define time time)
    hlsl = re.sub(
        r"^\s*#define\s+(\w+)\s+\1\s*(?://[^\n]*)?\n",
        "",
        hlsl,
        flags=re.M,
    )
    # Never rewrite #define lines (would create `#define texsize.xy texsize.xy`)
    def _texsize_xy_pass(src: str) -> str:
        out_lines = []
        for ln in src.split("\n"):
            if ln.lstrip().startswith("#"):
                out_lines.append(ln)
                continue
            ln2 = re.sub(r"\btexsize\b(?!\s*\.)", "texsize.xy", ln)
            out_lines.append(ln2)
        return "\n".join(out_lines)

    hlsl = _texsize_xy_pass(hlsl)
    # Fix accidental double: texsize.xy.xy / texsize.xy.x from prior .xy + rename
    hlsl = (
        hlsl.replace("texsize.xy.xy", "texsize.xy")
        .replace("texsize.xy.x", "texsize.x")
        .replace("texsize.xy.y", "texsize.y")
        .replace("texsize.xy.z", "texsize.z")
        .replace("texsize.xy.w", "texsize.w")
        .replace("texsize.xy.zw", "texsize.zw")
    )
    # Kill any remaining invalid preprocessor identifiers with a dot
    hlsl = re.sub(r"^\s*#define\s+\w+\.\w+[^\n]*\n", "", hlsl, flags=re.M)

    # gl_FragCoord substitutes need no Y fix-up: the converter emits
    # (uv * texsize.xy) and the Image pass quad already carries Sandbox/Shadertoy
    # UVs (y=0 at bottom) — see _SB_FRAG above. Flipping here inverts the image.

    # surfacePosition / surfaceSize are GLSL uniforms/varyings — visible to all
    # functions. Helpers in the preamble often read them; locals only in
    # shader_body leave those helpers with X3004 undeclared identifier.
    # Declare static globals + assign in shader_body (per-pixel).
    pre_part, body_part = _split_shader_body(hlsl)
    if body_part is None:
        pre_part, body_part = hlsl, ""

    has_sp_macro = bool(re.search(r"#define\s+surfacePosition\b", hlsl))
    needs_surf = flags["surfacePosition"] and (
        "surfacePosition" in hlsl or "_sb_surfPos" in hlsl
    )
    needs_size = flags["surfaceSize"] and "surfaceSize" in hlsl
    # Also if helpers reference them even without flags (defensive)
    if "surfaceSize" in pre_part:
        needs_size = True
    if "surfacePosition" in pre_part or "_sb_surfPos" in pre_part:
        needs_surf = True

    global_decls: list = []
    body_assign: list = []

    if needs_size:
        global_decls.append(
            "// GLSL Sandbox surfaceSize (uniform) — set each pixel in shader_body\n"
            "static float2 surfaceSize;\n"
        )
        body_assign.append(
            "  // default surfaceSize: height=1, width=aspect\n"
            "  surfaceSize = float2(texsize.x / max(texsize.y, 1.0), 1.0);\n"
        )

    if needs_surf:
        global_decls.append(
            "// GLSL Sandbox surfacePosition (varying) — set each pixel in shader_body\n"
            "static float2 _sb_surfPos;\n"
        )
        if not has_sp_macro:
            global_decls.append("static float2 surfacePosition;\n")
        body_assign.append(
            "  // default surface (no pan/zoom): x ±aspect/2, y ±0.5, y+ up\n"
            "  // (uv.y is already y-up in the Image pass — do not invert it)\n"
            "  _sb_surfPos = float2(\n"
            "    (uv.x - 0.5) * (texsize.x / max(texsize.y, 1.0)),\n"
            "    (uv.y - 0.5));\n"
        )
        if not has_sp_macro:
            body_assign.append("  surfacePosition = _sb_surfPos;\n")

    if global_decls:
        # MUST be at file top — helpers above shader_body call surfaceSize/etc.
        # (HLSL requires globals declared before first use.)
        hlsl = "".join(global_decls) + "\n" + hlsl

    if body_assign:
        hlsl = _inject_after_shader_body_open(hlsl, "".join(body_assign))

    # mouse:
    # - Sandbox `mouse` is 0..1 (origin bottom-left on web).
    # - milk3 `mouse` is pixels, ALSO origin bottom-left: the engine flips the
    #   client Y when it fills iMouse (engine.cpp: targetH-1 - sy). So the only
    #   conversion needed is a divide by texsize -- no Y flip. This file used to
    #   flip, which inverted vertical steering on every sandbox import.
    # - Ports often do `#define iMouse mouse*resolution` then `iMouse/iResolution`
    #   which is just normalized mouse — after rename that becomes mouse/texsize.
    #   milk3 pixel mouse / texsize is already correct; do NOT also map to 0..1.
    if flags["mouse"] and re.search(r"\bmouse\b", hlsl):
        pre, body = _split_shader_body(hlsl)
        if body is not None:
            # Bare sandbox `mouse` uses that treat it as 0..1 (not already
            # divided by resolution) just need the divide.
            body2 = body
            uses_norm_div = bool(
                re.search(
                    r"\bmouse(?:\.xy)?\s*/\s*texsize(?:\.xy)?",
                    body2,
                )
            )
            if uses_norm_div:
                # milk3 pixel mouse / texsize already IS sandbox mouse01, same
                # orientation — rewrite to the explicit reciprocal form, no Y-flip
                body2 = re.sub(
                    r"\bmouse\.xy\s*/\s*texsize\.xy",
                    "float2(mouse.x * texsize.z, mouse.y * texsize.w)",
                    body2,
                )
                body2 = re.sub(
                    r"\bmouse\s*/\s*texsize\.xy",
                    "float2(mouse.x * texsize.z, mouse.y * texsize.w)",
                    body2,
                )
                body2 = re.sub(
                    r"\bmouse\s*/\s*texsize\b",
                    "float2(mouse.x * texsize.z, mouse.y * texsize.w)",
                    body2,
                )
            else:
                body2 = re.sub(r"\bmouse\b", "_sb_mouse", body2)
                inject = (
                    "  // GLSL Sandbox mouse is 0..1, y=0 at bottom; milk3 mouse is the\n"
                    "  // same orientation in pixels — divide, do not flip\n"
                    "  float2 _sb_mouse = float2(mouse.x * texsize.z, mouse.y * texsize.w);\n"
                )
                if "// GLSL Sandbox mouse" not in body2:
                    body2 = inject + body2.lstrip("\n")
            hlsl = _join_shader_body(pre, body2)

    # Init common uninitialized vec accumulators (GLSL zero-fills; HLSL does not)
    hlsl = re.sub(r"\bfloat3\s+c\s*;", "float3 c = float3(0,0,0);", hlsl)

    return hlsl, flags


def _split_shader_body(src: str) -> Tuple[str, Optional[str]]:
    m = re.search(r"shader_body\s*\{", src)
    if not m:
        return src, None
    start = m.end()
    depth, i = 1, start
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    if depth:
        return src, None
    return src[: m.start()], src[start : i - 1]


def _join_shader_body(preamble: str, body: str) -> str:
    parts = []
    if preamble.strip():
        parts.append(preamble.rstrip())
        parts.append("")
    parts.append("shader_body {")
    parts.append(body.rstrip() if body.strip() else "  ret = float4(0,0,0,1);")
    parts.append("}")
    parts.append("")
    return "\n".join(parts)


def _inject_after_shader_body_open(hlsl: str, inject: str) -> str:
    pre, body = _split_shader_body(hlsl)
    if body is None:
        return hlsl
    if inject.strip() in body:
        return hlsl
    return _join_shader_body(pre, inject + body.lstrip("\n"))


# ---------------------------------------------------------------------------
# milk3 document
# ---------------------------------------------------------------------------

def build_milk3(
    *,
    ident: str,
    hlsl: str,
    flags: Dict[str, bool],
    meta: Dict[str, Any],
    original_glsl: Optional[str] = None,
    name: Optional[str] = None,
) -> Dict[str, Any]:
    title = name or f"glslsandbox_{ident.replace('.', '_')}"
    channels: Dict[str, Any] = {
        "image": {
            "iChannel0": "self" if flags.get("backbuffer") else "self",
        }
    }
    # Always allow self feedback — harmless if unused; needed for backbuffer
    if flags.get("backbuffer"):
        channels["image"]["iChannel0"] = "self"

    doc: Dict[str, Any] = {
        "version": 1,
        "shadertoy": True,
        "name": title,
        "source": {
            "site": "glslsandbox",
            "id": ident,
            "url": effect_page_url(ident),
            "user": meta.get("user") or "",
            "parent": meta.get("parent") or "",
        },
        "channels": channels,
        "notes": {
            "image": (
                f"Imported from {effect_page_url(ident)}. "
                "Best-effort GLSL->HLSL; complex effects may need Shader Import tweaks."
            ),
        },
        "image": hlsl,
    }
    if original_glsl is not None:
        doc["glsl"] = {"image": original_glsl}
    return doc


def default_out_path(ident: str, out_dir: Path) -> Path:
    safe = re.sub(r"[^\w.\-]+", "_", ident)
    return out_dir / f"glslsandbox_{safe}.milk3"


def resolve_output_path(output: Optional[Path], ident: str) -> Path:
    """Resolve -o to a .milk3 file path.

    Accepts:
      - None -> resources/presets/glslsandbox_<id>.milk3
      - existing directory, or path ending with / or \\ -> <dir>/glslsandbox_<id>.milk3
      - file path (optionally without .milk3 suffix)
    """
    if output is None:
        DEFAULT_OUT_DIR.mkdir(parents=True, exist_ok=True)
        return default_out_path(ident, DEFAULT_OUT_DIR)

    # Preserve trailing-slash intent before Path normalizes it away on some platforms
    raw = str(output)
    looks_like_dir = raw.endswith(("/", "\\")) or (output.exists() and output.is_dir())

    if looks_like_dir:
        out_dir = Path(raw.rstrip("/\\")) if raw.endswith(("/", "\\")) else output
        out_dir.mkdir(parents=True, exist_ok=True)
        return default_out_path(ident, out_dir)

    out = output
    if out.suffix.lower() != ".milk3":
        out = Path(str(out) + ".milk3")
    out.parent.mkdir(parents=True, exist_ok=True)
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[list] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Fetch a GLSL Sandbox effect and convert it to a .milk3 preset.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument(
        "effect",
        help="URL or id, e.g. https://glslsandbox.com/e#109677.0 or 109677.0",
    )
    ap.add_argument(
        "-o",
        "--output",
        type=Path,
        help=(
            "Output .milk3 file, or a directory (filename glslsandbox_<id>.milk3 is used). "
            "Default: resources/presets/glslsandbox_<id>.milk3"
        ),
    )
    ap.add_argument(
        "--name",
        help="Preset name field (default: glslsandbox_<id>)",
    )
    ap.add_argument(
        "--keep-glsl",
        action="store_true",
        help="Embed original GLSL under doc['glsl']['image']",
    )
    ap.add_argument(
        "--print",
        dest="do_print",
        action="store_true",
        help="Print converted HLSL to stdout instead of only writing the file",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="HTTP timeout seconds (default 30)",
    )
    args = ap.parse_args(argv)

    try:
        ident = parse_effect_id(args.effect)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print(f"fetching {ITEM_URL.format(ident=ident)} ...")
    try:
        meta = fetch_item(ident, timeout=args.timeout)
    except RuntimeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    glsl = meta["code"]
    print(f"  user={meta.get('user')!r} parent={meta.get('parent')!r} code={len(glsl)} chars")

    try:
        hlsl, flags = sandbox_to_hlsl(glsl)
    except SystemExit:
        raise
    except Exception as e:
        print(f"error: conversion failed: {e}", file=sys.stderr)
        return 1

    used = [k for k, v in flags.items() if v]
    print(f"  features: {', '.join(used) if used else '(basic)'}")

    out = resolve_output_path(args.output, ident)

    doc = build_milk3(
        ident=ident,
        hlsl=hlsl,
        flags=flags,
        meta=meta,
        original_glsl=glsl if args.keep_glsl else None,
        name=args.name,
    )
    text = json.dumps(doc, indent=2, ensure_ascii=False) + "\n"
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out} ({len(text)} bytes)")

    if args.do_print:
        print("\n----- image HLSL -----\n")
        print(hlsl)

    print("done. Load the .milk3 in MDropDX12 (preset browser or drag-and-drop).")
    print("If it fails to compile, open Shader Import and tweak the HLSL.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
