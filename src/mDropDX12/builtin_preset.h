// builtin_preset.h — the preset shown when there is nothing else to show.
//
// A fresh, portable-zip install has no preset library until the user points
// one at MilkAssets (or wherever theirs lives). Before this, that state
// rendered a plain black window forever, with no on-screen explanation --
// indistinguishable from a crash. This is what fills that gap: an embedded,
// audio-independent classic .milk preset applied straight from memory
// (Engine::ApplyPresetTextToState, the same mechanism the Preset Editor's
// live-Apply already uses), so the very first frame is already moving.
//
// Derived from the user's own most-played preset library (presets.json usage
// stats), not invented from scratch: "Zylot - Spiral (Hypnotic)" is the
// #10-most-played preset by use count and genuinely a spiral, which is also
// what was asked for. Reduced from its original ~685 lines to the handful of
// fields that actually matter -- CState::Import() calls Default() first, so
// everything unmentioned here already has a sane value -- and left with the
// double-armed spiral wave and rainbow colouring unchanged, since those are
// simple enough to read as-is and are the reason the original holds up over
// long play. It does not depend on audio: `sample` and `time` alone drive the
// whole shape, so it looks the same whether or not anything is playing.

namespace mdrop {

inline const wchar_t* const kBuiltinPresetDesc =
    L"Welcome to MDropDX12 -- add your own presets in Settings > Files";

// Kept close to plain, readable MilkDrop preset syntax on purpose: this also
// doubles as a first example for anyone opening the Preset Editor with
// nothing loaded yet (engine_preset_editor_ui.cpp's "Load Sample").
inline const wchar_t* const kBuiltinPresetMilk =
LR"MILK([preset00]
fRating=5.000
fDecay=0.920
nWaveMode=0
bAdditiveWaves=0
bWaveThick=1
fWaveAlpha=0.001

per_frame_1=warp = 0;
per_frame_2=decay = .92;

wavecode_0_enabled=1
wavecode_0_samples=512
wavecode_0_bDrawThick=1
wavecode_0_scaling=3.00000
wavecode_0_smoothing=0.50000
wavecode_0_r=1.000
wavecode_0_g=1.000
wavecode_0_b=1.000
wavecode_0_a=1.000
wave_0_per_point1=// A double-armed spiral: radius grows with `sample` (0..1
wave_0_per_point2=// across every point on the wave), angle spins with both
wave_0_per_point3=// position along the arm and time -- so the whole spiral
wave_0_per_point4=// turns as it draws.
wave_0_per_point5=x = .5 + .25*(sample*2)*sin(sample*100 + time*10);
wave_0_per_point6=y = .5 + .25*(sample*2)*cos(sample*100 + time*10);
wave_0_per_point7=// Cycle red/green/blue through a rainbow around the arc,
wave_0_per_point8=// each channel a sine offset from the others by 1/3 turn.
wave_0_per_point9=n2 = abs((sample*6.283) - 3.1415);
wave_0_per_point10=t = time*5;
wave_0_per_point11=r = sin(n2 + t)*0.5 + 0.5;
wave_0_per_point12=g = sin(n2 + 2.1 + t)*0.5 + 0.5;
wave_0_per_point13=b = sin(n2 + 4.2 + t)*0.5 + 0.5;
)MILK";

}  // namespace mdrop
