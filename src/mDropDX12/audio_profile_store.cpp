// audio_profile_store.cpp — read/write audioprofiles.json.
//
// One file, keyed by profile name, mirroring vfxprofiles.json. Load is an
// in/out merge rather than a fetch, for the same reason VFXProfileStore::Load
// is: a profile written before a field existed must leave that field where the
// caller had it instead of snapping it to zero.

#include "audio_profile_store.h"

#include "profile_paths.h"

#include <Windows.h>
#include <stdio.h>   // _snwprintf_s

#include "json_utils.h"
#include "utility.h"

namespace mdrop {

static const wchar_t* kStoreName = L"audioprofiles.json";
static const wchar_t* kProfiles = L"profiles";

AudioProfileStore& AudioProfiles() {
  static AudioProfileStore s;
  return s;
}

void AudioProfileStore::SetResourceDir(const wchar_t* dir) {
  m_resourceDir = dir ? dir : L"";
}

void AudioProfileStore::GetStorePath(wchar_t* out, size_t len) const {
  // The DIRECTORY now, not a file: profiles live one per file under
  // resources/profiles/audio/, and callers show this to say where.
  //
  // Not FormatTo (issue 147): the destination is a pointer and a count, so
  // there is no array to deduce from. _TRUNCATE gives the same contract --
  // truncate, null-terminate, never invoke the invalid-parameter handler --
  // which is the whole point of moving off the fatal _s formatters.
  _snwprintf_s(out, len, _TRUNCATE, L"%ls", ProfileDir().c_str());
}

std::wstring AudioProfileStore::ProfileDir() const {
  return profiles::DirForRes(m_resourceDir, profiles::Kind::Audio);
}

namespace {

// The leaf currently holding a profile of this name, or empty if none does.
std::wstring AudioLeafForName(const std::wstring& dir, const wchar_t* name) {
  for (const std::wstring& leaf : profiles::ListDir(dir)) {
    const JsonValue one = JsonLoadFile((dir + leaf).c_str());
    if (!one.isObject()) continue;
    std::wstring have = one[L"name"].asString();
    if (have.empty()) {
      have = leaf;
      if (have.size() > 5) have.resize(have.size() - 5);   // drop .json
    }
    if (_wcsicmp(have.c_str(), name) == 0) return leaf;
  }
  return std::wstring();
}

}  // namespace

// Every profile in the directory, assembled into the { profiles: { name: {} } }
// shape the rest of this file already reads. Only the storage changed.
JsonValue AudioProfileStore::LoadAll() const {
  JsonValue root;
  root.type = JsonValue::Object;
  JsonValue profs;
  profs.type = JsonValue::Object;

  const std::wstring dir = ProfileDir();
  for (const std::wstring& leaf : profiles::ListDir(dir)) {
    JsonValue one = JsonLoadFile((dir + leaf).c_str());
    if (!one.isObject()) continue;
    std::wstring name = one[L"name"].asString();
    if (name.empty()) {
      name = leaf;
      if (name.size() > 5) name.resize(name.size() - 5);
    }
    profs.members.push_back({ name, one });
  }
  root.members.push_back({ kProfiles, profs });
  return root;
}

void AudioProfileStore::MigrateSingleFile() {
  const std::wstring legacy =
      profiles::LegacyPathRes(m_resourceDir, profiles::Kind::Audio);
  if (legacy.empty()) return;
  if (GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) return;

  const JsonValue root = JsonLoadFile(legacy.c_str());
  const JsonValue& profs = root[kProfiles];
  if (!profs.isObject()) {
    DLOG_WARN("Audio profiles: %ls is unreadable; leaving it alone", legacy.c_str());
    return;
  }

  const std::wstring dir = ProfileDir();
  if (!profiles::EnsureDirPath(dir)) {
    DLOG_ERROR("Audio profiles: could not create %ls", dir.c_str());
    return;
  }

  // Write every profile out first. The old file is removed only after the new
  // layout is read back and counted, so a crash in between costs a duplicate
  // rather than the data -- which has been lost twice here before.
  size_t wrote = 0;
  for (const auto& [key, val] : profs.members) {
    std::wstring leaf = AudioLeafForName(dir, key.c_str());
    if (leaf.empty())
      leaf = profiles::WithJson(profiles::UniqueLeafIn(dir, key));
    JsonValue one = val;
    JsonWriter w;
    w.BeginObject();
    w.String(L"name", key);
    for (const auto& m : one.members) {
      if (_wcsicmp(m.first.c_str(), L"name") == 0) continue;
      w.Value(m.first.c_str(), m.second);
    }
    w.EndObject();
    if (w.SaveToFile((dir + leaf).c_str())) wrote++;
    else DLOG_ERROR("Audio profiles: could not write %ls", (dir + leaf).c_str());
  }

  const size_t before = profs.members.size();
  const size_t after = LoadAll()[kProfiles].members.size();
  if (wrote < before || after < before) {
    DLOG_ERROR("Audio profiles: migration produced %zu of %zu; keeping %ls",
               after, before, legacy.c_str());
    return;
  }
  if (DeleteFileW(legacy.c_str()))
    DLOG_INFO("Audio profiles: migrated %zu profile(s) to %ls", after, dir.c_str());
  else
    DLOG_WARN("Audio profiles: migrated %zu but could not remove %ls",
              after, legacy.c_str());
}

// ─── enum <-> string ────────────────────────────────────────────────────
//
// An unrecognised spelling leaves the seeded value alone. Mapping it to the
// first enumerator instead would turn a typo in a hand-edited profile into a
// silent behaviour change, which is exactly the class of bug this file's
// leave-alone contract exists to avoid.

static void ReadBandMode(const JsonValue& v, BandMode& out) {
  const std::wstring s = v.asString();
  if (s == L"custom") out = BandMode::Custom;
  else if (s == L"milkdrop") out = BandMode::MilkDrop;
}

static void ReadBandEdges(const JsonValue& v, BandEdges& out) {
  const std::wstring s = v.asString();
  if (s == L"linear") out = BandEdges::Linear;
  else if (s == L"octave") out = BandEdges::Octave;
}

static void ReadBandEnergy(const JsonValue& v, BandEnergy& out) {
  const std::wstring s = v.asString();
  if (s == L"sum") out = BandEnergy::Sum;
  else if (s == L"mean") out = BandEnergy::Mean;
}

static const wchar_t* BandModeName(BandMode m) {
  return m == BandMode::MilkDrop ? L"milkdrop" : L"custom";
}
static const wchar_t* BandEdgesName(BandEdges m) {
  return m == BandEdges::Octave ? L"octave" : L"linear";
}
static const wchar_t* BandEnergyName(BandEnergy m) {
  return m == BandEnergy::Mean ? L"mean" : L"sum";
}

// ─── read ───────────────────────────────────────────────────────────────

static void ReadProfile(const JsonValue& p, AudioProfile& io) {
  if (p.has(L"description")) io.description = p[L"description"].asString();
  if (p.has(L"_fftGroupIsFitted"))
    io.fftGroupIsFitted = p[L"_fftGroupIsFitted"].asBool(io.fftGroupIsFitted);

  if (p.has(L"bandMode"))   ReadBandMode(p[L"bandMode"], io.bandMode);
  if (p.has(L"bandEdges"))  ReadBandEdges(p[L"bandEdges"], io.bandEdges);
  if (p.has(L"bandEnergy")) ReadBandEnergy(p[L"bandEnergy"], io.bandEnergy);

  if (p.has(L"bandNormalise")) {
    const JsonValue& a = p[L"bandNormalise"];
    // A short array fills what it has: a hand-written profile naming only the
    // bass divisor should not zero the other two.
    for (size_t i = 0; i < a.size() && i < 3; i++)
      io.bandNormalise[i] = a.at(i).asFloat(io.bandNormalise[i]);
  }

  if (p.has(L"fpsRef"))       io.fpsRef = p[L"fpsRef"].asFloat(io.fpsRef);
  if (p.has(L"avgAttack"))    io.avgAttack = p[L"avgAttack"].asFloat(io.avgAttack);
  if (p.has(L"avgDecay"))     io.avgDecay = p[L"avgDecay"].asFloat(io.avgDecay);
  if (p.has(L"longMix"))      io.longMix = p[L"longMix"].asFloat(io.longMix);
  if (p.has(L"medMix"))       io.medMix = p[L"medMix"].asFloat(io.medMix);
  if (p.has(L"silenceValue")) io.silenceValue = p[L"silenceValue"].asFloat(io.silenceValue);
  if (p.has(L"inputDamp"))    io.inputDamp = p[L"inputDamp"].asBool(io.inputDamp);

  if (p.has(L"fftAttack"))       io.fftAttack = p[L"fftAttack"].asFloat(io.fftAttack);
  if (p.has(L"fftDecay"))        io.fftDecay = p[L"fftDecay"].asFloat(io.fftDecay);
  if (p.has(L"fftScale"))        io.fftScale = p[L"fftScale"].asFloat(io.fftScale);
  if (p.has(L"fftNoiseGate"))    io.fftNoiseGate = p[L"fftNoiseGate"].asFloat(io.fftNoiseGate);
  if (p.has(L"fftVisibleFloor")) io.fftVisibleFloor = p[L"fftVisibleFloor"].asFloat(io.fftVisibleFloor);
  if (p.has(L"fftLowRolloff"))   io.fftLowRolloff = p[L"fftLowRolloff"].asBool(io.fftLowRolloff);
  if (p.has(L"fftPeakHoldFrames")) io.fftPeakHoldFrames = p[L"fftPeakHoldFrames"].asInt(io.fftPeakHoldFrames);
  if (p.has(L"fftPeakDecay"))    io.fftPeakDecay = p[L"fftPeakDecay"].asFloat(io.fftPeakDecay);
  if (p.has(L"fftHzRef"))        io.fftHzRef = p[L"fftHzRef"].asFloat(io.fftHzRef);
  if (p.has(L"fftSqrt"))         io.fftSqrt = p[L"fftSqrt"].asBool(io.fftSqrt);

  // MD3's FFT texture treatment. These three shipped in 30b26ff set ONLY by
  // BuiltInMilkDrop3(), with no reader and no writer -- and since
  // ApplyPendingAudioProfile always reloads through Defaults() + Load(), they
  // snapped back to false/0/0 on every profile switch. The MilkDrop 3 audio path
  // was therefore unreachable for any profile that came from the store, which is
  // every profile, from the day it shipped.
  if (p.has(L"fftPeakNormalise")) io.fftPeakNormalise = p[L"fftPeakNormalise"].asBool(io.fftPeakNormalise);
  if (p.has(L"fftRelGate"))       io.fftRelGate = p[L"fftRelGate"].asFloat(io.fftRelGate);
  if (p.has(L"fftPostGateExp")) io.fftPostGateExp = p[L"fftPostGateExp"].asFloat(io.fftPostGateExp);
  if (p.has(L"fftSpreadTaps"))    io.fftSpreadTaps = p[L"fftSpreadTaps"].asInt(io.fftSpreadTaps);

  if (p.has(L"pcmGain")) io.pcmGain = p[L"pcmGain"].asFloat(io.pcmGain);
}

bool AudioProfileStore::Load(const wchar_t* name, AudioProfile& inout) const {
  if (!name || !name[0]) return false;
  const JsonValue root = LoadAll();
  if (root.isNull()) return false;
  const JsonValue& profiles = root[kProfiles];
  if (!profiles.isObject() || !profiles.has(name)) return false;
  inout.name = name;
  ReadProfile(profiles[name], inout);
  return true;
}

// ─── write ──────────────────────────────────────────────────────────────

// The profile's fields, with no object around them. Split out so the same
// writer serves both shapes: a per-file profile puts these at the top level
// beside "name", and the legacy single-file store nested them under the name.
static void WriteProfileBody(JsonWriter& w, const AudioProfile& d) {
  w.String(L"description", d.description);
  w.Bool(L"_fftGroupIsFitted", d.fftGroupIsFitted);

  w.String(L"bandMode", BandModeName(d.bandMode));
  w.String(L"bandEdges", BandEdgesName(d.bandEdges));
  w.String(L"bandEnergy", BandEnergyName(d.bandEnergy));
  // Precise: these three are transcribed out of MilkDrop 3's source, and the
  // file is a record of what that engine does.
  w.BeginArray(L"bandNormalise");
  for (int i = 0; i < 3; i++) w.FloatPreciseAnon(d.bandNormalise[i]);
  w.EndArray();
  w.Float(L"fpsRef", d.fpsRef);
  w.Float(L"avgAttack", d.avgAttack);
  w.Float(L"avgDecay", d.avgDecay);
  w.FloatPrecise(L"longMix", d.longMix);
  w.Float(L"medMix", d.medMix);
  w.Float(L"silenceValue", d.silenceValue);
  w.Bool(L"inputDamp", d.inputDamp);

  w.Float(L"fftAttack", d.fftAttack);
  w.Float(L"fftDecay", d.fftDecay);
  w.FloatPrecise(L"fftScale", d.fftScale);
  w.FloatPrecise(L"fftNoiseGate", d.fftNoiseGate);
  w.FloatPrecise(L"fftVisibleFloor", d.fftVisibleFloor);
  w.Bool(L"fftLowRolloff", d.fftLowRolloff);
  w.Int(L"fftPeakHoldFrames", d.fftPeakHoldFrames);
  w.FloatPrecise(L"fftPeakDecay", d.fftPeakDecay);
  w.Float(L"fftHzRef", d.fftHzRef);
  w.Bool(L"fftSqrt", d.fftSqrt);

  // See the matching block in ReadProfile: without these three the MilkDrop 3
  // audio path cannot survive a save/load round trip.
  w.Bool(L"fftPeakNormalise", d.fftPeakNormalise);
  w.FloatPrecise(L"fftRelGate", d.fftRelGate);
  w.FloatPrecise(L"fftPostGateExp", d.fftPostGateExp);
  w.Int(L"fftSpreadTaps", d.fftSpreadTaps);

  w.Float(L"pcmGain", d.pcmGain);
}

// One file per profile: the fields at the top level, with the name beside
// them. The name travels INSIDE the file so renaming never has to move it and
// a name may hold characters no filesystem accepts.
static void WriteProfileFile(JsonWriter& w, const wchar_t* name,
                             const AudioProfile& d) {
  w.BeginObject();
  w.String(L"name", name);
  WriteProfileBody(w, d);
  w.EndObject();
}

bool AudioProfileStore::Save(const wchar_t* name, const AudioProfile& d) {
  if (!name || !name[0]) return false;
  const std::wstring dir = ProfileDir();
  if (!profiles::EnsureDirPath(dir)) return false;

  // ONE file is written. The old store rewrote every profile on every save to
  // carry the untouched ones through verbatim; per-file there is nothing to
  // carry, because a profile this call did not name is a file it does not
  // open. That is the whole safety argument for the layout: a bad write can
  // only cost the profile being written.
  std::wstring leaf = AudioLeafForName(dir, name);
  if (leaf.empty()) leaf = profiles::WithJson(profiles::UniqueLeafIn(dir, name));

  JsonWriter w;
  WriteProfileFile(w, name, d);
  return w.SaveToFile((dir + leaf).c_str());
}

// ─── built-ins ──────────────────────────────────────────────────────────

AudioProfile AudioProfileStore::BuiltInMDropDX12() {
  AudioProfile d;   // the member initialisers ARE this build's constants
  d.name = L"MDropDX12";
  d.description = L"This build's own audio. The default; selecting it changes nothing.";
  return d;
}

AudioProfile AudioProfileStore::BuiltInMilkDrop3() {
  AudioProfile d;
  d.name = L"MilkDrop 3";
  d.description =
      L"MilkDrop 3's audio. The band group is transcribed from MilkDrop 3's own "
      L"source (pluginshell.cpp); fftHzRef and fftSqrt are transcribed from the "
      L"MD3 PRO decompile. The remaining FFT values are FITTED by measurement -- "
      L"MD3's texFFT fill was never located -- so treat them as an approximation "
      L"to improve, not as ground truth.";
  d.fftGroupIsFitted = true;

  // Transcribed: MilkDrop3/code/vis_milk2/pluginshell.cpp, AnalyzeNewSound.
  d.bandMode  = BandMode::MilkDrop;
  d.bandEdges = BandEdges::Octave;
  d.bandEnergy = BandEnergy::Mean;
  d.bandNormalise[0] = 0.326781557f;
  d.bandNormalise[1] = 0.380873770f;
  d.bandNormalise[2] = 0.199888934f;
  d.fpsRef       = 14.0f;
  d.avgAttack    = 0.2f;
  d.avgDecay     = 0.5f;
  d.longMix      = 0.96f;
  d.medMix       = 0.91f;
  d.silenceValue = 1.0f;    // MilkDrop substitutes 1.0, not 0.0, on silence
  d.inputDamp    = true;    // temp_wave = 0.5*(w[i] + w[i-1]) before the FFT

  // Transcribed: the shader preamble in the MD3 PRO decompile --
  //   #define get_fft(pos) tex2D(sampler_fft, float2(clamp(pos,0,1),0.5)).r
  //   #define get_fft_hz(freq) get_fft(clamp((freq)/24000.0, 0.0, 1.0))
  d.fftHzRef = 24000.0f;
  d.fftSqrt  = false;

  // Fitted. MD3's spectrum is unsmoothed, hotter, and has a noise floor
  // rather than a hard zero, so smoothing and both floors come off and the
  // scale goes up. Tune these against tools/milk2-probe, not by guessing.
  // Unsmoothed, which is what the two-engine spectrum capture shows MD3 to
  // be. UpdateAudioTexture computes decayFactor = (1 - decay)^2, so decay 0
  // means "fall instantly" -- i.e. track the instantaneous spectrum. decay 1
  // is the opposite: factor 0, a value that can rise and never fall. That is
  // a LATCH, and it is what these numbers said at first: the ring grew
  // monotonically (extent 201, 419, 475, 535) and looked like reactivity.
  d.fftAttack       = 1.0f;
  d.fftDecay        = 0.0f;
  d.fftScale        = 0.0028f;
  d.fftNoiseGate    = 0.0f;
  d.fftVisibleFloor = 0.0f;
  d.fftLowRolloff   = false;
  // The part scale could not buy. With the texture peak-normalised the
  // absolute level stops mattering, which is the whole point -- fftScale
  // survives only to keep the pre-normalise numbers in a sane range.
  // MD3's FFT treatment is done in the SHADER now (md3_fft_bin in
  // embedded_shaders.h), not here. It used to be done in both, so any preset
  // calling md3_fft_tex was normalised, gated and spread twice.
  //
  // The shader owns it so a recovered preset is self-contained -- it reproduces
  // MD3 without the user selecting an audio profile first. These fields remain
  // a real capability for a profile that wants the TEXTURE itself treated, and
  // must stay off here while the shader does it.
  d.fftPeakNormalise = false;
  d.fftRelGate = 0.0f;
  d.fftPostGateExp = 1.0f;
  d.fftSpreadTaps = 0;
  return d;
}

AudioProfile AudioProfileStore::BuiltInMilkwave() {
  AudioProfile d;   // identical to MDropDX12 today; see the description
  d.name = L"Milkwave";
  d.description =
      L"Milkwave Visualizer's audio. Its include.fx declares the identical "
      L"512x2 texture layout, row assignment, sqrt and 22050 divisor, so this "
      L"matches MDropDX12 exactly today. Kept as its own name so a preset can "
      L"say Milkwave and keep meaning it if the two ever diverge.";
  return d;
}

AudioProfile AudioProfileStore::SeedFor(const wchar_t* name) {
  if (name && name[0]) {
    const AudioProfile builtins[] = {
        BuiltInMDropDX12(), BuiltInMilkDrop3(), BuiltInMilkwave() };
    for (const AudioProfile& d : builtins)
      if (_wcsicmp(d.name.c_str(), name) == 0)
        return d;
  }
  return Defaults();
}

void AudioProfileStore::EnsureBuiltIns() {
  const AudioProfile builtins[] = {
      BuiltInMDropDX12(), BuiltInMilkDrop3(), BuiltInMilkwave() };
  for (const AudioProfile& d : builtins) {
    // Only when absent. Overwriting would silently discard a user's edit
    // every launch, which is the opposite of what a store is for.
    if (!Exists(d.name.c_str()))
      Save(d.name.c_str(), d);
  }
}

void AudioProfileStore::Names(std::vector<std::wstring>& out) const {
  out.clear();
  const JsonValue root = LoadAll();
  const JsonValue& profiles = root[kProfiles];
  if (!profiles.isObject()) return;
  for (const auto& [key, val] : profiles.members) {
    (void)val;
    out.push_back(key);
  }
}

bool AudioProfileStore::Exists(const wchar_t* name) const {
  if (!name || !name[0]) return false;
  const JsonValue root = LoadAll();
  const JsonValue& profiles = root[kProfiles];
  return profiles.isObject() && profiles.has(name);
}

}  // namespace mdrop
