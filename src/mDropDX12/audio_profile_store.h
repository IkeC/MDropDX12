// audio_profile_store.h — named audio behaviour profiles.
//
// A profile says how raw audio becomes the numbers a preset sees. Three
// engines disagree about that, and a preset recovered from one of them wants
// its own engine's answer, so the answer is selectable per preset rather than
// global. See docs/superpowers/specs/2026-08-22-audio-profiles-design.md.
//
// The band group is NOT new maths. MDropDX12 already runs both analyses every
// frame: EngineShell::AnalyzeNewSound fills m_sound and is a faithful port of
// MilkDrop 3's stock code, while Engine::DoCustomSoundAnalysis fills mysound
// with the "pre-vms" analysis -- and only the second reaches presets, through
// var_pf_bass = mysound.imm_rel[0]. bandMode picks which one they are fed.
//
// bass/mid/treb are scale-invariant on both paths (imm_rel = imm / long_avg),
// so a gain field would do nothing to them. Only rates, band edges and the
// sum-vs-mean choice move those. The FFT texture is the opposite: shaders read
// its values directly, so its scale matters and its units are absolute.
#pragma once

#include "json_utils.h"

#include <string>
#include <vector>

namespace mdrop {

enum class BandMode   { Custom, MilkDrop };
enum class BandEdges  { Linear, Octave };
enum class BandEnergy { Sum, Mean };

struct AudioProfile {
  std::wstring name;
  std::wstring description;

  // True when the FFT group was fitted by measurement rather than transcribed
  // from a source or a decompile. Carried so a profile cannot quietly claim
  // more authority than it has.
  bool fftGroupIsFitted = false;

  // ── Band analysis (relative; gain cancels through imm_rel) ──
  BandMode   bandMode        = BandMode::Custom;
  BandEdges  bandEdges       = BandEdges::Linear;
  BandEnergy bandEnergy      = BandEnergy::Sum;
  float      bandNormalise[3] = { 1.0f, 1.0f, 1.0f };
  float      fpsRef          = 30.0f;
  float      avgAttack       = 0.2f;
  float      avgDecay        = 0.5f;
  float      longMix         = 0.992f;
  float      medMix          = 0.91f;
  // 1.0, matching both engines: DoCustomSoundAnalysis substitutes 1.0 when
  // long_avg is ~0, and so does MilkDrop. Kept as a field because it is a
  // real fork point that was once changed and reverted, not because the
  // two disagree today.
  float      silenceValue    = 1.0f;
  bool       inputDamp       = false;

  // ── FFT texture (absolute; shaders read these values directly) ──
  float      fftAttack       = 0.5f;
  float      fftDecay        = 0.5f;
  float      fftScale        = 0.00035f;
  float      fftNoiseGate    = 5e-5f;
  float      fftVisibleFloor = 2.5e-4f;
  bool       fftLowRolloff   = true;
  int        fftPeakHoldFrames = 30;
  float      fftPeakDecay    = 0.97f;

  // MD3 PRO does not write absolute magnitude into the FFT texture. It
  // PEAK-NORMALISES every frame (MilkDroprev.c:57120-57131), gates at a
  // fraction of that peak (:57132), then max-spreads each bin over its
  // neighbours (:57136-57152), so its texels always reach 1.0 no matter how
  // loud the music is. Presets recovered from MD3 -- the Equalizer family --
  // have their constants calibrated for that 0..1 texel, and at our absolute
  // scale their terms never fire: the ring measured 322/321/322/322 px across
  // four different pieces of music, i.e. not responding to audio at all.
  //
  // Off by default: this changes what every get_fft() shader sees, so it is
  // opted into by the MilkDrop 3 profile only. Scale alone does not substitute
  // -- fftScale 0.0028 (8x) was tried and the ring stayed pinned.
  bool       fftPeakNormalise = false;   // v /= max(eps, frame peak)
  float      fftRelGate       = 0.0f;    // drop v below this FRACTION of peak
  // Applied to the gated, normalised value BEFORE the spread. MD3 PRO uses
  // 2.0 -- recovered from the binary, not guessed: the decompile renders the
  // call as one-argument `FUN_0049d380(v)`, but the disassembly at 0x455051
  // shows `movsd xmm1,[0x4CF4B0]` loading the second argument, and the double
  // at that address is exactly 2.0. (The gate constant at 0x4CF278 reads 0.2,
  // matching the decompile, which is what validates the address mapping.)
  //
  // It cannot be folded into the shader's _fftParams exponent. MD3 squares
  // BEFORE the max-spread, and with neighbour weights w < 1,
  // max(v_j^2 * w_j) != (max(v_j * w_j))^2 -- squaring first changes which
  // neighbour wins the max.
  //
  // 1.0 is the identity and the default, so this is inert for every profile
  // that does not ask for it.
  float      fftPostGateExp   = 1.0f;    // v = pow(v, e) after the gate
  int        fftSpreadTaps    = 0;       // max-spread +/- N bins (0 = off)
  float      fftHzRef        = 22050.0f;
  bool       fftSqrt         = true;

  // ── Waveform ──
  float      pcmGain         = 1.0f;
};

class AudioProfileStore {
public:
  // Directory holding audioprofiles.json. Set once at startup, after the base
  // directory is known.
  void SetResourceDir(const wchar_t* dir);
  void GetStorePath(wchar_t* out, size_t len) const;

  // resources/profiles/audio/, one file per profile.
  std::wstring ProfileDir() const;

  // Every profile in that directory, in the { profiles: { name: {} } }
  // shape the readers already expect. Only the storage changed.
  JsonValue LoadAll() const;

  // Folds a legacy resources/audioprofiles.json into the directory,
  // once. Writes, reads back, counts, and only then removes the old
  // file. Safe when there is nothing to do.
  void MigrateSingleFile();

  void Names(std::vector<std::wstring>& out) const;
  bool Exists(const wchar_t* name) const;

  // Reads a profile INTO an existing AudioProfile and overwrites only what the
  // stored profile actually names. Seed `inout` with Defaults() first: a
  // profile written before a field existed must leave that field alone rather
  // than snap it to zero. Same contract as VFXProfileStore::Load, and the same
  // reason -- see its header.
  bool Load(const wchar_t* name, AudioProfile& inout) const;
  bool Save(const wchar_t* name, const AudioProfile& d);

  // Today's shipping behaviour, field for field. This is the struct's own
  // member initialisers, which is what makes "the default profile changes
  // nothing" checkable rather than merely asserted.
  static AudioProfile Defaults() { return AudioProfile(); }

  // Write any built-in profile the store is missing.
  //
  // The built-ins are defined in code, not shipped as a file: Release_x64 is
  // gitignored, so a file-only profile would exist on the machine that made it
  // and nowhere else. This follows the same self-bootstrapping rule as the
  // embedded shaders. A profile the user has edited is left alone -- only
  // absent ones are written.
  void EnsureBuiltIns();

  static AudioProfile BuiltInMDropDX12();
  static AudioProfile BuiltInMilkDrop3();
  static AudioProfile BuiltInMilkwave();

  // What to seed an AudioProfile with BEFORE Load() overlays the stored JSON:
  // the matching built-in when `name` is one, else Defaults().
  //
  // Load() only assigns fields the JSON actually carries, so seeding from
  // generic Defaults() means a stored profile written before a field existed
  // silently gets that field's GENERIC default rather than the built-in's
  // value. EnsureBuiltIns writes a built-in only when absent -- correctly, so
  // user edits survive -- so a stale file is never repaired and the gap is
  // permanent.
  //
  // That is what happened to MD3's FFT treatment: 30b26ff added
  // fftPeakNormalise / fftRelGate / fftSpreadTaps to BuiltInMilkDrop3(), the
  // already-present MilkDrop 3.json did not carry them, and every load reset
  // them to false/0/0 -- so the Equalizer presets had no audio drive from the
  // day the feature shipped.
  //
  // Seeding from the built-in makes the JSON an OVERLAY on code-defined
  // semantics, which is what a built-in profile should be, and means a field
  // added later works without asking anyone to delete their profile.
  static AudioProfile SeedFor(const wchar_t* name);

private:
  std::wstring m_resourceDir;
};

AudioProfileStore& AudioProfiles();

}  // namespace mdrop
