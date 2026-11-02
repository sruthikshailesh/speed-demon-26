// Speed Demon 2026, Track A (Ingest): per-hour, per-endpoint latency summary.
//
// mmap + thread-local aggregation; compact the merge into a direct hour/endpoint grid.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sched.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace {
constexpr uint32_t kEndpointLimit = 1842, kHourCount = 72;
constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
constexpr size_t kBatch = 2048;  // groups per output batch

struct Group {
    uint32_t hour, ep, count = 0, errors = 0, p50 = 0, p95 = 0, p99 = 0, off = 0, cap = 0;
};
struct Work {  // one per thread; hash slots hold index + 1, 0 means empty
    std::vector<std::string_view> ep;  // endpoint names, views into the mapping
    std::vector<uint64_t> eh;          // their hashes
    std::vector<Group> group;
    std::vector<uint32_t> es = std::vector<uint32_t>(2048), gs = std::vector<uint32_t>(kEndpointLimit * kHourCount);
    size_t used = 0, capacity = 0;
    std::unique_ptr<uint32_t[]> samples;
    size_t last = 0;                   // most recently used group
    explicit Work(size_t bytes = 0) : capacity(bytes / 10 + 1), samples(new uint32_t[capacity]) {}
};
int thread_count() {
    unsigned n = std::thread::hardware_concurrency();
#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) n = CPU_COUNT(&set);
#endif
    return int(std::min(12u, std::max(1u, n)));
}
template <class F> void par(int n, F f) {
    std::vector<std::thread> ts;
    for (int i = 0; i < n; ++i) ts.emplace_back([&f, i] { f(i); });
    for (std::thread& t : ts) t.join();
}
inline uint64_t hash_bytes(const char* s, size_t n) {  // length + first and last 8 bytes; equality is still a full compare
    uint64_t first = 0, last = 0;
    if (n >= 8) { std::memcpy(&first, s, 8); std::memcpy(&last, s + n - 8, 8); }
    else std::memcpy(&first, s, n);
    uint64_t h = ((first ^ n * kMul) * kMul ^ last) * kMul;
    return h ^ (h >> 32);
}
template <class H> void grow(std::vector<uint32_t>& s, size_t n, H hash) {
    std::vector<uint32_t> t(s.size() * 2);
    for (size_t i = 0; i < n; ++i) {
        size_t j = hash(i) & (t.size() - 1);
        while (t[j]) j = (j + 1) & (t.size() - 1);
        t[j] = uint32_t(i + 1);
    }
    s.swap(t);
}
uint32_t add_ep(Work& w, std::string_view s) {
    const uint64_t h = hash_bytes(s.data(), s.size());
    const size_t m = w.es.size() - 1;
    size_t j = h & m;
    for (; w.es[j]; j = (j + 1) & m)
        if (w.eh[w.es[j] - 1] == h && w.ep[w.es[j] - 1] == s) return w.es[j] - 1;
    w.ep.push_back(s);
    w.eh.push_back(h);
    w.es[j] = uint32_t(w.ep.size());
    if (w.ep.size() * 2 > w.es.size()) grow(w.es, w.ep.size(), [&](size_t i) { return w.eh[i]; });
    return uint32_t(w.ep.size() - 1);
}
Group& add_group(Work& w, uint32_t hour, uint32_t ep) {
    const size_t j = size_t(hour) * kEndpointLimit + ep;
    if (w.gs[j]) { w.last = w.gs[j] - 1; return w.group[w.last]; }
    w.group.push_back(Group{hour, ep});
    w.last = w.group.size() - 1;
    w.gs[j] = uint32_t(w.group.size());
    return w.group.back();
}
void add_sample(Work& w, Group& g, uint32_t value) {
    if (g.count == g.cap) {
        const uint32_t cap = g.cap ? g.cap * 2 : 1;
        if (w.used + cap > w.capacity) {
            size_t n = std::max(w.capacity * 2, w.used + cap);
            std::unique_ptr<uint32_t[]> p(new uint32_t[n]);
            std::memcpy(p.get(), w.samples.get(), w.used * sizeof(uint32_t));
            w.samples = std::move(p);
            w.capacity = n;
        }
        if (g.count) std::memcpy(w.samples.get() + w.used, w.samples.get() + g.off, size_t(g.count) * sizeof(uint32_t));
        g.off = uint32_t(w.used);
        g.cap = cap;
        w.used += cap;
    }
    w.samples[g.off + g.count] = value;
    ++g.count;
}
inline uint32_t hour_key(const char* p) {  // Fixed 72-hour spec span -> [0,72).
    const uint32_t month = uint32_t(p[5] - '0') * 10 + uint32_t(p[6] - '0');
    const uint32_t day = uint32_t(p[8] - '0') * 10 + uint32_t(p[9] - '0');
    const uint32_t hour = uint32_t(p[11] - '0') * 10 + uint32_t(p[12] - '0');
    return (month == 10 ? day - 30 : day + 1) * 24 + hour - 18;
}
// One line is "<27-byte timestamp> <method> <endpoint> <status> <latency>"; the spec guarantees it.
void parse_slice(const char* p, const char* end, Work& w) {
    const char* hp = nullptr;  // line whose hour prefix was last decoded
    uint32_t hour = 0;
    while (p < end) {
        const char* e = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
        if (!e) e = end;
        const char* b = p;
        p = e < end ? e + 1 : end;
        if (e - b < 30) continue;
        if (!hp || std::memcmp(b, hp, 13)) { hour = hour_key(b); hp = b; }
        const char* q = static_cast<const char*>(std::memchr(b + 28, ' ', size_t(e - b - 28)));  // after method
        if (!q) continue;
        const char* ep = q + 1;
        q = static_cast<const char*>(std::memchr(ep, ' ', size_t(e - ep)));  // after endpoint
        if (!q) continue;
        uint32_t lat = 0, st = 0;
        const char* r = q + 1;
        bool error;
        if (q + 4 < e && q[4] == ' ') { error = q[1] == '5'; r = q + 5; }
        else { while (r < e && *r != ' ') st = st * 10 + uint32_t(*r++ - '0'); error = st / 100 == 5; ++r; }
        for (; r < e; ++r) lat = lat * 10 + uint32_t(*r - '0');
        const uint32_t id = add_ep(w, std::string_view(ep, size_t(q - ep)));
        Group* g = w.group.empty() ? nullptr : &w.group[w.last];
        if (!g || g->hour != hour || g->ep != id) g = &add_group(w, hour, id);
        g->errors += error;
        add_sample(w, *g, lat);
    }
}
void score_group(Group& g, uint32_t* samples) {  // nearest rank: ceil(p*n/100), 1-based
    uint32_t* p = samples + g.off;
    const size_t n = g.count;
    const size_t r[3] = {(50 * n + 99) / 100 - 1, (95 * n + 99) / 100 - 1, (99 * n + 99) / 100 - 1};
    uint32_t* out[3] = {&g.p50, &g.p95, &g.p99};
    if (n <= 32) std::sort(p, p + n);
    for (int k = 0, lo = 0; k < 3; ++k) {  // each selection only has to look right of the previous one
        if (n > 32) std::nth_element(p + lo, p + r[k], p + n);
        *out[k] = p[r[k]];
        lo = int(r[k]);
    }
}
inline void put(char*& p, uint64_t v, int width = 0) {
    char t[24];
    int n = 0;
    do { t[n++] = char('0' + v % 10); v /= 10; } while (v || n < width);
    while (n) *p++ = t[--n];
}
void append_line(std::string& out, const Group& g, uint32_t hour_slot, std::string_view ep) {
    char b[128], *p = b;
    const uint32_t d = hour_slot + 18, day = d / 24, hour = d % 24;
    static constexpr uint8_t md[4][2] = {{10, 30}, {10, 31}, {11, 1}, {11, 2}};
    put(p, 2026, 4); *p++ = '-'; put(p, md[day][0], 2); *p++ = '-'; put(p, md[day][1], 2); *p++ = 'T'; put(p, hour, 2);
    std::memcpy(p, ":00:00Z ", 8); p += 8;
    out.append(b, size_t(p - b));
    out.append(ep);
    p = b;
    for (uint64_t v : {uint64_t(g.count), uint64_t(g.p50), uint64_t(g.p95), uint64_t(g.p99), uint64_t(g.errors),
                       uint64_t(g.errors) * 1000000 / g.count}) {
        *p++ = ' ';
        put(p, v);
    }
    *p++ = '\n';
    out.append(b, size_t(p - b));
}
}  // namespace
int main(int argc, char** argv) {
    const char *data_dir = "/data", *out_dir = "/out";
    for (int i = 1; i + 1 < argc; i += 2) (std::strcmp(argv[i], "--data") ? out_dir : data_dir) = argv[i + 1];
    const int fd = open((std::string(data_dir) + "/requests.log").c_str(), O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) return 1;
    const size_t size = size_t(st.st_size);
    const char* map = size ? static_cast<const char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0)) : "";
    if (map == MAP_FAILED) return 1;
    // Slice boundaries sit just after a newline so no line is split.
    const int T = thread_count();
    std::vector<size_t> cut(T + 1, size);
    cut[0] = 0;
    for (int t = 1; t < T; ++t) {
        const size_t pos = std::max(cut[t - 1], size * t / T);
        const void* nl = std::memchr(map + pos, '\n', size - pos);
        cut[t] = nl ? size_t(static_cast<const char*>(nl) - map) + 1 : size;
    }
    std::vector<Work> work;
    work.reserve(T);
    for (int t = 0; t < T; ++t) work.emplace_back(cut[t + 1] - cut[t]);
    par(T, [&](int t) { parse_slice(map + cut[t], map + cut[t + 1], work[t]); });
    // Endpoint ranks give the required byte order; the hour/endpoint domain is small and dense.
    std::vector<std::string_view> names;
    for (const Work& w : work) names.insert(names.end(), w.ep.begin(), w.ep.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    const size_t E = names.size(), K = size_t(72) * E;
    std::vector<Group> groups(K);
    std::vector<uint32_t> rank_cursor(K);
    std::vector<std::vector<uint32_t>> ranks(T);
    for (int t = 0; t < T; ++t) {
        std::vector<uint32_t> rank;
        rank.reserve(work[t].ep.size());
        for (std::string_view s : work[t].ep) rank.push_back(uint32_t(std::lower_bound(names.begin(), names.end(), s) - names.begin()));
        ranks[t] = std::move(rank);
        for (Group& g : work[t].group) {
            Group& d = groups[size_t(g.hour) * E + ranks[t][g.ep]];
            d.count += g.count;
            d.errors += g.errors;
        }
    }
    size_t total = 0;
    for (size_t i = 0; i < K; ++i) {
        groups[i].off = uint32_t(total);
        rank_cursor[i] = groups[i].off;
        total += groups[i].count;
    }
    std::unique_ptr<uint32_t[]> samples(new uint32_t[total]);
    for (int t = 0; t < T; ++t) {
        for (const Group& g : work[t].group) {
            const size_t i = size_t(g.hour) * E + ranks[t][g.ep];
            std::memcpy(samples.get() + rank_cursor[i], work[t].samples.get() + g.off, size_t(g.count) * sizeof(uint32_t));
            rank_cursor[i] += g.count;
        }
    }
    // Select exact nearest ranks and format populated keys in parallel batches.
    const size_t nb = (K + kBatch - 1) / kBatch;
    std::vector<std::string> bufs(nb);
    std::atomic<size_t> next{0};
    par(T, [&](int) {
        for (size_t b; (b = next.fetch_add(1, std::memory_order_relaxed)) < nb;)
            for (size_t i = b * kBatch; i < std::min(K, (b + 1) * kBatch); ++i) {
                if (!groups[i].count) continue;
                score_group(groups[i], samples.get());
                append_line(bufs[b], groups[i], uint32_t(i / E), names[i % E]);
            }
    });
    mkdir(out_dir, 0777);
    const int ofd = open((std::string(out_dir) + "/summary.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) return 1;
    for (const std::string& s : bufs)
        for (size_t o = 0; o < s.size();) {
            const ssize_t n = write(ofd, s.data() + o, s.size() - o);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return 1;
            o += size_t(n);
        }
    _exit(0);  // skip tearing down millions of small objects
}
