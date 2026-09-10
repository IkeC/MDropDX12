#include "tcp_server.h"
#include "format_to.h"
#include "utility.h"
#include <algorithm>
#include <mstcpip.h>
#include "config_store.h"

using mdrop::Config;
using mdrop::ConfigFile;

thread_local TcpClientConnection* g_respondingTcpClient = nullptr;

// peerIp holds sin_addr.s_addr, which is NETWORK byte order: the first octet
// is the LOWEST byte on x86. Shifting down from bit 24 therefore prints the
// address backwards -- 192.168.0.106 came out as 106.0.168.192, which matches
// no DHCP lease, router page or phone settings screen, so the one log that says
// which device is churning its connection named a host that did not exist.
// Every log site goes through here so the shifts cannot be open-coded again.
static std::string FormatPeerIp(uint32_t netOrderIp) {
    in_addr addr{};
    addr.s_addr = netOrderIp;
    char buf[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &addr, buf, sizeof(buf))) return "<unknown>";
    return buf;
}

TcpServer::TcpServer() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

TcpServer::~TcpServer() {
    Stop();
}

bool TcpServer::Start(int port, MessageHandler onMessage, AuthRequestHandler onAuthRequest) {
    if (m_running.load()) return false;
    m_port = port;
    m_onMessage = std::move(onMessage);
    m_onAuthRequest = std::move(onAuthRequest);

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    u_long mode = 1;
    ioctlsocket(m_listenSocket, FIONBIO, &mode);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    m_running.store(true);
    return true;
}

void TcpServer::Stop() {
    m_running.store(false);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c.socket != INVALID_SOCKET) closesocket(c.socket);
    }
    m_clients.clear();
    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }
    WSACleanup();
}

void TcpServer::AcceptNewClients() {
    // Enforce max client limit
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_clients.size() >= MAX_CLIENTS) return;
    }

    sockaddr_in clientAddr{};
    int addrLen = sizeof(clientAddr);
    SOCKET clientSocket = accept(m_listenSocket, (sockaddr*)&clientAddr, &addrLen);
    if (clientSocket == INVALID_SOCKET) return;

    u_long mode = 1;
    ioctlsocket(clientSocket, FIONBIO, &mode);

    // Enable TCP keepalive so OS detects dead connections
    BOOL keepAlive = TRUE;
    setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepAlive, sizeof(keepAlive));
    // Start probing after 15s idle, probe every 5s, fail after 3 probes (30s total)
    DWORD keepIdle = 15000, keepInterval = 5000;
    setsockopt(clientSocket, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&keepIdle, sizeof(keepIdle));
    setsockopt(clientSocket, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&keepInterval, sizeof(keepInterval));

    TcpClientConnection conn;
    conn.socket = clientSocket;
    conn.peerIp = clientAddr.sin_addr.s_addr;
    conn.lastActivity = GetTickCount64();

    std::lock_guard<std::mutex> lock(m_clientsMutex);
    // Android often opens a fresh socket without cleanly closing the old one.
    // Drop stale unauthenticated sockets from the same device IP immediately.
    EvictStaleUnauthFromPeer(conn.peerIp, m_clients.size());
    conn.connectedAt = GetTickCount64();
    DLOG_INFO("tcp: client connected from %s; %d now held",
              FormatPeerIp(conn.peerIp).c_str(),
              (int)m_clients.size() + 1);
    m_clients.push_back(std::move(conn));
}

void TcpServer::ReadFromClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    uint8_t buf[RECV_BUFFER_SIZE];

    for (size_t i = 0; i < m_clients.size(); ) {
        auto& c = m_clients[i];
        int bytesRead = recv(c.socket, (char*)buf, RECV_BUFFER_SIZE, 0);

        if (bytesRead > 0) {
            c.lastActivity = GetTickCount64();
            c.readBuffer.insert(c.readBuffer.end(), buf, buf + bytesRead);
            ProcessFrames(i);
            ++i;
        } else if (bytesRead == 0) {
            // Orderly FIN: the peer closed. Its own choice, not a failure.
            RemoveClient(i, "peer closed the connection");
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                ++i;
            } else {
                char why[64];
                FormatToA(why, "recv failed, WSA error %d", err);
                RemoveClient(i, why);
            }
        }
    }
}

void TcpServer::ProcessFrames(size_t clientIndex) {
    while (clientIndex < m_clients.size() && m_clients[clientIndex].readBuffer.size() >= 4) {
        auto& client = m_clients[clientIndex];
        uint32_t payloadLen = 0;
        memcpy(&payloadLen, client.readBuffer.data(), 4);

        if (payloadLen > 4 * 1024 * 1024) {
            RemoveClient(clientIndex, "frame larger than 4MB");
            return;
        }

        if (client.readBuffer.size() < 4 + payloadLen) break;

        std::string utf8((char*)client.readBuffer.data() + 4, payloadLen);
        client.readBuffer.erase(client.readBuffer.begin(), client.readBuffer.begin() + 4 + payloadLen);

        // Handle AUTH specially — evict stale connections from the same device
        if (utf8.rfind("AUTH|", 0) == 0) {
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t pos = 0; pos <= utf8.size(); ++pos) {
                if (pos == utf8.size() || utf8[pos] == '|') {
                    parts.push_back(utf8.substr(start, pos - start));
                    start = pos + 1;
                }
            }
            if (parts.size() >= 4) {
                const std::string& deviceId = parts[2];
                const std::string& deviceName = parts[3];
                if (!deviceId.empty()) {
                    // Reconnect: close any prior socket for this authorized device.
                    clientIndex = EvictConnectionsForDevice(deviceId, clientIndex);
                }
                // Single-phone assumption: drop any prior socket from this IP (even other deviceIds).
                clientIndex = EvictConnectionsFromPeer(m_clients[clientIndex].peerIp, clientIndex);

                auto& authClient = m_clients[clientIndex];
                authClient.deviceId = deviceId;
                authClient.deviceName = deviceName;
                authClient.authRequiredSent = false;

                if (m_onAuthRequest) {
                    m_onAuthRequest(authClient, parts[1], deviceId, deviceName);
                }
            } else {
                SendTo(m_clients[clientIndex], "AUTH_FAIL|MALFORMED");
            }
            continue;
        }

        // Drop non-AUTH commands from unauthenticated clients; prompt re-auth once.
        if (client.authState != TcpAuthState::Authenticated) {
            if (!client.authRequiredSent) {
                client.authRequiredSent = true;
                SendTo(client, "AUTH_REQUIRED");
            }
            continue;
        }

        // Handle PING (authenticated only)
        if (utf8 == "PING") {
            SendTo(client, "PONG");
            continue;
        }

        // Convert to wide and dispatch
        std::wstring wide = UTF8ToWide(utf8);
        if (!wide.empty() && m_onMessage) {
            m_onMessage(client, wide);
        }
    }
}

// WideToUtf8 / Utf8ToWide — now use free functions from utility.h (UTF8ToWide / WideToUTF8)

void TcpServer::SendRaw(SOCKET sock, const uint8_t* data, int len) {
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    int sent = 0;
    while (sent < len) {
        int r = send(sock, (const char*)(data + sent), len - sent, 0);
        if (r == SOCKET_ERROR) break;
        sent += r;
    }
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
}

void TcpServer::SendTo(TcpClientConnection& client, const std::string& utf8Message) {
    uint32_t payloadLen = (uint32_t)utf8Message.size();
    uint8_t header[4];
    memcpy(header, &payloadLen, 4);
    SendRaw(client.socket, header, 4);
    SendRaw(client.socket, (const uint8_t*)utf8Message.data(), (int)payloadLen);
}

void TcpServer::SendTo(TcpClientConnection& client, const std::wstring& message) {
    SendTo(client, WideToUTF8(message));
}

void TcpServer::Broadcast(const std::wstring& message) {
    std::string utf8 = WideToUTF8(message);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c.authState == TcpAuthState::Authenticated) {
            SendTo(c, utf8);
        }
    }
}

void TcpServer::Poll() {
    if (!m_running.load()) return;
    AcceptNewClients();
    ReadFromClients();
    CheckTimeouts();
}

void TcpServer::CheckTimeouts() {
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (size_t i = 0; i < m_clients.size(); ) {
        ULONGLONG limit = (m_clients[i].authState == TcpAuthState::Authenticated)
            ? AUTH_CLIENT_TIMEOUT_MS : UNAUTH_CLIENT_TIMEOUT_MS;
        if (now - m_clients[i].lastActivity > limit) {
            RemoveClient(i, "idle past its timeout");
        } else {
            ++i;
        }
    }
}

size_t TcpServer::EvictConnectionsForDevice(const std::string& deviceId, size_t exceptIndex) {
    if (deviceId.empty() || exceptIndex >= m_clients.size()) return exceptIndex;
    size_t removedBelow = 0;
    for (size_t j = m_clients.size(); j-- > 0; ) {
        if (j == exceptIndex) continue;
        if (m_clients[j].deviceId == deviceId) {
            RemoveClient(j, "superseded by a newer connection from the same device");
            if (j < exceptIndex) removedBelow++;
        }
    }
    return exceptIndex - removedBelow;
}

size_t TcpServer::EvictStaleUnauthFromPeer(uint32_t peerIp, size_t exceptIndex) {
    if (peerIp == 0) return exceptIndex;
    size_t removedBelow = 0;
    for (size_t j = m_clients.size(); j-- > 0; ) {
        if (j == exceptIndex) continue;
        if (m_clients[j].peerIp == peerIp &&
            m_clients[j].authState == TcpAuthState::Unauthenticated) {
            RemoveClient(j, "stale unauthenticated connection from the same address");
            if (j < exceptIndex) removedBelow++;
        }
    }
    return exceptIndex - removedBelow;
}

size_t TcpServer::EvictConnectionsFromPeer(uint32_t peerIp, size_t exceptIndex) {
    if (peerIp == 0) return exceptIndex;
    size_t removedBelow = 0;
    for (size_t j = m_clients.size(); j-- > 0; ) {
        if (j == exceptIndex) continue;
        if (m_clients[j].peerIp == peerIp) {
            RemoveClient(j, "evicted: another connection authenticated from the same address");
            if (j < exceptIndex) removedBelow++;
        }
    }
    return exceptIndex - removedBelow;
}

void TcpServer::RemoveClient(size_t index, const char* reason) {
    if (index < m_clients.size()) {
        // Log BEFORE erasing: "the app keeps reconnecting" left no record at
        // either end, because nothing in this file logged a connect, a drop or
        // an auth result. A reconnect that cannot be counted cannot be
        // diagnosed, and the reason is the whole value -- a clean FIN from the
        // phone and a reset from a dead Wi-Fi link are different problems.
        const auto& c = m_clients[index];
        const ULONGLONG heldMs = GetTickCount64() - c.connectedAt;
        DLOG_INFO("tcp: client %s (%s) dropped after %llus -- %s; %d remain",
                  c.deviceId.empty() ? "<unauthenticated>" : c.deviceId.c_str(),
                  FormatPeerIp(c.peerIp).c_str(),
                  (unsigned long long)(heldMs / 1000),
                  reason ? reason : "unspecified",
                  (int)m_clients.size() - 1);
        closesocket(m_clients[index].socket);
        m_clients.erase(m_clients.begin() + index);
    }
}

void TcpServer::ApproveDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto& c : m_clients) {
        if (c.deviceId == deviceId && c.authState == TcpAuthState::Pending) {
            c.authState = TcpAuthState::Authenticated;
            SendTo(c, "AUTH_OK");
            break;
        }
    }
}

void TcpServer::DenyDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (size_t i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].deviceId == deviceId && m_clients[i].authState == TcpAuthState::Pending) {
            SendTo(m_clients[i], "AUTH_FAIL|DENIED");
            RemoveClient(i, "authorisation denied");
            break;
        }
    }
}

void TcpServer::DisconnectDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (size_t i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].deviceId == deviceId) {
            RemoveClient(i, "disconnected from the PC");
            break;
        }
    }
}

void TcpServer::LoadAuthorizedDevices(const std::wstring& iniPath) {
    m_iniPath = iniPath;
    m_authorizedDevices.clear();

    wchar_t countBuf[32] = {};
    ConfigFile(iniPath.c_str()).GetStringTo(L"AuthorizedDevices", L"count", L"0", countBuf, 32);
    int count = _wtoi(countBuf);

    for (int i = 0; i < count; ++i) {
        wchar_t keyBuf[64];
        wchar_t valBuf[512];

        swprintf(keyBuf, 64, L"device%d_id", i);
        ConfigFile(iniPath.c_str()).GetStringTo(L"AuthorizedDevices", keyBuf, L"", valBuf, 512);
        std::wstring wId(valBuf);

        swprintf(keyBuf, 64, L"device%d_name", i);
        ConfigFile(iniPath.c_str()).GetStringTo(L"AuthorizedDevices", keyBuf, L"", valBuf, 512);
        std::wstring wName(valBuf);

        swprintf(keyBuf, 64, L"device%d_added", i);
        ConfigFile(iniPath.c_str()).GetStringTo(L"AuthorizedDevices", keyBuf, L"", valBuf, 512);
        std::wstring wAdded(valBuf);

        if (wId.empty()) continue;

        AuthorizedDevice dev;
        dev.id      = WideToUTF8(wId);
        dev.name    = WideToUTF8(wName);
        dev.dateAdded = WideToUTF8(wAdded);
        m_authorizedDevices.push_back(std::move(dev));
    }
}

void TcpServer::SaveAuthorizedDevices(const std::wstring& iniPath) {
    wchar_t countBuf[32];
    swprintf(countBuf, 32, L"%d", (int)m_authorizedDevices.size());
    ConfigFile(iniPath.c_str()).SetString(L"AuthorizedDevices", L"count", countBuf);

    for (int i = 0; i < (int)m_authorizedDevices.size(); ++i) {
        const auto& dev = m_authorizedDevices[i];
        wchar_t keyBuf[64];

        swprintf(keyBuf, 64, L"device%d_id", i);
        ConfigFile(iniPath.c_str()).SetString(L"AuthorizedDevices", keyBuf, UTF8ToWide(dev.id).c_str());

        swprintf(keyBuf, 64, L"device%d_name", i);
        ConfigFile(iniPath.c_str()).SetString(L"AuthorizedDevices", keyBuf, UTF8ToWide(dev.name).c_str());

        swprintf(keyBuf, 64, L"device%d_added", i);
        ConfigFile(iniPath.c_str()).SetString(L"AuthorizedDevices", keyBuf, UTF8ToWide(dev.dateAdded).c_str());
    }
}

bool TcpServer::IsDeviceAuthorized(const std::string& deviceId) const {
    for (const auto& dev : m_authorizedDevices) {
        if (dev.id == deviceId) return true;
    }
    return false;
}

void TcpServer::AddAuthorizedDevice(const std::string& id, const std::string& name) {
    // Replace if already present, otherwise append
    for (auto& dev : m_authorizedDevices) {
        if (dev.id == id) {
            dev.name = name;
            if (!m_iniPath.empty()) SaveAuthorizedDevices(m_iniPath);
            return;
        }
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char dateBuf[16];
    sprintf(dateBuf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    AuthorizedDevice dev;
    dev.id        = id;
    dev.name      = name;
    dev.dateAdded = dateBuf;
    m_authorizedDevices.push_back(std::move(dev));

    if (!m_iniPath.empty()) SaveAuthorizedDevices(m_iniPath);
}

void TcpServer::RemoveAuthorizedDevice(const std::string& id) {
    auto it = std::remove_if(m_authorizedDevices.begin(), m_authorizedDevices.end(),
        [&](const AuthorizedDevice& dev) { return dev.id == id; });
    if (it != m_authorizedDevices.end()) {
        m_authorizedDevices.erase(it, m_authorizedDevices.end());
        if (!m_iniPath.empty()) SaveAuthorizedDevices(m_iniPath);
    }
}

std::vector<AuthorizedDevice> TcpServer::GetAuthorizedDevices() const {
    return m_authorizedDevices;
}

std::vector<TcpClientInfo> TcpServer::GetConnectedClients() const {
    std::vector<TcpClientInfo> result;
    // Note: const_cast needed because mutex is not mutable
    auto& mtx = const_cast<std::mutex&>(m_clientsMutex);
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& c : m_clients) {
        TcpClientInfo info;
        info.deviceId = c.deviceId;
        info.deviceName = c.deviceName;
        info.authState = c.authState;
        result.push_back(info);
    }
    return result;
}
