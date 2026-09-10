#include "mixer_provider_sonar.h"

#include "json_utils.h"

#include <windows.h>

#include "format_to.h"

namespace mdrop {

namespace {

// The channel keys Sonar uses, and the labels a person recognises.
struct ChannelDef { const wchar_t* key; const wchar_t* label; ChannelKind kind; };

const ChannelDef kChannels[] = {
    { L"masters",     L"Master", ChannelKind::Master },
    { L"game",        L"Game",   ChannelKind::Mix },
    { L"chatRender",  L"Chat",   ChannelKind::Mix },
    { L"chatCapture", L"Mic",    ChannelKind::Mix },
    { L"media",       L"Media",  ChannelKind::Mix },
    { L"aux",         L"Aux",    ChannelKind::Mix },
};

// Poll ceilings. 1 Hz for levels is the hard limit; routes change far less
// often and are refreshed on every device event anyway.
const unsigned kVolumeIntervalMs = 1000;
const unsigned kRouteIntervalMs  = 5000;

bool Elapsed(unsigned last, unsigned intervalMs) {
  return last == 0 || (GetTickCount() - last) >= intervalMs;
}

}  // namespace

std::wstring SonarProvider::KeyFromPrefixedId(const std::wstring& id) {
  const std::wstring prefix = L"sonar:";
  if (id.compare(0, prefix.size(), prefix) != 0) return std::wstring();
  return id.substr(prefix.size());
}

std::wstring SonarProvider::WritePath(const std::wstring& channelKey,
                                      const std::wstring& faderId,
                                      const wchar_t* property,
                                      const std::wstring& value) const {
  // Sonar's JSON and Sonar's write path disagree about ONE name, and the
  // disagreement is a single letter.
  //
  // The document has a top-level `masters` object sitting BESIDE `devices`:
  //
  //     { "masters": {...}, "devices": { game, chatRender, chatCapture,
  //                                      media, aux } }
  //
  // so `masters` is the right key to READ. It is the wrong one to WRITE:
  // the write path names a device, there is no device called `masters`, and
  // the request comes back 400 with the fader unmoved. That looks exactly
  // like Sonar refusing to let the master be driven from outside its own app,
  // which is what forgejo#63 concluded. It is not a refusal -- it is a typo.
  //
  // Verified with private/tools/milk2-probe/sonar_masters_probe.py:
  //     .../monitoring/masters/Volume/0.42  -> 400, value unchanged
  //     .../monitoring/master/Volume/0.42   -> 200, value moves
  // and the same for isMuted. Only streamer mode was exercised, because that
  // is the mode this machine runs; the classic path uses the same singular on
  // the same reasoning, unverified.
  const std::wstring seg = (channelKey == L"masters") ? L"master" : channelKey;

  if (m_streamMode) {
    // slider BEFORE channel -- see the header.
    return L"/volumeSettings/streamer/" + faderId + L"/" + seg + L"/" +
           property + L"/" + value;
  }
  return L"/volumeSettings/classic/" + seg + L"/" + property + L"/" + value;
}

bool SonarProvider::Start(const ProviderHost& host) {
  m_host = host;
  if (!m_http.Discover()) {
    m_health = ProviderHealth::Unavailable;
    return false;
  }

  std::wstring body;
  m_requests++;
  if (m_http.Get(L"/mode", body))
    m_streamMode = body.find(L"stream") != std::wstring::npos;

  RefreshVolumes(true);
  RefreshRedirections(true);
  return m_health != ProviderHealth::Unavailable;
}

void SonarProvider::Stop() {
  m_http.Forget();
  m_channels.clear();
  m_routes.clear();
  m_deviceNames.clear();
  m_health = ProviderHealth::Unavailable;
}

void SonarProvider::SetSubscribed(bool subscribed) {
  m_subscribed = subscribed;
  // Becoming watched refreshes once immediately, so a client does not stare at
  // a stale value for up to a second before the first poll lands.
  if (subscribed) RefreshVolumes(true);
}

void SonarProvider::MarkFailure() {
  m_failures++;
  // Two consecutive failures, not one: a single miss during a GG restart is
  // ordinary and should not black out every channel.
  if (m_failures >= 2) m_health = ProviderHealth::Unavailable;
  else if (m_health == ProviderHealth::Ok) m_health = ProviderHealth::Degraded;
}

void SonarProvider::MarkSuccess() {
  m_failures = 0;
  m_health = ProviderHealth::Ok;
}

bool SonarProvider::RefreshVolumes(bool force) {
  if (!force && !m_subscribed) return true;           // nobody is looking
  if (!force && !Elapsed(m_lastVolumeTick, kVolumeIntervalMs)) return true;

  std::wstring body;
  m_requests++;
  if (!m_http.Get(m_streamMode ? L"/volumeSettings/streamer"
                               : L"/volumeSettings/classic", body)) {
    MarkFailure();
    return false;
  }
  m_lastVolumeTick = GetTickCount();

  try {
    const JsonValue root = JsonParse(body);
    const JsonValue& devices = root[L"devices"];

    std::vector<Channel> built;
    for (const ChannelDef& def : kChannels) {
      const bool isMaster = (wcscmp(def.key, L"masters") == 0);
      const JsonValue& node = isMaster ? root[L"masters"] : devices[def.key];
      if (node.isNull()) continue;

      Channel c;
      c.id = std::wstring(L"sonar:") + def.key;
      c.displayName = def.label;
      c.providerId = L"sonar";
      c.kind = def.kind;
      c.health = ProviderHealth::Ok;

      if (m_streamMode) {
        const JsonValue& stream = node[L"stream"];
        const wchar_t* ids[2] = { L"monitoring", L"streaming" };
        const wchar_t* labels[2] = { L"Monitoring", L"Streaming" };
        for (int i = 0; i < 2; i++) {
          const JsonValue& slider = stream[ids[i]];
          if (slider.isNull()) continue;
          Fader f;
          f.id = ids[i];
          f.label = labels[i];
          f.volume = slider[L"volume"].asFloat(1.0f);
          f.muted = slider[L"muted"].asBool(false);
          // The master's mute is DESTRUCTIVE, so it is not offered.
          //
          // Once the write path was corrected the master finally answered, and
          // what it does is write every render channel's isMuted -- then, on
          // unmute, put back a snapshot Sonar took at some earlier point rather
          // than the states that were live a moment before. Measured on the
          // streaming slider with a deliberately mixed pattern and three
          // seconds of settling at each step, so it is not a race:
          //
          //   mixed        aux=1 chatCapture=1 chatRender=0 game=1 media=0
          //   master=1     aux=1 chatCapture=1 chatRender=1 game=1 media=1
          //   master=0     aux=0 chatCapture=1 chatRender=0 game=0 media=0
          //
          // aux and game were muted going in and came back unmuted. Shane hit
          // this as "when I unmute I get all the channels unmuted". The mic
          // (chatCapture) is left alone throughout.
          //
          // Volume is NOT affected and stays available: it is a true
          // multiplier, and restoring the master restores every channel
          // exactly -- 0.78 -> 0.273 at master 0.35 -> 0.78 again at 1.0.
          f.canMute = !isMaster;
          c.faders.push_back(f);
        }
      } else {
        const JsonValue& classic = node[L"classic"];
        Fader f;
        f.id = L"classic";
        f.label = L"Volume";
        f.volume = classic[L"volume"].asFloat(1.0f);
        f.muted = classic[L"muted"].asBool(false);
        // Same rule as streamer mode above, on the same reasoning. Classic
        // mode was not measured -- this machine runs streamer -- so this is
        // the cautious side of an untested question rather than a finding.
        f.canMute = !isMaster;
        c.faders.push_back(f);
      }

      if (!c.faders.empty()) built.push_back(c);
    }

    m_channels.swap(built);
  } catch (...) {
    // Malformed or unexpected JSON must degrade this provider, not kill the
    // process. The module is built with /EHa and a static CRT, where an
    // escaped exception is abort() -- 0xC0000409, no log, no SEH recovery.
    MarkFailure();
    return false;
  }
  MarkSuccess();
  return true;
}

bool SonarProvider::RefreshRedirections(bool force) {
  if (!force && !Elapsed(m_lastRouteTick, kRouteIntervalMs)) return true;

  // Device names first, so a route can be shown by name rather than by GUID.
  std::wstring body;
  m_requests++;
  try {
    if (m_http.Get(L"/audioDevices", body)) {
      const JsonValue devices = JsonParse(body);
      m_deviceNames.clear();
      for (size_t i = 0; i < devices.size(); i++) {
        const JsonValue& d = devices.at(i);
        const std::wstring id = d[L"id"].asString();
        if (!id.empty()) m_deviceNames[id] = d[L"friendlyName"].asString();
      }
    }

    m_requests++;
    if (!m_http.Get(m_streamMode ? L"/streamRedirections"
                                 : L"/classicRedirections", body)) {
      MarkFailure();
      return false;
    }
    m_lastRouteTick = GetTickCount();

    const JsonValue list = JsonParse(body);
    std::vector<RouteTarget> built;
    for (size_t i = 0; i < list.size(); i++) {
      const JsonValue& r = list.at(i);
      const std::wstring key = m_streamMode ? r[L"streamRedirectionId"].asString()
                                            : r[L"id"].asString();
      if (key.empty()) continue;
      RouteTarget t;
      t.id = std::wstring(L"sonar:") + key;
      t.currentDeviceId = r[L"deviceId"].asString();
      const auto it = m_deviceNames.find(t.currentDeviceId);
      t.displayName =
          key + L" -> " +
          (it != m_deviceNames.end() ? it->second : t.currentDeviceId);
      built.push_back(t);
    }

    m_routes.swap(built);
  } catch (...) {
    MarkFailure();
    return false;
  }
  MarkSuccess();
  return true;
}

void SonarProvider::RefreshRoutes() {
  // Called on a device event. Routes are exactly what a device event can
  // invalidate, so this forces past the interval.
  //
  RefreshRedirections(true);
  // The levels too, but UNFORCED -- gated on the 1s interval and on anyone
  // actually looking, exactly as before. This path is also what RefreshNow
  // drives, which is the Refresh button and the failover watcher's tick, so
  // dropping it entirely would have meant an explicit refresh that never
  // re-read a level. What changed is only that a WRITE no longer forces its
  // own read-back; a device event still gets the cheap gated one.
  RefreshVolumes(false);
}

void SonarProvider::RefreshLevels() {
  // Forced past the interval, because this is called after a write and the
  // whole point is to replace the optimistic value with the truth.
  //
  // Once per batch. Doing it per command turned a two-fader group nudge into
  // six round trips where three will do.
  RefreshVolumes(true);
}

bool SonarProvider::SetVolume(const std::wstring& channelId,
                              const std::wstring& faderId, float volume) {
  const std::wstring key = KeyFromPrefixedId(channelId);
  if (key.empty() || m_health == ProviderHealth::Unavailable) return false;
  if (m_streamMode) {
    if (faderId != L"monitoring" && faderId != L"streaming") return false;
  } else if (faderId != L"classic") {
    return false;
  }

  const float clamped = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
  wchar_t value[32];
  FormatTo(value, L"%.2f", clamped);

  m_requests++;
  if (!m_http.Put(WritePath(key, faderId, L"Volume", value))) {
    MarkFailure();
    return false;
  }
  MarkSuccess();
  // No read-back here. The worker calls RefreshLevels() once after the whole
  // batch, and the caller's snapshot already carries the requested value
  // optimistically -- reading it back now costs a GET per write and races the
  // service, which has not necessarily applied the PUT yet.
  // No m_host.Changed() here.
  //
  // Changed() means "something moved that nobody asked us for" -- a device
  // event, an endpoint callback -- and it marks the mixer dirty, which is the
  // signal to re-read ROUTES. A write is the opposite: we asked for it, and
  // the worker already rebuilds after the batch that contains it.
  //
  // Calling it from a write made every write cost two route GETs one pass
  // later, which is exactly the traffic this change exists to remove.
  return true;
}

bool SonarProvider::SetMute(const std::wstring& channelId,
                            const std::wstring& faderId, bool muted) {
  const std::wstring key = KeyFromPrefixedId(channelId);
  if (key.empty() || m_health == ProviderHealth::Unavailable) return false;
  if (m_streamMode) {
    if (faderId != L"monitoring" && faderId != L"streaming") return false;
  } else if (faderId != L"classic") {
    return false;
  }

  // Refuse a fader that reports canMute=false rather than sending the write.
  // For the master that write lands, which is precisely the problem: it mutes
  // every render channel and loses their individual states on the way back.
  // See where canMute is set in RefreshVolumes for the measurements.
  for (const Channel& c : m_channels) {
    if (c.id != channelId) continue;
    for (const Fader& f : c.faders)
      if (f.id == faderId && !f.canMute) return false;
  }

  // `isMuted`, camelCase. `Mute` returns 404, and it does not match `Volume`'s
  // capitalisation -- the API is simply inconsistent here.
  m_requests++;
  if (!m_http.Put(WritePath(key, faderId, L"isMuted",
                            muted ? L"true" : L"false"))) {
    MarkFailure();
    return false;
  }
  MarkSuccess();
  // Same as SetVolume: the batch refresh does the read-back, and a write is
  // not an unsolicited change.
  return true;
}

bool SonarProvider::SetRouteDevice(const std::wstring& routeId,
                                   const std::wstring& endpointId) {
  const std::wstring key = KeyFromPrefixedId(routeId);
  if (key.empty() || endpointId.empty()) return false;
  if (m_health == ProviderHealth::Unavailable) return false;

  // The PATH is confirmed; the EFFECT is not. Worth keeping those apart.
  //
  // Probed 2026-08-31 with private/tools/milk2-probe/sonar_redirect_probe.py,
  // which writes the device a redirection ALREADY points at -- so a correct
  // shape is a no-op that answers 200 and a wrong one answers 404 without
  // moving anything. This path answered 200; four plausible alternatives,
  // including the property-before-channel ordering that the VOLUME write
  // turned out to need, all answered 404.
  //
  // And the EFFECT is proven too, as of 2026-08-31. Shane pulled the
  // WF-1000XM6 while failover_watch.py was recording:
  //
  //   08:08:41  -device Headphones (WF-1000XM6)   state idle -> arming
  //   08:08:44  MOVED -> Headphones (WF-1000XM5-4)   state arming -> idle
  //
  // Three seconds, which is the configured stability window, and the audio
  // followed. So this really does move a Sonar redirection, not merely accept
  // a request to. Failure still marks the provider degraded rather than
  // pretending.
  const std::wstring path =
      (m_streamMode ? L"/streamRedirections/" : L"/classicRedirections/") + key +
      L"/deviceId/" + endpointId;

  m_requests++;
  if (!m_http.Put(path)) {
    MarkFailure();
    return false;
  }
  MarkSuccess();
  RefreshRedirections(true);
  m_host.Changed();
  return true;
}

}  // namespace mdrop
