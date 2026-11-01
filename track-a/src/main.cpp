#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

inline int affinity_threads() {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) return std::max(1, CPU_COUNT(&set));
#endif
    unsigned n = std::thread::hardware_concurrency();
    return n ? int(std::min<unsigned>(n, 12u)) : 12;
}

const int kThreads = std::min(12, std::max(1, affinity_threads()));

struct Ep { const char* s; uint32_t len; };
struct Group {
    uint32_t hour, ep, count, errors;
    uint32_t p50, p95, p99;
    std::vector<uint32_t> lat;
};
struct Work {
    std::vector<Ep> ep;
    std::vector<Group> group;
    Group* last = nullptr;
};

inline uint32_t hour_key(const char* p) {
    const uint32_t y = (p[0] - '0') * 1000u + (p[1] - '0') * 100u + (p[2] - '0') * 10u + (p[3] - '0');
    const uint32_t m = (p[5] - '0') * 10u + (p[6] - '0');
    const uint32_t d = (p[8] - '0') * 10u + (p[9] - '0');
    const uint32_t h = (p[11] - '0') * 10u + (p[12] - '0');
    return ((y * 100u + m) * 100u + d) * 100u + h;
}

inline bool dec_u32(const char* b, const char* e, uint32_t& v) {
    if (b >= e || *b < '0' || *b > '9') return false;
    uint32_t x = 0;
    for (; b < e && *b >= '0' && *b <= '9'; ++b) x = x * 10u + uint32_t(*b - '0');
    v = x;
    return true;
}

inline uint32_t add_ep(Work& w, const char* s, uint32_t len) {
    for (uint32_t i = 0; i < w.ep.size(); ++i) {
        const Ep& e = w.ep[i];
        if (e.len == len && std::memcmp(e.s, s, len) == 0) return i;
    }
    w.ep.push_back(Ep{s, len});
    return uint32_t(w.ep.size() - 1u);
}

inline Group* add_group(Work& w, uint32_t hour, uint32_t ep) {
    if (w.last && w.last->hour == hour && w.last->ep == ep) return w.last;
    for (Group& g : w.group) {
        if (g.hour == hour && g.ep == ep) {
            w.last = &g;
            return &g;
        }
    }
    w.group.push_back(Group{hour, ep, 0, 0, 0, 0, 0, {}});
    w.last = &w.group.back();
    return w.last;
}

inline bool parse_line(const char* b, const char* e, uint32_t& hour, const char*& ep, uint32_t& ep_len, uint32_t& status, uint32_t& lat) {
    if (e - b < 30) return false;
    hour = hour_key(b);
    const char* p = b + 27;
    if (p >= e || *p != ' ') return false;
    ++p;
    while (p < e && *p != ' ') ++p;
    if (p >= e) return false;
    ++p;
    const char* ep0 = p;
    while (p < e && *p != ' ') ++p;
    if (p <= ep0 || p >= e) return false;
    ep = ep0;
    ep_len = uint32_t(p - ep0);
    if (ep_len == 0 || ep[0] != '/') return false;
    if (*p != ' ') return false;
    ++p;
    const char* s0 = p;
    while (p < e && *p != ' ') ++p;
    if (p <= s0 || p >= e) return false;
    uint32_t st = 0;
    if (!dec_u32(s0, p, st)) return false;
    if (st < 100u || st > 599u) return false;
    status = st;
    if (*p != ' ') return false;
    ++p;
    const char* lat0 = p;
    while (p < e && *p != '\n') ++p;
    if (p <= lat0) return false;
    uint32_t v = 0;
    if (!dec_u32(lat0, p, v)) return false;
    lat = v;
    return true;
}

void parse_slice(const char* data, size_t lo, size_t hi, Work& w) {
    const char* p = data + lo;
    const char* end = data + hi;
    while (p < end) {
        const char* q = p;
        while (q < end && *q != '\n') ++q;
        const char* line_end = q < end ? q : end;
        uint32_t hour, status, lat, elen;
        const char* ep = nullptr;
        if (parse_line(p, line_end, hour, ep, elen, status, lat)) {
            uint32_t idx = add_ep(w, ep, elen);
            Group* g = add_group(w, hour, idx);
            ++g->count;
            if (status >= 500u && status <= 599u) ++g->errors;
            g->lat.push_back(lat);
        }
        if (q < end) p = q + 1; else break;
    }
}

inline void put_num(char*& p, uint64_t v) {
    char b[32];
    int n = 0;
    do {
        b[n++] = char('0' + (v % 10u));
        v /= 10u;
    } while (v);
    while (n--) *p++ = b[n];
}

inline void put2(char*& p, uint32_t v) {
    *p++ = char('0' + (v / 10u) % 10u);
    *p++ = char('0' + v % 10u);
}

inline void put4(char*& p, uint32_t v) {
    *p++ = char('0' + (v / 1000u) % 10u);
    *p++ = char('0' + (v / 100u) % 10u);
    *p++ = char('0' + (v / 10u) % 10u);
    *p++ = char('0' + v % 10u);
}

inline void score_group(Group& g) {
    std::vector<uint32_t>& v = g.lat;
    const size_t n = v.size();
    if (n <= 32) {
        for (size_t i = 1; i < n; ++i) {
            const uint32_t x = v[i];
            size_t j = i;
            while (j && x < v[j - 1]) {
                v[j] = v[j - 1];
                --j;
            }
            v[j] = x;
        }
    } else {
        const size_t r50 = (50ull * n + 99ull) / 100ull - 1ull;
        const size_t r95 = (95ull * n + 99ull) / 100ull - 1ull;
        const size_t r99 = (99ull * n + 99ull) / 100ull - 1ull;
        std::nth_element(v.begin(), v.begin() + r50, v.end());
        g.p50 = v[r50];
        std::nth_element(v.begin() + r50, v.begin() + r95, v.end());
        g.p95 = v[r95];
        std::nth_element(v.begin() + r95, v.begin() + r99, v.end());
        g.p99 = v[r99];
        return;
    }
    g.p50 = v[(50ull * n + 99ull) / 100ull - 1ull];
    g.p95 = v[(95ull * n + 99ull) / 100ull - 1ull];
    g.p99 = v[(99ull * n + 99ull) / 100ull - 1ull];
}

inline void emit_line(char*& p, const Group& g, const std::string& ep) {
    const uint32_t year = g.hour / 1000000u;
    const uint32_t month = (g.hour / 10000u) % 100u;
    const uint32_t day = (g.hour / 100u) % 100u;
    const uint32_t h = g.hour % 100u;
    put4(p, year);
    *p++ = '-';
    put2(p, month);
    *p++ = '-';
    put2(p, day);
    *p++ = 'T';
    put2(p, h);
    std::memcpy(p, ":00:00Z ", 8); p += 8;
    std::memcpy(p, ep.data(), ep.size()); p += ep.size();
    *p++ = ' ';
    put_num(p, g.count);
    *p++ = ' ';
    put_num(p, g.p50);
    *p++ = ' ';
    put_num(p, g.p95);
    *p++ = ' ';
    put_num(p, g.p99);
    *p++ = ' ';
    put_num(p, g.errors);
    *p++ = ' ';
    put_num(p, uint64_t(g.errors) * 1000000ull / g.count);
    *p++ = '\n';
}

bool write_output(const std::vector<std::string>& ep_names, std::vector<Group>& groups, const char* out_dir) {
    std::string path(out_dir);
    path += "/summary.txt";
    if (::mkdir(out_dir, 0777) != 0 && errno != EEXIST) return false;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const size_t threads = std::min<size_t>(kThreads, groups.size() ? groups.size() : 1u);
    std::vector<std::string> bufs(threads);
    std::vector<std::thread> ts;
    ts.reserve(threads);
    const size_t chunk = (groups.size() + threads - 1u) / threads;
    for (size_t t = 0; t < threads; ++t) {
        const size_t lo = t * chunk;
        const size_t hi = std::min(groups.size(), lo + chunk);
        if (lo >= hi) continue;
        ts.emplace_back([&](size_t a, size_t b, size_t tid) {
            std::string& out = bufs[tid];
            out.clear();
            out.reserve((b - a) * 96u);
            char tmp[256];
            for (size_t i = a; i < b; ++i) {
                const Group& g = groups[i];
                char* p = tmp;
                emit_line(p, g, ep_names[g.ep]);
                out.append(tmp, size_t(p - tmp));
            }
        }, lo, hi, t);
    }
    for (std::thread& t : ts) t.join();
    for (const std::string& s : bufs) {
        const char* p = s.data();
        size_t rem = s.size();
        while (rem) {
            ssize_t n = ::write(fd, p, rem);
            if (n < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return false;
            }
            if (n == 0) { ::close(fd); return false; }
            p += n;
            rem -= size_t(n);
        }
    }
    ::close(fd);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* data_dir = "/data";
    const char* out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    std::string in = std::string(data_dir) + "/requests.log";
    int fd = ::open(in.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    if (::fstat(fd, &st) != 0) { ::close(fd); return 1; }
    size_t file_size = size_t(st.st_size);
    if (file_size == 0) {
        ::close(fd);
        std::vector<std::string> empty;
        std::vector<Group> empty_groups;
        return write_output(empty, empty_groups, out_dir) ? 0 : 1;
    }
    char* map = static_cast<char*>(::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) { ::close(fd); return 1; }
    ::madvise(map, file_size, MADV_SEQUENTIAL);

    std::vector<Work> work(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    size_t lo = 0;
    for (int t = 0; t < kThreads; ++t) {
        size_t hi = (file_size * size_t(t + 1u)) / size_t(kThreads);
        if (hi < file_size) while (hi < file_size && map[hi] != '\n') ++hi;
        if (hi < file_size) ++hi;
        if (lo < hi) threads.emplace_back(parse_slice, map, lo, hi, std::ref(work[t]));
        lo = hi;
    }
    for (std::thread& t : threads) t.join();

    std::vector<Ep> all;
    std::vector<std::vector<uint32_t>> remap(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        remap[t].resize(work[t].ep.size());
        for (uint32_t i = 0; i < uint32_t(work[t].ep.size()); ++i) {
            const Ep& e = work[t].ep[i];
            uint32_t j = 0;
            for (; j < uint32_t(all.size()); ++j) {
                if (all[j].len == e.len && std::memcmp(all[j].s, e.s, e.len) == 0) break;
            }
            if (j == uint32_t(all.size())) all.push_back(e);
            remap[t][i] = j;
        }
    }

    std::vector<std::string> ep_names(all.size());
    for (uint32_t i = 0; i < uint32_t(all.size()); ++i) ep_names[i].assign(all[i].s, all[i].len);

    std::vector<Group> groups;
    for (int t = 0; t < kThreads; ++t) {
        for (const Group& g : work[t].group) {
            const uint32_t ep = remap[t][g.ep];
            Group* dst = nullptr;
            for (Group& x : groups) {
                if (x.hour == g.hour && x.ep == ep) { dst = &x; break; }
            }
            if (!dst) {
                groups.push_back(Group{g.hour, ep, 0, 0, 0, 0, 0, {}});
                dst = &groups.back();
            }
            dst->count += g.count;
            dst->errors += g.errors;
            dst->lat.insert(dst->lat.end(), g.lat.begin(), g.lat.end());
        }
    }

    std::vector<size_t> order(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return groups[a].lat.size() > groups[b].lat.size();
    });
    std::atomic<size_t> next(0);
    std::vector<std::thread> pct;
    pct.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        pct.emplace_back([&] {
            for (;;) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= order.size()) return;
                score_group(groups[order[i]]);
            }
        });
    }
    for (std::thread& t : pct) t.join();

    std::sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
        if (a.hour != b.hour) return a.hour < b.hour;
        const std::string& sa = ep_names[a.ep];
        const std::string& sb = ep_names[b.ep];
        const size_t n = std::min(sa.size(), sb.size());
        const int c = std::memcmp(sa.data(), sb.data(), n);
        if (c) return c < 0;
        return sa.size() < sb.size();
    });
    const int ok = write_output(ep_names, groups, out_dir);
    ::munmap(map, file_size);
    ::close(fd);
    if (ok) _exit(0);
    return 1;
}
