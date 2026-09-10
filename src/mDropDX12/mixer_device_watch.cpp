#include "mixer_device_watch.h"

#include <functiondiscoverykeys_devpkey.h>
#include <cfgmgr32.h>

#include <map>
#include <set>

#pragma comment(lib, "cfgmgr32.lib")

namespace mdrop {

namespace {

// Returns the raw EndpointFormFactor, or -1 when the device does not say.
//
// Read once and asked two questions, because both answers come from it: is
// this a monitor rather than a listening device, and is this a headset's
// hands-free profile rather than its stereo one.
int ReadFormFactor(IMMDevice* device) {
  IPropertyStore* props = nullptr;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props)
    return -1;
  PROPVARIANT pv;
  PropVariantInit(&pv);
  int formFactor = -1;
  // Defined here rather than linked. The symbol lives in a UUID library the
  // app happens to pull in and the native tests do not, and one PROPERTYKEY is
  // cheaper than a link dependency -- the same choice the endpoint rename makes
  // for PKEY_Device_DeviceDesc.
  static const PROPERTYKEY kFormFactor = {
      {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}},
      0};
  if (SUCCEEDED(props->GetValue(kFormFactor, &pv)) && pv.vt == VT_UI4)
    formFactor = (int)pv.uintVal;
  PropVariantClear(&pv);
  props->Release();
  return formFactor;
}

// DigitalAudioDisplayDevice is the HDMI/DisplayPort category; SPDIF and
// UnknownDigitalPassthrough sit beside it. Everything else -- speakers,
// headphones, headsets, microphones, and the UnknownFormFactor that virtual
// devices like Sonar's report -- is a real audio destination.
bool IsDisplayFormFactor(int ff) {
  return ff == DigitalAudioDisplayDevice || ff == SPDIF ||
         ff == UnknownDigitalPassthrough;
}

// The hands-free half of a Bluetooth headset, as opposed to its stereo half.
//
// Windows publishes one physical headset as two endpoints that share a
// ContainerId and a battery reading and differ by one word in the name:
// "Headphones (X)" is A2DP, stereo and high quality, and "Headset (X)" enables
// the microphone at the cost of dropping to mono narrowband. They are not
// interchangeable -- Shane has to physically change headsets when something
// activates the second one -- so anything that offers a device to choose from
// must keep them apart.
//
// Taken from the form factor rather than from the name. The name test is
// tempting and wrong: this app carries its OWN names for these devices, and
// Windows names have been edited too, so the endpoints most in need of the
// distinction are exactly the ones whose names no longer contain "Headset".
// Measured with endpoint_formfactor_probe.cpp -- Headphones reports 3 and
// Headset reports 5, cleanly, across every device on this machine.
bool IsHandsFreeFormFactor(int ff) { return ff == Headset; }

// The last-write time of an endpoint's registry key.
//
// MMDevice itself exposes no "last connected" property, but Windows keeps one
// key per endpoint under MMDevices\\Audio and rewrites it as the device's state
// changes. An endpoint id looks like "{0.0.0.00000000}.{guid}", and the guid
// half is the key name.
unsigned long long ReadLastSeen(const std::wstring& endpointId, bool isRender) {
  const size_t brace = endpointId.rfind(L'{');
  if (brace == std::wstring::npos) return 0;

  std::wstring path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\";
  path += isRender ? L"Render\\" : L"Capture\\";
  path += endpointId.substr(brace);

  HKEY key = nullptr;
  // WOW64_64KEY because a 32-bit build would otherwise be redirected to a view
  // of the registry that does not hold these.
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                    KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
    return 0;
  FILETIME written = {};
  const LONG r = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr,
                                  nullptr, &written);
  RegCloseKey(key);
  if (r != ERROR_SUCCESS) return 0;
  return ((unsigned long long)written.dwHighDateTime << 32) |
         written.dwLowDateTime;
}

// ── Bluetooth battery ─────────────────────────────────────────────────────
//
// Windows knows the battery level of a connected Bluetooth headset -- it is
// what Settings shows -- but it does not keep it where anything about audio
// would look. It is a PnP device property, and it is on a THIRD node:
//
//   SWD\MMDEVAPI\{0.0.0.00000000}.{guid}          the audio endpoint
//   BTHENUM\DEV_<mac>\...                         the Bluetooth device
//   BTHENUM\{0000111E-...}\...&<mac>_C00000000    "<name> Hands-Free AG"  <-- here
//
// Only the last one carries it, its device class is System rather than
// Bluetooth or AudioEndpoint, and nothing about it is persisted: a sweep of
// the registry under Enum, DeviceContainers and BTHPORT finds no trace. It has
// to be read live through the configuration manager.
//
// The two nodes are joined by ContainerId, which Windows assigns per physical
// device and which both of them carry. That is why no MAC address has to be
// parsed out of an instance path.
//
// Only devices exposing the hands-free profile report at all, which in
// practice means headsets and earbuds -- exactly the devices a failover list
// is made of.

// Spelled out rather than linked, the same choice ReadIsDisplayAudio makes for
// PKEY_AudioEndpoint_FormFactor: devpkey.h declares these extern and the
// definitions live in a library the native tests do not link.
const DEVPROPKEY kDevPkeyContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}},
    2};
const DEVPROPKEY kDevPkeyBluetoothBattery = {
    {0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}},
    2};
// PKEY_DeviceInterface_Bluetooth_LastConnectedTime. When this device last
// connected, which is what "last seen" is actually asking.
//
// The value is a FILETIME expressed in LOCAL time, not UTC -- see
// ToUtcFromLocalFileTime below for how that was established and why it matters.
const DEVPROPKEY kDevPkeyBluetoothLastConnected = {
    {0x2bd67d8b, 0x8beb, 0x48d5, {0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a}},
    11};

// DEVPKEY_Device_IsPresent -- whether this node is attached right now, as
// distinct from whether Windows remembers it.
const DEVPROPKEY kDevPkeyDeviceIsPresent = {
    {0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}},
    5};

// What the Bluetooth stack knows about one physical device.
struct BluetoothInfo {
  int battery = -1;
  // When it last connected, as a UTC FILETIME (0 = the stack has no record).
  unsigned long long lastConnected = 0;
};

bool ReadBool(DEVINST inst, const DEVPROPKEY& key) {
  DEVPROPTYPE type = 0;
  DEVPROP_BOOLEAN value = DEVPROP_FALSE;
  ULONG size = sizeof(value);
  if (CM_Get_DevNode_PropertyW(inst, &key, &type, (PBYTE)&value, &size, 0) !=
          CR_SUCCESS ||
      type != DEVPROP_TYPE_BOOLEAN)
    return false;
  return value != DEVPROP_FALSE;
}

// A FILETIME the source expressed in LOCAL time, as UTC.
//
// DEVPKEY_Bluetooth_LastConnectedTime is stored in local time. That is
// measured, not assumed, and it matters enough to record how: this property
// was tried once before and REJECTED because it "could not be reconciled with
// the registry time for the same device -- two headsets sat exactly one
// timezone offset away". They did, and this is why: converting an
// already-local value with FileTimeToLocalFileTime subtracts the offset a
// second time.
//
// The rest of that earlier rejection ("a third was eleven and a half hours
// out, which no single offset explains") was the REFERENCE being wrong rather
// than this value. The registry time it was compared against is the endpoint
// key's last-write, and Windows rewrites every endpoint key in bulk whenever
// the audio stack re-enumerates -- so for most devices it is simply the last
// boot and says nothing about that device at all.
//
// bt_seen_reconcile_probe.cpp settles it on this machine: of fourteen
// Bluetooth containers, exactly one has an audio endpoint whose registry stamp
// is NOT the boot, and for that one the local reading matches to the second
// (06:10:29 against 06:10:28) while the UTC reading is five hours out.
//
// Converting to UTC at all is worth justifying, since the value arrives local
// and is displayed local: it makes the two sources of lastSeen the SAME unit.
// The registry fallback is genuinely UTC, and a field that is sometimes one and
// sometimes the other is how the original confusion happened in the first
// place.
//
// The round trip is exact for display. This converts local -> UTC with the
// current daylight-saving state and FormatLastSeen converts UTC -> local with
// the same one, so the two cancel and the shown time is the wall-clock Windows
// recorded, whatever the offset was on the day. The stored UTC value can be an
// hour out for a timestamp from the other side of a DST change -- which would
// matter if it were ever compared against a real UTC instant, and nothing does
// that. Noted rather than fixed: carrying a timezone history would cost far
// more than it could buy for "which headset have I not used for longest".
unsigned long long ToUtcFromLocalFileTime(const FILETIME& local) {
  FILETIME utc = {};
  if (!LocalFileTimeToFileTime(&local, &utc)) return 0;
  return ((unsigned long long)utc.dwHighDateTime << 32) | utc.dwLowDateTime;
}

unsigned long long ReadLastConnected(DEVINST inst) {
  DEVPROPTYPE type = 0;
  FILETIME ft = {};
  ULONG size = sizeof(ft);
  if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyBluetoothLastConnected, &type,
                               (PBYTE)&ft, &size, 0) != CR_SUCCESS ||
      type != DEVPROP_TYPE_FILETIME)
    return 0;
  return ToUtcFromLocalFileTime(ft);
}

std::wstring ReadContainerId(DEVINST inst) {
  DEVPROPTYPE type = 0;
  GUID container = {};
  ULONG size = sizeof(container);
  if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyContainerId, &type,
                               (PBYTE)&container, &size, 0) != CR_SUCCESS ||
      type != DEVPROP_TYPE_GUID)
    return std::wstring();
  wchar_t text[64] = {};
  StringFromGUID2(container, text, 64);

  // The NULL container is not an identity, and treating it as one would be a
  // silent disaster.
  //
  // Windows gives {00000000-0000-0000-FFFF-FFFFFFFFFFFF} to every device that
  // belongs to no physical container at all -- on this machine that is all six
  // SteelSeries Sonar virtual endpoints, NVIDIA Broadcast and the GS Wavetable
  // Synth, sharing one value. Since the battery join is BY container, a single
  // Bluetooth node falling into it would paste that device's battery and its
  // last-connected time onto every one of them at once.
  //
  // Rejected here rather than at each use, so no future join can reintroduce
  // it by forgetting.
  static const wchar_t* const kNoContainer =
      L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";
  if (_wcsicmp(text, kNoContainer) == 0) return std::wstring();
  return text;
}

// A percentage, but NOT stored as a UINT32.
//
// Windows writes it as DEVPROP_TYPE_BYTE. A read that insists on UINT32 gets
// CR_SUCCESS back with the wrong type and quietly finds nothing, which is a
// silent wrong answer rather than a failure -- the probe that established all
// of this reported "0 of 14 endpoints have a battery" on a machine where seven
// devices were plainly reporting one. Both widths are accepted so a future
// Windows widening the field cannot reintroduce that.
int ReadBatteryPercent(DEVINST inst) {
  DEVPROPTYPE type = 0;
  BYTE raw[8] = {};
  ULONG size = sizeof(raw);
  if (CM_Get_DevNode_PropertyW(inst, &kDevPkeyBluetoothBattery, &type, raw,
                               &size, 0) != CR_SUCCESS)
    return -1;
  int value = -1;
  if (type == DEVPROP_TYPE_BYTE && size >= 1) value = raw[0];
  else if (type == DEVPROP_TYPE_UINT32 && size >= 4) value = (int)*(ULONG*)raw;
  return (value >= 0 && value <= 100) ? value : -1;
}

// ContainerId -> what the Bluetooth stack knows, for every present node.
//
// Filtered to the BTHENUM enumerator rather than walking all ~500 present
// devices: every node carrying either fact is under it, and asking for the
// whole machine costs several milliseconds to reach the same answer.
//
// One sweep for both, because the two facts live on DIFFERENT nodes of the
// same physical device -- battery on "<name> Hands-Free AG", last-connected on
// "BTHENUM\DEV_<mac>" -- and every node of a device shares its ContainerId.
// So each is merged into the entry as it is found, and neither overwrites the
// other with a blank.
std::map<std::wstring, BluetoothInfo> BluetoothByContainer() {
  std::map<std::wstring, BluetoothInfo> out;
  ULONG len = 0;
  // NOT filtered to present devices, and located as PHANTOM.
  //
  // Both halves of this used to ask only about connected hardware, which made
  // the whole last-connected lookup inert: a disconnected device has no
  // present node, so nothing was found for it and it fell back to the registry
  // -- and a disconnected device is the ONLY case where "when did I last see
  // this" is a question worth asking. The failure was silent, because the
  // fallback produces a plausible timestamp.
  //
  // A phantom devnode is one Windows still remembers but that is not attached.
  // Its properties, including the last-connected time, are exactly what is
  // wanted here.
  // Several enumerators, not just BTHENUM.
  //
  // One headset's nodes are spread across them: BTHENUM carries the device and
  // its A2DP endpoint, BTHHFENUM carries the hands-free audio, BTHLE the
  // low-energy side. Sweeping only BTHENUM built a container set that was
  // missing every hands-free container -- so the very endpoints this is used
  // to identify were the ones it could not see, and every one of them went
  // unmarked while the code looked correct.
  const ULONG filter = CM_GETIDLIST_FILTER_ENUMERATOR;
  static const wchar_t* const kEnumerators[] = {L"BTHENUM", L"BTHHFENUM",
                                                L"BTHLE", L"BTHLEDevice"};
  std::vector<std::wstring> nodes;
  for (const wchar_t* enumerator : kEnumerators) {
    len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, enumerator, filter) != CR_SUCCESS || !len)
      continue;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(enumerator, buf.data(), len, filter) != CR_SUCCESS)
      continue;
    for (const wchar_t* id = buf.data(); *id; id += wcslen(id) + 1)
      nodes.push_back(id);
  }

  for (const std::wstring& node : nodes) {
    DEVINST inst = 0;
    if (CM_Locate_DevNodeW(&inst, (DEVINSTID_W)node.c_str(),
                           CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
      continue;
    const std::wstring container = ReadContainerId(inst);
    if (container.empty()) continue;

    // The entry exists for EVERY Bluetooth container, battery or not.
    //
    // Its presence is itself an answer: "is this endpoint a Bluetooth device".
    // That is what stops the hands-free marking from firing on things that
    // merely report the same form factor -- SteelSeries Sonar's Chat channel
    // is a virtual render device that reports Headset, and marking it "low
    // quality" and burying it at the bottom of the picker would be wrong, since
    // it is a channel Shane chooses deliberately.
    BluetoothInfo& info = out[container];

    // Last-connected BEFORE the present-only gate below.
    //
    // A disconnected device is the only case where "when did I last see this"
    // is a question worth asking, and a disconnected device has only a phantom
    // node -- so reading this after the gate would answer it for exactly the
    // devices that do not need it.
    //
    // The newest across the device's nodes wins. One headset spreads over
    // several (BTHENUM carries the device and its A2DP endpoint, BTHHFENUM the
    // hands-free audio) and they do not all carry the same value; the most
    // recent is the one that means "last used".
    const unsigned long long seen = ReadLastConnected(inst);
    if (seen > info.lastConnected) info.lastConnected = seen;

    // Battery only from a node that is ATTACHED. Phantom nodes are included
    // above so a disconnected headset still registers as Bluetooth, but their
    // battery is whatever it read on the way out -- one of these earbud sets
    // has said 1% for months in its case.
    if (!ReadBool(inst, kDevPkeyDeviceIsPresent)) continue;
    const int percent = ReadBatteryPercent(inst);
    if (percent >= 0) info.battery = percent;
  }
  return out;
}

// The container an audio endpoint belongs to, or empty.
std::wstring ContainerForEndpoint(const std::wstring& endpointId) {
  if (endpointId.empty()) return std::wstring();
  DEVINST inst = 0;
  const std::wstring node = L"SWD\\MMDEVAPI\\" + endpointId;
  // PHANTOM for the same reason the sweep above uses it: the endpoint list
  // deliberately includes devices that are switched off, and NORMAL refuses to
  // locate their nodes -- so the lookup failed for precisely the devices whose
  // last-connected time is the interesting one.
  if (CM_Locate_DevNodeW(&inst, (DEVINSTID_W)node.c_str(),
                         CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS)
    return std::wstring();
  return ReadContainerId(inst);
}

// The container from the endpoint's REGISTRY key, for a device that is away.
//
// ContainerForEndpoint above resolves an SWD\MMDEVAPI node, and that node only
// resolves while the endpoint is ACTIVE -- the exact opposite of when this is
// needed, since "when did I last see this" is only ever asked about something
// that is NOT here. Windows also stores DEVPKEY_Device_ContainerId on the
// endpoint's own registry key, and that survives the device being switched off.
//
// The stored form is a serialised PROPVARIANT: a 4-byte vt, four bytes of
// flags, then the payload. vt 72 is VT_CLSID and the GUID begins at offset 8.
// Verified against real keys rather than assumed -- a wrong offset would yield
// a plausible-looking GUID that quietly joins to nothing, which is the same
// class of silent failure the battery type-width mistake was.
//
// Deliberately NOT folded into ContainerForEndpoint. That one gates isVirtual
// and isHandsFree, both of which are written to mean "the node resolved while
// active"; making it answer for inactive endpoints too would change which rows
// are hidden as provider-owned (forgejo#51) and which are marked hands-free.
// This is a second, narrower question with its own answer.
std::wstring ContainerFromRegistry(const std::wstring& endpointId) {
  const size_t brace = endpointId.rfind(L'{');
  if (brace == std::wstring::npos) return std::wstring();
  // The flow is encoded in the id, but trying both costs one failed open and
  // avoids depending on that encoding.
  static const wchar_t* const kFlows[] = {L"Render\\", L"Capture\\"};
  for (const wchar_t* flow : kFlows) {
    std::wstring path =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\";
    path += flow;
    path += endpointId.substr(brace);
    path += L"\\Properties";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
      continue;
    BYTE blob[64] = {};
    DWORD size = sizeof(blob), type = 0;
    const LONG r = RegQueryValueExW(
        key, L"{8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c},2", nullptr, &type,
        blob, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_BINARY || size < 24) continue;
    DWORD vt = 0;
    memcpy(&vt, blob, sizeof(vt));
    if (vt != 72) continue;                 // VT_CLSID
    GUID container = {};
    memcpy(&container, blob + 8, sizeof(container));
    wchar_t text[64];
    if (!StringFromGUID2(container, text, 64)) continue;
    return text;
  }
  return std::wstring();
}

const BluetoothInfo* BluetoothForEndpoint(
    const std::wstring& endpointId,
    const std::map<std::wstring, BluetoothInfo>& byContainer) {
  if (byContainer.empty()) return nullptr;
  const std::wstring container = ContainerForEndpoint(endpointId);
  if (container.empty()) return nullptr;
  const auto it = byContainer.find(container);
  return it == byContainer.end() ? nullptr : &it->second;
}

// The battery pass, shared by Refresh() and RefreshBattery() so the two cannot
// disagree. They disagreeing IS the original bug: they held separate copies of
// the same rule, one of them was corrected, and the other kept overwriting it
// once a second.
//
// The gate is whether the DEVICE is connected, which is not the same question
// as whether THIS endpoint is active, and confusing the two has now failed in
// both directions:
//
//   isActive per endpoint   blanked the battery of a headset that had just
//                           come back, because a reconnecting device's
//                           hands-free node has no battery to read yet
//   DEVPKEY_Device_IsPresent
//                           filtered nothing at all. A paired Bluetooth device
//                           keeps a present device node while it sits in its
//                           case, so every disconnected headset went on
//                           reporting the figure it left behind -- measured
//                           here as five separate WF-1000XM5 pairs and a Razer
//                           showing 19%, 51% and 100% with nothing switched on.
//
// A CONTAINER is the right unit because it is the physical device: Windows
// publishes one headset as an A2DP endpoint and a hands-free endpoint sharing
// a ContainerId, and they share the battery too. So the container counts as
// connected when ANY of its endpoints is active, which reads the battery for
// both halves the moment either one comes up -- and reads it for neither when
// the device is away.
void ApplyBattery(std::vector<EndpointInfo>& endpoints,
                  const std::map<std::wstring, BluetoothInfo>& bluetooth) {
  std::set<std::wstring> connected;
  for (const EndpointInfo& e : endpoints)
    if (e.isActive && !e.containerId.empty()) connected.insert(e.containerId);

  for (EndpointInfo& e : endpoints) {
    e.batteryPercent = -1;
    if (e.containerId.empty() || !connected.count(e.containerId)) continue;
    const auto it = bluetooth.find(e.containerId);
    if (it != bluetooth.end()) e.batteryPercent = it->second.battery;
  }
}

std::wstring ReadFriendlyName(IMMDevice* device) {
  IPropertyStore* props = nullptr;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props)
    return std::wstring();
  PROPVARIANT pv;
  PropVariantInit(&pv);
  std::wstring name;
  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) &&
      pv.vt == VT_LPWSTR && pv.pwszVal)
    name = pv.pwszVal;
  PropVariantClear(&pv);
  props->Release();
  return name;
}

std::wstring ReadId(IMMDevice* device) {
  LPWSTR raw = nullptr;
  if (FAILED(device->GetId(&raw)) || !raw) return std::wstring();
  std::wstring id = raw;
  CoTaskMemFree(raw);
  return id;
}

}  // namespace

// The COM sink. Every callback does the same thing: refresh the cache and tell
// the owner. No filtering, because a device appearing, disappearing, being
// disabled, or becoming the default all change what failover may do.
class DeviceWatcher::Notify : public IMMNotificationClient {
 public:
  explicit Notify(DeviceWatcher* owner) : m_owner(owner) {}

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
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
      *ppv = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
    Bump();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Bump(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Bump(); return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
    Bump();
    return S_OK;
  }
  // Deliberately ignored: it fires constantly and never changes presence.
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }

 private:
  void Bump() {
    if (!m_owner) return;
    m_owner->Refresh();
    if (m_owner->m_onDeviceChange) m_owner->m_onDeviceChange();
  }

  LONG m_ref = 1;
  DeviceWatcher* m_owner = nullptr;
};

DeviceWatcher::~DeviceWatcher() { Stop(); }

bool DeviceWatcher::Start(std::function<void()> onDeviceChange) {
  if (m_enumerator) return true;
  m_onDeviceChange = std::move(onDeviceChange);

  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              (void**)&m_enumerator)) ||
      !m_enumerator)
    return false;

  m_notify = new Notify(this);
  if (FAILED(m_enumerator->RegisterEndpointNotificationCallback(m_notify))) {
    // Presence still works, it just stops being live. Better than failing to
    // start the whole subsystem over a notification registration.
    m_notify->Release();
    m_notify = nullptr;
  }
  Refresh();
  return true;
}

void DeviceWatcher::Stop() {
  // Unregister before releasing, or the endpoint keeps a dangling sink.
  if (m_notify && m_enumerator)
    m_enumerator->UnregisterEndpointNotificationCallback(m_notify);
  if (m_notify) {
    m_notify->Release();
    m_notify = nullptr;
  }
  if (m_enumerator) {
    m_enumerator->Release();
    m_enumerator = nullptr;
  }
  m_onDeviceChange = nullptr;
}

void DeviceWatcher::Refresh() {
  if (!m_enumerator) return;

  // Once for the whole enumeration, not once per endpoint.
  const std::map<std::wstring, BluetoothInfo> bluetooth = BluetoothByContainer();

  std::vector<EndpointInfo> found;
  for (int pass = 0; pass < 2; pass++) {
    const bool isRender = (pass == 0);
    IMMDeviceCollection* collection = nullptr;
    // Not just ACTIVE.
    //
    // A failover allowlist is a list of devices to move to WHEN SOMETHING
    // GOES AWAY, so the entries most worth offering are exactly the ones
    // switched off right now -- a pair of Bluetooth headphones is unplugged
    // far more often than it is connected. Enumerating only active endpoints
    // meant Shane's four other Sony sets could not be picked at all.
    //
    // Every state Windows knows, including disabled ones -- a device turned
    // off in Windows is still a device you own and may turn back on, and
    // hiding it is a worse answer than listing it as not connected.
    //
    // Connected or not is the whole distinction: `isActive` carries it, and
    // everything that needs a live device keeps checking it. There is no finer
    // grading, because unplugged, not-present and disabled all mean the same
    // thing to someone choosing where audio should go next.
    if (FAILED(m_enumerator->EnumAudioEndpoints(isRender ? eRender : eCapture,
                                                DEVICE_STATEMASK_ALL,
                                                &collection)) ||
        !collection)
      continue;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
      IMMDevice* device = nullptr;
      if (FAILED(collection->Item(i, &device)) || !device) continue;
      EndpointInfo info;
      info.id = ReadId(device);
      info.friendlyName = ReadFriendlyName(device);
      info.windowsName = info.friendlyName;
      const int formFactor = ReadFormFactor(device);
      info.isDisplayAudio = IsDisplayFormFactor(formFactor);
      info.lastSeen = ReadLastSeen(info.id, isRender);
      // Our name wins if we have one. Applied here so every consumer agrees.
      // windowsName keeps the original, because an allowlist entry added
      // before a rename still holds the old one.
      if (m_nameHook) {
        const std::wstring ours = m_nameHook(info.id, info.windowsName);
        if (!ours.empty()) info.friendlyName = ours;
      }
      info.isRender = isRender;
      DWORD state = DEVICE_STATE_NOTPRESENT;
      device->GetState(&state);
      info.isActive = (state == DEVICE_STATE_ACTIVE);

      // A hands-free profile belongs to REAL hardware.
      //
      // The form factor alone is not enough: SteelSeries Sonar's Chat channel
      // is a virtual render device that also reports Headset, and marking it
      // "low quality" and sorting it to the bottom of the picker would be
      // wrong -- it is a channel chosen deliberately, not a degraded copy of
      // the one above it.
      //
      // Having a container is what separates them. Windows puts every device
      // belonging to no physical container into the null container, which is
      // where all six Sonar endpoints, NVIDIA Broadcast and the GS Wavetable
      // Synth live; ReadContainerId already returns empty for it. A real
      // headset has a real container.
      //
      // Two other tests were tried and do not work, recorded so they are not
      // retried: the endpoint's PARENT never reaches BTH*, because an
      // SWD MMDEVAPI endpoint is a software device whose parent is not the
      // hardware node; and membership of the container set swept from the BTH*
      // enumerators misses the hands-free containers entirely.
      // The container test is only applied where it can be ANSWERED.
      //
      // An endpoint's device node only resolves while the endpoint is active,
      // which is why the battery join works and this did not: every hands-free
      // endpoint here is currently disconnected, so the container came back
      // empty and the flag was never set for anything. The false positive this
      // guards against -- Sonar's Chat channel -- is a virtual device and is
      // therefore always active, so restricting the test to active endpoints
      // still catches it.
      info.isHandsFree =
          IsHandsFreeFormFactor(formFactor) &&
          (!info.isActive || !ContainerForEndpoint(info.id).empty());

      // No container and present: a software endpoint something else owns.
      // Same read as above, and only meaningful while active for the same
      // reason -- an inactive endpoint's device node does not resolve, so it
      // is left unmarked rather than guessed at. Everything this is for is
      // virtual and therefore always active.
      // Resolved once and kept: ApplyBattery needs it below, and the battery
      // tick needs it every second. It is a registry read.
      info.containerId = ContainerForEndpoint(info.id);
      info.isVirtual = info.isActive && info.containerId.empty();

      const BluetoothInfo* bt = BluetoothForEndpoint(info.id, bluetooth);
      // Battery is NOT set here. It needs to know whether any OTHER endpoint of
      // this same physical device is active, which is not knowable until the
      // whole list exists -- so ApplyBattery does it once, below, and
      // RefreshBattery calls the same function. See ApplyBattery for why the
      // container rather than this endpoint is the thing to ask.

      // Last seen has to join while the device is AWAY, which the join above
      // cannot do -- its node has gone with it. Fall back to the container
      // recorded on the endpoint's registry key, which has not.
      if (!bt && !bluetooth.empty()) {
        const std::wstring container = ContainerFromRegistry(info.id);
        if (!container.empty()) {
          const auto it = bluetooth.find(container);
          if (it != bluetooth.end()) bt = &it->second;
        }
      }

      // The Bluetooth stack's own record WINS over the registry time.
      //
      // The registry value is the endpoint key's last-WRITE time, and Windows
      // rewrites every endpoint key when the audio stack re-enumerates -- so
      // after a reboot every device reads as "seen at boot" and the column
      // that exists to rank devices by recency ranks them all equal. Measured
      // here: one boot stamped 33 endpoints with the same second.
      //
      // This one is per-device, survives reboots, and is already populated
      // months back, so the ranking is right on the first run rather than only
      // after this app has been watching for weeks.
      //
      // Bluetooth only, and that is enough. Windows treats everything else as
      // permanently connected, so "when did I last see it" barely applies to a
      // monitor or an onboard output -- and the devices worth rotating to rest
      // their batteries are exactly the ones this covers.
      if (bt && bt->lastConnected) info.lastSeen = bt->lastConnected;
      device->Release();
      if (!info.id.empty() && !info.friendlyName.empty())
        found.push_back(info);
    }
    collection->Release();
  }

  std::wstring defaultRender;
  IMMDevice* def = nullptr;
  if (SUCCEEDED(m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)) &&
      def) {
    defaultRender = ReadId(def);
    def->Release();
  }

  // After the list is complete, because the gate is per physical device and a
  // device's two endpoints are not adjacent in the enumeration.
  ApplyBattery(found, bluetooth);

  std::lock_guard<std::mutex> lock(m_mutex);
  m_endpoints.swap(found);
  m_defaultRender = defaultRender;
}

int DeviceWatcher::BatteryFor(const std::wstring& id) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const EndpointInfo& e : m_endpoints)
    if (e.id == id) return e.batteryPercent;
  return -1;
}

int DeviceWatcher::RefreshBattery() {
  // Built outside the lock: it is the slow half, it touches nothing of ours,
  // and holding the mutex across it would block every reader of the snapshot
  // for no reason.
  const std::map<std::wstring, BluetoothInfo> bluetooth = BluetoothByContainer();

  std::lock_guard<std::mutex> lock(m_mutex);
  // The same function Refresh() uses, rather than a second copy of the rule.
  // The two holding separate copies is what let one of them be corrected while
  // the other went on overwriting it once a second.
  ApplyBattery(m_endpoints, bluetooth);

  int reporting = 0;
  for (const EndpointInfo& e : m_endpoints)
    if (e.batteryPercent >= 0) reporting++;
  return reporting;
}

std::vector<EndpointInfo> DeviceWatcher::Endpoints() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_endpoints;
}

bool DeviceWatcher::IsActive(const std::wstring& id) const {
  if (id.empty()) return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  for (const EndpointInfo& e : m_endpoints)
    if (e.id == id) return e.isActive;
  return false;
}

std::wstring DeviceWatcher::DefaultRenderId() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_defaultRender;
}

}  // namespace mdrop
