// numa_mem_monitor.cpp
#include <numa.h>
#include <numaif.h>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

static volatile sig_atomic_t g_stop = 0;
void handle_sigint(int) { g_stop = 1; }

std::string now_string() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%F %T", &tm);
    return buf;
}

std::string color_for_usage(double pct) {
    if (pct < 50) return "\033[32m";   // green
    if (pct < 80) return "\033[33m";   // yellow
    return "\033[31m";                 // red
}

int main() {
    if (numa_available() < 0) {
        std::cerr << "NUMA not available.\n";
        return 1;
    }

    struct bitmask *online = numa_allocate_nodemask();
    numa_bitmask_clearall(online);
    for (int n = 0; n <= numa_max_node(); ++n) {
        if (numa_node_size64(n, nullptr) > 0)
            numa_bitmask_setbit(online, n);
    }

    std::vector<int> nodes;
    for (int n = 0; n <= numa_max_node(); ++n)
        if (numa_bitmask_isbitset(online, n))
            nodes.push_back(n);

    if (nodes.empty()) {
        std::cerr << "No online NUMA nodes found.\n";
        return 1;
    }

    std::signal(SIGINT, handle_sigint);

    while (!g_stop) {
        std::string ts = now_string();
        std::cout << "\033[2J\033[H";  // clear screen
        std::cout << "NUMA Node Memory Usage Monitor (libnuma)\n";
        std::cout << "Time: " << ts << "\n\n";
        std::cout << " Node |   Total (GB)  |   Used (GB)  |   Free (GB)  |  Used % \n";
        std::cout << "-------+--------------+--------------+--------------+----------\n";

        for (int n : nodes) {
            long long free_b = 0;
            long long total_b = numa_node_size64(n, &free_b);
            if (total_b < 0) continue;
            long long used_b = total_b - free_b;
            double pct = 100.0 * used_b / total_b;

            double tGB = total_b / (1024.0 * 1024 * 1024);
            double uGB = used_b / (1024.0 * 1024 * 1024);
            double fGB = free_b / (1024.0 * 1024 * 1024);

            std::string color = color_for_usage(pct);
            std::string reset = "\033[0m";

            printf("  %3d  | %10.2f GB | %10.2f GB | %10.2f GB |  %s%6.2f%%%s\n",
                   n, tGB, uGB, fGB, color.c_str(), pct, reset.c_str());
        }

        std::cout << "\n(Press Ctrl+C to exit)\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    numa_free_nodemask(online);
    return 0;
}
