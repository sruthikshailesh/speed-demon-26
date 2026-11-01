#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <new>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// Official Track A limits: 72 hours, 1,842 endpoints, 125 bytes per endpoint.
constexpr uint32_t kEndpointLimit = 1842;
constexpr uint32_t kHourCount = 72;
constexpr uint32_t kGroupCount = kEndpointLimit * kHourCount;
constexpr uint32_t kEndpointBytes = 125;
constexpr uint32_t kHashSize = 4096;
constexpr size_t kReadSize = 1u << 17;
constexpr size_t kThreadReadSize = 1u << 16;
constexpr size_t kMaxLine = 176;
constexpr uint32_t kThreads = 4;
constexpr uint16_t kEmpty = UINT16_MAX;

struct Endpoint { uint32_t offset; uint16_t length; };
struct Group { uint32_t count, errors; std::atomic<uint32_t> cursor{0}; };
struct Parsed { uint32_t hour, status, latency, endpoint_length; const char* endpoint; };
template<class T> struct MmapAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;
    MmapAllocator() noexcept = default;
    template<class U> MmapAllocator(const MmapAllocator<U>&) noexcept {}
    T* allocate(size_t n) {
        const size_t bytes = n * sizeof(T);
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t n) noexcept { munmap(p, n * sizeof(T)); }
    template<class U> bool operator==(const MmapAllocator<U>&) const noexcept { return true; }
    template<class U> bool operator!=(const MmapAllocator<U>&) const noexcept { return false; }
};

// The packed key is hour * kEndpointLimit + this worker's endpoint id.
struct LocalGroup { uint32_t hour_endpoint, count, errors; };
struct LocalWork {
    Endpoint endpoints[kEndpointLimit];
    uint16_t endpoint_slots[kHashSize];
    char arena[kEndpointLimit * kEndpointBytes];
    uint32_t endpoint_count = 0, arena_used = 0;
    std::vector<LocalGroup, MmapAllocator<LocalGroup>> groups;
    std::vector<uint32_t, MmapAllocator<uint32_t>> group_slots;
};

Endpoint endpoints[kEndpointLimit];
Group groups[kGroupCount];
uint16_t endpoint_slots[kHashSize];
uint16_t endpoint_order[kEndpointLimit];
char endpoint_arena[kEndpointLimit * kEndpointBytes];
uint32_t hour_keys[kHourCount];
uint32_t endpoint_count, arena_used;

inline uint64_t hash_bytes(const char* s, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) { h ^= uint8_t(s[i]); h *= 1099511628211ull; }
    return h;
}

inline bool digits(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i] < '0' || p[i] > '9') return false;
    return true;
}

inline uint32_t number(const char* p, size_t n) {
    uint32_t x = 0;
    for (size_t i = 0; i < n; ++i) x = x * 10u + uint32_t(p[i] - '0');
    return x;
}

inline bool parse_u32(const char* b, const char* e, uint32_t& value) {
    if (b == e || (e - b > 1 && *b == '0')) return false;
    uint32_t x = 0;
    for (; b < e; ++b) {
        const uint32_t d = uint32_t(*b - '0');
        if (d > 9 || x > (UINT32_MAX - d) / 10u) return false;
        x = x * 10u + d;
    }
    value = x;
    return true;
}

inline bool method_ok(const char* b, const char* e) {
    const size_t n = size_t(e - b);
    switch (n) {
        case 3: return std::memcmp(b, "GET", 3) == 0 || std::memcmp(b, "PUT", 3) == 0;
        case 4: return std::memcmp(b, "POST", 4) == 0 || std::memcmp(b, "HEAD", 4) == 0;
        case 5: return std::memcmp(b, "PATCH", 5) == 0;
        case 6: return std::memcmp(b, "DELETE", 6) == 0;
        case 7: return std::memcmp(b, "OPTIONS", 7) == 0;
        default: return false;
    }
}

inline bool endpoint_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
           c == '.' || c == '{' || c == '}' || c == ':';
}

bool parse_line(const char* b, const char* e, Parsed& out) {
    const size_t n = size_t(e - b);
    const size_t ts_len = 27;
    if (n < ts_len || b[19] != '.' || b[26] != 'Z') return false;
    if (b[4] != '-' || b[7] != '-' || b[10] != 'T' || b[13] != ':' ||
        b[16] != ':' || !digits(b, 4) || !digits(b + 5, 2) || !digits(b + 8, 2) ||
        !digits(b + 11, 2) || !digits(b + 14, 2) || !digits(b + 17, 2) ||
        !digits(b + 20, ts_len - 21)) return false;
    const int year = int(number(b, 4));
    const unsigned month = number(b + 5, 2), day = number(b + 8, 2);
    const unsigned hour = number(b + 11, 2), minute = number(b + 14, 2), second = number(b + 17, 2);
    if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) return false;
    if (year != 2026) return false;
    unsigned day_offset;
    if (month == 10 && day == 30) day_offset = 0;
    else if (month == 10 && day == 31) day_offset = 1;
    else if (month == 11 && day == 1) day_offset = 2;
    else if (month == 11 && day == 2) day_offset = 3;
    else return false;
    const int64_t h = int64_t(day_offset * 24u + hour) - 18;
    if (h < 0 || h >= kHourCount) return false;

    const char* p = b + ts_len;
    if (p == e || *p++ != ' ') return false;
    const char* m0 = p;
    while (p < e && *p != ' ') ++p;
    if (p == e || !method_ok(m0, p)) return false;
    ++p;
    const char* ep = p;
    while (p < e && *p != ' ') ++p;
    const size_t ep_len = size_t(p - ep);
    if (p == e || ep_len == 0 || ep_len > kEndpointBytes || ep[0] != '/') return false;
    for (size_t i = 0; i < ep_len; ++i) if (!endpoint_char(ep[i])) return false;
    ++p;
    const char* s0 = p;
    while (p < e && *p != ' ') ++p;
    if (p == e || p - s0 != 3 || !digits(s0, 3)) return false;
    const uint32_t status = number(s0, 3);
    if (status < 100 || status > 599) return false;
    ++p;
    uint32_t latency;
    if (!parse_u32(p, e, latency)) return false;
    out.hour = uint32_t(h);
    out.status = status;
    out.latency = latency;
    out.endpoint = ep;
    out.endpoint_length = uint32_t(ep_len);
    return true;
}

uint32_t intern_endpoint(const char* s, uint32_t len, bool add) {
    const size_t mask = kHashSize - 1u;
    for (size_t i = size_t(hash_bytes(s, len)) & mask;; i = (i + 1u) & mask) {
        const uint16_t id = endpoint_slots[i];
        if (id == kEmpty) {
            if (!add || endpoint_count == kEndpointLimit || arena_used + len > sizeof(endpoint_arena)) return UINT32_MAX;
            const uint32_t fresh = endpoint_count++;
            endpoints[fresh] = Endpoint{arena_used, uint16_t(len)};
            std::memcpy(endpoint_arena + arena_used, s, len);
            arena_used += len;
            endpoint_slots[i] = uint16_t(fresh);
            return fresh;
        }
        const Endpoint& ep = endpoints[id];
        if (ep.length == len && std::memcmp(endpoint_arena + ep.offset, s, len) == 0) return id;
    }
}

uint32_t local_endpoint(LocalWork& w, const char* s, uint32_t len) {
    const size_t mask = kHashSize - 1u;
    for (size_t i = size_t(hash_bytes(s, len)) & mask;; i = (i + 1u) & mask) {
        const uint16_t id = w.endpoint_slots[i];
        if (id == kEmpty) {
            if (w.endpoint_count == kEndpointLimit || w.arena_used + len > sizeof(w.arena)) return UINT32_MAX;
            const uint32_t fresh = w.endpoint_count++;
            w.endpoints[fresh] = Endpoint{w.arena_used, uint16_t(len)};
            std::memcpy(w.arena + w.arena_used, s, len);
            w.arena_used += len;
            w.endpoint_slots[i] = uint16_t(fresh);
            return fresh;
        }
        const Endpoint& ep = w.endpoints[id];
        if (ep.length == len && std::memcmp(w.arena + ep.offset, s, len) == 0) return id;
    }
}

void grow_local_groups(LocalWork& w) {
    std::vector<uint32_t, MmapAllocator<uint32_t>> old = std::move(w.group_slots);
    const size_t cap = old.empty() ? 1024u : old.size() * 2u;
    w.group_slots.assign(cap, UINT32_MAX);
    const size_t mask = cap - 1u;
    for (uint32_t i = 0; i < w.groups.size(); ++i) {
        const LocalGroup& g = w.groups[i];
        const uint64_t key = g.hour_endpoint;
        for (size_t j = (key * 0x9E3779B97F4A7C15ull >> 32) & mask;; j = (j + 1u) & mask) {
            if (w.group_slots[j] == UINT32_MAX) { w.group_slots[j] = i; break; }
        }
    }
}

LocalGroup* local_group(LocalWork& w, uint32_t hour, uint32_t endpoint) {
    if (w.group_slots.empty()) w.group_slots.assign(1024u, UINT32_MAX);
    if ((w.groups.size() + 1u) * 10u > w.group_slots.size() * 7u) grow_local_groups(w);
    const uint32_t key = hour * kEndpointLimit + endpoint;
    const uint64_t hashed = key;
    const size_t mask = w.group_slots.size() - 1u;
    for (size_t i = (hashed * 0x9E3779B97F4A7C15ull >> 32) & mask;; i = (i + 1u) & mask) {
        const uint32_t id = w.group_slots[i];
        if (id == UINT32_MAX) {
            w.groups.push_back(LocalGroup{key, 0, 0});
            w.group_slots[i] = uint32_t(w.groups.size() - 1u);
            return &w.groups.back();
        }
        LocalGroup& g = w.groups[id];
        if (g.hour_endpoint == key) return &g;
    }
}

size_t find_line_end(int fd, size_t pos, size_t file_size) {
    char buf[4096];
    while (pos < file_size) {
        const size_t want = std::min(sizeof(buf), file_size - pos);
        const ssize_t n = pread(fd, buf, want, off_t(pos));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return file_size;
        const char* nl = static_cast<const char*>(std::memchr(buf, '\n', size_t(n)));
        if (nl) return pos + size_t(nl - buf) + 1u;
        pos += size_t(n);
    }
    return file_size;
}

template<size_t BufferSize, class F>
bool parse_chunk(int fd, size_t lo, size_t hi, uint32_t worker_id, F& consume, std::atomic<bool>& io_ok) {
    char buf[BufferSize + kMaxLine];
    size_t offset = lo, carry = 0;
    bool discarding = false;
    while (offset < hi) {
        const size_t want = std::min(BufferSize, hi - offset);
        const ssize_t n = pread(fd, buf + carry, want, off_t(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { io_ok.store(false, std::memory_order_relaxed); return false; }
        offset += size_t(n);
        char* p = buf;
        char* const end = buf + carry + size_t(n);
        while (p < end) {
            char* nl = static_cast<char*>(std::memchr(p, '\n', size_t(end - p)));
            if (!nl) break;
            if (!discarding && size_t(nl - p) <= kMaxLine) consume(worker_id, p, nl);
            p = nl + 1;
            if (discarding) discarding = false;
        }
        carry = size_t(end - p);
        if (discarding || carry > kMaxLine) {
            carry = 0;
            discarding = true;
        } else if (carry) {
            std::memmove(buf, p, carry);
        }
    }
    if (!discarding && carry) consume(worker_id, buf, buf + carry);
    return true;
}

template<size_t BufferSize, class F>
bool scan_parallel(int fd, size_t file_size, F&& consume) {
    size_t bounds[kThreads + 1]{};
    bounds[0] = 0;
    bounds[kThreads] = file_size;
    for (uint32_t t = 1; t < kThreads; ++t) {
        const size_t nominal = file_size * t / kThreads;
        bounds[t] = find_line_end(fd, nominal, file_size);
    }
    std::atomic<bool> io_ok{true};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (uint32_t t = 0; t < kThreads; ++t) {
        const size_t lo = bounds[t], hi = bounds[t + 1];
        if (lo < hi) workers.emplace_back([&, t, lo, hi] { parse_chunk<BufferSize>(fd, lo, hi, t, consume, io_ok); });
    }
    for (std::thread& worker : workers) worker.join();
    return io_ok.load(std::memory_order_relaxed);
}

void build_hours() {
    int y = 2026, m = 10, d = 30, h = 18;
    for (uint32_t i = 0; i < kHourCount; ++i) {
        hour_keys[i] = uint32_t(((y * 100u + unsigned(m)) * 100u + unsigned(d)) * 100u + unsigned(h));
        if (++h == 24) {
            h = 0;
            static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
            int lim = mdays[m - 1] + (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
            if (++d > lim) { d = 1; if (++m == 13) { m = 1; ++y; } }
        }
    }
}

struct Writer {
    int fd;
    char buf[1u << 16];
    size_t used = 0;
    bool ok = true;
    void flush() {
        size_t off = 0;
        while (ok && off < used) {
            const ssize_t n = write(fd, buf + off, used - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            off += size_t(n);
        }
        used = 0;
    }
    void put(char c) { if (used == sizeof(buf)) flush(); if (ok) buf[used++] = c; }
    void append(const char* s, size_t n) {
        while (n && ok) {
            if (used == sizeof(buf)) flush();
            const size_t take = std::min(n, sizeof(buf) - used);
            std::memcpy(buf + used, s, take); used += take; s += take; n -= take;
        }
    }
    void num(uint64_t n) {
        char tmp[24]; size_t len = 0;
        do { tmp[len++] = char('0' + n % 10); n /= 10; } while (n);
        while (len) put(tmp[--len]);
    }
};

inline void append_2(Writer& w, uint32_t v) {
    w.put(char('0' + (v / 10) % 10)); w.put(char('0' + v % 10));
}
inline void append_4(Writer& w, uint32_t v) {
    w.put(char('0' + (v / 1000) % 10)); w.put(char('0' + (v / 100) % 10));
    w.put(char('0' + (v / 10) % 10)); w.put(char('0' + v % 10));
}

bool write_output(const char* out_dir, uint32_t* latencies) {
    std::string path(out_dir);
    path += "/summary.txt";
    if (mkdir(out_dir, 0777) != 0 && errno != EEXIST) return false;
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    Writer w{fd};
    for (uint32_t ep = 0; ep < endpoint_count; ++ep) endpoint_order[ep] = uint16_t(ep);
    std::sort(endpoint_order, endpoint_order + endpoint_count, [](uint16_t a, uint16_t b) {
        const Endpoint& x = endpoints[a]; const Endpoint& y = endpoints[b];
        const size_t n = std::min(x.length, y.length);
        const int c = std::memcmp(endpoint_arena + x.offset, endpoint_arena + y.offset, n);
        return c < 0 || (c == 0 && x.length < y.length);
    });
    for (uint32_t h = 0; h < kHourCount && w.ok; ++h) {
        const uint32_t key = hour_keys[h];
        for (uint32_t i = 0; i < endpoint_count && w.ok; ++i) {
            const uint32_t ep = endpoint_order[i];
            Group& g = groups[h * kEndpointLimit + ep];
            const uint32_t n = g.count;
            if (!n) continue;
            const uint32_t begin = g.cursor.load(std::memory_order_relaxed) - n;
            uint32_t* const first = latencies + begin;
            uint32_t* const last = first + n;
            const uint32_t r50 = (50u * n + 99u) / 100u;
            const uint32_t r95 = (95u * n + 99u) / 100u;
            const uint32_t r99 = (99u * n + 99u) / 100u;
            std::nth_element(first, first + r50 - 1u, last);
            const uint32_t p50 = first[r50 - 1u];
            uint32_t p95 = p50, p99;
            if (r95 > r50) std::nth_element(first + r50, first + r95 - 1u, last);
            p95 = first[r95 - 1u];
            if (r99 > r95) std::nth_element(first + r95, first + r99 - 1u, last);
            p99 = first[r99 - 1u];
            append_4(w, key / 1000000u); w.put('-'); append_2(w, (key / 10000u) % 100u);
            w.put('-'); append_2(w, (key / 100u) % 100u); w.put('T'); append_2(w, key % 100u);
            w.append(":00:00Z ", 8);
            const Endpoint& name = endpoints[ep];
            w.append(endpoint_arena + name.offset, name.length); w.put(' ');
            w.num(n); w.put(' '); w.num(p50); w.put(' '); w.num(p95); w.put(' ');
            w.num(p99); w.put(' '); w.num(g.errors); w.put(' ');
            w.num(uint64_t(g.errors) * 1000000ull / n); w.put('\n');
        }
    }
    w.flush();
    const bool ok = w.ok && close(fd) == 0;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const char* data_dir = "/data";
    const char* out_dir = "/out";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }
    std::string input(data_dir);
    input += "/requests.log";
    const int fd = open(input.c_str(), O_RDONLY);
    if (fd < 0) return 1;
    std::memset(endpoint_slots, 0xFF, sizeof(endpoint_slots));
    build_hours();
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0) { close(fd); return 1; }
    const size_t file_size = size_t(st.st_size);
    std::vector<LocalWork, MmapAllocator<LocalWork>> local(kThreads);
    for (LocalWork& w : local) std::memset(w.endpoint_slots, 0xFF, sizeof(w.endpoint_slots));
    std::atomic<bool> first_ok{true};
    // Pass one counts each group and interns endpoint names worker-locally.
    if (!scan_parallel<kReadSize>(fd, file_size, [&](uint32_t worker, const char* b, const char* e) {
        Parsed r;
        if (!parse_line(b, e, r)) return;
        LocalWork& w = local[worker];
        const uint32_t ep = local_endpoint(w, r.endpoint, r.endpoint_length);
        if (ep == UINT32_MAX) { first_ok.store(false, std::memory_order_relaxed); return; }
        LocalGroup* g = local_group(w, r.hour, ep);
        ++g->count;
        if (r.status >= 500 && r.status <= 599) ++g->errors;
    }) || !first_ok.load(std::memory_order_relaxed)) { close(fd); return 1; }

    std::vector<std::vector<uint16_t>> remap(kThreads);
    for (uint32_t t = 0; t < kThreads; ++t) {
        LocalWork& w = local[t];
        remap[t].resize(w.endpoint_count);
        for (uint32_t i = 0; i < w.endpoint_count; ++i) {
            const Endpoint& ep = w.endpoints[i];
            const uint32_t id = intern_endpoint(w.arena + ep.offset, ep.length, true);
            if (id == UINT32_MAX) { close(fd); return 1; }
            remap[t][i] = uint16_t(id);
        }
        for (const LocalGroup& lg : w.groups) {
            const uint32_t hour = lg.hour_endpoint / kEndpointLimit;
            const uint32_t ep = remap[t][lg.hour_endpoint % kEndpointLimit];
            Group& g = groups[hour * kEndpointLimit + ep];
            g.count += lg.count;
            g.errors += lg.errors;
        }
    }
    std::vector<LocalWork, MmapAllocator<LocalWork>>().swap(local);
    std::vector<std::vector<uint16_t>>().swap(remap);

    uint32_t total = 0;
    for (Group& g : groups) { g.cursor.store(total, std::memory_order_relaxed); total += g.count; }
    // Full-range uint32 latencies make histograms too large; pool one value per row.
    uint32_t* latencies = nullptr;
    try { if (total) latencies = new uint32_t[total]; }
    catch (...) { close(fd); return 1; }
    std::atomic<bool> fill_ok{true};
    // Pass two scatters each latency into its group's exact contiguous range.
    if (!scan_parallel<kThreadReadSize>(fd, file_size, [&](uint32_t, const char* b, const char* e) {
        Parsed r;
        if (!parse_line(b, e, r)) return;
        const uint32_t ep = intern_endpoint(r.endpoint, r.endpoint_length, false);
        if (ep == UINT32_MAX) { fill_ok.store(false, std::memory_order_relaxed); return; }
        Group& g = groups[r.hour * kEndpointLimit + ep];
        const uint32_t pos = g.cursor.fetch_add(1, std::memory_order_relaxed);
        if (pos >= total) { fill_ok.store(false, std::memory_order_relaxed); return; }
        latencies[pos] = r.latency;
    }) || !fill_ok.load(std::memory_order_relaxed)) { delete[] latencies; close(fd); return 1; }
    close(fd);
    const bool written = write_output(out_dir, latencies);
    delete[] latencies;
    return written ? 0 : 1;
}
