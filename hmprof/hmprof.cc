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

// ---------- data models ----------
struct NodeExtra {
    long long anon_bytes = -1, file_bytes = -1, slab_bytes = -1, shmem_bytes = -1;
};
struct NodeStat {
    int node = -1;
    long long total = 0, free = 0, used = 0;
    double used_pct = 0.0;
    NodeExtra extra;
};

// Per-column historical sample (frozen at sampling time)
struct HistSample {
    double used_pct;          // used / total in [0..100]
    double anon_frac_used;    // anon / used in [0..1]
    double file_frac_used;    // file / used in [0..1]
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
    bool graph2d = false;     // enable 2D graph (x=time, y=usage in GB)
    int  graph_width = 90;    // DEFAULT 90 columns
    int  graph_height = 10;   // rows (for the largest node); each row has 5 sub-steps
    bool graph_legend = true;
    bool scale_by_total = false; // scale each node's height by its total memory

    // CSV logging
    std::string csv_path;     // empty => disabled
    bool csv_append = false;
    bool csv_no_header = false;
    bool csv_wide = false;    // NEW: sample-per-row (wide) mode
} g_opt;

// node -> history (length <= graph_width)
static std::map<int, std::deque<HistSample>> g_hist;

// -------- nvitop-style UP symbol table --------
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

// ---------------- utils ----------------
static std::string now_string() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%F %T", &tm);
    return buf;
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

// ---------------- CSV helpers ----------------
static bool file_exists_and_nonempty(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, std::ios::end);
    return f.tellg() > 0;
}

// narrow header: one row per node
static void csv_write_header_narrow(std::ofstream& ofs) {
    ofs << "timestamp,node,total_bytes,used_bytes,free_bytes,used_pct,"
           "anon_bytes,file_bytes,slab_bytes,shmem_bytes\n";
}

static void csv_write_rows_narrow(std::ofstream& ofs, const std::vector<NodeStat>& stats) {
    std::string ts = now_string();
    for (const auto& s : stats) {
        long long anon_b = s.extra.anon_bytes >= 0 ? s.extra.anon_bytes : 0;
        long long file_b = s.extra.file_bytes >= 0 ? s.extra.file_bytes : 0;
        long long slab_b = s.extra.slab_bytes >= 0 ? s.extra.slab_bytes : 0;
        long long shmem_b= s.extra.shmem_bytes>= 0 ? s.extra.shmem_bytes: 0;
        ofs << ts << ','
            << s.node << ','
            << s.total << ','
            << s.used << ','
            << s.free << ','
            << std::fixed << std::setprecision(2) << s.used_pct << ','
            << anon_b << ','
            << file_b << ','
            << slab_b << ','
            << shmem_b << '\n';
    }
    ofs.flush();
}

// wide header: one row per sample (all nodes in one line)
static void csv_write_header_wide(std::ofstream& ofs, const std::vector<int>& nodes) {
    ofs << "timestamp";
    for (int n : nodes) {
        ofs << ",node" << n << "_total_bytes"
            << ",node" << n << "_used_bytes"
            << ",node" << n << "_free_bytes"
            << ",node" << n << "_used_pct"
            << ",node" << n << "_anon_bytes"
            << ",node" << n << "_file_bytes"
            << ",node" << n << "_slab_bytes"
            << ",node" << n << "_shmem_bytes";
    }
    ofs << "\n";
}

static void csv_write_row_wide(std::ofstream& ofs, const std::vector<NodeStat>& stats, const std::vector<int>& nodes) {
    std::string ts = now_string();
    ofs << ts;

    // Build map: node -> stat
    std::map<int, const NodeStat*> by_node;
    for (const auto& s : stats) by_node[s.node] = &s;

    for (int n : nodes) {
        auto it = by_node.find(n);
        if (it == by_node.end()) {
            // Node missing: write empty fields
            ofs << ",,,,,,,,";
        } else {
            const NodeStat& s = *it->second;
            long long anon_b = s.extra.anon_bytes >= 0 ? s.extra.anon_bytes : 0;
            long long file_b = s.extra.file_bytes >= 0 ? s.extra.file_bytes : 0;
            long long slab_b = s.extra.slab_bytes >= 0 ? s.extra.slab_bytes : 0;
            long long shmem_b= s.extra.shmem_bytes>= 0 ? s.extra.shmem_bytes: 0;
            ofs << ',' << s.total
                << ',' << s.used
                << ',' << s.free
                << ',' << std::fixed << std::setprecision(2) << s.used_pct
                << ',' << anon_b
                << ',' << file_b
                << ',' << slab_b
                << ',' << shmem_b;
        }
    }
    ofs << "\n";
    ofs.flush();
}

// ---------------- table printer ----------------
static void print_table(const std::vector<NodeStat>& stats) {
    auto now = now_string();
    if (g_opt.top_mode) std::cout << "\033[2J\033[H";
    std::cout << "NUMA Node Memory Usage (table)   Time: " << now << "\n";
    int barw = std::max(0, term_width() - 72);
    if (!g_opt.bars) barw = 0;

    std::cout << " Node |   Total(GB)  |    Used(GB)  |    Free(GB)  |  Used% ";
    if (g_opt.bars) std::cout << "| " << std::setw(barw) << std::left << "Usage";
    std::cout << "\n";
    std::cout << "------+--------------+--------------+--------------+--------";
    if (g_opt.bars) std::cout << "+-" << std::string(barw, '-');
    std::cout << "\n";

    for (const auto& s : stats) {
        const char* rst = CLR_RESET(g_opt.use_color);
        double used_pct = s.used_pct;
        std::string col;
        if (g_opt.use_color) {
            if (used_pct < 50) col = "\033[32m";
            else if (used_pct < 80) col = "\033[33m";
            else col = "\033[31m";
        }
        printf(" %4d | %10.2f GB | %10.2f GB | %10.2f GB |  %s%6.2f%%%s",
               s.node, toGB(s.total), toGB(s.used), toGB(s.free),
               col.c_str(), used_pct, rst);
        if (g_opt.bars) {
            int width = barw;
            int filled = (int)((used_pct/100.0)*width + 0.5);
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

// ---------------- graph helpers (unchanged from v2.0) ----------------
static int effective_height_for_node(long long node_total, long long max_total) {
    if (!g_opt.scale_by_total || max_total <= 0) return g_opt.graph_height;
    double ratio = static_cast<double>(node_total) / static_cast<double>(max_total);
    int h = static_cast<int>(std::round(g_opt.graph_height * ratio));
    if (h < 2) h = 2;
    return h;
}
static int choose_tick_step_secs(int total_span_secs) {
    static const int CAND[] = {
        5, 10, 15, 20, 30,
        60, 120, 180, 300, 600,
        900, 1200, 1800, 3600
    };
    for (int step : CAND) {
        int ticks = (total_span_secs >= step) ? (total_span_secs / step) : 0;
        if (ticks >= 4 && ticks <= 8) return step;
    }
    int step = std::max(1, total_span_secs / 8);
    step = ((step + 4) / 5) * 5;
    if (step == 0) step = 1;
    return step;
}
static const char* color_anon_file_other_by_ratio(double t_in_used, double anon_frac, double file_frac, bool use_color) {
    if (!use_color) return "";
    static const char* C_ANON = "\033[32m";
    static const char* C_FILE = "\033[33m";
    static const char* C_OTHR = "\033[34m";
    if (t_in_used <= anon_frac + 1e-9) return C_ANON;
    if (t_in_used <= anon_frac + file_frac + 1e-9) return C_FILE;
    return C_OTHR;
}

// ---------------- 2D graph printer (abridged: identical to v2.0 except x-label line placement) ----------------
static void print_graph2d_for_node(
    int n, int H, const NodeStat& s_latest, const std::deque<HistSample>& hist
) {
    const int steps_total = H * 5;
    std::vector<int> used_steps(g_opt.graph_width, 0);
    std::vector<double> anon_frac(g_opt.graph_width, 0.0);
    std::vector<double> file_frac(g_opt.graph_width, 0.0);

    int pad = g_opt.graph_width - (int)hist.size();
    if (pad < 0) pad = 0;

    auto clampi = [&](int v, int lo, int hi){ return std::max(lo, std::min(hi, v)); };

    for (int x = 0; x < g_opt.graph_width; ++x) {
        if (x < pad) {
            used_steps[x] = 0;
            anon_frac[x] = file_frac[x] = 0.0;
        } else {
            const HistSample& hs = hist[x - pad];
            double u = std::max(0.0, std::min(100.0, hs.used_pct)) / 100.0;
            int steps = (int)std::round(u * steps_total);
            used_steps[x] = clampi(steps, 0, steps_total);
            anon_frac[x]  = std::max(0.0, std::min(1.0, hs.anon_frac_used));
            file_frac[x]  = std::max(0.0, std::min(1.0, hs.file_frac_used));
            if (anon_frac[x] + file_frac[x] > 1.0) {
                double sum = anon_frac[x] + file_frac[x];
                anon_frac[x] /= sum;
                file_frac[x] /= sum;
            }
        }
    }

    std::cout << "Node " << n
              << "  (" << std::fixed << std::setprecision(1) << s_latest.used_pct << "%, "
              << "H=" << H << ", "
              << "Total=" << std::fixed << std::setprecision(2) << toGB(s_latest.total) << " GB, "
              << "Used="  << std::fixed << std::setprecision(2) << toGB(s_latest.used)  << " GB, "
              << "Free="  << std::fixed << std::setprecision(2) << toGB(s_latest.free)  << " GB"
              << ")\n";

    double total_gb = toGB(s_latest.total);
    double mid_gb   = total_gb / 2.0;

    for (int row = H-1; row >= 0; --row) {
        if (row==H-1)      std::cout << y_label(total_gb);
        else if (row==H/2) std::cout << y_label(mid_gb);
        else if (row==0)   std::cout << y_label(0.0);
        else               std::cout << "         | ";

        auto cell_fill = [&](int used_steps_col)->int{
            int base = row * 5;
            int rem  = used_steps_col - base;
            if (rem < 0) rem = 0;
            if (rem > 5) rem = 5;
            return rem;
        };
        auto step4 = [](int f)->int{
            if (f <= 0) return 0;
            if (f >= 5) return 4;
            return f;
        };

        for (int x = 0; x < g_opt.graph_width; ++x) {
            int uc = used_steps[x];
            int up = (x>0) ? used_steps[x-1] : used_steps[x];

            int fc = cell_fill(uc);
            int fp = cell_fill(up);

            int sc = step4(fc);
            int sp = step4(fp);
            const char* sym = pick_symbol_up(sp, sc);

            if (sc == 0) {
                std::cout << sym;
                continue;
            }

            int level_steps = row*5 + std::min(fc, 5);
            double t_in_used = (uc > 0) ? (double)level_steps / (double)uc : 0.0;
            if (t_in_used < 0.0) t_in_used = 0.0;
            if (t_in_used > 1.0) t_in_used = 1.0;

            const char* col = color_anon_file_other_by_ratio(t_in_used, anon_frac[x], file_frac[x], g_opt.use_color);
            std::cout << col << sym << CLR_RESET(g_opt.use_color);
        }
        std::cout << "\n";
    }

    // X-axis and labels on the next line
    std::string axis(g_opt.graph_width, '-');
    int total_span_secs = (g_opt.graph_width - 1) * g_opt.interval;
    int step_secs = choose_tick_step_secs(total_span_secs);
    std::cout << "         +" << axis << "→ time\n";

    std::string labels(g_opt.graph_width, ' ');
    auto place_label = [&](int col_index_from_left, const std::string& txt){
        if (col_index_from_left < 0 || col_index_from_left >= (int)labels.size()) return;
        int start = col_index_from_left - (int)txt.size()/2;
        if (start < 0) start = 0;
        if (start + (int)txt.size() > (int)labels.size())
            start = (int)labels.size() - (int)txt.size();
        for (size_t i=0; i<txt.size(); ++i)
            labels[start + (int)i] = txt[i];
    };
    for (int age = step_secs; age <= total_span_secs; age += step_secs) {
        int pos_from_right = age / g_opt.interval;
        int col_left = g_opt.graph_width - 1 - pos_from_right;
        if (col_left < 0) break;
        std::string label = (age >= 60 && age % 60 == 0)
                            ? "-" + std::to_string(age/60) + "m"
                            : "-" + std::to_string(age) + "s";
        place_label(col_left, label);
    }
    place_label(g_opt.graph_width - 1, "now");
    std::cout << "          " << labels << "\n\n";
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
        std::cout << "Legend (colors bottom→top within USED at each time column): "
                  << "\033[32mAnon\033[0m, "
                  << "\033[33mFilePages\033[0m, "
                  << "\033[34mOther\033[0m"
                  << "   y=memory (GB labels from latest), x=time\n\n";
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
        auto hit = g_hist.find(n);
        if (hit == g_hist.end()) continue;
        print_graph2d_for_node(n, H, it->second, hit->second);
    }

    if (g_opt.top_mode) std::cout << "(Press Ctrl+C to exit)\n";
    std::cout.flush();
}

// ---------------- argp ----------------
const char *argp_program_version = "numa_mem_monitor 2.1";
const char *argp_program_bug_address = "<bugs@example.com>";
static char doc[] = "Per-NUMA node memory monitor with 2D Unicode graph and CSV logging (wide mode supported)";
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
    KEY_SCALE_BY_TOTAL,
    // CSV
    KEY_CSV,
    KEY_CSV_APPEND,
    KEY_CSV_NO_HEADER,
    KEY_CSV_WIDE
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
    {"graph2d",        KEY_GRAPH2D,       0,   0, "Enable 2D graph mode (x=time, y=GB)"},
    {"graph-width",    KEY_GRAPH_WIDTH,  "N",  0, "Graph width (columns, default 90)"},
    {"graph-height",   KEY_GRAPH_HEIGHT, "N",  0, "Graph base height (rows for largest node, default 10)"},
    {"scale-by-total", KEY_SCALE_BY_TOTAL,0,   0, "Scale each node's height by its total memory"},
    {"no-graph-legend",KEY_NO_GRAPH_LEGEND,0,  0, "Hide graph legend"},
    // CSV
    {"csv",            KEY_CSV,          "PATH", 0, "Write snapshots to CSV at PATH"},
    {"csv-append",     KEY_CSV_APPEND,    0,     0, "Append to CSV if it exists"},
    {"csv-no-header",  KEY_CSV_NO_HEADER, 0,     0, "Do not write CSV header row"},
    {"csv-wide",       KEY_CSV_WIDE,      0,     0, "Write one CSV row per sample (all nodes in one line)"},
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
        case KEY_GRAPH_WIDTH: g_opt.graph_width = std::max(10, atoi(arg?arg:"90")); break; // default 90
        case KEY_GRAPH_HEIGHT: g_opt.graph_height = std::max(2, atoi(arg?arg:"10")); break;
        case KEY_SCALE_BY_TOTAL: g_opt.scale_by_total = true; break;
        case KEY_NO_GRAPH_LEGEND: g_opt.graph_legend = false; break;
        // CSV
        case KEY_CSV: g_opt.csv_path = arg ? std::string(arg) : std::string(); break;
        case KEY_CSV_APPEND: g_opt.csv_append = true; break;
        case KEY_CSV_NO_HEADER: g_opt.csv_no_header = true; break;
        case KEY_CSV_WIDE: g_opt.csv_wide = true; break;
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

    // Discover and filter nodes (this order defines the CSV-wide header order)
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

    // CSV setup
    std::ofstream csv_ofs;
    bool csv_enabled = !g_opt.csv_path.empty();
    if (csv_enabled) {
        std::ios::openmode mode = std::ios::out;
        if (g_opt.csv_append) mode |= std::ios::app;
        csv_ofs.open(g_opt.csv_path, mode);
        if (!csv_ofs.is_open()) {
            std::cerr << "Failed to open CSV path: " << g_opt.csv_path << "\n";
            return 1;
        }
        bool need_header = !g_opt.csv_no_header;
        if (g_opt.csv_append && file_exists_and_nonempty(g_opt.csv_path)) {
            need_header = false;
        }
        if (need_header) {
            if (g_opt.csv_wide) csv_write_header_wide(csv_ofs, nodes);
            else                csv_write_header_narrow(csv_ofs);
        }
    }

    do {
        // Snapshot (need details in graph2d OR CSV to get anon/file/slab/shmem)
        bool need_details = g_opt.details || g_opt.graph2d || csv_enabled;
        auto stats = snapshot_nodes(nodes, /*details*/ need_details);
        sort_stats(stats, g_opt.sort_key);

        // Update history from this snapshot (for graph2d)
        std::map<int, NodeStat> latest_by_node;
        for (const auto& s : stats) {
            latest_by_node[s.node] = s;

            // compute anon/file fractions INSIDE used for this snapshot
            double used_gb = toGB(s.used);
            double anon_gb = 0.0, file_gb = 0.0;
            if (need_details) {
                NodeExtra ex = s.extra;
                if (ex.anon_bytes < 0 || ex.file_bytes < 0)
                    (void)read_node_meminfo(s.node, ex);
                anon_gb = (ex.anon_bytes >= 0) ? toGB(ex.anon_bytes) : 0.0;
                file_gb = (ex.file_bytes >= 0) ? toGB(ex.file_bytes) : 0.0;
            }
            if (anon_gb < 0) anon_gb = 0;
            if (file_gb < 0) file_gb = 0;
            if (anon_gb > used_gb) anon_gb = used_gb;
            if (file_gb > std::max(0.0, used_gb - anon_gb))
                file_gb = std::max(0.0, used_gb - anon_gb);

            HistSample hs;
            hs.used_pct = s.used_pct;
            if (used_gb > 0) {
                hs.anon_frac_used = anon_gb / used_gb;
                hs.file_frac_used = file_gb / used_gb;
            } else {
                hs.anon_frac_used = hs.file_frac_used = 0.0;
            }

            auto &dq = g_hist[s.node];
            dq.push_back(hs);
            if ((int)dq.size() > g_opt.graph_width) dq.pop_front();
        }

        // CSV: write snapshot
        if (csv_enabled) {
            if (g_opt.csv_wide) csv_write_row_wide(csv_ofs, stats, nodes);
            else                csv_write_rows_narrow(csv_ofs, stats);
        }

        // Render to terminal
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
