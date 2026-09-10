// audio_mixer.h — the mixer core: one worker thread, a cached snapshot, and a
// coalescing command queue.
//
// The queue is not an optimisation. A slider drag emits hundreds of sets, and
// a provider may talk to an application that hangs regularly, so an
// uncoalesced queue would pile requests onto something already wedged. One
// pending value per target, newest wins.
//
// Threading contract: every provider call happens on the worker. Callers only
// ever touch the queue and the cached snapshot, so a wedged provider parks the
// worker and nothing else -- not the render thread, not the message pump, not
// the TCP server.
#pragma once

#include "mixer_provider.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mdrop {

enum class MixerCommandKind { Volume, Mute, Route };

struct MixerCommand {
  MixerCommandKind kind = MixerCommandKind::Volume;
  std::wstring channelId;   // Volume/Mute: the channel. Route: unused.
  std::wstring faderId;     // Volume/Mute: the fader.  Route: unused.
  std::wstring routeId;     // Route only.
  std::wstring deviceId;    // Route only.
  float volume = 0.0f;      // Volume only.
  bool  muted  = false;     // Mute only.
};

// Not thread-safe by itself; AudioMixer owns one behind its own mutex.
class MixerQueue {
 public:
  void PostVolume(const std::wstring& channelId, const std::wstring& faderId,
                  float volume);
  void PostMute(const std::wstring& channelId, const std::wstring& faderId,
                bool muted);
  void PostRoute(const std::wstring& routeId, const std::wstring& deviceId);

  // Returns everything pending and empties the queue.
  std::vector<MixerCommand> Drain();

  size_t PendingCount() const { return m_pending.size(); }

 private:
  // Replaces the pending command with the same identity, or appends. Keeping
  // the original position means a replaced command does not jump the queue.
  void Replace(const MixerCommand& cmd);

  std::vector<MixerCommand> m_pending;
};

struct MixerSnapshot {
  std::vector<Channel>     channels;
  std::vector<RouteTarget> routes;
};

class AudioMixer {
 public:
  AudioMixer() = default;
  ~AudioMixer();

  AudioMixer(const AudioMixer&) = delete;
  AudioMixer& operator=(const AudioMixer&) = delete;

  // Call before Start(). Takes ownership.
  void AddProvider(std::unique_ptr<IMixerProvider> provider);

  bool Start();
  void Stop();

  // Cached; never performs I/O and never blocks on the worker.
  MixerSnapshot Snapshot() const;

  void SetVolume(const std::wstring& channelId, const std::wstring& faderId,
                 float volume);
  void SetMute(const std::wstring& channelId, const std::wstring& faderId,
               bool muted);
  void SetRouteDevice(const std::wstring& routeId, const std::wstring& deviceId);

  // Gates provider polling. The caller is responsible for reference counting
  // multiple watchers down to one boolean.
  void SetSubscribed(bool subscribed);

  // Ask the worker to re-read the providers and rebuild the snapshot.
  void RefreshNow();

  // Fired on the worker thread after the snapshot changes. Keep it cheap.
  void SetOnChanged(std::function<void()> fn) { m_onChanged = std::move(fn); }

  // Fired on the worker thread once per loop, which is at least every 100ms
  // even when nothing happens. The failover watcher needs this: it arms on one
  // tick and commits on a later one, so a route that armed and then saw no
  // further device event would never commit. Must be cheap and do no I/O.
  void SetOnTick(std::function<void()> fn) { m_onTick = std::move(fn); }

  // Test and shutdown aid: true once the queue is empty and nothing is in
  // flight, false on timeout. Never called from the IPC or render thread.
  bool WaitForIdle(unsigned timeoutMs);

 private:
  void WorkerMain();
  // Writes a requested value straight into the cached snapshot, so the next
  // read-modify-write starts from what was asked for rather than from what the
  // worker has managed to apply so far. Either pointer may be null.
  void Echo(const std::wstring& channelId, const std::wstring& faderId,
            const float* volume, const bool* muted);
  void Apply(const MixerCommand& cmd);
  void Rebuild();
  static void OnProviderChanged(void* context);

  std::vector<std::unique_ptr<IMixerProvider>> m_providers;

  mutable std::mutex m_snapshotMutex;
  MixerSnapshot m_snapshot;

  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  MixerQueue m_queue;
  bool m_dirty = false;          // a rebuild is wanted
  bool m_busy = false;           // a command is in flight
  int  m_subscribeWanted = -1;   // -1 none, 0 off, 1 on

  std::condition_variable m_idleCv;

  std::thread m_worker;
  std::atomic<bool> m_running{false};
  std::function<void()> m_onChanged;
  std::function<void()> m_onTick;
};

}  // namespace mdrop
