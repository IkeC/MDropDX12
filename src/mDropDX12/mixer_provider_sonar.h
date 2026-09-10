// mixer_provider_sonar.h — SteelSeries Sonar channels as mixer channels.
//
// This is the provider that makes the two-knob model real: in stream mode every
// Sonar channel carries independent monitoring and streaming levels, so "what I
// hear" and "what the stream hears" become two faders on one channel.
//
// It exists because Sonar's virtual endpoints cannot be driven through Windows:
// measured, `SteelSeries Sonar - Aux` accepts SetMasterVolumeLevelScalar,
// returns success, and holds its level at 1.0. Only Sonar can move a Sonar
// channel.
//
// The provider holds a cached view and refreshes it only when someone is
// looking: nothing is read unless asked for or watched, and 1 Hz is a ceiling.
#pragma once

#include "mixer_provider.h"
#include "mixer_sonar_http.h"

#include <map>

namespace mdrop {

class SonarProvider : public IMixerProvider {
 public:
  const wchar_t* Id() const override { return L"sonar"; }
  bool Start(const ProviderHost& host) override;
  void Stop() override;
  ProviderHealth Health() const override { return m_health; }

  std::vector<Channel> Channels() override { return m_channels; }
  std::vector<RouteTarget> Routes() override { return m_routes; }

  bool SetVolume(const std::wstring& channelId, const std::wstring& faderId,
                 float volume) override;
  bool SetMute(const std::wstring& channelId, const std::wstring& faderId,
               bool muted) override;
  bool SetRouteDevice(const std::wstring& routeId,
                      const std::wstring& endpointId) override;

  void RefreshRoutes() override;
  void RefreshLevels() override;
  void SetSubscribed(bool subscribed) override;

  bool IsStreamMode() const { return m_streamMode; }

  // Test seam: how many HTTP requests have been issued, so a test can prove an
  // unsubscribed provider goes nowhere near the network.
  int RequestCountForTest() const { return m_requests; }

 private:
  // Called only from the mixer worker. Return false when Sonar could not be
  // reached.
  bool RefreshVolumes(bool force);
  bool RefreshRedirections(bool force);
  void MarkFailure();
  void MarkSuccess();

  // Builds a write path. CONFIRMED against a live GG on 2026-08-30: the SLIDER
  // comes BEFORE the CHANNEL, which is the reverse of the obvious reading of
  // the GET shape. Channel-first returns 400 "Request validation error", and so
  // does a nonsense channel, so the error distinguishes nothing.
  std::wstring WritePath(const std::wstring& channelKey,
                         const std::wstring& faderId,
                         const wchar_t* property,
                         const std::wstring& value) const;

  static std::wstring KeyFromPrefixedId(const std::wstring& id);

  SonarHttp m_http;
  ProviderHost m_host;
  ProviderHealth m_health = ProviderHealth::Unavailable;

  bool m_streamMode = true;
  bool m_subscribed = false;
  int  m_failures = 0;
  int  m_requests = 0;

  unsigned m_lastVolumeTick = 0;
  unsigned m_lastRouteTick = 0;

  std::vector<Channel> m_channels;
  std::vector<RouteTarget> m_routes;
  std::map<std::wstring, std::wstring> m_deviceNames;  // id -> friendly name
};

}  // namespace mdrop
