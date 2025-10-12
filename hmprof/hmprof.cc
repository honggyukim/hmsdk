// Build (glibc): g++ -O2 -std=c++17 -lnuma -o numa_mem_monitor numa_mem_monitor.cpp
// Build (musl/Alpine): g++ -O2 -std=c++17 -lnuma -largp -o numa_mem_monitor numa_mem_monitor.cpp

#include <numa.h>
#include <numaif.h>

#include <argp.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static volatile sig_atomic_t g_stop = 0;
void handle_sigint(int){ g_stop = 1; }

struct NodeExtra {
    long long anon_bytes = -1, file_bytes = -1, slab_bytes = -1, shmem_bytes = -1;
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

    // Table mode
    bool table = true;
    bool bars = false;
    bool details = false;
    std::string sort_key = "id"; // id|usedpct|used|free

    // Filters
    std::vector<int> filter_nodes;

    // 2D graph mode
    bool graph2d = false;     // enable 2D graph (x=time, y=usage)
    int  graph_width = 80;    // columns (time)
    int  graph_height = 10;   // rows (for the largest node); each row has 5 sub-steps
    bool graph_legend = true;
    bool scale_by_total = false; // scale each node's height by its total memory
} g_opt;

// node -> history of usage (%) length <= graph_width
static std::map<int, std::deque<double>> g_hist;

// -------- nvitop-style UP symbol table (no DOWN table) --------
static const char* VALUE2SYMBOL_UP[5][5] = {
    /*prev=0*/ {" ", "⢀", "⢠", "⢰", "⢸"},
    /*prev=1*/ {"⡀","⣀","⣠","⣰","⣸"},
    /*prev=2*/ {"⡄","⣄","⣤","⣴","⣼"},
    /*prev=3*/ {"⡆","⣆","⣦","⣶","⣾"},
    /*prev=4*/ {"⡇","⣇","⣧","⣷","⣿"},
};
static inline const char* pick_symbol_up(int prev4, int curr4) {
    if (prev4 < 0) prev4 = 0; if (prev4 > 4) prev4 = 4;
    if (curr4 < 0) curr4 = 0; if (curr4 > 4) curr4 = 4;
    return VALUE2SYMBOL_UP[prev4][curr4];
}

// ---------------- Utils ----------------
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

// fixed-width y-axis label like " 128.0G | "
static std::string y_label(double gb) {
    std::ostringstream o;
    o << std::fixed << std::setprecision((gb >= 100.0) ? 0 : (gb >= 10.0 ? 1 : 2))
      << std::setw(7) << gb << "G | ";
    return o.str();
}

static bool parse_nodes_list(const std::string& s, std::vector<int>& out) {
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
    std::ostringstream p; p << "/sys/devices/system/node/node" << node << "/meminfo";
    std::ifstream f(p.str());
    if (!f.is_open()) return false;
    std::string line;
    std::regex re(R"(Node\s+\d+\s+([A-Za-z]+):\s+(\d+)\s+kB)");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re) && m.size()==3) {
            std::string key = m[1].str();
            long long kb = std::stoll(m[2].str());
            long long bytes = kb*1024LL;
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

// ---------------- Table printer ----------------
static void print_table(const std::vector<NodeStat>& stats) {
    std::string ts = now_string();
    if (g_opt.top_mode) std::cout << "\033[2J\033[H";
    std::cout << "NUMA Node Memory Usage (table)   Time: " << ts << "\n";
    int barw = std::max(0, term_width() - 72);
    if (!g_opt.bars) barw = 0;

    std::cout << " Node |   Total(GB)  |    Used(GB)  |    Free(GB)  |  Used% ";
    if (g_opt.bars) std::cout << "| " << std::setw(barw) << std::left << "Usage";
    std::cout << "\n";
    std::cout << "------+--------------+--------------+--------------+--------";
    if (g_opt.bars) std::cout << "+-" << std::string(barw, '-');
    std::cout << "\n";

    for (const auto& s : stats) {
        std::string col = color_for_usage(s.used_pct, g_opt.use_color);
        const char* rst = CLR_RESET(g_opt.use_color);
        printf(" %4d | %10.2f GB | %10.2f GB | %10.2f GB |  %s%6.2f%%%s",
               s.node, toGB(s.total), toGB(s.used), toGB(s.free),
               col.c_str(), s.used_pct, rst);
        if (g_opt.bars) {
            int width = barw;
            int filled = (int)((s.used_pct/100.0)*width + 0.5);
            if (filled<0) filled=0; if (filled>width) filled=width;
            std::cout << " | " << std::string(filled, '#') << std::string(width - filled, '-');
        }
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
    if (g_opt.top_mode) std::cout << "\n(Press Ctrl+C to exit)\n";
    std::cout.flush();
}

// ---------------- 2D graph helpers ----------------
static int effective_height_for_node(long long node_total, long long max_total) {
    if (!g_opt.scale_by_total || max_total <= 0) return g_opt.graph_height;
    double ratio = static_cast<double>(node_total) / static_cast<double>(max_total);
    int h = static_cast<int>(std::round(g_opt.graph_height * ratio));
    if (h < 2) h = 2; // minimum readable height
    return h;
}

static void print_graph2d_for_node(int n, int H, const NodeStat& s_latest) {
    const auto &dq = g_hist[n];

    // Precompute per-column levels in [0, H*5]
    std::vector<int> col_levels(g_opt.graph_width, 0);
    int pad = g_opt.graph_width - (int)dq.size();
    if (pad < 0) pad = 0;

    auto pct_at = [&](int col)->double{
        if (col < 0) return 0.0;
        if (col < pad) return 0.0;
        size_t idx = (size_t)(col - pad);
        if (idx < dq.size()) return dq[idx];
        return 0.0;
    };
    auto lvl = [&](double pct)->int{
        int L = (int)((pct/100.0) * (H*5) + 0.5);
        if (L < 0) L = 0; if (L > H*5) L = H*5;
        return L;
    };

    for (int x = 0; x < g_opt.graph_width; ++x) {
        col_levels[x] = lvl(pct_at(x));
    }

    // Header with percent, height, total, used, free
    std::cout << "Node " << n
              << "  (" << std::fixed << std::setprecision(1) << (dq.empty()?0.0:dq.back()) << "%, "
              << "H=" << H << ", "
              << "Total=" << std::fixed << std::setprecision(2) << toGB(s_latest.total) << " GB, "
              << "Used="  << std::fixed << std::setprecision(2) << toGB(s_latest.used)  << " GB, "
              << "Free="  << std::fixed << std::setprecision(2) << toGB(s_latest.free)  << " GB"
              << ")\n";

    // Precompute Y labels (top/mid/bottom) in GB for this node
    double total_gb = toGB(s_latest.total);
    double mid_gb   = total_gb / 2.0;

    for (int row = H-1; row >= 0; --row) {
        if (row==H-1)      std::cout << y_label(total_gb);
        else if (row==H/2) std::cout << y_label(mid_gb);
        else if (row==0)   std::cout << y_label(0.0);
        else               std::cout << "         | ";

        auto cell_fill = [&](int L)->int{
            // how much of this row's 5 sub-steps are filled (0..5)
            int base = row * 5;
            int rem  = L - base;
            if (rem < 0) rem = 0;
            if (rem > 5) rem = 5;
            return rem;
        };
        auto step4 = [](int f)->int{
            // map 0..5 -> 0..4 (5 collapses to 4)
            if (f <= 0) return 0;
            if (f >= 5) return 4;
            return f; // 1..4
        };

        for (int x = 0; x < g_opt.graph_width; ++x) {
            int Lc = col_levels[x];
            int Lp = (x>0) ? col_levels[x-1] : col_levels[x]; // first col: prev=curr

            int fc = cell_fill(Lc);
            int fp = cell_fill(Lp);

            int sc = step4(fc);
            int sp = step4(fp);

            const char* sym = pick_symbol_up(sp, sc);

            if (g_opt.use_color) {
                double pct_curr = (double)Lc / (double)(H*5) * 100.0;
                std::string col = color_for_usage(pct_curr, true);
                std::cout << col << sym << "\033[0m";
            } else {
                std::cout << sym;
            }
        }
        std::cout << "\n";
    }
    std::cout << "         +" << std::string(g_opt.graph_width, '-') << "→ time\n\n";
}

static void print_graph2d(const std::vector<int>& nodes,
                          const std::map<int, NodeStat>& latest_by_node) {
    if (g_opt.top_mode) std::cout << "\033[2J\033[H";
    std::cout << "NUMA Node Memory Usage (2D graph)   Time: " << now_string()
              << "   base_height=" << g_opt.graph_height
              << " cols=" << g_opt.graph_width
              << (g_opt.scale_by_total ? "  [scaled by total]" : "")
              << "\n";
    if (g_opt.graph_legend) {
        std::cout << "Legend: ⣿=full, ⣾/⣼/⣸/⢸ partial; bottom-stacked; UP table only; y=memory (GB), x=time\n\n";
    }

    long long max_total = 0;
    for (int n : nodes) {
        auto it = latest_by_node.find(n);
        if (it != latest_by_node.end())
            max_total = std::max(max_total, it->second.total);
    }
    if (max_total <= 0) max_total = 1;

    for (int n : nodes) {
        auto it = latest_by_node.find(n);
        if (it == latest_by_node.end()) continue;
        int H = effective_height_for_node(it->second.total, max_total);
        print_graph2d_for_node(n, H, it->second);
    }

    if (g_opt.top_mode) std::cout << "(Press Ctrl+C to exit)\n";
    std::cout.flush();
}

// ---------------- argp ----------------
const char *argp_program_version = "numa_mem_monitor 1.7";
const char *argp_program_bug_address = "<bugs@example.com>";
static char doc[] = "Per-NUMA node memory usage monitor (libnuma, GNU argp) with 2D Unicode graph (UP-table only), per-node scaled height, and headers incl. Total/Used/Free; y-axis in GB";
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
    KEY_NO_TOP,
    KEY_GRAPH2D,
    KEY_GRAPH_WIDTH,
    KEY_GRAPH_HEIGHT,
    KEY_NO_GRAPH_LEGEND,
    KEY_SCALE_BY_TOTAL
};

static struct argp_option options[] = {
    {"interval", KEY_INTERVAL, "SEC", 0, "Refresh interval in seconds (default 1)"},
    {"nodes",    KEY_NODES,    "LIST",0, "Filter nodes, e.g. 0,1,3-5"},
    {"sort",     KEY_SORT,     "KEY", 0, "Sort by: id|usedpct|used|free (table mode)"},
    {"bars",     KEY_BARS,     0,     0, "Show ASCII bars (table mode)"},
    {"details",  KEY_DETAILS,  0,     0, "Show Anon/File/Slab/Shmem (table mode)"},
    {"no-color", KEY_NO_COLOR, 0,     0, "Disable ANSI colors"},
    {"once",     KEY_ONCE,     0,     0, "Print once and exit"},
    {"top",      KEY_TOP,      0,     0, "Full-screen refresh mode (default)"},
    {"no-top",   KEY_NO_TOP,   0,     0, "Append mode (no screen clear)"},
    // 2D graph options
    {"graph2d",        KEY_GRAPH2D,       0,   0, "Enable 2D graph mode (x=time, y=usage)"},
    {"graph-width",    KEY_GRAPH_WIDTH,  "N",  0, "Graph width (columns, default 80)"},
    {"graph-height",   KEY_GRAPH_HEIGHT, "N",  0, "Graph base height (rows for largest node, default 10)"},
    {"scale-by-total", KEY_SCALE_BY_TOTAL,0,   0, "Scale each node's height by its total memory"},
    {"no-graph-legend",KEY_NO_GRAPH_LEGEND,0,  0, "Hide graph legend"},
    {0}
};

static error_t parse_opt (int key, char *arg, struct argp_state *state) {
    switch (key) {
        case KEY_INTERVAL: g_opt.interval = std::max(1, atoi(arg?arg:"1")); break;
        case KEY_NODES: {
            std::vector<int> tmp;
            if (!parse_nodes_list(arg?std::string(arg):std::string(), tmp))
                argp_error(state, "Invalid --nodes format");
            g_opt.filter_nodes = std::move(tmp);
            break;
        }
        case KEY_SORT: {
            std::string v = arg?std::string(arg):"id";
            if (v=="id"||v=="usedpct"||v=="used"||v=="free") g_opt.sort_key=v;
            else argp_error(state, "--sort must be one of: id|usedpct|used|free");
            break;
        }
        case KEY_BARS: g_opt.bars = true; break;
        case KEY_DETAILS: g_opt.details = true; break;
        case KEY_NO_COLOR: g_opt.use_color = false; break;
        case KEY_ONCE: g_opt.once = true; break;
        case KEY_TOP: g_opt.top_mode = true; break;
        case KEY_NO_TOP: g_opt.top_mode = false; break;
        case KEY_GRAPH2D: g_opt.graph2d = true; g_opt.table = false; break;
        case KEY_GRAPH_WIDTH: g_opt.graph_width = std::max(10, atoi(arg?arg:"80")); break;
        case KEY_GRAPH_HEIGHT: g_opt.graph_height = std::max(2, atoi(arg?arg:"10")); break;
        case KEY_SCALE_BY_TOTAL: g_opt.scale_by_total = true; break;
        case KEY_NO_GRAPH_LEGEND: g_opt.graph_legend = false; break;
        case ARGP_KEY_ARG: argp_usage(state); break; // no positional args
        default: return ARGP_ERR_UNKNOWN;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, args_doc, doc };

// ---------------- main ----------------
int main(int argc, char** argv) {
    argp_parse(&argp, argc, argv, 0, 0, 0);

    if (numa_available() < 0) {
        std::cerr << "NUMA not available.\n";
        return 1;
    }
    std::signal(SIGINT, handle_sigint);

    // Discover and filter nodes
    std::vector<int> online = detect_online_nodes();
    if (online.empty()) { std::cerr << "No online NUMA nodes found.\n"; return 1; }

    std::vector<int> nodes;
    if (g_opt.filter_nodes.empty()) nodes = online;
    else {
        std::sort(online.begin(), online.end());
        for (int n : g_opt.filter_nodes)
            if (std::binary_search(online.begin(), online.end(), n)) nodes.push_back(n);
        if (nodes.empty()) { std::cerr << "No matching nodes online.\n"; return 1; }
    }

    do {
        // Snapshot
        auto stats = snapshot_nodes(nodes, /*details*/ g_opt.details && !g_opt.graph2d);
        sort_stats(stats, g_opt.sort_key);

        // Update history and build latest-by-node map for printing headers
        std::map<int, NodeStat> latest_by_node;
        for (const auto& s : stats) {
            latest_by_node[s.node] = s;
            auto &dq = g_hist[s.node];
            dq.push_back(s.used_pct);
            if ((int)dq.size() > g_opt.graph_width) dq.pop_front();
        }

        // Render
        if (g_opt.graph2d) {
            print_graph2d(nodes, latest_by_node);
        } else {
            print_table(stats);
        }

        if (g_opt.once) break;
        for (int ms = 0; ms < g_opt.interval*1000 && !g_stop; ms += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (!g_stop);

    return 0;
}
