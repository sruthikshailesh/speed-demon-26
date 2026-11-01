// Speed Demon 2026, Track A (Ingest): per-hour, per-endpoint latency summary.
//
// requests.log is mmap'ed and cut into newline-aligned slices, one per thread.
// Each thread aggregates its slice in private open-addressing tables; the slices
// are then merged in (hour, endpoint) order, and percentile selection plus text
// formatting run together in parallel batches that are written out in order.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
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

constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
constexpr size_t kBatch = 2048;  // groups per output batch

struct Group {
    uint32_t hour, ep, count = 0, errors = 0, p50 = 0, p95 = 0, p99 = 0;
    std::vector<uint32_t> lat;
};
struct Work {  // one per thread; hash slots hold index + 1, 0 means empty
    std::vector<std::string_view> ep;  // endpoint names, views into the mapping
    std::vector<uint64_t> eh;          // their hashes
    std::vector<Group> group;
    std::vector<uint32_t> es = std::vector<uint32_t>(2048), gs = std::vector<uint32_t>(4096);
    size_t last = 0;                   // most recently used group
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

inline uint64_t hash_bytes(const char* s, size_t n) {
    uint64_t h = n * kMul;
    for (; n >= 8; s += 8, n -= 8) {
        uint64_t k;
        std::memcpy(&k, s, 8);
        h = (h ^ k) * kMul;
        h ^= h >> 32;
    }
    uint64_t k = 0;
    std::memcpy(&k, s, n);
    h = (h ^ k) * kMul;
    return h ^ (h >> 32);
}

inline size_t group_hash(uint32_t hour, uint32_t ep) { return size_t(((uint64_t(hour) << 32 | ep) * kMul) >> 32); }

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
    const size_t m = w.gs.size() - 1;
    size_t j = group_hash(hour, ep) & m;
    for (; w.gs[j]; j = (j + 1) & m) {
        Group& g = w.group[w.gs[j] - 1];
        if (g.hour == hour && g.ep == ep) { w.last = w.gs[j] - 1; return g; }
    }
    w.group.push_back(Group{hour, ep});
    w.last = w.group.size() - 1;
    w.gs[j] = uint32_t(w.group.size());
    if (w.group.size() * 2 > w.gs.size())
        grow(w.gs, w.group.size(), [&](size_t i) { return group_hash(w.group[i].hour, w.group[i].ep); });
    return w.group.back();
}

inline uint32_t hour_key(const char* p) {  // "YYYY-MM-DDTHH" -> YYYYMMDDHH
    auto d = [&](int i) { return uint32_t(p[i] - '0'); };
    return (((d(0) * 1000 + d(1) * 100 + d(2) * 10 + d(3)) * 100 + d(5) * 10 + d(6)) * 100 + d(8) * 10 + d(9)) * 100 +
           d(11) * 10 + d(12);
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
        uint32_t st = 0, lat = 0;
        const char* r = q + 1;
        while (r < e && *r != ' ') st = st * 10 + uint32_t(*r++ - '0');
        while (++r < e) lat = lat * 10 + uint32_t(*r - '0');
        const uint32_t id = add_ep(w, std::string_view(ep, size_t(q - ep)));
        Group* g = w.group.empty() ? nullptr : &w.group[w.last];
        if (!g || g->hour != hour || g->ep != id) g = &add_group(w, hour, id);
        ++g->count;
        g->errors += st / 100 == 5;
        g->lat.push_back(lat);
    }
}

void score_group(Group& g) {  // nearest rank: ceil(p*n/100), 1-based
    std::vector<uint32_t>& v = g.lat;
    const size_t n = v.size();
    const size_t r[3] = {(50 * n + 99) / 100 - 1, (95 * n + 99) / 100 - 1, (99 * n + 99) / 100 - 1};
    uint32_t* out[3] = {&g.p50, &g.p95, &g.p99};
    if (n <= 32) std::sort(v.begin(), v.end());
    for (int k = 0, lo = 0; k < 3; ++k) {  // each selection only has to look right of the previous one
        if (n > 32) std::nth_element(v.begin() + lo, v.begin() + r[k], v.end());
        *out[k] = v[r[k]];
        lo = int(r[k]);
    }
}

inline void put(char*& p, uint64_t v, int width = 0) {
    char t[24];
    int n = 0;
    do { t[n++] = char('0' + v % 10); v /= 10; } while (v || n < width);
    while (n) *p++ = t[--n];
}

void append_line(std::string& out, const Group& g, std::string_view ep) {
    char b[128], *p = b;
    put(p, g.hour / 1000000, 4); *p++ = '-'; put(p, g.hour / 10000 % 100, 2); *p++ = '-';
    put(p, g.hour / 100 % 100, 2); *p++ = 'T'; put(p, g.hour % 100, 2);
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
    std::vector<Work> work(T);
    par(T, [&](int t) { parse_slice(map + cut[t], map + cut[t + 1], work[t]); });

    // Rank every endpoint name once; ranks follow byte order, so (hour, rank) is the output order.
    std::vector<std::string_view> names;
    for (const Work& w : work) names.insert(names.end(), w.ep.begin(), w.ep.end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    struct Item { uint64_t key; uint32_t t, g; };
    std::vector<Item> items;
    for (int t = 0; t < T; ++t) {
        std::vector<uint32_t> rank;
        for (std::string_view s : work[t].ep) rank.push_back(uint32_t(std::lower_bound(names.begin(), names.end(), s) - names.begin()));
        for (uint32_t i = 0; i < work[t].group.size(); ++i) {
            const Group& g = work[t].group[i];
            items.push_back({uint64_t(g.hour) << 32 | rank[g.ep], uint32_t(t), i});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });

    std::vector<Group> groups;  // one per (hour, endpoint), slices merged
    groups.reserve(items.size());
    for (const Item& it : items) {
        Group& g = work[it.t].group[it.g];
        const uint32_t hour = uint32_t(it.key >> 32), ep = uint32_t(it.key);
        if (!groups.empty() && groups.back().hour == hour && groups.back().ep == ep) {
            Group& d = groups.back();
            d.count += g.count;
            d.errors += g.errors;
            d.lat.insert(d.lat.end(), g.lat.begin(), g.lat.end());
        } else {
            g.hour = hour;
            g.ep = ep;
            groups.push_back(std::move(g));
        }
    }

    // Percentiles and formatting in one parallel pass; batches are written in order.
    const size_t nb = (groups.size() + kBatch - 1) / kBatch;
    std::vector<std::string> bufs(nb);
    std::atomic<size_t> next{0};
    par(T, [&](int) {
        for (size_t b; (b = next.fetch_add(1, std::memory_order_relaxed)) < nb;)
            for (size_t i = b * kBatch; i < std::min(groups.size(), (b + 1) * kBatch); ++i) {
                score_group(groups[i]);
                append_line(bufs[b], groups[i], names[groups[i].ep]);
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
