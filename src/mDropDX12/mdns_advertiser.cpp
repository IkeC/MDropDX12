#include "mdns_advertiser.h"
#include <windows.h>
#include <windns.h>
#include <string>

// DnsServiceRegister / DnsServiceDeRegister are available on Windows 10 1803+.
// We load them dynamically so the binary still runs on older Windows builds.

using FnDnsServiceRegister   = DWORD(WINAPI*)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
using FnDnsServiceDeRegister = DWORD(WINAPI*)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
using FnDnsServiceFreeInstance = VOID(WINAPI*)(PDNS_SERVICE_INSTANCE);
using FnDnsServiceConstructInstance = PDNS_SERVICE_INSTANCE(WINAPI*)(
    PCWSTR, PCWSTR, PIP4_ARRAY, PIP6_ARRAY, WORD, WORD, DWORD, DWORD, WORD, PPCWSTR, PPCWSTR);

static FnDnsServiceRegister            s_DnsServiceRegister            = nullptr;
static FnDnsServiceDeRegister          s_DnsServiceDeRegister          = nullptr;
static FnDnsServiceFreeInstance        s_DnsServiceFreeInstance        = nullptr;
static FnDnsServiceConstructInstance  s_DnsServiceConstructInstance   = nullptr;
static bool s_loaded = false;

static bool LoadDnsFunctions() {
    if (s_loaded) return s_DnsServiceRegister != nullptr;
    s_loaded = true;
    HMODULE hDns = GetModuleHandleW(L"dnsapi.dll");
    if (!hDns) hDns = LoadLibraryW(L"dnsapi.dll");
    if (!hDns) return false;

    s_DnsServiceRegister           = (FnDnsServiceRegister)GetProcAddress(hDns, "DnsServiceRegister");
    s_DnsServiceDeRegister         = (FnDnsServiceDeRegister)GetProcAddress(hDns, "DnsServiceDeRegister");
    s_DnsServiceFreeInstance       = (FnDnsServiceFreeInstance)GetProcAddress(hDns, "DnsServiceFreeInstance");
    s_DnsServiceConstructInstance  = (FnDnsServiceConstructInstance)GetProcAddress(hDns, "DnsServiceConstructInstance");

    return s_DnsServiceRegister != nullptr
        && s_DnsServiceDeRegister != nullptr
        && s_DnsServiceFreeInstance != nullptr
        && s_DnsServiceConstructInstance != nullptr;
}

// Convert narrow UTF-8 string to wide string
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &result[0], n);
    return result;
}

// Static callback required by DnsServiceRegister — no-op (fire and forget).
static VOID WINAPI RegisterCallback(DWORD /*status*/, PVOID /*context*/, PDNS_SERVICE_INSTANCE /*instance*/) {
}

bool MdnsAdvertiser::Register(const std::string& serviceName, int port, int pid) {
    if (m_registered) Unregister();

    if (!LoadDnsFunctions()) return false;

    // Build the fully-qualified service instance name:
    // "<serviceName>._milkwave._tcp.local"
    std::wstring instanceName = Utf8ToWide(serviceName) + L"._milkwave._tcp.local";

    // TXT key/value pairs: version=1, pid=<pid>
    std::wstring txtVersion = L"version=1";
    std::wstring txtPid     = L"pid=" + std::to_wstring(pid);

    PCWSTR keys[]   = { L"version", L"pid" };
    PCWSTR values[] = { L"1", nullptr };
    wchar_t pidBuf[32];
    swprintf_s(pidBuf, L"%d", pid);
    values[1] = pidBuf;

    m_instance = s_DnsServiceConstructInstance(
        instanceName.c_str(),   // pServiceName
        nullptr,                // pHostName (use local host)
        nullptr,                // IP4 address (auto)
        nullptr,                // IP6 address (auto)
        (WORD)port,             // wPort
        0,                      // wPriority
        0,                      // wWeight
        0,                      // dwPropertyCount (unused)
        2,                      // dwTxtCount
        keys,                   // keys
        values                  // values
    );

    if (!m_instance) return false;

    ZeroMemory(&m_request, sizeof(m_request));
    m_request.Version              = DNS_QUERY_REQUEST_VERSION1;
    m_request.pServiceInstance     = m_instance;
    m_request.pRegisterCompletionCallback = RegisterCallback;
    m_request.pQueryContext        = nullptr;
    m_request.hCredentials         = nullptr;
    m_request.unicastEnabled       = FALSE;

    DWORD result = s_DnsServiceRegister(&m_request, nullptr);
    // DNS_REQUEST_PENDING (9506) is the expected success return for async registration.
    if (result == DNS_REQUEST_PENDING || result == ERROR_SUCCESS) {
        m_registered = true;
        return true;
    }

    // Registration failed — free the instance and bail out silently.
    s_DnsServiceFreeInstance(m_instance);
    m_instance = nullptr;
    return false;
}

void MdnsAdvertiser::Unregister() {
    if (!m_registered) return;
    m_registered = false;

    if (LoadDnsFunctions() && m_instance) {
        s_DnsServiceDeRegister(&m_request, nullptr);
    }

    if (s_DnsServiceFreeInstance && m_instance) {
        s_DnsServiceFreeInstance(m_instance);
    }
    m_instance = nullptr;
}
