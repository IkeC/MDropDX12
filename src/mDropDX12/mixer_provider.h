// mixer_provider.h — the vocabulary every audio provider speaks.
//
// A channel owns one or more faders. That is what lets a Sonar channel, which
// carries independent monitoring and streaming levels, and a Windows endpoint,
// which has one, share a single abstraction without either being special-cased
// by a client. A client renders faders.size() sliders and never asks which
// provider it is talking to.
//
// Nothing here may include a project header. The whole module depends only on
// <windows.h>, the audio headers and the STL, which is what would make hosting
// it in a separate process later a change of owner rather than a rewrite.
#pragma once

#include <string>
#include <vector>

namespace mdrop {

enum class ChannelKind { Master, Mix, Device };

// Ok          answering, state is current
// Degraded    answering, but some state could not be read this cycle — values
//             are shown with a staleness marker rather than hidden
// Unavailable not answering, or never discovered — channels are drawn as
//             unavailable and no value is shown
enum class ProviderHealth { Ok, Degraded, Unavailable };

struct Fader {
  std::wstring id;        // "main" | "monitoring" | "streaming" | "classic"
  std::wstring label;
  float        volume  = 1.0f;   // 0..1
  bool         muted   = false;
  bool         canMute = true;
};

struct Channel {
  std::wstring       id;          // "sonar:media", "endpoint:{0.0.0...}.{...}"
  std::wstring       displayName; // the SHORT name if one was given, else Windows'
  // What Windows calls it, when that differs from displayName -- i.e. only
  // when the user has given this device a short name. Empty otherwise, and
  // empty for a provider's own channels, which have no Windows name at all.
  //
  // Both are carried so a reader has both without joining back to the device
  // list: a remote wants the short name on a narrow row and the full one in a
  // tooltip or a detail view, and it cannot cheaply do that join.
  std::wstring       windowsName;
  std::wstring       providerId;  // "sonar" | "endpoint"
  ChannelKind        kind = ChannelKind::Device;
  std::vector<Fader> faders;
  ProviderHealth     health = ProviderHealth::Ok;

  // This channel is a software endpoint another program may own -- see
  // EndpointInfo::isVirtual. Carried here so a fader record can say so without
  // its reader having to join back to the device list, which a phone cannot
  // cheaply do. Always false for a provider's own channels: a sonar: channel
  // is the real thing, not a shadow of one.
  bool               isVirtual = false;
};

// A named output path a provider can re-home onto a different endpoint. The
// failover watcher only ever sees these, so it needs no knowledge of Sonar or
// of the Windows default-device API.
struct RouteTarget {
  std::wstring id;              // "endpoint:default-render", "sonar:monitoring"
  std::wstring displayName;
  std::wstring currentDeviceId; // a Windows endpoint ID
};

struct EndpointInfo {
  std::wstring id;
  std::wstring friendlyName;
  bool isRender = true;
  bool isActive = true;

  // The PnP ContainerId: the PHYSICAL device this endpoint belongs to.
  //
  // Empty for a virtual endpoint, which has no physical device behind it.
  //
  // Carried on the struct rather than resolved on demand because it is what
  // decides whether a battery may be shown: a headset publishes two endpoints
  // sharing this id, and the device is connected when EITHER of them is
  // active, so no endpoint can answer that question by itself. Resolving it
  // costs a registry read, and the battery tick would otherwise pay it twice
  // per endpoint every second.
  std::wstring containerId;

  // True for an endpoint that is really a MONITOR: HDMI and DisplayPort
  // outputs, and the digital passthroughs beside them. Windows keeps one per
  // port per graphics card whether anything is plugged in or not, so they are
  // the great majority of the endpoint list -- 76 of 84 here -- and not one of
  // them is somewhere a person would send their headphones.
  //
  // Taken from PKEY_AudioEndpoint_FormFactor rather than guessed from the
  // name, so it does not depend on how a driver spells "HDMI".
  bool isDisplayAudio = false;

  // The hands-free half of a Bluetooth headset, rather than its stereo half.
  //
  // Windows publishes one physical headset as two endpoints that share a
  // ContainerId and a battery reading and differ by one word:
  // "Headphones (X)" is A2DP, stereo, high quality -- what anyone wants for
  // listening -- while "Headset (X)" turns the microphone on and drops to mono
  // narrowband. In Shane's words, "the headset is terrible and once that
  // channel gets enabled I have to switch headsets."
  //
  // So they look like duplicates of each other and are close to opposites.
  // Anything offering a device to pick must keep them apart and must never
  // choose one on the user's behalf. Never group these two by ContainerId,
  // however redundant the rows appear.
  //
  // From PKEY_AudioEndpoint_FormFactor (Headphones 3, Headset 5), not from the
  // name -- the app carries its own names for these devices, so a name test
  // would fail on exactly the ones that need it.
  bool isHandsFree = false;

  // A software endpoint with no physical device behind it.
  //
  // These are the ones another program OWNS. Measured and recorded in
  // docs/mixer.md: "SteelSeries Sonar - Aux" accepts SetMasterVolumeLevelScalar,
  // returns success, and then holds its level at 1.0, because Sonar owns that
  // channel and the Windows endpoint volume is not where its level lives. So it
  // is named like a channel, sits at full, and moving it does nothing -- a
  // decoy for the real sonar:aux fader below it (forgejo#51).
  //
  // Taken from the CONTAINER, not from the name. Windows puts every device
  // belonging to no physical container into the null container, which is where
  // all six Sonar endpoints, NVIDIA Broadcast and the GS Wavetable Synth live;
  // a real headset has a real container. The same test already decides
  // isHandsFree, and a name test would fail on exactly these, since this app
  // carries its own names for devices.
  //
  // Note what it does NOT mean: NVIDIA Broadcast and the GS Wavetable Synth are
  // caught by it too, and THEIR endpoint volumes work. So this marks "a
  // provider may own me", not "I am inert" -- which is why hiding these rows is
  // a view default with a switch, never a removal.
  //
  // Only answerable while the endpoint is ACTIVE: a device node resolves only
  // then. Every device this is meant to catch is virtual and therefore always
  // active, so an inactive endpoint is left unmarked rather than guessed at.
  bool isVirtual = false;

  // What WINDOWS calls it, before any name of ours replaced friendlyName.
  //
  // Both are needed. friendlyName is what a person should see, so it carries
  // the alias when there is one -- but a failover allowlist entry stores the
  // name that was current when it was added, and renaming a device afterwards
  // would otherwise strand that entry: the name it holds would match nothing.
  // Matching against either name keeps an existing list working across a
  // rename.
  std::wstring windowsName;

  // When Windows last touched this endpoint, as a FILETIME (0 = unknown).
  //
  // Which is how you tell four identical pairs of earbuds apart. Windows names
  // them WF-1000XM5-1 through -4 and every failed attempt to rename one leaves
  // another dead entry behind, so the list fills with duplicates that look the
  // same. The timestamps do not: the four pairings actually in use were last
  // touched within a day of each other, and the fifteen stale ones are from
  // months earlier.
  //
  // Two sources, best first.
  //
  // For a BLUETOOTH device it is DEVPKEY_Bluetooth_LastConnectedTime, read
  // from the device's own node. Per-device, months deep, and it survives a
  // reboot -- a real answer to "when did I last use this".
  //
  // For everything else it is the last-write time of the endpoint's registry
  // key, which is an APPROXIMATION and a poor one. Windows rewrites every
  // endpoint key whenever the audio stack re-enumerates, so after a reboot
  // each device reads as "seen at boot": one boot here stamped 33 endpoints
  // with the same second. It is kept only because it is better than a blank
  // for the devices the Bluetooth path cannot answer for -- and Windows
  // regards those as permanently connected anyway, so the question barely
  // applies to them.
  //
  // The Bluetooth property was tried once before and REJECTED, and that
  // rejection was wrong on both of its grounds. Recorded here because the note
  // stood for months and would otherwise be believed again:
  //
  //   "two headsets sat exactly one timezone offset away" -- because the value
  //   is stored in LOCAL time and was being converted with
  //   FileTimeToLocalFileTime, which subtracts the offset a second time.
  //
  //   "a third was eleven and a half hours out, which no single offset
  //   explains" -- because the REFERENCE was wrong. It was compared against
  //   the registry time, which for that device was a bulk re-enumeration
  //   months away from anything the device did.
  //
  // bt_seen_reconcile_probe.cpp establishes it on the machine rather than from
  // the documentation: of fourteen Bluetooth containers, exactly one has an
  // audio endpoint whose registry stamp is not merely the boot, and for that
  // one the local reading matches to the second while the UTC reading is five
  // hours out.
  //
  // Still called "seen" rather than "connected", because the fallback really
  // is only a sighting.
  unsigned long long lastSeen = 0;

  // Battery percentage 0..100, or -1 when there is none to show.
  //
  // Populated ONLY for an endpoint that is active, and that is a correctness
  // rule rather than a saving. Windows keeps the last value it was told after
  // a device disconnects, so a pair of earbuds sitting in its case still reads
  // whatever it read on the way out -- one of Shane's reads 1% and has done
  // for months. Showing that next to a device you might switch to would be
  // worse than showing nothing.
  //
  // Not every device reports one either. Two of his do not, so -1 is an
  // ordinary answer and not an error.
  int batteryPercent = -1;
};

// Posted by a provider when something it owns changed, so the mixer can refresh
// its snapshot and broadcast. May arrive on a system thread, so implementations
// must do nothing but wake the worker.
using ProviderChangedFn = void (*)(void* context);

struct ProviderHost {
  ProviderChangedFn onChanged = nullptr;
  void*             context   = nullptr;
  void Changed() const { if (onChanged) onChanged(context); }
};

class IMixerProvider {
 public:
  virtual ~IMixerProvider() = default;

  virtual const wchar_t* Id() const = 0;
  virtual bool  Start(const ProviderHost& host) = 0;
  virtual void  Stop() = 0;
  virtual ProviderHealth Health() const = 0;

  // Both return cached or cheap local state. Neither may block on the network.
  virtual std::vector<Channel>     Channels() = 0;
  virtual std::vector<RouteTarget> Routes()   = 0;

  virtual bool SetVolume(const std::wstring& channelId,
                         const std::wstring& faderId, float volume) = 0;
  virtual bool SetMute(const std::wstring& channelId,
                       const std::wstring& faderId, bool muted) = 0;
  virtual bool SetRouteDevice(const std::wstring& routeId,
                              const std::wstring& endpointId) = 0;

  // Called on a device-presence event or an explicit refresh, never on a timer.
  //
  // ROUTES ONLY. A volume write cannot change which device a route points at,
  // so the worker no longer calls this after an ordinary batch -- doing so
  // cost two HTTP GETs per batch at a service that hangs regularly.
  virtual void RefreshRoutes() = 0;

  // Re-read the levels, after the worker has applied a batch of writes.
  //
  // Once per BATCH, not once per command, and never from inside SetVolume or
  // SetMute. Each write used to force its own read-back, which was two things
  // wrong at once: a GET per write, and a read of values the service had not
  // necessarily applied yet -- so the fresh value was overwritten by a stale
  // one and a just-moved fader sprang back.
  virtual void RefreshLevels() = 0;

  // Gates any polling the provider does. False means "nobody is looking", and
  // a provider must then poll nothing at all.
  virtual void SetSubscribed(bool subscribed) = 0;
};

}  // namespace mdrop
