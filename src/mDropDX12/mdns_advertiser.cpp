#include "mdns_advertiser.h"
#include <string>

// DnsServiceRegister / DnsServiceDeRegister are available on Windows 10 1803+.
// We load them dynamically so the binary still runs on older Windows builds.
// Use opaque function pointer types to avoid pulling in additional SDK headers.

static HMODULE s_hDns = nullptr;

typedef PDNS_SERVICE_INSTANCE (WINAPI* FnConstructInstance)(
    PCWSTR, PCWSTR, void*, void*, WORD, WORD, DWORD, DWORD, WORD, const PCWSTR*, const PCWSTR*);
typedef DWORD (WINAPI* FnRegister)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
typedef DWORD (WINAPI* FnDeRegister)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
typedef VOID  (WINAPI* FnFreeInstance)(PDNS_SERVICE_INSTANCE);

static FnConstructInstance s_Construct = nullptr;
static FnRegister         s_Register  = nullptr;
static FnDeRegister       s_DeRegister = nullptr;
static FnFreeInstance     s_Free       = nullptr;
static bool s_loaded = false;

static bool LoadDnsFunctions() {
    if (s_loaded) return s_Register != nullptr;
    s_loaded = true;
    s_hDns = LoadLibraryW(L"dnsapi.dll");
    if (!s_hDns) return false;

    s_Construct  = (FnConstructInstance)GetProcAddress(s_hDns, "DnsServiceConstructInstance");
    s_Register   = (FnRegister)GetProcAddress(s_hDns, "DnsServiceRegister");
    s_DeRegister = (FnDeRegister)GetProcAddress(s_hDns, "DnsServiceDeRegister");
    s_Free       = (FnFreeInstance)GetProcAddress(s_hDns, "DnsServiceFreeInstance");

    return s_Construct && s_Register && s_DeRegister && s_Free;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &result[0], n);
    return result;
}

static VOID WINAPI RegisterCallback(DWORD, PVOID, PDNS_SERVICE_INSTANCE) {}

bool MdnsAdvertiser::Register(const std::string& serviceName, int port, int pid) {
    if (m_registered) Unregister();
    if (!LoadDnsFunctions()) return false;

    std::wstring instanceName = Utf8ToWide(serviceName) + L"._milkwave._tcp.local";

    wchar_t pidBuf[32];
    swprintf_s(pidBuf, L"%d", pid);
    const PCWSTR keys[]   = { L"version", L"pid" };
    const PCWSTR values[] = { L"1", pidBuf };

    m_instance = s_Construct(
        instanceName.c_str(), nullptr, nullptr, nullptr,
        (WORD)port, 0, 0, 0, 2, keys, values);
    if (!m_instance) return false;

    ZeroMemory(&m_request, sizeof(m_request));
    m_request.Version                     = DNS_QUERY_REQUEST_VERSION1;
    m_request.pServiceInstance            = m_instance;
    m_request.pRegisterCompletionCallback = RegisterCallback;

    DWORD result = s_Register(&m_request, nullptr);
    if (result == DNS_REQUEST_PENDING || result == ERROR_SUCCESS) {
        m_registered = true;
        return true;
    }

    s_Free(m_instance);
    m_instance = nullptr;
    return false;
}

void MdnsAdvertiser::Unregister() {
    if (!m_registered) return;
    m_registered = false;

    if (s_DeRegister && m_instance)
        s_DeRegister(&m_request, nullptr);
    if (s_Free && m_instance)
        s_Free(m_instance);
    m_instance = nullptr;
}
