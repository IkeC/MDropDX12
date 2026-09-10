// mixer_provider_endpoint.h — Windows audio endpoints as mixer channels.
//
// One channel per active endpoint, each with a single "main" fader driven by
// IAudioEndpointVolume. This is the provider that always works: no third-party
// software, no network, present on every machine.
//
// It also publishes the one route the Windows side can re-home,
// "endpoint:default-render", moved with IPolicyConfig. That interface is
// undocumented but is what every audio switcher on Windows uses; if it fails,
// only this route degrades and other providers' routes are untouched.
//
// It never polls. Volume changes made by anything else arrive as endpoint
// callbacks, and device presence comes from the shared DeviceWatcher.
#pragma once

#include "mixer_provider.h"
#include "mixer_device_watch.h"

#include <endpointvolume.h>

namespace mdrop {

// Renames an endpoint, the way the Sound settings page does.
//
// Four identical pairs of earbuds pair as WF-1000XM5-1 through -4 and there is
// nothing on screen to say which is which -- Shane has "3 colors, 2 black, one
// rose gold, and one off white" and cannot tell them apart in Windows. The
// name is a per-device property, so it survives disconnection and is what the
// failover allowlist matches on when a re-paired device returns under a new
// id.
//
// Writes PKEY_Device_DeviceDesc through IPolicyConfig, which is the same
// undocumented interface the default-device switch already uses; keeping both
// callers in this one file is why this is declared here rather than beside the
// mixer verbs. Returns false if the interface is unavailable or the write is
// refused.
bool RenameEndpoint(const std::wstring& endpointId, const std::wstring& name);

class EndpointProvider : public IMixerProvider {
 public:
  explicit EndpointProvider(DeviceWatcher* watcher) : m_watcher(watcher) {}
  ~EndpointProvider() override;

  const wchar_t* Id() const override { return L"endpoint"; }
  bool Start(const ProviderHost& host) override;
  void Stop() override;
  ProviderHealth Health() const override { return m_health; }

  std::vector<Channel> Channels() override;
  std::vector<RouteTarget> Routes() override;

  bool SetVolume(const std::wstring& channelId, const std::wstring& faderId,
                 float volume) override;
  bool SetMute(const std::wstring& channelId, const std::wstring& faderId,
               bool muted) override;
  bool SetRouteDevice(const std::wstring& routeId,
                      const std::wstring& endpointId) override;

  // Re-hooks the volume callbacks after the device list changes. Called on a
  // device-presence event or an explicit refresh, never on a timer.
  void RefreshRoutes() override;
  void RefreshLevels() override;

  void SetSubscribed(bool) override {}   // event-driven; never polls

 private:
  class VolumeNotify;

  void HookEndpoints();
  void UnhookEndpoints();

  // "endpoint:<device id>" -> "<device id>", or empty if not ours.
  static std::wstring DeviceIdFromChannel(const std::wstring& channelId);

  DeviceWatcher* m_watcher = nullptr;
  ProviderHost   m_host;
  ProviderHealth m_health = ProviderHealth::Ok;

  // One registration per endpoint. Both halves must be released, and the
  // callback unregistered before its interface, or the endpoint leaks.
  struct Hook {
    IAudioEndpointVolume* volume = nullptr;
    VolumeNotify*         notify = nullptr;
  };
  std::vector<Hook> m_hooks;

  // Stamped on our own writes so they do not come back as though a person had
  // moved the slider.
  GUID m_eventContext = {};
};

}  // namespace mdrop
