// mixer_device_watch.h — one IMMNotificationClient for the whole subsystem.
//
// Device presence is never polled. Both the endpoint provider and, later, the
// failover watcher read presence from here and are woken by its callback,
// which is what lets failover be entirely event-driven.
//
// Callbacks arrive on a system thread. This class refreshes its own cache and
// invokes the supplied callback, which must itself do nothing but wake a
// worker -- never real work, and never anything that can block.
#pragma once

#include "mixer_provider.h"

#include <windows.h>
#include <mmdeviceapi.h>

#include <functional>
#include <mutex>

namespace mdrop {

class DeviceWatcher {
 public:
  DeviceWatcher() = default;
  ~DeviceWatcher();

  DeviceWatcher(const DeviceWatcher&) = delete;
  DeviceWatcher& operator=(const DeviceWatcher&) = delete;

  // onDeviceChange may be null. It is called from a system thread.
  bool Start(std::function<void()> onDeviceChange);

  // An optional rename, applied as each endpoint is read.
  //
  // The mixer keeps its own names for devices, because Windows will not: four
  // identical earbud sets arrive as WF-1000XM5-1 through -4 and renaming them
  // there does not stick. Substituting here rather than at each display site
  // means the channel list, the route names, the MIXER_DEVICE records AND the
  // failover allowlist's name matching all see the same name -- there is no
  // second place for the two to disagree. The hook is a std::function so this
  // module still knows nothing about settings or the engine.
  void SetNameHook(std::function<std::wstring(const std::wstring& id,
                                              const std::wstring& windowsName)> fn) {
    m_nameHook = std::move(fn);
  }
  void Stop();

  std::vector<EndpointInfo> Endpoints() const;
  bool IsActive(const std::wstring& id) const;
  // One endpoint's battery, 0..100, or -1 when there is none to show.
  //
  // Separate from Endpoints() because a fader record needs this per fader and
  // Endpoints() copies the whole list under the lock -- a hundred and twenty
  // structs, twenty-five times, to read one integer each.
  int BatteryFor(const std::wstring& id) const;
  std::wstring DefaultRenderId() const;

  // Re-reads the endpoint list. Called by the notification client and once at
  // Start; safe to call from any thread.
  void Refresh();

  // Re-reads ONLY the battery percentages, for endpoints already cached.
  //
  // Separate from Refresh because the two have nothing like the same cost or
  // the same cadence. Refresh enumerates every endpoint over COM and reads
  // several properties from each; this walks the Bluetooth nodes alone and
  // costs well under a millisecond, which is what makes it reasonable on the
  // window's one-second tick. Battery is also the only thing here that changes
  // on its own, with no device-change callback to announce it -- everything
  // else in the snapshot is event-driven and does not need polling at all.
  //
  // Safe to call from any thread. Returns how many endpoints now report one.
  int RefreshBattery();

 private:
  class Notify;

  IMMDeviceEnumerator* m_enumerator = nullptr;
  Notify* m_notify = nullptr;

  mutable std::mutex m_mutex;
  std::vector<EndpointInfo> m_endpoints;
  std::wstring m_defaultRender;
  std::function<void()> m_onDeviceChange;
  std::function<std::wstring(const std::wstring&, const std::wstring&)> m_nameHook;
};

}  // namespace mdrop
