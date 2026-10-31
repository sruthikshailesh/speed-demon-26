#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr int kThreads = 12;

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
    madvise((void*)(data + lo), hi - lo, MADV_DONTNEED);
}

void append_u64(std::string& out, uint64_t v) {
    char buf[32];
    int n = 0;
    do {
        buf[n++] = char('0' + (v % 10u));
        v /= 10u;
    } while (v);
    while (n--) out.push_back(buf[n]);
}

void append_2(std::string& out, uint32_t v) {
    char b[2];
    b[0] = char('0' + (v / 10u) % 10u);
    b[1] = char('0' + v % 10u);
    out.append(b, 2);
}

void append_4(std::string& out, uint32_t v) {
    char b[4];
    b[0] = char('0' + (v / 1000u) % 10u);
    b[1] = char('0' + (v / 100u) % 10u);
    b[2] = char('0' + (v / 10u) % 10u);
    b[3] = char('0' + v % 10u);
    out.append(b, 4);
}

bool write_output(const std::vector<std::string>& ep_names, const std::vector<Group>& groups, const char* out_dir) {
    std::string out;
    out.reserve(groups.size() * 96u);
    for (const Group& g : groups) {
        const uint32_t year = g.hour / 1000000u;
        const uint32_t month = (g.hour / 10000u) % 100u;
        const uint32_t day = (g.hour / 100u) % 100u;
        const uint32_t h = g.hour % 100u;
        append_4(out, year);
        out.push_back('-');
        append_2(out, month);
        out.push_back('-');
        append_2(out, day);
        out.push_back('T');
        append_2(out, h);
        out += ":00:00Z ";
        out.append(ep_names[g.ep]);
        out.push_back(' ');
        append_u64(out, g.count);
        out.push_back(' ');
        append_u64(out, g.p50);
        out.push_back(' ');
        append_u64(out, g.p95);
        out.push_back(' ');
        append_u64(out, g.p99);
        out.push_back(' ');
        append_u64(out, g.errors);
        out.push_back(' ');
        append_u64(out, uint64_t(g.errors) * 1000000ull / g.count);
        out.push_back('\n');
    }
    std::string path(out_dir);
    path += "/summary.txt";
    ::mkdir(out_dir, 0755);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const ssize_t n = ::write(fd, out.data(), out.size());
    ::close(fd);
    return n == ssize_t(out.size());
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
    if (fd < 0) { std::fprintf(stderr, "cannot open %s\n", in.c_str()); return 1; }
    struct stat st;
    if (::fstat(fd, &st) != 0) { std::fprintf(stderr, "fstat failed\n"); ::close(fd); return 1; }
    size_t file_size = size_t(st.st_size);
    char* map = static_cast<char*>(::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) { std::fprintf(stderr, "mmap failed\n"); ::close(fd); return 1; }
    ::madvise(map, file_size, MADV_SEQUENTIAL);

    std::vector<Work> work(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    size_t span = (file_size + kThreads - 1u) / kThreads;
    for (int t = 0; t < kThreads; ++t) {
        size_t lo = span * size_t(t);
        size_t hi = std::min(span * size_t(t + 1u), file_size);
        if (t != 0 && lo < file_size) while (lo < file_size && map[lo] != '\n') ++lo;
        if (hi < file_size) while (hi < file_size && map[hi] != '\n') ++hi;
        if (hi > lo) {
            size_t s = lo;
            while (s < hi) {
                size_t e = std::min(s + (1u << 20), hi);
                threads.emplace_back(parse_slice, map, s, e, std::ref(work[t]));
                s = e;
            }
        }
    }
    for (std::thread& t : threads) t.join();
    ::munmap(map, file_size);
    ::close(fd);

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

    for (Group& g : groups) {
        std::sort(g.lat.begin(), g.lat.end());
        const uint64_t n = g.count;
        const uint64_t r50 = (50ull * n + 99ull) / 100ull;
        const uint64_t r95 = (95ull * n + 99ull) / 100ull;
        const uint64_t r99 = (99ull * n + 99ull) / 100ull;
        g.p50 = g.lat[r50 - 1ull];
        g.p95 = g.lat[r95 - 1ull];
        g.p99 = g.lat[r99 - 1ull];
    }

    std::sort(groups.begin(), groups.end(), [&](const Group& a, const Group& b) {
        if (a.hour != b.hour) return a.hour < b.hour;
        const std::string& sa = ep_names[a.ep];
        const std::string& sb = ep_names[b.ep];
        const size_t n = std::min(sa.size(), sb.size());
        const int c = std::memcmp(sa.data(), sb.data(), n);
        if (c) return c < 0;
        return sa.size() < sb.size();
    });

    if (!write_output(ep_names, groups, out_dir)) return 1;
    return 0;
}
