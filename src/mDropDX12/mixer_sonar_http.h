// mixer_sonar_http.h — transport to the SteelSeries Sonar local API.
//
// Knows nothing about mixer channels. Its whole job is: find the sub-app,
// speak to it, and never block a caller for long.
//
// Discovery is coreProps.json -> ggEncryptedAddress -> ONE HTTPS GET /subApps
// -> the sonar sub-app's plain-HTTP address. Only that first call needs TLS,
// and its certificate is self-signed for localhost, so validation is relaxed
// for that request alone and for nothing else.
//
// The port MOVES when GG restarts. A connection-level failure therefore
// triggers exactly one re-discovery before the caller is told no: a GG that
// restarted onto a new port and a GG that hung are otherwise identical.
#pragma once

#include <string>

namespace mdrop {

class SonarHttp {
 public:
  // Deliberately short. The user reports SteelSeries hangs regularly, and a
  // wedged GG must park one worker thread rather than the application.
  static const int kConnectTimeoutMs = 1000;
  static const int kReceiveTimeoutMs = 2000;

  bool Discover();
  bool IsDiscovered() const { return !m_base.empty(); }
  std::wstring BaseUrl() const { return m_base; }

  // Both retry discovery once on a connection-level failure, never on an HTTP
  // status. Both return false for any non-2xx.
  bool Get(const std::wstring& path, std::wstring& body);
  bool Put(const std::wstring& path);

  // Drops the cached address, so the next call re-discovers.
  void Forget() { m_base.clear(); }

  // Test seam: point the client at a chosen address without discovery.
  //
  // `pinned` suppresses re-discovery. Without it a dead address silently heals
  // -- the connection fails, discovery finds the real GG, and the retry
  // succeeds -- so the timeout path could never be observed. That healing is
  // exactly right in production and exactly wrong in a test of the timeout.
  void SetBaseUrlForTest(const std::wstring& base, bool pinned = false) {
    m_base = base;
    m_pinned = pinned;
  }

 private:
  bool Request(const wchar_t* verb, const std::wstring& path,
               std::wstring* body, bool allowRediscover);

  std::wstring m_base;   // "http://127.0.0.1:32371", no trailing slash
  bool m_pinned = false; // test only: never re-discover
};

}  // namespace mdrop
