#pragma once
#include <string>
#include <vector>

// One network interface that has an IPv4 address (Wi-Fi, phone over USB, ...).
struct NetInterface {
    std::string name;   // "en0", "en8", ...
    std::string ipv4;   // "192.168.1.5"
    std::string kind;   // "Wi-Fi", "iPhone USB", ... (best guess)
    bool up = false;    // interface is up and running
};

// All interfaces with an IPv4 address, except loopback (lo0).
std::vector<NetInterface> list_interfaces();

// The interface with this name, or nullptr if there is none.
const NetInterface* find_interface(const std::vector<NetInterface>& all,
                                   const std::string& name);
