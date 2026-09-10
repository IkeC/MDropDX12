#include "audio_mixer.h"

#include <chrono>

namespace mdrop {

namespace {

bool SameTarget(const MixerCommand& a, const MixerCommand& b) {
  if (a.kind != b.kind) return false;
  if (a.kind == MixerCommandKind::Route) return a.routeId == b.routeId;
  return a.channelId == b.channelId && a.faderId == b.faderId;
}

float Clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

}  // namespace

void MixerQueue::Replace(const MixerCommand& cmd) {
  for (size_t i = 0; i < m_pending.size(); i++) {
    if (SameTarget(m_pending[i], cmd)) {
      // In place, so a value replaced mid-drag keeps its position rather than
      // jumping ahead of commands posted before it.
      m_pending[i] = cmd;
      return;
    }
  }
  m_pending.push_back(cmd);
}

void MixerQueue::PostVolume(const std::wstring& channelId,
                            const std::wstring& faderId, float volume) {
  MixerCommand cmd;
  cmd.kind = MixerCommandKind::Volume;
  cmd.channelId = channelId;
  cmd.faderId = faderId;
  cmd.volume = Clamp01(volume);
  Replace(cmd);
}

void MixerQueue::PostMute(const std::wstring& channelId,
                          const std::wstring& faderId, bool muted) {
  MixerCommand cmd;
  cmd.kind = MixerCommandKind::Mute;
  cmd.channelId = channelId;
  cmd.faderId = faderId;
  cmd.muted = muted;
  Replace(cmd);
}

void MixerQueue::PostRoute(const std::wstring& routeId,
                           const std::wstring& deviceId) {
  MixerCommand cmd;
  cmd.kind = MixerCommandKind::Route;
  cmd.routeId = routeId;
  cmd.deviceId = deviceId;
  Replace(cmd);
}

std::vector<MixerCommand> MixerQueue::Drain() {
  std::vector<MixerCommand> out;
  out.swap(m_pending);
  return out;
}

// ===========================================================================
// AudioMixer
// ===========================================================================

AudioMixer::~AudioMixer() { Stop(); }

void AudioMixer::AddProvider(std::unique_ptr<IMixerProvider> provider) {
  if (provider) m_providers.push_back(std::move(provider));
}

void AudioMixer::OnProviderChanged(void* context) {
  AudioMixer* self = static_cast<AudioMixer*>(context);
  if (!self) return;
  // May be a system thread (an endpoint volume callback, a device event). Do
  // nothing here but mark the snapshot stale and wake the worker.
  {
    std::lock_guard<std::mutex> lock(self->m_queueMutex);
    self->m_dirty = true;
  }
  self->m_queueCv.notify_one();
}

bool AudioMixer::Start() {
  if (m_running.load()) return true;

  ProviderHost host;
  host.onChanged = &AudioMixer::OnProviderChanged;
  host.context = this;
  for (auto& p : m_providers) p->Start(host);

  m_running.store(true);
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_dirty = true;            // build the first snapshot immediately
  }
  m_worker = std::thread(&AudioMixer::WorkerMain, this);
  return true;
}

void AudioMixer::Stop() {
  if (!m_running.exchange(false)) return;
  m_queueCv.notify_all();
  if (m_worker.joinable()) m_worker.join();
  for (auto& p : m_providers) p->Stop();
}

MixerSnapshot AudioMixer::Snapshot() const {
  std::lock_guard<std::mutex> lock(m_snapshotMutex);
  return m_snapshot;
}

// Optimistic local echo, applied before the worker has done anything.
//
// Without it a second read-modify-write computes from a value that has already
// been superseded: a held-down volume key, or a dragged slider, reads the same
// stale base twice and its second step undoes the first. Measured -- 0.5, down
// a step, up a step, and the fader ended at 0.6.
//
// The provider's next refresh overwrites this with the truth, so a value the
// provider clamps or quantises differently is wrong only until then.
void AudioMixer::Echo(const std::wstring& channelId,
                      const std::wstring& faderId, const float* volume,
                      const bool* muted) {
  std::lock_guard<std::mutex> lock(m_snapshotMutex);
  for (Channel& c : m_snapshot.channels) {
    if (c.id != channelId) continue;
    for (Fader& f : c.faders) {
      if (f.id != faderId) continue;
      if (volume) f.volume = Clamp01(*volume);
      if (muted) f.muted = *muted;
      return;
    }
  }
}

void AudioMixer::SetVolume(const std::wstring& channelId,
                           const std::wstring& faderId, float volume) {
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.PostVolume(channelId, faderId, volume);
  }
  Echo(channelId, faderId, &volume, nullptr);
  m_queueCv.notify_one();
}

void AudioMixer::SetMute(const std::wstring& channelId,
                         const std::wstring& faderId, bool muted) {
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.PostMute(channelId, faderId, muted);
  }
  Echo(channelId, faderId, nullptr, &muted);
  m_queueCv.notify_one();
}

void AudioMixer::SetRouteDevice(const std::wstring& routeId,
                                const std::wstring& deviceId) {
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_queue.PostRoute(routeId, deviceId);
  }
  m_queueCv.notify_one();
}

void AudioMixer::SetSubscribed(bool subscribed) {
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_subscribeWanted = subscribed ? 1 : 0;
  }
  m_queueCv.notify_one();
}

void AudioMixer::RefreshNow() {
  {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_dirty = true;
  }
  m_queueCv.notify_one();
}

bool AudioMixer::WaitForIdle(unsigned timeoutMs) {
  std::unique_lock<std::mutex> lock(m_queueMutex);
  return m_idleCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this]() {
                             return m_queue.PendingCount() == 0 && !m_busy &&
                                    !m_dirty && m_subscribeWanted < 0;
                           });
}

void AudioMixer::Apply(const MixerCommand& cmd) {
  // First provider that claims the target wins. A provider returns false for
  // anything it does not own, which is how "endpoint:" and "sonar:" ids route
  // themselves without the mixer parsing prefixes.
  for (auto& p : m_providers) {
    switch (cmd.kind) {
      case MixerCommandKind::Volume:
        if (p->SetVolume(cmd.channelId, cmd.faderId, cmd.volume)) return;
        break;
      case MixerCommandKind::Mute:
        if (p->SetMute(cmd.channelId, cmd.faderId, cmd.muted)) return;
        break;
      case MixerCommandKind::Route:
        if (p->SetRouteDevice(cmd.routeId, cmd.deviceId)) return;
        break;
    }
  }
}

void AudioMixer::Rebuild() {
  MixerSnapshot built;
  for (auto& p : m_providers) {
    const std::vector<Channel> channels = p->Channels();
    built.channels.insert(built.channels.end(), channels.begin(), channels.end());
    const std::vector<RouteTarget> routes = p->Routes();
    built.routes.insert(built.routes.end(), routes.begin(), routes.end());
  }
  {
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_snapshot = built;
  }
  // Outside the lock: the callback broadcasts, and must never be able to
  // deadlock against a reader taking the snapshot.
  if (m_onChanged) m_onChanged();
}

void AudioMixer::WorkerMain() {
  // A drag posts hundreds of sets; applying each would hammer a provider that
  // may be slow or hung, and SteelSeries Sonar hangs regularly on this
  // machine. 5 Hz at Shane's request -- "don't want to overburden sonar
  // service".
  //
  // This governs a DRAG and nothing else. A hotkey press or one IPC write is a
  // single command: it is drained on the next pass and answered immediately,
  // so the interval never delays a deliberate change. What it bounds is the
  // stream of sets a dragged slider emits, where only the newest value per
  // fader matters anyway -- the queue has already collapsed the rest.
  //
  // It was 50 ms. Combined with the per-write read-back that used to sit
  // inside SetVolume, a drag reached roughly eighty requests a second.
  const auto kMinInterval = std::chrono::milliseconds(200);
  auto lastApply = std::chrono::steady_clock::now() - kMinInterval;

  while (m_running.load()) {
    std::vector<MixerCommand> batch;
    bool wantRoutes = false;
    int wantSubscribe = -1;

    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_idleCv.notify_all();
      m_queueCv.wait_for(lock, std::chrono::milliseconds(100), [this]() {
        return !m_running.load() || m_dirty || m_subscribeWanted >= 0 ||
               m_queue.PendingCount() > 0;
      });
      if (!m_running.load()) break;

      wantSubscribe = m_subscribeWanted;
      m_subscribeWanted = -1;

      const auto now = std::chrono::steady_clock::now();
      if (m_queue.PendingCount() > 0 && now - lastApply >= kMinInterval) {
        batch = m_queue.Drain();
        lastApply = now;
      }
      // m_dirty means a DEVICE EVENT -- a provider telling us the world
      // changed underneath us. That, and only that, is a reason to re-read
      // routes. Applying a batch is not: a volume write cannot move a
      // redirection, and forcing the route GETs after every batch cost two
      // extra round trips per write at a service that hangs regularly.
      wantRoutes = m_dirty;
      m_dirty = false;
      // Busy covers the rebuild as well as the batch. Without the rebuild
      // half, m_dirty is cleared here and WaitForIdle is satisfied while
      // Rebuild() is still running, so a caller reads the previous (on the
      // first pass, empty) snapshot and believes it is current.
      m_busy = !batch.empty() || wantRoutes;
    }

    if (wantSubscribe >= 0)
      for (auto& p : m_providers) p->SetSubscribed(wantSubscribe != 0);

    for (const MixerCommand& cmd : batch) Apply(cmd);

    // Two different questions, asked separately, because they cost different
    // amounts and are invalidated by different things.
    if (wantRoutes) {
      // What devices and redirections exist. Only a device event invalidates
      // this, and it is the expensive one.
      for (auto& p : m_providers) p->RefreshRoutes();
    }
    if (!batch.empty()) {
      // What the levels now ARE, once per batch rather than once per command.
      // A two-fader group nudge is one refresh, not two.
      for (auto& p : m_providers) p->RefreshLevels();
    }
    if (wantRoutes || !batch.empty()) Rebuild();

    {
      std::lock_guard<std::mutex> lock(m_queueMutex);
      m_busy = false;
    }
    m_idleCv.notify_all();

    // Once per loop, whether or not anything happened. This is the heartbeat
    // the failover watcher runs on; without it an armed route would wait for a
    // device event that may never come.
    if (m_onTick) m_onTick();
  }
}

}  // namespace mdrop
