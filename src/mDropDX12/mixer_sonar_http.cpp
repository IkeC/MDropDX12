#include "mixer_sonar_http.h"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace mdrop {

namespace {

const wchar_t* const kCoreProps =
    L"C:\\ProgramData\\SteelSeries\\SteelSeries Engine 3\\coreProps.json";

std::wstring Widen(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
  return out;
}

// Pulls "key":"value" out of flat JSON without a parser. Used only for the two
// bootstrap documents, whose shape is fixed and tiny; everything richer goes
// through json_utils in the provider.
std::wstring FlatField(const std::wstring& text, const wchar_t* key) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t i = text.find(needle);
  if (i == std::wstring::npos) return std::wstring();
  i = text.find(L':', i + needle.size());
  if (i == std::wstring::npos) return std::wstring();
  const size_t open = text.find(L'"', i);
  if (open == std::wstring::npos) return std::wstring();
  const size_t close = text.find(L'"', open + 1);
  if (close == std::wstring::npos) return std::wstring();
  return text.substr(open + 1, close - open - 1);
}

struct Url {
  bool https = false;
  std::wstring host;
  INTERNET_PORT port = 0;
  std::wstring path;
};

bool SplitUrl(const std::wstring& url, const std::wstring& path, Url& out) {
  const size_t i = url.find(L"://");
  if (i == std::wstring::npos) return false;
  out.https = url.compare(0, 5, L"https") == 0;
  const std::wstring rest = url.substr(i + 3);
  const size_t colon = rest.find(L':');
  if (colon == std::wstring::npos) return false;
  out.host = rest.substr(0, colon);
  out.port = (INTERNET_PORT)_wtoi(rest.substr(colon + 1).c_str());
  out.path = path;
  return out.port != 0 && !out.host.empty();
}

// One request. Returns false for a transport failure or any non-2xx, and sets
// *connectionFailed when nothing answered at all -- the only case worth
// re-discovering for.
bool RawRequest(const wchar_t* verb, const Url& u, std::wstring* body,
                bool* connectionFailed) {
  if (connectionFailed) *connectionFailed = false;

  HINTERNET session = WinHttpOpen(L"MDropDX12/1.0",
                                  WINHTTP_ACCESS_TYPE_NO_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    if (connectionFailed) *connectionFailed = true;
    return false;
  }
  WinHttpSetTimeouts(session, SonarHttp::kConnectTimeoutMs,
                     SonarHttp::kConnectTimeoutMs,
                     SonarHttp::kReceiveTimeoutMs,
                     SonarHttp::kReceiveTimeoutMs);

  HINTERNET connect = WinHttpConnect(session, u.host.c_str(), u.port, 0);
  if (!connect) {
    WinHttpCloseHandle(session);
    if (connectionFailed) *connectionFailed = true;
    return false;
  }

  HINTERNET request = WinHttpOpenRequest(
      connect, verb, u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0);
  if (!request) {
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (connectionFailed) *connectionFailed = true;
    return false;
  }

  if (u.https) {
    // GG's certificate is self-signed for localhost. Relaxed for this one
    // loopback call and nowhere else -- everything after discovery is plain
    // HTTP to 127.0.0.1.
    DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                  SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                  SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags,
                     sizeof(flags));
  }

  bool ok = false;
  if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(request, nullptr)) {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    std::string raw;
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
      std::vector<char> chunk(avail);
      DWORD read = 0;
      if (!WinHttpReadData(request, chunk.data(), avail, &read) || read == 0)
        break;
      raw.append(chunk.data(), read);
    }
    ok = (status >= 200 && status < 300);
    if (body) *body = Widen(raw);
  } else if (connectionFailed) {
    *connectionFailed = true;
  }

  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

}  // namespace

bool SonarHttp::Discover() {
  m_base.clear();

  std::ifstream f(kCoreProps, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  const std::wstring props = Widen(ss.str());

  const std::wstring gg = FlatField(props, L"ggEncryptedAddress");
  if (gg.empty()) return false;

  Url u;
  if (!SplitUrl(L"https://" + gg, L"/subApps", u)) return false;

  std::wstring body;
  if (!RawRequest(L"GET", u, &body, nullptr)) return false;

  // "webServerAddress" appears for several sub-apps, so take the one AFTER the
  // "sonar" key or a neighbouring sub-app's address gets picked up instead.
  const size_t sonar = body.find(L"\"sonar\"");
  if (sonar == std::wstring::npos) return false;
  const std::wstring addr = FlatField(body.substr(sonar), L"webServerAddress");
  if (addr.empty()) return false;

  m_base = (addr.rfind(L"http", 0) == 0) ? addr : (L"http://" + addr);
  while (!m_base.empty() && m_base.back() == L'/') m_base.pop_back();
  return true;
}

bool SonarHttp::Request(const wchar_t* verb, const std::wstring& path,
                        std::wstring* body, bool allowRediscover) {
  if (!IsDiscovered() && !Discover()) return false;

  Url u;
  if (!SplitUrl(m_base, path, u)) return false;

  bool connectionFailed = false;
  if (RawRequest(verb, u, body, &connectionFailed)) return true;

  // Only a connection-level failure is worth retrying, and only once: a GG
  // that restarted onto a new port looks exactly like one that hung, and
  // retrying an HTTP status would just repeat a rejection.
  if (connectionFailed && allowRediscover && !m_pinned) {
    Forget();
    if (Discover()) return Request(verb, path, body, false);
  }
  return false;
}

bool SonarHttp::Get(const std::wstring& path, std::wstring& body) {
  body.clear();
  return Request(L"GET", path, &body, true);
}

bool SonarHttp::Put(const std::wstring& path) {
  return Request(L"PUT", path, nullptr, true);
}

}  // namespace mdrop
