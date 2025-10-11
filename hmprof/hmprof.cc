// numa_mem_monitor.cpp
#include <numa.h>
#include <numaif.h>

#include <argp.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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
    long long total = 0, free = 0, used = 0;
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
} g_opt;

// ---------- util ----------
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
static double toGB(long long bytes) { return bytes / (1024.0 * 1024.0 * 1024.0); }

static bool parse_nodes_list(const std::string& s, std::vector<int>& out) {
    // supports "0,1,3-5"
    std::stringstream ss(s);
    std::string tok;
    std::vector<int> res;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        size_t dash = tok.find('-');
        try {
            if (dash == std::string::npos) res.push_back(std::stoi(tok));
            else {
                int a = std::stoi(tok.substr(0, dash));
                int b = std::stoi(tok.substr(dash+1));
                if (b < a) std::swap(a,b);
                for (int i=a;i<=b;i++) res.push_back(i);
            }
        } catch (...) { return false; }
    }
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    out = std::move(res);
    return true;
}

static bool read_node_meminfo(int node, NodeExtra& ex) {
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
        if (want_details) (void)read_node_meminfo(n, s.extra);
        stats.push_back(std::move(s));
    }
    return stats;
}

static void sort_stats(std::vector<NodeStat>& v, const std::string& key) {
    if (key=="id") std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.node < b.node;});
    else if (key=="usedpct") std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.used_pct > b.used_pct;});
    else if (key=="used") std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.used > b.used;});
    else if (key=="free") std::sort(v.begin(), v.end(), [](auto&a, auto&b){return a.free > b.free;});
}

static int term_width() {
    const char* c = std::getenv("COLUMNS");
    if (c) { int w = std::atoi(c); if (w >= 40 && w <= 500) return w; }
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

static void print_table(const std::vector<NodeStat>& stats) {
    std::string ts = now_string();
    if (g_opt.top_mode) std::cout << "\033[2J\033[H";
    std::cout << "NUMA Node Memory Usage (libnuma)   Time: " << ts << "\n";

    int barw = std::max(0, term_width() - 72);
    if (!g_opt.bars) barw = 0;

    std::cout << " Node |   Total(GB)  |    Used(GB)  |    Free(GB)  |  Used% ";
    if (g_opt.bars) std::cout << "| " << std::setw(barw) << std::left << "Usage";
    std::cout << "\n";
    std::cout << "------+--------------+--------------+--------------+--------";
    if (g_opt.bars) { std::cout << "+-" << std::string(barw, '-'); }
    std::cout << "\n";

    for (const auto& s : stats) {
        std::string col = color_for_usage(s.used_pct, g_opt.use_color);
        const char* rst = CLR_RESET(g_opt.use_color);
        printf(" %4d | %10.2f GB | %10.2f GB | %10.2f GB |  %s%6.2f%%%s",
               s.node, toGB(s.total), toGB(s.used), toGB(s.free),
               col.c_str(), s.used_pct, rst);
        if (g_opt.bars) std::cout << " | " << std::left << std::setw(barw) << make_bar(s.used_pct, barw);
        std::cout << "\n";

        if (g_opt.details) {
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
    std::cout << (g_opt.top_mode ? "\n(Press Ctrl+C to exit)" : "") << "\n";
    std::cout.flush();
}

// ---------- argp ----------
const char *argp_program_version = "numa_mem_monitor 1.0";
const char *argp_program_bug_address = "<bugs@example.com>";
static char doc[] = "Per-NUMA node memory usage monitor (libnuma, GNU argp)";
static char args_doc[] = "";

enum {
    KEY_INTERVAL = 'i',
    KEY_NODES    = 'n',
    KEY_SORT     = 's',
    KEY_BARS     = 1000,
    KEY_DETAILS,
    KEY_NO_COLOR,
    KEY_ONCE,
    KEY_TOP,
    KEY_NO_TOP
};

static struct argp_option options[] = {
    {"interval", KEY_INTERVAL, "SEC", 0, "Refresh interval in seconds (default 1)"},
    {"nodes",    KEY_NODES,    "LIST",0, "Filter nodes, e.g. 0,1,3-5"},
    {"sort",     KEY_SORT,     "KEY", 0, "Sort by: id|usedpct|used|free (default id)"},
    {"bars",     KEY_BARS,     0,     0, "Show ASCII usage bars"},
    {"details",  KEY_DETAILS,  0,     0, "Show Anon/File/Slab/Shmem if available"},
    {"no-color", KEY_NO_COLOR, 0,     0, "Disable ANSI colors"},
    {"once",     KEY_ONCE,     0,     0, "Print once and exit"},
    {"top",      KEY_TOP,      0,     0, "Full-screen refresh mode (default)"},
    {"no-top",   KEY_NO_TOP,   0,     0, "Append mode (no screen clear)"},
    {0}
};

static error_t parse_opt (int key, char *arg, struct argp_state *state) {
    switch (key) {
        case KEY_INTERVAL: {
            int v = std::max(1, atoi(arg ? arg : "1"));
            g_opt.interval = v;
            break;
        }
        case KEY_NODES: {
            std::vector<int> tmp;
            if (!parse_nodes_list(arg ? std::string(arg) : std::string(), tmp)) {
                argp_error(state, "Invalid --nodes format");
            }
            g_opt.filter_nodes = std::move(tmp);
            break;
        }
        case KEY_SORT: {
            std::string v = arg ? std::string(arg) : "id";
            if (v=="id" || v=="usedpct" || v=="used" || v=="free") g_opt.sort_key = v;
            else argp_error(state, "--sort must be one of: id|usedpct|used|free");
            break;
        }
        case KEY_BARS:     g_opt.bars     = true; break;
        case KEY_DETAILS:  g_opt.details  = true; break;
        case KEY_NO_COLOR: g_opt.use_color = false; break;
        case KEY_ONCE:     g_opt.once     = true; break;
        case KEY_TOP:      g_opt.top_mode = true; break;
        case KEY_NO_TOP:   g_opt.top_mode = false; break;

        case ARGP_KEY_ARG:
            // no positional args supported
            argp_usage(state);
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, args_doc, doc };

// ---------- main ----------
int main(int argc, char** argv) {
    // defaults already in g_opt
    argp_parse(&argp, argc, argv, 0, 0, 0);

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
    if (g_opt.filter_nodes.empty()) nodes = online;
    else {
        std::sort(online.begin(), online.end());
        for (int n : g_opt.filter_nodes) {
            if (std::binary_search(online.begin(), online.end(), n)) nodes.push_back(n);
        }
        if (nodes.empty()) {
            std::cerr << "No matching nodes online.\n";
            return 1;
        }
    }

    do {
        auto stats = snapshot_nodes(nodes, g_opt.details);
        sort_stats(stats, g_opt.sort_key);
        print_table(stats);
        if (g_opt.once) break;

        for (int ms = 0; ms < g_opt.interval*1000 && !g_stop; ms += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    } while (!g_stop);

    return 0;
}
