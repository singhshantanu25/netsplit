#include "network.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>

namespace {

// macOS knows the real name of each port ("Wi-Fi", "iPhone USB", ...).
// `networksetup -listallhardwareports` prints pairs of lines:
//   Hardware Port: Wi-Fi
//   Device: en0
// Returns {} on other systems (the command does not exist there).
std::map<std::string, std::string> hardware_port_names() {
    std::map<std::string, std::string> names;
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(
        popen("networksetup -listallhardwareports 2>/dev/null", "r"), pclose);
    if (!pipe) return names;

    char line[256];
    std::string port;
    while (std::fgets(line, sizeof(line), pipe.get())) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("Hardware Port: ", 0) == 0) {
            port = s.substr(15);
        } else if (s.rfind("Device: ", 0) == 0 && !port.empty()) {
            names[s.substr(8)] = port;
            port.clear();
        }
    }
    return names;
}

// Best guess when macOS did not tell us the port name.
std::string guess_kind(const std::string& name, const std::string& ip) {
    if (ip.rfind("172.20.10.", 0) == 0) return "iPhone hotspot";   // iPhone always uses 172.20.10.0/28
    if (name.rfind("utun", 0) == 0) return "VPN tunnel";
    if (name.rfind("bridge", 0) == 0) return "bridge";
    if (name == "en0") return "Wi-Fi (usually)";
    return "unknown";
}

}  // namespace

std::vector<NetInterface> list_interfaces() {
    ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0) throw std::runtime_error("getifaddrs failed");
    // RAII: freeifaddrs runs even if something below throws.
    std::unique_ptr<ifaddrs, void (*)(ifaddrs*)> guard(head, freeifaddrs);

    const auto ports = hardware_port_names();
    std::vector<NetInterface> result;

    for (ifaddrs* a = head; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) continue;   // IPv4 only
        if (a->ifa_flags & IFF_LOOPBACK) continue;                                  // skip lo0

        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));

        NetInterface n;
        n.name = a->ifa_name;
        n.ipv4 = buf;
        n.up = (a->ifa_flags & IFF_UP) && (a->ifa_flags & IFF_RUNNING);
        auto it = ports.find(n.name);
        n.kind = (it != ports.end()) ? it->second : guess_kind(n.name, n.ipv4);
        if (n.ipv4.rfind("172.20.10.", 0) == 0 && it != ports.end())
            n.kind += " (phone hotspot)";
        if (n.ipv4.rfind("169.254.", 0) == 0) {
            // Self-assigned: nobody gave this interface an address (no DHCP),
            // so it is not connected to any network.
            n.kind += " (link-local, no network)";
            n.up = false;
        }
        result.push_back(n);
    }
    return result;
}

const NetInterface* find_interface(const std::vector<NetInterface>& all,
                                   const std::string& name) {
    for (const auto& n : all)
        if (n.name == name) return &n;
    return nullptr;
}

std::unique_ptr<Network> make_network(const NetInterface& n) {
    bool phone = n.kind.find("iPhone") != std::string::npos ||
                 n.kind.find("phone") != std::string::npos ||
                 n.ipv4.rfind("172.20.10.", 0) == 0;
    if (phone) return std::make_unique<PhoneNetwork>(n.name, n.ipv4);
    return std::make_unique<WifiNetwork>(n.name, n.ipv4);
}
