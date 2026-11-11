#include "MultiplayerSession.h"
#include "LAN.h"
#include "ThreadLifecycleScope.h"
#include "performancehint/PerformanceHintWorker.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace MelonDSAndroid::Multiplayer {
namespace {
constexpr uint64_t pause_grace_ms = 3000;
constexpr uint64_t interrupted_deadline = UINT64_MAX;

uint64_t elapsed_ms()
{
#ifdef __ANDROID__
    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    return uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1000000;
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

std::string quote(const char* value)
{
    std::string result = "\"";
    for (const unsigned char* byte = reinterpret_cast<const unsigned char*>(value); *byte; byte++) {
        if (*byte == '"' || *byte == '\\') { result += '\\'; result += *byte; }
        else if (*byte < 32) {
            static const char hex[] = "0123456789abcdef";
            result += "\\u00"; result += hex[*byte >> 4]; result += hex[*byte & 15];
        } else result += *byte;
    }
    result += '"';
    return result;
}

std::string address(uint32_t raw)
{
    in_addr ip{raw};
    char output[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &ip, output, sizeof(output));
    return output;
}

struct SocketWake {
    int descriptors[2] = {-1, -1};
    bool pending = false;

    SocketWake() = default;
    SocketWake(const SocketWake&) = delete;
    SocketWake& operator=(const SocketWake&) = delete;
    ~SocketWake() { close(); }

    void close()
    {
        for (int& descriptor : descriptors) {
            if (descriptor >= 0) ::close(descriptor);
            descriptor = -1;
        }
        pending = false;
    }

    bool open()
    {
        if (descriptors[0] >= 0) return true;
        if (pipe(descriptors) != 0) return false;
        for (const int descriptor : descriptors) {
            if (fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0
                || fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
                close();
                return false;
            }
        }
        return true;
    }

    void signal()
    {
        if (pending || descriptors[1] < 0) return;
        const uint8_t value = 1;
        ssize_t result;
        do { result = write(descriptors[1], &value, sizeof(value)); }
        while (result < 0 && errno == EINTR);
        pending = result > 0 || (result < 0 && errno == EAGAIN);
    }

    void consume()
    {
        uint8_t buffer[64];
        ssize_t result;
        do { result = read(descriptors[0], buffer, sizeof(buffer)); }
        while (result > 0 || (result < 0 && errno == EINTR));
        pending = false;
    }
};

struct Session {
    std::mutex mutex;
    std::mutex lifecycle;
    std::condition_variable wake;
    SocketWake socket_wake;
    std::shared_ptr<melonDS::LAN> lan;
    std::thread worker;
    std::atomic<bool> enabled{false};
    std::atomic<bool> readable{false};
    std::atomic<bool> game{false};
    std::atomic<uint64_t> pause_deadline{0};
    bool stopping = false;
    bool receiving = false;
    void* owner = nullptr;
    int mode = 0;
    uint8_t mac[6]{};
    uint64_t sent = 0;
    uint64_t received = 0;
    uint64_t replies = 0;
    std::string error;
    std::string snapshot = "{\"mode\":0,\"game\":false,\"players\":[],\"rooms\":[],\"error\":\"\"}";

    ~Session() { stop(); }

    void update()
    {
        char address_mac[18];
        std::snprintf(address_mac, sizeof(address_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        std::ostringstream json;
        json << "{\"mode\":" << mode << ",\"game\":" << (game ? "true" : "false")
             << ",\"conservative_ack\":" << (lan && lan->UsesConservativeAck() ? "true" : "false")
             << ",\"interrupted\":" << (pause_deadline == interrupted_deadline ? "true" : "false")
             << ",\"mac\":" << quote(address_mac)
             << ",\"sent\":" << sent << ",\"received\":" << received
             << ",\"replies\":" << replies << ",\"error\":" << quote(error.c_str())
             << ",\"players\":[";
        bool separator = false;
        if (lan) for (const auto& player : lan->GetPlayerList()) {
            if (separator) json << ',';
            separator = true;
            json << "{\"id\":" << player.ID << ",\"name\":" << quote(player.Name)
                 << ",\"status\":" << player.Status << ",\"ip\":" << quote(address(player.Address).c_str())
                 << ",\"local\":" << (player.IsLocalPlayer ? "true" : "false")
                 << ",\"ping\":" << player.Ping << '}';
        }
        json << "],\"rooms\":[";
        separator = false;
        if (lan) for (const auto& room : lan->GetDiscoveryList()) {
            if (separator) json << ',';
            separator = true;
            json << "{\"ip\":" << quote(address(htonl(room.first)).c_str())
                 << ",\"name\":" << quote(room.second.SessionName)
                 << ",\"players\":" << unsigned(room.second.NumPlayers)
                 << ",\"maximum\":" << unsigned(room.second.MaxPlayers) << '}';
        }
        json << "]}";
        snapshot = json.str();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            enabled = false;
            readable = false;
            pause_deadline = 0;
            stopping = true;
            wake.notify_all();
            socket_wake.signal();
        }
        if (worker.joinable()) worker.join();
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return !receiving; });
        lan.reset();
        socket_wake.close();
        mode = 0;
        error.clear();
        update();
    }

    void run()
    {
        melonDS::ThreadLifecycleScope hints(setPerformanceHintWorkerActive);
        std::unique_lock<std::mutex> lock(mutex);
        auto next_update = std::chrono::steady_clock::now();
        while (!stopping) {
            if (receiving) {
                wake.wait(lock, [&] { return stopping || !receiving; });
                continue;
            }
            socket_wake.consume();
            auto deadline = pause_deadline.load();
            if (deadline && game && enabled
                && (deadline == interrupted_deadline || elapsed_ms() >= deadline)
                && pause_deadline.compare_exchange_strong(deadline, interrupted_deadline)) {
                lan.reset();
                readable = false;
                wake.notify_all();
                mode = 0;
                error = "Disconnected after pausing the game.";
                update();
                break;
            }
            auto transport = lan;
            transport->Flush();
            const auto queued = transport->GetReceiveQueueSize();
            transport->Process();
            readable.store(transport->HasPendingPacket(), std::memory_order_release);
            transport->Flush();
            const bool received_packet = transport->GetReceiveQueueSize() > queued;
            if (received_packet) wake.notify_all();
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_update) {
                update();
                next_update = now + std::chrono::milliseconds(100);
            }
            const auto socket = transport->GetSocket();
            pollfd descriptors[2] = {{socket, POLLIN, 0}, {socket_wake.descriptors[0], POLLIN, 0}};
            const bool process_again = received_packet || transport->HasPendingWMDispatch();
            lock.unlock();
            if (process_again) {
                std::this_thread::yield();
            } else {
                poll(descriptors, 2, 5);
            }
            lock.lock();
        }
    }
};

Session& session()
{
    static Session value;
    return value;
}

bool accepts(const Session& value, void* owner)
{
    return value.lan && value.enabled && value.owner == owner;
}

struct ReceiveContext {
    Session& value;
    std::unique_lock<std::mutex>& lock;
    std::shared_ptr<melonDS::LAN> transport;
    void* owner;

    bool valid() const
    {
        return accepts(value, owner) && value.lan == transport;
    }

    static void wait(void* context, int timeout)
    {
        auto& call = *static_cast<ReceiveContext*>(context);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        call.value.receiving = true;
        while (call.valid()) {
            call.value.socket_wake.consume();
            call.transport->Flush();
            call.transport->Process();
            call.transport->Flush();
            call.value.readable.store(call.transport->HasPendingPacket(), std::memory_order_release);
            if (call.transport->GetReceiveQueueSize() != 0) break;
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            pollfd descriptors[2] = {{call.transport->GetSocket(), POLLIN, 0},
                                     {call.value.socket_wake.descriptors[0], POLLIN, 0}};
            call.lock.unlock();
            const int result = poll(descriptors, 2, static_cast<int>(remaining));
            const int error = errno;
            call.lock.lock();
            if (result < 0 && error != EINTR) break;
        }
        call.value.receiving = false;
        call.value.wake.notify_all();
    }
};
}

int Start(int mode, const char* name, const char* host, const uint8_t mac[6], int maximum, uint32_t bindAddress, unsigned interfaceIndex)
{
    if (mode < 1 || mode > 3 || !name || !host || !mac || maximum < 2 || maximum > 16
        || std::strlen(name) > 31 || std::strlen(host) > 253 || (mac[0] & 1)) return -1;
    auto& value = session();
    std::lock_guard<std::mutex> lifecycle(value.lifecycle);
    if (value.game) return -2;
    value.stop();
    std::lock_guard<std::mutex> lock(value.mutex);
#ifdef __ANDROID__
    char hleProperty[PROP_VALUE_MAX]{};
    if (mode != 3 && __system_property_get("debug.melonds.wm_hle", hleProperty) > 0
        && std::strcmp(hleProperty, "0") == 0)
    {
        value.error = "Wireless support is disabled for this multiplayer room.";
        value.update();
        return -3;
    }
#endif
    value.lan = std::make_shared<melonDS::LAN>();
    const bool success = mode == 1 ? value.lan->StartHost(name, maximum, true, bindAddress, interfaceIndex)
        : mode == 2 ? value.lan->StartClient(name, host, true, bindAddress, interfaceIndex) : value.lan->StartDiscovery();
    if (!success) {
        value.lan.reset();
        value.error = "Could not open the multiplayer session. Check the address and local network.";
        value.update();
        return -3;
    }
    if (!value.socket_wake.open()) {
        value.lan.reset();
        value.error = "Could not open the multiplayer worker signal.";
        value.update();
        return -3;
    }
    value.lan->SetFlushOnSend(true);
    std::memcpy(value.mac, mac, sizeof(value.mac));
    value.sent = value.received = value.replies = 0;
    value.mode = mode;
    value.stopping = false;
    value.enabled = mode != 3;
    value.update();
    value.worker = std::thread([&value] { value.run(); });
    return 0;
}

int Stop()
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.lifecycle);
    if (value.game && value.enabled) return -2;
    value.stop();
    return 0;
}

bool Pause(bool paused)
{
    auto& value = session();
    if (!value.enabled) return false;
    auto deadline = value.pause_deadline.load();
    if (paused) {
        if (!deadline)
            value.pause_deadline.compare_exchange_strong(deadline, elapsed_ms() + pause_grace_ms);
    } else {
        while (deadline && deadline != interrupted_deadline) {
            const auto next = elapsed_ms() >= deadline ? interrupted_deadline : uint64_t{0};
            if (value.pause_deadline.compare_exchange_weak(deadline, next)) break;
        }
    }
    return value.pause_deadline == interrupted_deadline;
}

std::string Snapshot()
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    return value.snapshot;
}

bool Active() { return session().enabled; }

void GameStarted(void* owner, bool firmware)
{
    auto& value = session();
    std::lock_guard<std::mutex> lifecycle(value.lifecycle);
    std::lock_guard<std::mutex> lock(value.mutex);
    value.owner = owner;
    if (value.lan && value.enabled) value.lan->SetFirmwareBoot(firmware);
    value.socket_wake.signal();
    value.game = true;
    value.update();
}

void GameStopped(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lifecycle(value.lifecycle);
    std::lock_guard<std::mutex> lock(value.mutex);
    if (value.owner != owner) return;
    value.game = false;
    value.owner = nullptr;
    value.wake.notify_all();
    if (value.lan) value.lan->SetFirmwareBoot(false);
    value.socket_wake.signal();
    value.pause_deadline = 0;
    if (!value.lan) value.enabled = false;
    value.update();
}

bool CopyMac(uint8_t mac[6])
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (!value.enabled) return false;
    std::memcpy(mac, value.mac, sizeof(value.mac));
    return true;
}

void Begin(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (accepts(value, owner)) {
        value.lan->Begin(0);
        value.readable = false;
        value.socket_wake.signal();
    }
}

void End(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (accepts(value, owner)) {
        value.lan->End(0);
        value.readable = false;
        value.socket_wake.signal();
    }
}

int Send(void* owner, unsigned type, const uint8_t* data, int length, uint64_t timestamp, uint16_t aid)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (!accepts(value, owner) || length < 0 || length > 2048 || (length && !data)) return 0;
    auto* packet = const_cast<uint8_t*>(data);
    int result = 0;
    switch (type) {
    case 0: result = value.lan->SendPacket(0, packet, length, timestamp); break;
    case 1: result = value.lan->SendCmd(0, packet, length, timestamp); break;
    case 2: result = value.lan->SendReply(0, packet, length, timestamp, aid); break;
    case 3: result = value.lan->SendAck(0, packet, length, timestamp); break;
    case 4: result = value.lan->SendProgress(0, timestamp); break;
    default: return 0;
    }
    if (result > 0) value.sent++;
    return result;
}

int Receive(void* owner, bool host, uint8_t* data, uint64_t* timestamp)
{
    auto& value = session();
    if (!host && !value.readable.load(std::memory_order_acquire)) return 0;
    std::unique_lock<std::mutex> lock(value.mutex);
    if (!accepts(value, owner)) return host && value.enabled ? -1 : 0;
    ReceiveContext call{value, lock, value.lan, owner};
    const melonDS::LAN::ReceiveWait wait{&call, ReceiveContext::wait};
    const int result = host ? call.transport->RecvHostPacket(0, data, timestamp, &wait)
                            : call.transport->RecvPacket(0, data, timestamp, &wait);
    if (!call.valid()) return host && value.enabled ? -1 : 0;
    value.readable.store(call.transport->HasPendingPacket(), std::memory_order_release);
    if (result > 0 || result == -2) value.received++;
    return result;
}

uint16_t Replies(void* owner, uint8_t* data, uint64_t timestamp, uint16_t aidMask)
{
    auto& value = session();
    std::unique_lock<std::mutex> lock(value.mutex);
    if (!accepts(value, owner)) return 0;
    ReceiveContext call{value, lock, value.lan, owner};
    const melonDS::LAN::ReceiveWait wait{&call, ReceiveContext::wait};
    const auto result = call.transport->RecvReplies(0, data, timestamp, aidMask, &wait);
    if (!call.valid()) return 0;
    value.readable.store(call.transport->HasPendingPacket(), std::memory_order_release);
    value.replies += __builtin_popcount(result);
    return result;
}

bool ConservativeAck(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    return accepts(value, owner) && value.lan->UsesConservativeAck();
}

melonDS::WMServiceInfo WMInfo(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (!accepts(value, owner)) return {};
    return value.lan->GetWMServiceInfo();
}

void WMAbort(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (!accepts(value, owner)) return;
    value.lan->EndSession();
    value.enabled = false;
    value.readable = false;
    value.stopping = true;
    value.error = "This wireless mode is not supported in this multiplayer room.";
    value.wake.notify_all();
    value.socket_wake.signal();
    value.update();
}

uint16_t WMSend(void* owner, uint16_t peers, const uint8_t* data, uint16_t length)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (!accepts(value, owner)) return 0;
    const auto queued = value.lan->SendWMService(peers, data, length);
    if (queued) value.socket_wake.signal();
    return queued;
}

int WMReceive(void* owner, melonDS::WMServicePacket& packet)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (!accepts(value, owner)) return -1;
    const int result = value.lan->ReceiveWMService(packet);
    if (value.lan->HasPendingWMDispatch()) value.socket_wake.signal();
    return result;
}

void WMReset(void* owner)
{
    auto& value = session();
    std::lock_guard<std::mutex> lock(value.mutex);
    if (accepts(value, owner)) value.lan->ResetWMService();
}
}
