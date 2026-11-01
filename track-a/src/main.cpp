#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
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

constexpr int kThreads = 6;

struct Ep { std::string s; uint32_t len; };
struct Group {
    uint32_t hour, ep;
    uint64_t count, errors;
    uint32_t p50, p95, p99;
    std::vector<uint32_t> lat;
};
struct Work {
    std::vector<Ep> eps;
    std::vector<uint32_t> ep_slots;
    std::vector<Group> groups;
    std::vector<uint32_t> group_slots;
};

inline uint64_t fnv1a(const char* s, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= uint8_t(s[i]);
        h *= 1099511628211ull;
    }
    return h;
}

inline void grow_ep_table(Work& w) {
    std::vector<uint32_t> old = std::move(w.ep_slots);
    const size_t new_cap = old.empty() ? 1024u : old.size() * 2u;
    w.ep_slots.assign(new_cap, UINT32_MAX);
    for (size_t i = 0; i < old.size(); ++i) {
        if (old[i] == UINT32_MAX) continue;
        const Ep& e = w.eps[old[i]];
        const size_t n = e.s.size();
        const uint64_t h = fnv1a(e.s.data(), n);
        for (size_t j = h & (new_cap - 1u);; j = (j + 1u) & (new_cap - 1u)) {
            if (w.ep_slots[j] == UINT32_MAX) { w.ep_slots[j] = old[i]; break; }
        }
    }
}

inline void grow_group_table(Work& w) {
    std::vector<uint32_t> old = std::move(w.group_slots);
    const size_t new_cap = old.empty() ? 1024u : old.size() * 2u;
    w.group_slots.assign(new_cap, UINT32_MAX);
    for (size_t i = 0; i < old.size(); ++i) {
        if (old[i] == UINT32_MAX) continue;
        const Group& g = w.groups[old[i]];
        const uint64_t key = (uint64_t(g.hour) << 32) | uint64_t(g.ep);
        const size_t mask = new_cap - 1u;
        for (size_t j = ((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; j = (j + 1u) & mask) {
            if (w.group_slots[j] == UINT32_MAX) { w.group_slots[j] = old[i]; break; }
        }
    }
}

inline uint32_t hour_key(const char* p) {
    const uint32_t y = (p[0] - '0') * 1000u + (p[1] - '0') * 100u + (p[2] - '0') * 10u + (p[3] - '0');
    const uint32_t m = (p[5] - '0') * 10u + (p[6] - '0');
    const uint32_t d = (p[8] - '0') * 10u + (p[9] - '0');
    const uint32_t h = (p[11] - '0') * 10u + (p[12] - '0');
    return ((y * 100u + m) * 100u + d) * 100u + h;
}

inline bool parse_u32(const char* b, const char* e, uint32_t& v) {
    if (b >= e) return false;
    uint32_t x = 0;
    for (; b < e; ++b) {
        const char c = *b;
        if (c < '0' || c > '9') return false;
        x = x * 10u + uint32_t(c - '0');
    }
    v = x;
    return true;
}

inline uint32_t add_ep(Work& w, const char* s, uint32_t len) {
    if (w.ep_slots.empty()) w.ep_slots.assign(1024u, UINT32_MAX);
    if ((w.eps.size() + 1u) * 10u > w.ep_slots.size() * 7u) grow_ep_table(w);
    const uint64_t h = fnv1a(s, len);
    const size_t mask = w.ep_slots.size() - 1u;
    for (size_t i = h & mask;; i = (i + 1u) & mask) {
        const uint32_t idx = w.ep_slots[i];
        if (idx == UINT32_MAX) {
            Ep e;
            e.s.assign(s, len);
            e.len = len;
            w.eps.push_back(e);
            w.ep_slots[i] = uint32_t(w.eps.size() - 1u);
            return w.ep_slots[i];
        }
        const Ep& e = w.eps[idx];
        if (e.len == len && std::memcmp(e.s.data(), s, len) == 0) return idx;
    }
}

inline Group* add_group(Work& w, uint32_t hour, uint32_t ep) {
    if (w.group_slots.empty()) w.group_slots.assign(1024u, UINT32_MAX);
    if ((w.groups.size() + 1u) * 10u > w.group_slots.size() * 7u) grow_group_table(w);
    const uint64_t key = (uint64_t(hour) << 32) | uint64_t(ep);
    const size_t mask = w.group_slots.size() - 1u;
    for (size_t i = ((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; i = (i + 1u) & mask) {
        const uint32_t idx = w.group_slots[i];
        if (idx == UINT32_MAX) {
            w.groups.push_back(Group{hour, ep, 0, 0, 0, 0, 0, {}});
            w.group_slots[i] = uint32_t(w.groups.size() - 1u);
            return &w.groups.back();
        }
        Group& g = w.groups[idx];
        if (g.hour == hour && g.ep == ep) return &g;
    }
}

inline bool parse_line(const char* b, const char* e, uint32_t& hour, const char*& ep, uint32_t& ep_len, uint32_t& status, uint32_t& lat) {
    if (e - b < 30) return false;
    if (b[4] != '-' || b[7] != '-' || b[10] != 'T' || b[13] != ':' || b[16] != ':' || b[19] != '.' || b[26] != 'Z') return false;
    hour = hour_key(b);
    const char* p = b + 27;
    if (p >= e || *p != ' ') return false;
    ++p;
    if (p >= e) return false;
    const char* m0 = p;
    while (p < e && *p != ' ') ++p;
    if (p <= m0 || p >= e) return false;
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
    if (!parse_u32(s0, p, st)) return false;
    if (st < 100u || st > 599u) return false;
    status = st;
    if (*p != ' ') return false;
    ++p;
    const char* lat0 = p;
    while (p < e && *p != '\n') ++p;
    if (p <= lat0) return false;
    uint32_t v = 0;
    if (!parse_u32(lat0, p, v)) return false;
    lat = v;
    return true;
}

void parse_chunk(const char* data, size_t lo, size_t hi, Work& w) {
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
        if (q < end) p = q + 1;
        else break;
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
    if (mkdir(out_dir, 0777) != 0 && errno != EEXIST) return false;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    const char* p = out.data();
    size_t rem = out.size();
    while (rem) {
        ssize_t n = write(fd, p, rem);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) { close(fd); return false; }
        p += n;
        rem -= size_t(n);
    }
    close(fd);
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

    const std::string in = std::string(data_dir) + "/requests.log";
    int fd = open(in.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 1; }
    const size_t file_size = size_t(st.st_size);
    if (file_size == 0) {
        close(fd);
        std::vector<std::string> empty;
        std::vector<Group> empty_groups;
        return write_output(empty, empty_groups, out_dir) ? 0 : 1;
    }
    char* map = static_cast<char*>(mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map == MAP_FAILED) { close(fd); return 1; }

    std::vector<Work> work(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    size_t lo = 0;
    for (int t = 0; t < kThreads; ++t) {
        size_t hi = (t + 1 == kThreads) ? file_size : (file_size * size_t(t + 1)) / size_t(kThreads);
        if (hi < file_size) {
            while (hi < file_size && map[hi] != '\n') ++hi;
            if (hi < file_size) ++hi;
        }
        if (lo < hi) threads.emplace_back(parse_chunk, map, lo, hi, std::ref(work[t]));
        lo = hi;
    }
    for (std::thread& t : threads) t.join();

    std::vector<Ep> all;
    std::vector<std::vector<uint32_t>> remap(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        remap[t].resize(work[t].eps.size());
        for (uint32_t i = 0; i < uint32_t(work[t].eps.size()); ++i) {
            const Ep& e = work[t].eps[i];
            uint32_t j = 0;
            for (; j < uint32_t(all.size()); ++j) {
                const Ep& x = all[j];
                if (x.len == e.len && std::memcmp(x.s.data(), e.s.data(), e.len) == 0) break;
            }
            if (j == uint32_t(all.size())) all.push_back(e);
            remap[t][i] = j;
        }
    }

    std::vector<std::string> ep_names(all.size());
    for (uint32_t i = 0; i < uint32_t(all.size()); ++i) ep_names[i] = all[i].s;

    std::vector<Group> groups;
    for (int t = 0; t < kThreads; ++t) {
        for (Group& g : work[t].groups) {
            groups.push_back(Group{g.hour, remap[t][g.ep], g.count, g.errors, 0, 0, 0, std::move(g.lat)});
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.hour != b.hour) return a.hour < b.hour;
        return a.ep < b.ep;
    });
    std::vector<Group> merged;
    for (size_t i = 0; i < groups.size();) {
        Group g = std::move(groups[i]);
        size_t j = i + 1;
        while (j < groups.size() && groups[j].hour == g.hour && groups[j].ep == g.ep) {
            g.count += groups[j].count;
            g.errors += groups[j].errors;
            g.lat.insert(g.lat.end(), groups[j].lat.begin(), groups[j].lat.end());
            ++j;
        }
        merged.push_back(std::move(g));
        i = j;
    }
    groups.swap(merged);

    std::atomic<size_t> next{0};
    std::vector<std::thread> pct(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        pct[t] = std::thread([&]() {
            while (true) {
                size_t idx = next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= groups.size()) return;
                Group& g = groups[idx];
                const size_t n = g.lat.size();
                if (!n) continue;
                const size_t r50 = (50ull * n + 99ull) / 100ull;
                const size_t r95 = (95ull * n + 99ull) / 100ull;
                const size_t r99 = (99ull * n + 99ull) / 100ull;
                std::nth_element(g.lat.begin(), g.lat.begin() + (r50 - 1ull), g.lat.end());
                g.p50 = g.lat[r50 - 1ull];
                if (r95 > r50) {
                    std::nth_element(g.lat.begin() + (r50), g.lat.begin() + (r95 - 1ull), g.lat.end());
                    g.p95 = g.lat[r95 - 1ull];
                } else {
                    g.p95 = g.p50;
                }
                if (r99 > r95) {
                    std::nth_element(g.lat.begin() + (r95), g.lat.begin() + (r99 - 1ull), g.lat.end());
                    g.p99 = g.lat[r99 - 1ull];
                } else {
                    g.p99 = g.p95;
                }
            }
        });
    }
    for (std::thread& t : pct) t.join();
    if (!write_output(ep_names, groups, out_dir)) {
        munmap(map, file_size);
        close(fd);
        return 1;
    }
    munmap(map, file_size);
    close(fd);
    return 0;
}
