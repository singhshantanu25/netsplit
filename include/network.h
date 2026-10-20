#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
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

// A path to the internet that workers download through.
// Workers use every Network the same way (polymorphism); only the details that
// really differ (type name, does it cost mobile data) are virtual.
class Network {
public:
    Network(std::string iface, std::string ipv4)
        : iface_(std::move(iface)), ipv4_(std::move(ipv4)) {}
    virtual ~Network() = default;   // deleting through Network* must call the right destructor

    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    virtual std::string type() const = 0;   // "Wi-Fi", "Phone", ...
    virtual bool metered() const = 0;       // does every byte cost mobile data?

    const std::string& iface() const { return iface_; }   // "" = OS default route
    const std::string& ipv4() const { return ipv4_; }
    std::string label() const {
        return (iface_.empty() ? std::string("default") : iface_) + " (" + type() + ")";
    }

    // Counters, updated by many workers at once (lock-free atomics).
    void add_received(std::uint64_t n) { received_.fetch_add(n); }   // every byte that arrived
    void add_kept(std::uint64_t n) { kept_.fetch_add(n); }           // bytes of finished chunks
    void add_chunk() { chunks_.fetch_add(1); }
    std::uint64_t received() const { return received_.load(); }
    std::uint64_t kept() const { return kept_.load(); }
    int chunks() const { return chunks_.load(); }
    const std::atomic<std::uint64_t>& received_counter() const { return received_; }

private:
    std::string iface_;
    std::string ipv4_;
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> kept_{0};
    std::atomic<int> chunks_{0};
};

class WifiNetwork : public Network {
public:
    using Network::Network;
    std::string type() const override { return "Wi-Fi"; }
    bool metered() const override { return false; }
};

// A phone's hotspot (USB tethering). Every byte uses mobile data.
class PhoneNetwork : public Network {
public:
    using Network::Network;
    std::string type() const override { return "Phone"; }
    bool metered() const override { return true; }
};

// No --interfaces: the OS picks the route.
class DefaultNetwork : public Network {
public:
    DefaultNetwork() : Network("", "") {}
    std::string type() const override { return "default route"; }
    bool metered() const override { return false; }
};

// Wi-Fi or Phone object for this interface (phone = iPhone USB / 172.20.10.x hotspot).
std::unique_ptr<Network> make_network(const NetInterface& n);
