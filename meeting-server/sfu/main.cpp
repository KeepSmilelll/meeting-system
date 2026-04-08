#include "server/SfuDaemon.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> g_shouldStop{false};

void HandleSignal(int signalValue) {
    (void)signalValue;
    g_shouldStop.store(true);
}

} // namespace

int main() {
    const sfu::SfuDaemonConfig config = sfu::LoadDaemonConfigFromEnv();
    sfu::SfuDaemon daemon(config);
    if (!daemon.Start()) {
        std::cerr << "meeting_sfu: failed to start daemon\n";
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::cout << "meeting_sfu started"
              << " node_id=" << daemon.NodeId()
              << " rpc_port=" << daemon.RpcPort()
              << " media_port=" << daemon.MediaPort()
              << " advertised_media=" << daemon.AdvertisedMediaAddress()
              << std::endl;

    while (!g_shouldStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    daemon.Stop();
    return 0;
}

