// Speed Demon 2026 — Track A (Ingest)
//
// Reads <data>/requests.log and writes <out>/summary.txt.
//
// Input  (one request per line, LF terminated):
//   YYYY-MM-DDTHH:MM:SS.ffffffZ SP method SP endpoint SP status SP latency_us LF
//
// Output (for every (UTC hour, endpoint) pair, one line):
//   YYYY-MM-DDTHH:00:00Z endpoint count p50 p95 p99 errors error_ppm LF
//
//   * percentile rank:  (p * n + 99) / 100, 1-indexed into ascending latencies
//   * errors:           status in [500, 599]
//   * error_ppm:        floor(errors * 1000000 / count)
//   * ordering:         hour ascending, then endpoint byte order
//
// Build:  g++ -O3 -std=c++17 -o ingest src/main.cpp
// Run:    ./ingest --data /data --out /out

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

constexpr size_t kChunk = 8u << 20;  // 8 MiB read buffer

// One parsed request. 16 bytes: sorting 3.1M of these is cheap and cache friendly.
struct Rec {
    uint32_t hour;  // packed YYYYMMDDHH, e.g. 2026103018
    uint32_t ep;    // endpoint id
    uint32_t lat;   // latency in microseconds (uint32 per spec)
    uint32_t err;   // 1 if status in [500, 599], else 0
};

// One output row (aggregated group).
struct Group {
    uint32_t hour;
    uint32_t ep;
    uint32_t count;
    uint32_t errors;
    uint32_t p50;
    uint32_t p95;
    uint32_t p99;
};

// Endpoint interning table: open addressing, FNV-1a hash, strings stored in an arena.
class Endpoints {
public:
    Endpoints() : slots_(4096, 0) {}

    uint32_t intern(const char* s, uint32_t len) {
        const uint64_t h = fnv1a(s, len);
        size_t mask = slots_.size() - 1;
        size_t i = static_cast<size_t>(h) & mask;
        for (;;) {
            const uint32_t idp1 = slots_[i];
            if (idp1 == 0) break;
            const uint32_t id = idp1 - 1;
            if (lens_[id] == len && std::memcmp(&arena_[offs_[id]], s, len) == 0)
                return id;
            i = (i + 1) & mask;
        }
        const uint32_t id = static_cast<uint32_t>(offs_.size());
        offs_.push_back(static_cast<uint32_t>(arena_.size()));
        lens_.push_back(len);
        arena_.insert(arena_.end(), s, s + len);
        slots_[i] = id + 1;
        if (offs_.size() * 10 >= slots_.size() * 7) rehash();
        return id;
    }

    const char* str(uint32_t id) const { return &arena_[offs_[id]]; }
    uint32_t len(uint32_t id) const { return lens_[id]; }

private:
    static uint64_t fnv1a(const char* s, uint32_t n) {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(s[i]);
            h *= 1099511628211ull;
        }
        return h;
    }

    void rehash() {
        std::vector<uint32_t> old;
        old.swap(slots_);
        slots_.assign(old.size() * 2, 0);
        const size_t mask = slots_.size() - 1;
        for (size_t id = 0; id < offs_.size(); ++id) {
            const uint64_t h = fnv1a(&arena_[offs_[id]], lens_[id]);
            size_t i = static_cast<size_t>(h) & mask;
            while (slots_[i] != 0) i = (i + 1) & mask;
            slots_[i] = static_cast<uint32_t>(id) + 1;
        }
    }

    std::vector<uint32_t> slots_;
    std::vector<uint32_t> offs_;
    std::vector<uint32_t> lens_;
    std::vector<char> arena_;
};

inline uint32_t hour_key(const char* p) {
    const uint32_t y = (p[0] - '0') * 1000u + (p[1] - '0') * 100u +
                       (p[2] - '0') * 10u + (p[3] - '0');
    const uint32_t mo = (p[5] - '0') * 10u + (p[6] - '0');
    const uint32_t d = (p[8] - '0') * 10u + (p[9] - '0');
    const uint32_t h = (p[11] - '0') * 10u + (p[12] - '0');
    return ((y * 100u + mo) * 100u + d) * 100u + h;
}

inline uint64_t parse_u64(const char* p, const char* end) {
    uint64_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') v = v * 10 + static_cast<uint64_t>(*p++ - '0');
    return v;
}

void process_line(const char* line, const char* nl, Endpoints& eps,
                  std::vector<Rec>& recs) {
    // Timestamp is fixed width: 27 bytes, then a space.
    if (nl - line < 30) return;

    const uint32_t hour = hour_key(line);

    // Skip method.
    const char* p = line + 27;  // points at the space after 'Z'
    ++p;                        // first byte of method
    while (p < nl && *p != ' ') ++p;  // space after method
    if (p >= nl) return;
    const char* ep = ++p;       // endpoint start
    while (p < nl && *p != ' ') ++p;  // space after endpoint
    if (p >= nl) return;
    const uint32_t ep_id = eps.intern(ep, static_cast<uint32_t>(p - ep));

    ++p;  // status start
    const char* status_end = p;
    while (status_end < nl && *status_end != ' ') ++status_end;
    if (status_end >= nl) return;
    const uint64_t status = parse_u64(p, status_end);

    ++status_end;  // latency start
    const uint64_t lat = parse_u64(status_end, nl);

    recs.push_back(Rec{hour, ep_id, static_cast<uint32_t>(lat),
                       (status >= 500 && status <= 599) ? 1u : 0u});
}

bool read_file(const char* path, std::vector<Rec>& recs, Endpoints& eps) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "ingest: cannot open %s: %s\n", path, std::strerror(errno));
        return false;
    }

    std::vector<char> buf(kChunk + 65536);
    size_t carry = 0;

    for (;;) {
        if (buf.size() - carry < kChunk) buf.resize(buf.size() * 2);
        const size_t got = std::fread(buf.data() + carry, 1, kChunk, f);
        if (got == 0) {
            if (std::ferror(f)) {
                std::fprintf(stderr, "ingest: read error on %s: %s\n", path,
                             std::strerror(errno));
                std::fclose(f);
                return false;
            }
            if (carry) process_line(buf.data(), buf.data() + carry, eps, recs);
            break;
        }

        const size_t total = carry + got;
        const char* p = buf.data();
        const char* end = buf.data() + total;
        for (;;) {
            const char* nl = static_cast<const char*>(
                std::memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) break;
            process_line(p, nl, eps, recs);
            p = nl + 1;
        }

        carry = static_cast<size_t>(end - p);
        if (carry) std::memmove(buf.data(), p, carry);
        if (carry >= kChunk) {  // pathological line: make room
            size_t extra = buf.size();
            buf.resize(extra * 2);
        }
    }

    std::fclose(f);
    return true;
}

bool write_output(const char* out_dir, std::vector<Group>& groups, const Endpoints& eps) {
    // Group order: hour ascending, then endpoint byte order.
    std::sort(groups.begin(), groups.end(), [&eps](const Group& a, const Group& b) {
        if (a.hour != b.hour) return a.hour < b.hour;
        const uint32_t la = eps.len(a.ep), lb = eps.len(b.ep);
        const int c = std::memcmp(eps.str(a.ep), eps.str(b.ep), std::min(la, lb));
        if (c != 0) return c < 0;
        return la < lb;
    });

    std::vector<char> out;
    out.reserve(groups.size() * 96);
    char line[512];

    for (const Group& g : groups) {
        const uint32_t year = g.hour / 1000000u;
        const uint32_t month = (g.hour / 10000u) % 100u;
        const uint32_t day = (g.hour / 100u) % 100u;
        const uint32_t hour = g.hour % 100u;
        const unsigned long long ppm =
            static_cast<unsigned long long>(g.errors) * 1000000ull / g.count;

        const int n = std::snprintf(
            line, sizeof line, "%04u-%02u-%02uT%02u:00:00Z %.*s %u %u %u %u %u %llu\n",
            year, month, day, hour, static_cast<int>(eps.len(g.ep)), eps.str(g.ep),
            g.count, g.p50, g.p95, g.p99, g.errors, ppm);
        out.insert(out.end(), line, line + n);
    }

    ::mkdir(out_dir, 0755);  // EEXIST is fine
    std::string path(out_dir);
    path += "/summary.txt";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "ingest: cannot write %s: %s\n", path.c_str(), std::strerror(errno));
        return false;
    }
    if (!out.empty() && std::fwrite(out.data(), 1, out.size(), f) != out.size()) {
        std::fprintf(stderr, "ingest: short write on %s\n", path.c_str());
        std::fclose(f);
        return false;
    }
    if (std::fclose(f) != 0) {
        std::fprintf(stderr, "ingest: close failed on %s\n", path.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* data_dir = "/data";
    const char* out_dir = "/out";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--data DIR] [--out DIR]\n", argv[0]);
            return 2;
        }
    }

    std::string input(data_dir);
    input += "/requests.log";

    Endpoints eps;
    std::vector<Rec> recs;
    recs.reserve(1u << 21);

    if (!read_file(input.c_str(), recs, eps)) return 1;

    // Sort by (hour, endpoint, latency). Within a group this makes latencies ascending.
    std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
        if (a.hour != b.hour) return a.hour < b.hour;
        if (a.ep != b.ep) return a.ep < b.ep;
        return a.lat < b.lat;
    });

    std::vector<Group> groups;
    const size_t n = recs.size();
    groups.reserve(n / 8 + 1);

    for (size_t i = 0; i < n;) {
        const uint32_t hour = recs[i].hour;
        const uint32_t ep = recs[i].ep;
        size_t j = i;
        uint32_t errs = 0;
        while (j < n && recs[j].hour == hour && recs[j].ep == ep) {
            errs += recs[j].err;
            ++j;
        }
        const uint64_t count = j - i;
        const uint64_t r50 = (50 * count + 99) / 100;
        const uint64_t r95 = (95 * count + 99) / 100;
        const uint64_t r99 = (99 * count + 99) / 100;
        groups.push_back(Group{hour, ep, static_cast<uint32_t>(count), errs,
                               recs[i + r50 - 1].lat, recs[i + r95 - 1].lat,
                               recs[i + r99 - 1].lat});
        i = j;
    }

    if (!write_output(out_dir, groups, eps)) return 1;
    return 0;
}
