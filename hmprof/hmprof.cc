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

struct NodeExtra { long long anon_bytes=-1,file_bytes=-1,slab_bytes=-1,shmem_bytes=-1; };
struct NodeStat {
    int node=-1; long long total=0, free=0, used=0; double used_pct=0.0; NodeExtra extra;
};

// ---------------- Options ----------------
struct Options {
    int interval = 1;
    bool use_color = true;
    bool top_mode = true;
    bool once = false;
    bool details = false;   // table 모드에서만 사용
    std::string sort_key = "id"; // id|usedpct|used|free
    std::vector<int> filter_nodes;

    // table(bar) 모드 (기존)
    bool bars = false;

    // 2D graph 모드
    bool graph2d = false;       // --graph2d
    int  graph_width = 80;      // 열(시간) 개수
    int  graph_height = 10;     // 행(세로 셀 수) — 각 셀은 5 단계 부분 채움
    bool graph_legend = true;
} g_opt;

// node -> history of used_pct (0~100), 길이=graph_width
static std::map<int, std::deque<double>> g_hist;

// ---------------- nvitop-style charset (UP 계열 일부) ----------------
// 부분 채움 정도(0~4)에 따른 문자 (세로로 위에서 차오르는 느낌)
static inline char32_t partial_up_symbol(int part /*1..4*/) {
    // nvitop VALUE2SYMBOL_UP에서 (0..4, 4) 열을 사용: 0→'⢸', 1→'⣸', 2→'⣼', 3→'⣾'
    // 다만 "0"은 빈 칸을 써야 하므로 1..4만 대응하고, 0은 ' ' 처리.
    switch (part) {
        case 1: return U'⢸'; // (0,4)
        case 2: return U'⣸'; // (1,4)
        case 3: return U'⣼'; // (2,4)
        case 4: return U'⣾'; // (3,4)
        default: return U' '; // safety
    }
}

#if 0
static inline std::string u32_to_utf8(char32_tch){
    // 간단한 UTF-8 변환 (BMP 및 이 코드포인트군에 충분)
    std::string out;
    uint32_t c = (uint32_t)ch;
    if (c <= 0x7F) { out.push_back(char(c)); }
    else if (c <= 0x7FF) {
        out.push_back(char(0xC0 | ((c>>6)&0x1F)));
        out.push_back(char(0x80 | (c&0x3F)));
    } else if (c <= 0xFFFF) {
        out.push_back(char(0xE0 | ((c>>12)&0x0F)));
        out.push_back(char(0x80 | ((c>>6)&0x3F)));
        out.push_back(char(0x80 | (c&0x3F)));
    } else {
        out.push_back(char(0xF0 | ((c>>18)&0x07)));
        out.push_back(char(0x80 | ((c>>12)&0x3F)));
        out.push_back(char(0x80 | ((c>>6)&0x3F)));
        out.push_back(char(0x80 | (c&0x3F)));
    }
    return out;
}
static inline std::string cell_symbol_from_fill(int fill /*0..5*/) {
    // 한 셀(높이 1) 내의 채움 정도 0..5 → 문자
    // 0: ' ' (빈칸), 1..4: 부분, 5: '⣿' (풀)
    if (fill <= 0) return " ";
    if (fill >= 5) return "⣿";
    return u32_to_utf8(partial_up_symbol(fill));
}
#else
static inline std::string u32_to_utf8(char32_t ch) {
    std::string out;
    uint32_t c = static_cast<uint32_t>(ch);
    if (c <= 0x7F) {
        out.push_back(static_cast<char>(c));
    } else if (c <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((c >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((c >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((c >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    return out;
}

static inline std::string cell_symbol_from_fill(int fill) {
    if (fill <= 0) return " ";
    if (fill >= 5) return "⣿";
    return u32_to_utf8(partial_up_symbol(fill));  // 1..4 → ⢸/⣸/⣼/⣾
}
#endif

// ---------------- Utils ----------------
static std::string now_string() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{}; localtime_r(&t, &tm);
    char buf[64]; strftime(buf, sizeof(buf), "%F %T", &tm); return buf;
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
    std::stringstream ss(s); std::string tok; std::vector<int> res;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        size_t dash = tok.find('-');
        try {
            if (dash == std::string::npos) res.push_back(std::stoi(tok));
            else {
                int a = std::stoi(tok.substr(0, dash)), b = std::stoi(tok.substr(dash+1));
                if (b < a) std::swap(a,b);
                for (int i=a;i<=b;i++) res.push_back(i);
            }
        } catch (...) { return false; }
    }
    std::sort(res.begin(), res.end());
    res.erase(std::unique(res.begin(), res.end()), res.end());
    out = std::move(res); return true;
}

static bool read_node_meminfo(int node, NodeExtra& ex) {
    std::ostringstream p; p << "/sys/devices/system/node/node" << node << "/meminfo";
    std::ifstream f(p.str()); if (!f.is_open()) return false;
    std::string line; std::regex re(R"(Node\s+\d+\s+([A-Za-z]+):\s+(\d+)\s+kB)");
    while (std::getline(f, line)) {
        std::smatch m;
        if (std::regex_search(line, m, re) && m.size()==3) {
            std::string key = m[1].str(); long long kb = std::stoll(m[2].str()); long long bytes = kb*1024LL;
            if (key=="AnonPages") ex.anon_bytes = bytes;
            else if (key=="FilePages") ex.file_bytes = bytes;
            else if (key=="Slab") ex.slab_bytes = bytes;
            else if (key=="Shmem") ex.shmem_bytes = bytes;
        }
    }
    return true;
}

static std::vector<int> detect_online_nodes() {
    std::vector<int> nodes; int maxn = numa_max_node();
    for (int n=0;n<=maxn;n++) if (numa_node_size64(n, nullptr) > 0) nodes.push_back(n);
    return nodes;
}

static std::vector<NodeStat> snapshot_nodes(const std::vector<int>& nodes, bool want_details) {
    std::vector<NodeStat> stats; stats.reserve(nodes.size());
    for (int n : nodes) {
        long long free_b = 0; long long total_b = numa_node_size64(n, &free_b);
        if (total_b <= 0) continue;
        long long used_b = total_b - free_b;
        NodeStat s; s.node=n; s.total=total_b; s.free=free_b; s.used=used_b;
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

// ------------- Printers -------------
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

// 2D graph: x=time (좌→우 오래→최신), y=usage% (아래→위)
// 각 패널은 graph_height 행 × graph_width 열의 그리드.
// 한 셀은 0..5 단계 부분 채움으로 렌더링(⢸/⣸/⣼/⣾/⣿).
static void print_graph2d(const std::vector<int>& nodes) {
    if (g_opt.top_mode) std::cout << "\033[2J\033[H";
    std::cout << "NUMA Node Memory Usage (2D graph)   Time: " << now_string()
              << "   height=" << g_opt.graph_height << " cells, width=" << g_opt.graph_width << "\n";
    if (g_opt.graph_legend) {
        std::cout << "Legend: ⣿=full, ⣾/⣼/⣸/⢸=partial, ' '=empty   y=usage%, x=time (old→new)\n\n";
    }

    // 각 노드별 패널을 차례로 출력
    for (int n : nodes) {
        const auto &dq = g_hist[n];
        // 라벨
        std::cout << "Node " << n << "  (" << std::fixed << std::setprecision(1)
                  << (dq.empty()?0.0:dq.back()) << "%)\n";

        // 열 별로 사용률을 0..(H*5) 스텝으로 양자화
        int H = g_opt.graph_height;
        std::vector<int> col_levels(g_opt.graph_width, 0); // per column quantized height in 0..H*5
        // 왼쪽 패딩: 히스토리가 짧으면 앞을 비움
        int pad = g_opt.graph_width - (int)dq.size();
        if (pad < 0) pad = 0;

        for (int x = 0; x < g_opt.graph_width; ++x) {
            double pct = 0.0;
            if (x >= pad) {
                size_t idx = (size_t)(x - pad);
                if (idx < dq.size()) pct = dq[idx];
            }
            // 0..100 → 0..(H*5)
            int levels = (int)((pct / 100.0) * (H * 5) + 0.5);
            if (levels < 0) levels = 0;
            if (levels > H*5) levels = H*5;
            col_levels[x] = levels;
        }

        // 위쪽 행부터 출력 (top→bottom)
        for (int row = H-1; row >= 0; --row) {
            // y축 눈금(간단): 최상단/중간/바닥에만 표시
            if (row==H-1)      std::cout << " 100% | ";
            else if (row==H/2) std::cout << "  50% | ";
            else if (row==0)   std::cout << "   0% | ";
            else               std::cout << "      | ";

            for (int x = 0; x < g_opt.graph_width; ++x) {
                int remaining = col_levels[x] - row*5; // 이 셀에 들어갈 채움량 0..5
                std::string sym = cell_symbol_from_fill(remaining);
                if (g_opt.use_color) {
                    // 현재 열의 실제 사용률 기반 색상
                    double pct = 0.0;
                    if (x >= pad) {
                        size_t idx = (size_t)(x - pad);
                        if (idx < dq.size()) pct = dq[idx];
                    }
                    std::string col = color_for_usage(pct, true);
                    std::cout << col << sym << "\033[0m";
                } else {
                    std::cout << sym;
                }
            }
            std::cout << "\n";
        }
        // x축
        std::cout << "       +" << std::string(g_opt.graph_width, '-') << "→ time\n\n";
    }
    if (g_opt.top_mode) std::cout << "(Press Ctrl+C to exit)\n";
    std::cout.flush();
}

// ---------------- argp ----------------
const char *argp_program_version = "numa_mem_monitor 1.2";
const char *argp_program_bug_address = "<bugs@example.com>";
static char doc[] = "Per-NUMA node memory usage monitor (libnuma, GNU argp) with 2D Unicode graph";
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
    KEY_NO_GRAPH_LEGEND
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
    // 2D graph
    {"graph2d",        KEY_GRAPH2D,       0,   0, "Enable 2D graph mode (x=time, y=usage)"},
    {"graph-width",    KEY_GRAPH_WIDTH,  "N",  0, "Graph width (columns, default 80)"},
    {"graph-height",   KEY_GRAPH_HEIGHT, "N",  0, "Graph height (cells, default 10)"},
    {"no-graph-legend",KEY_NO_GRAPH_LEGEND,0,  0, "Hide graph legend"},
    {0}
};

static Options g_default;

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
        case KEY_GRAPH2D: g_opt.graph2d = true; break;
        case KEY_GRAPH_WIDTH: g_opt.graph_width = std::max(10, atoi(arg?arg:"80")); break;
        case KEY_GRAPH_HEIGHT: g_opt.graph_height = std::max(2, atoi(arg?arg:"10")); break;
        case KEY_NO_GRAPH_LEGEND: g_opt.graph_legend = false; break;
        case ARGP_KEY_ARG: argp_usage(state); break; // no positional args
        default: return ARGP_ERR_UNKNOWN;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, args_doc, doc };

// --------------- main ---------------
int main(int argc, char** argv) {
    g_default = Options(); g_opt = g_default;
    argp_parse(&argp, argc, argv, 0, 0, 0);

    if (numa_available() < 0) { std::cerr << "NUMA not available.\n"; return 1; }
    std::signal(SIGINT, handle_sigint);

    // 노드 탐색/필터
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

    // 루프
    do {
        auto stats = snapshot_nodes(nodes, /*details*/ g_opt.details && !g_opt.graph2d);
        sort_stats(stats, g_opt.sort_key);

        // 히스토리 갱신 (노드별)
        for (const auto& s : stats) {
            auto &dq = g_hist[s.node];
            dq.push_back(s.used_pct);
            if ((int)dq.size() > g_opt.graph_width) dq.pop_front();
        }

        if (g_opt.graph2d) {
            print_graph2d(nodes);
        } else {
            // 기존 테이블 모드
            print_table(stats);
        }

        if (g_opt.once) break;
        for (int ms = 0; ms < g_opt.interval*1000 && !g_stop; ms += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (!g_stop);

    return 0;
}
