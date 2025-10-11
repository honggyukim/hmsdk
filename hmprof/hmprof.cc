#include <numa.h>
#include <numaif.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

static volatile sig_atomic_t g_stop = 0;
void handle_sigint(int){ g_stop = 1; }

struct NodeExtra {
    long long anon_bytes = -1;
    long long file_bytes = -1;
    long long slab_bytes = -1;
    long long shmem_bytes = -1;
};

struct NodeStat {
    int node = -1;
    long long total = 0;
    long long free = 0;
    long long used = 0;
    double used_pct = 0.0;
    NodeExtra extra;
};

struct Options {
    int interval = 1;
    bool use_color = true;
    bool top_mode = true;
    bool once = false;
    bool bars = false;
    bool details = false;
    std::string sort_key = "id"; // id|usedpct|used|free
    std::vector<int> filter_nodes; // empty = all online
};

static std::string now_string() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%F %T", &tm);
    return buf;
}

static std::string color_for_usage(double pct, bool use_color) {
    if (!use_color) return "";
    if (pct < 50) return "\033[32m";   // green
    if (pct < 80) return "\033[33m";   // yellow
    return "\033[31m";                 // red
}
static const char* CLR_RESET(bool use_color){ return use_color ? "\033[0m" : ""; }

static double toGB(long long bytes) {
    return bytes / (1024.0 * 1024.0 * 1024.0);
}

static bool parse_nodes_list(const std::string& s, std::vector<int>& out) {
    // supports "0,1,3-5"
    std::stringstream ss(s);
    std::string tok;
    std::vector<int> res;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        size_t dash = tok.find('-');
        try {
            if (dash == std::string::npos) {
                res.push_back(std::stoi(tok));
            } else {
                int a = std::stoi(tok.substr(0, dash));
                int b = std::stoi(tok.substr(dash+1));
                if (b < a) std::swap(a,b);
                for (int i=a;i<=b;i++) res.push_back(i);
            }
        } catch (...) { return false; }
    }
    // dedup & sort
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    out = std::move(res);
    return true;
}

static void parse_args(int argc, char** argv, Options& opt) {
    for (int i=1;i<argc;i++){
        std::string a = argv[i];
        auto need_val = [&](int& i)->char*{
            if (i+1>=argc) { std::cerr<<"Missing value for "<<a<<"\n"; std::exit(2); }
            return argv[++i];
        };
        if (a=="--interval") { opt.interval = std::max(1, std::atoi(need_val(i))); }
        else if (a=="--nodes") {
            std::string v = need_val(i);
            if (!parse_nodes_list(v, opt.filter_nodes)) {
                std::cerr<<"Invalid --nodes format\n"; std::exit(2);
            }
        }
        else if (a=="--sort") {
            std::string v = need_val(i);
            if (v=="id"||v=="usedpct"||v=="used"||v=="free") opt.sort_key = v;
            else { std::cerr<<"--sort must be one of: id|usedpct|used|free\n"; std::exit(2); }
        }
        else if (a=="--no-color") { opt.use_color = false; }
        else if (a=="--bars") { opt.bars = true; }
        else if (a=="--details") { opt.details = true; }
        else if (a=="--once") { opt.once = true; }
        else if (a=="--top") { opt.top_mode = true; }
        else if (a=="--no-top") { opt.top_mode = false; }
        else if (a=="-h" || a=="--help") {
            std::cout <<
"Usage: numa_mem_monitor [options]\n"
"  --interval <sec>     Refresh interval (default 1)\n"
"  --nodes <list>       Nodes filter (e.g. 0,1,3-5)\n"
"  --sort id|usedpct|used|free  Sort key (default id)\n"
"  --bars               Show ASCII usage bars\n"
"  --details            Show per-node Anon/File/Slab if available\n"
"  --no-color           Disable ANSI colors\n"
"  --top | --no-top     Full-screen refresh vs append mode (default: --top)\n"
"  --once               Print once and exit\n";
            std::exit(0);
        }
        else {
            std::cerr<<"Unknown option: "<<a<<"\n"; std::exit(2);
        }
    }
}

static bool read_node_meminfo(int node, NodeExtra& ex) {
    // /sys/devices/system/node/nodeN/meminfo lines like:
    // Node 0 AnonPages:   12345 kB
    // Node 0 FilePages:   67890 kB
    // Node 0 Slab:        11111 kB
    // Node 0 Shmem:       22222 kB
    std::ostringstream p;
    p << "/sys/devices/system/node/node" << node << "/meminfo";
    std::ifstream f(p.str());
    if (!f.is_open()) return false;

    std::string line;
    std::regex re(R"(Node\s+\d+\s+([A-Za-z]+):\s+(\d+)\s+kB)");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re) && m.size()==3) {
            std::string key = m[1].str();
            long long kb = std::stoll(m[2].str());
            long long bytes = kb * 1024LL;
            if (key=="AnonPages") ex.anon_bytes = bytes;
            else if (key=="FilePages") ex.file_bytes = bytes;
            else if (key=="Slab") ex.slab_bytes = bytes;
            else if (key=="Shmem") ex.shmem_bytes = bytes;
        }
    }
    return true;
}

static std::vector<int> detect_online_nodes() {
    std::vector<int> nodes;
    int maxn = numa_max_node();
    for (int n=0;n<=maxn;n++){
        if (numa_node_size64(n, nullptr) > 0) nodes.push_back(n);
    }
    return nodes;
}

static std::vector<NodeStat> snapshot_nodes(const std::vector<int>& nodes, bool want_details) {
    std::vector<NodeStat> stats;
    stats.reserve(nodes.size());
    for (int n : nodes) {
        long long free_b = 0;
        long long total_b = numa_node_size64(n, &free_b);
        if (total_b <= 0) continue;
        long long used_b = total_b - free_b;
        NodeStat s;
        s.node = n; s.total = total_b; s.free = free_b; s.used = used_b;
        s.used_pct = (total_b>0) ? (100.0 * (double)used_b / (double)total_b) : 0.0;
        if (want_details) {
            read_node_meminfo(n, s.extra); // best-effort
        }
        stats.push_back(std::move(s));
    }
    return stats;
}

static void sort_stats(std::vector<NodeStat>& v, const std::string& key) {
    if (key=="id") {
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.node < b.node;});
    } else if (key=="usedpct") {
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.used_pct > b.used_pct;});
    } else if (key=="used") {
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.used > b.used;});
    } else if (key=="free") {
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.free > b.free;});
    }
}

static int term_width() {
    // best-effort: check COLUMNS env, fallback 100
    const char* c = std::getenv("COLUMNS");
    if (c) {
        int w = std::atoi(c);
        if (w >= 40 && w <= 500) return w;
    }
    return 100;
}

static std::string make_bar(double pct, int width) {
    int filled = (int)((pct/100.0) * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    std::string s; s.reserve(width);
    s.append(filled, '#');
    s.append(width - filled, '-');
    return s;
}

static void print_table(const std::vector<NodeStat>& stats, const Options& opt) {
    std::string ts = now_string();
    if (opt.top_mode) std::cout << "\033[2J\033[H"; // clear screen
    std::cout << "NUMA Node Memory Usage (libnuma)   Time: " << ts << "\n";

    int barw = std::max(0, term_width() - 72); // adjust for columns
    if (!opt.bars) barw = 0;

    std::cout << " Node |   Total(GB)  |    Used(GB)  |    Free(GB)  |  Used% ";
    if (opt.bars) std::cout << "| " << std::setw(barw) << std::left << "Usage";
    std::cout << "\n";
    std::cout << "------+--------------+--------------+--------------+--------";
    if (opt.bars) { std::cout << "+-" << std::string(barw, '-'); }
    std::cout << "\n";

    for (const auto& s : stats) {
        std::string col = color_for_usage(s.used_pct, opt.use_color);
        const char* rst = CLR_RESET(opt.use_color);
        printf(" %4d | %10.2f GB | %10.2f GB | %10.2f GB |  %s%6.2f%%%s",
               s.node, toGB(s.total), toGB(s.used), toGB(s.free),
               col.c_str(), s.used_pct, rst);
        if (opt.bars) {
            std::cout << " | " << std::left << std::setw(barw) << make_bar(s.used_pct, barw);
        }
        std::cout << "\n";

        if (opt.details) {
            // print one extra line with details, if available
            auto show = [&](const char* name, long long v){
                if (v >= 0) {
                    std::cout << "      | " << std::setw(12) << name << " "
                              << std::setw(10) << std::fixed << std::setprecision(2)
                              << toGB(v) << " GB\n";
                }
            };
            show("Anon", s.extra.anon_bytes);
            show("File", s.extra.file_bytes);
            show("Slab", s.extra.slab_bytes);
            show("Shmem", s.extra.shmem_bytes);
        }
    }
    std::cout << (opt.top_mode ? "\n(Press Ctrl+C to exit)" : "") << "\n";
    std::cout.flush();
}

int main(int argc, char** argv) {
    Options opt;
    parse_args(argc, argv, opt);

    if (numa_available() < 0) {
        std::cerr << "NUMA not available.\n";
        return 1;
    }

    std::signal(SIGINT, handle_sigint);

    // discover nodes
    std::vector<int> online = detect_online_nodes();
    if (online.empty()) {
        std::cerr << "No online NUMA nodes found.\n";
        return 1;
    }
    std::vector<int> nodes;
    if (opt.filter_nodes.empty()) nodes = online;
    else {
        // intersect
        std::sort(online.begin(), online.end());
        for (int n : opt.filter_nodes) {
            if (std::binary_search(online.begin(), online.end(), n)) nodes.push_back(n);
        }
        if (nodes.empty()) {
            std::cerr << "No matching nodes online.\n";
            return 1;
        }
    }

    do {
        auto stats = snapshot_nodes(nodes, opt.details);
        sort_stats(stats, opt.sort_key);
        print_table(stats, opt);
        if (opt.once) break;

        // sleep in small chunks to react to SIGINT
        for (int ms = 0; ms < opt.interval*1000 && !g_stop; ms += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (!g_stop);

    return 0;
}
