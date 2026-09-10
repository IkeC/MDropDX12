#include "mixer_provider_endpoint.h"

namespace mdrop {

namespace {

const wchar_t* const kChannelPrefix = L"endpoint:";
const size_t kChannelPrefixLen = 9;
const wchar_t* const kDefaultRenderRoute = L"endpoint:default-render";

// IPolicyConfig — undocumented, and the only way to move the default endpoint.
// Declared here because the SDK ships no header for it. The CLSID and IID are
// the Windows 7+ values every audio switcher uses. The vtable order matters:
// SetDefaultEndpoint must sit at the right slot or the call lands elsewhere.
const CLSID CLSID_CPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
const IID IID_IPolicyConfig = {
    0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, INT64*, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

// Opens the endpoint volume interface for one device id. Caller releases.
IAudioEndpointVolume* OpenVolume(const std::wstring& deviceId) {
  if (deviceId.empty()) return nullptr;

  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              (void**)&enumerator)) ||
      !enumerator)
    return nullptr;

  IMMDevice* device = nullptr;
  const HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
  enumerator->Release();
  if (FAILED(hr) || !device) return nullptr;

  IAudioEndpointVolume* volume = nullptr;
  const HRESULT hrv = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                       nullptr, (void**)&volume);
  device->Release();
  if (FAILED(hrv)) return nullptr;
  return volume;
}

}  // namespace

// The volume sink. One per endpoint. It does nothing but wake the mixer: a
// callback arriving on a system thread must not do work inline.
class EndpointProvider::VolumeNotify : public IAudioEndpointVolumeCallback {
 public:
  VolumeNotify(const ProviderHost& host, const GUID& selfContext)
      : m_host(host), m_self(selfContext) {}

  ULONG STDMETHODCALLTYPE AddRef() override {
    return (ULONG)InterlockedIncrement(&m_ref);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&m_ref);
    if (n == 0) delete this;
    return (ULONG)n;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IAudioEndpointVolumeCallback)) {
      *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
    // Our own writes carry m_self, and re-reporting them would make every set
    // cost an extra rebuild for no new information.
    //
    // Measured, not assumed: Windows emits TWO notifications for one scalar
    // volume change, and only one of them carries the context we passed. So
    // this filter halves the cost of a self-write rather than eliminating it,
    // which is why the test asserts a self-write is no dearer than an external
    // one instead of asserting silence.
    if (data && IsEqualGUID(data->guidEventContext, m_self)) return S_OK;
    m_host.Changed();
    return S_OK;
  }

 private:
  LONG m_ref = 1;
  ProviderHost m_host;
  GUID m_self;
};

EndpointProvider::~EndpointProvider() { UnhookEndpoints(); }

std::wstring EndpointProvider::DeviceIdFromChannel(const std::wstring& channelId) {
  if (channelId.compare(0, kChannelPrefixLen, kChannelPrefix) != 0)
    return std::wstring();
  return channelId.substr(kChannelPrefixLen);
}

void EndpointProvider::HookEndpoints() {
  UnhookEndpoints();
  // Only active endpoints have a volume interface to hook.
  if (!m_watcher) return;
  for (const EndpointInfo& e : m_watcher->Endpoints()) {
    if (!e.isActive) continue;
    IAudioEndpointVolume* volume = OpenVolume(e.id);
    if (!volume) continue;
    VolumeNotify* notify = new VolumeNotify(m_host, m_eventContext);
    if (FAILED(volume->RegisterControlChangeNotify(notify))) {
      notify->Release();
      volume->Release();
      continue;
    }
    Hook hook;
    hook.volume = volume;
    hook.notify = notify;
    m_hooks.push_back(hook);
  }
}

void EndpointProvider::UnhookEndpoints() {
  for (Hook& h : m_hooks) {
    if (h.volume && h.notify) h.volume->UnregisterControlChangeNotify(h.notify);
    if (h.notify) h.notify->Release();
    if (h.volume) h.volume->Release();
  }
  m_hooks.clear();
}

void EndpointProvider::RefreshRoutes() { HookEndpoints(); }

// Nothing to do. A Windows endpoint pushes its own volume changes through the
// IAudioEndpointVolume callback, so there is no polling to force -- this
// exists only because the interface asks every provider for it.
void EndpointProvider::RefreshLevels() {}

bool EndpointProvider::Start(const ProviderHost& host) {
  m_host = host;
  m_health = m_watcher ? ProviderHealth::Ok : ProviderHealth::Unavailable;
  if (!m_watcher) return false;
  if (FAILED(CoCreateGuid(&m_eventContext))) m_eventContext = GUID_NULL;
  HookEndpoints();
  return true;
}

void EndpointProvider::Stop() { UnhookEndpoints(); }

std::vector<Channel> EndpointProvider::Channels() {
  std::vector<Channel> out;
  if (!m_watcher) return out;

  for (const EndpointInfo& e : m_watcher->Endpoints()) {
    // Active only. The watcher now also reports endpoints that are unplugged
    // or not present, so the failover list can offer them, but a fader for a
    // device that is not there would have nothing behind it.
    if (!e.isActive) continue;
    Channel c;
    c.id = std::wstring(kChannelPrefix) + e.id;
    c.displayName = e.friendlyName;      // the alias, once one is set
    // Only when it differs. A device with no short name has one name, and
    // repeating it would make every reader test for equality itself.
    if (e.windowsName != e.friendlyName) c.windowsName = e.windowsName;
    c.providerId = L"endpoint";
    c.kind = ChannelKind::Device;
    c.health = ProviderHealth::Ok;
    c.isVirtual = e.isVirtual;

    Fader f;
    f.id = L"main";
    f.label = L"Volume";
    f.canMute = true;

    IAudioEndpointVolume* volume = OpenVolume(e.id);
    if (volume) {
      float level = 1.0f;
      BOOL muted = FALSE;
      if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))) f.volume = level;
      if (SUCCEEDED(volume->GetMute(&muted))) f.muted = (muted != FALSE);
      volume->Release();
    } else {
      // The device is listed but will not open. Say so rather than showing a
      // confident 1.0 that nothing backs.
      c.health = ProviderHealth::Degraded;
    }

    c.faders.push_back(f);
    out.push_back(c);
  }
  return out;
}

std::vector<RouteTarget> EndpointProvider::Routes() {
  std::vector<RouteTarget> out;
  if (!m_watcher) return out;
  RouteTarget r;
  r.id = kDefaultRenderRoute;
  r.displayName = L"Windows default output";
  r.currentDeviceId = m_watcher->DefaultRenderId();
  out.push_back(r);
  return out;
}

bool EndpointProvider::SetVolume(const std::wstring& channelId,
                                 const std::wstring& faderId, float value) {
  if (faderId != L"main") return false;
  const std::wstring deviceId = DeviceIdFromChannel(channelId);
  if (deviceId.empty()) return false;

  IAudioEndpointVolume* volume = OpenVolume(deviceId);
  if (!volume) return false;
  const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
  const HRESULT hr = volume->SetMasterVolumeLevelScalar(clamped, &m_eventContext);
  volume->Release();
  if (SUCCEEDED(hr)) m_host.Changed();
  return SUCCEEDED(hr);
}

bool EndpointProvider::SetMute(const std::wstring& channelId,
                               const std::wstring& faderId, bool muted) {
  if (faderId != L"main") return false;
  const std::wstring deviceId = DeviceIdFromChannel(channelId);
  if (deviceId.empty()) return false;

  IAudioEndpointVolume* volume = OpenVolume(deviceId);
  if (!volume) return false;
  const HRESULT hr = volume->SetMute(muted ? TRUE : FALSE, &m_eventContext);
  volume->Release();
  if (SUCCEEDED(hr)) m_host.Changed();
  return SUCCEEDED(hr);
}

bool RenameEndpoint(const std::wstring& endpointId, const std::wstring& name) {
  if (endpointId.empty() || name.empty()) return false;

  IPolicyConfig* policy = nullptr;
  if (FAILED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
                              IID_IPolicyConfig, (void**)&policy)) || !policy)
    return false;

  // PKEY_Device_DeviceDesc: the half of the name a person may change. The
  // other half, the interface name, is what Windows appends in brackets and is
  // not ours to touch.
  static const PROPERTYKEY kDeviceDesc = {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
      2};

  PROPVARIANT value;
  PropVariantInit(&value);
  value.vt = VT_LPWSTR;
  value.pwszVal = (LPWSTR)CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t));
  bool ok = false;
  if (value.pwszVal) {
    // Allocated at exactly this size two lines up, so truncation is
    // unreachable -- spelled with _TRUNCATE anyway so no call that can
    // __fastfail on overflow survives anywhere (issue 149).
    wcsncpy_s(value.pwszVal, name.size() + 1, name.c_str(), _TRUNCATE);
    ok = SUCCEEDED(policy->SetPropertyValue(endpointId.c_str(), kDeviceDesc,
                                            &value));
  }
  PropVariantClear(&value);
  policy->Release();
  return ok;
}

bool EndpointProvider::SetRouteDevice(const std::wstring& routeId,
                                      const std::wstring& endpointId) {
  if (routeId != kDefaultRenderRoute) return false;
  if (!m_watcher || !m_watcher->IsActive(endpointId)) return false;

  IPolicyConfig* policy = nullptr;
  if (FAILED(CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
                              IID_IPolicyConfig, (void**)&policy)) ||
      !policy) {
    // Only this route dies. Other providers' routes are unaffected, which is
    // the point of scoping the undocumented call to one place.
    m_health = ProviderHealth::Degraded;
    return false;
  }

  // All three roles, or an app that asks for a specific role keeps the old
  // device and the switch looks like it half-worked.
  //
  // The cost of that is worth stating: Windows keeps Console, Multimedia and
  // Communications separately, and a user may well have them pointed at
  // different devices -- Sonar routes apps that way. Writing all three
  // FLATTENS that arrangement, and it cannot be undone from here because the
  // previous per-role assignment is not recorded. Callers that only mean to
  // verify the call works must not use this: there is no such thing as a
  // no-op route write. Measured the hard way, by moving a media player
  // between Sonar channels while asserting nothing had changed.
  bool ok = SUCCEEDED(policy->SetDefaultEndpoint(endpointId.c_str(), eConsole));
  ok = SUCCEEDED(policy->SetDefaultEndpoint(endpointId.c_str(), eMultimedia)) && ok;
  ok = SUCCEEDED(policy->SetDefaultEndpoint(endpointId.c_str(), eCommunications)) && ok;
  policy->Release();

  if (ok) {
    m_watcher->Refresh();
    m_host.Changed();
  }
  return ok;
}

}  // namespace mdrop
