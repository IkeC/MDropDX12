#pragma once
#include <windns.h>
#include <string>

#pragma comment(lib, "dnsapi.lib")

class MdnsAdvertiser {
public:
    bool Register(const std::string& serviceName, int port, int pid);
    void Unregister();
    ~MdnsAdvertiser() { Unregister(); }
private:
    DNS_SERVICE_INSTANCE* m_instance = nullptr;
    DNS_SERVICE_REGISTER_REQUEST m_request{};
    bool m_registered = false;
};
