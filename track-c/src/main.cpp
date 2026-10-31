// Speed Demon 2026 - Track C (Join)
// Reads /data/records.csv + /data/regions.csv, writes /out/aggregates.txt.
//
// Semantics follow specs/c-join.md exactly:
//   * hostile CSV: quoted commas, "" escapes, embedded LF/CRLF, BOM at file
//     start and at some record boundaries, mixed CRLF/LF terminators
//   * amount is exact integer cents, never floating point
//   * group key is country from regions.csv, or _UNMATCHED
//   * one line per group, sorted bytewise, LF terminated.
//
// Shape: records.csv is mapped read-only (no copy, and the page cache it lives
// in is not charged to the run's cgroup) and cut into N byte ranges on record
// boundaries found by counting quotes (a newline is a boundary only at even
// quote parity). Every thread parses its range straight into per-country
// accumulators. Names are interned into one shared open-addressing table: hits
// are a plain acquire load (no lock, no per-thread copy to merge later), and
// only the ~3% of lookups that are first sightings take a spinlock. Ids are
// global, so one shared bitmap per country counts distinct names directly.
// That bitmap is updated with a relaxed atomic OR: it costs ~20 MB peak RSS,
// where a private bitmap per thread per country cost ~52 MB (PERF.md §6.3).
// Name work is batched 32 records at a time: the batch loop issues independent
// probes, so the table miss of record k+1 is in flight while k is compared
// (measured 0.886x wall, PERF.md §6.4).
//
// Build: g++ -O3 -std=c++17 -DNDEBUG -flto -pthread -static -s -o solution src/main.cpp

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr uint32_t kNoCountry = 0xFFFF;
constexpr uint32_t kThreads = 8;         // measured knee in PERF.md

// Shared name table. 2^18 slots, 70% max load => 183500 names; the dataset has
// 100520, and both caps below are loud failures rather than silent corruption.
constexpr uint32_t kShift = 18;
constexpr uint32_t kNameSlots = 1u << kShift;
constexpr uint32_t kSlotMask = kNameSlots - 1;
constexpr uint32_t kNameCap = kNameSlots * 7 / 10;
constexpr uint32_t kBlockShift = 20;
constexpr uint32_t kBlockSize = 1u << kBlockShift;
constexpr uint32_t kBlockMask = kBlockSize - 1;
constexpr uint32_t kMaxBlocks = 64;      // 64 MiB arena cap
constexpr uint32_t kNotFound = 0xFFFFFFFFu;
constexpr uint32_t kSeenWords = (kNameCap + 63) / 64;
uint64_t* g_seen = nullptr;  // shared (ncountry+1) x kSeenWords bitset

// POD slot so static zero-init is guaranteed; h is touched with __atomic and is
// stored last (release) so a reader that observes it also sees the rest. 16 bytes
// puts four slots on a cache line and cuts the table footprint to ~1.6 MB of
// occupied lines: measured ~5% faster end to end than 24-byte slots.
struct Slot {
    uint64_t h;
    uint32_t off;    // arena offset
    uint32_t idlen;  // name id in bits 0..17, byte length in bits 18..31
};

alignas(64) Slot g_tab[kNameSlots];
char* g_block[kMaxBlocks];
uint32_t g_used = 0;    // arena bytes used, block-aligned
uint32_t g_count = 0;   // names interned; also the next id
int g_spin = 0;

[[noreturn]] void overflow(const char* what) {
    std::fprintf(stderr, "name table overflow: %s\n", what);
    std::exit(1);
}

inline uint64_t fnv(const char* s, size_t n) {
    // First and last 8 bytes plus the length, not a multiply per byte: FNV-1a
    // is a 4-cycle dependency chain per byte and the name hash is on every
    // record. Exactness is the arena memcmp's job, so a weaker hash can only
    // cost probes; measured probe/lookup 1.296 vs 1.300 for FNV-1a, and 1.6%
    // less wall time (PERF.md §7).
    uint64_t a = 0, b = 0;
    if (n >= 8) {
        std::memcpy(&a, s, 8);
        std::memcpy(&b, s + n - 8, 8);
    } else {
        for (size_t i = 0; i < n; ++i)
            a |= static_cast<uint64_t>(static_cast<unsigned char>(s[i])) << (8 * i);
    }
    uint64_t h = a * 0x9E3779B97F4A7C15ull + b * 0xC2B2AE3D27D4EB4Full + n * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h ? h : 1;
}

inline const char* name_str(uint32_t off) {
    return g_block[off >> kBlockShift] + (off & kBlockMask);
}

// Lock-free probe: acquire loads only, terminates because slots are never
// cleared and the table is never rehashed.
inline uint32_t name_lookup(uint64_t h, const char* s, uint32_t n) {
    uint32_t i = static_cast<uint32_t>(h) & kSlotMask;
    for (;;) {
        const Slot& e = g_tab[i];
        const uint64_t eh = __atomic_load_n(&e.h, __ATOMIC_ACQUIRE);
        if (!eh) return kNotFound;
        if (eh == h && (e.idlen >> 18) == n && std::memcmp(name_str(e.off), s, n) == 0)
            return e.idlen & 0x3FFFF;
        i = (i + 1) & kSlotMask;
    }
}

uint32_t name_intern_h(uint64_t h, const char* s, uint32_t n) {
    uint32_t id = name_lookup(h, s, n);
    if (id != kNotFound) return id;

    // Spinlock, not std::mutex: an insert is a few dozen instructions, but the
    // futex path costs a sleeping thread a full wake-up when two threads insert
    // at once. Spin+yield measured 8% faster end to end (PERF.md).
    while (__atomic_exchange_n(&g_spin, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&g_spin, __ATOMIC_RELAXED)) std::this_thread::yield();
    }
    struct Unlock {
        ~Unlock() { __atomic_store_n(&g_spin, 0, __ATOMIC_RELEASE); }
    } unlock_guard;
    id = name_lookup(h, s, n);  // lost the race to an identical insert
    if (id != kNotFound) return id;
    if (g_count >= kNameCap) overflow("too many distinct names");
    if (n > 0x3FFF) overflow("name longer than 16383 bytes");
    uint32_t b = g_used >> kBlockShift;
    if ((g_used & kBlockMask) + n > kBlockSize) {  // pad so no name straddles a block
        g_used = (b + 1) << kBlockShift;
        ++b;
    }
    if (b >= kMaxBlocks) overflow("arena full");
    if (!g_block[b]) {
        g_block[b] = static_cast<char*>(std::malloc(kBlockSize));
        if (!g_block[b]) overflow("arena alloc failed");
    }
    const uint32_t off = g_used;
    std::memcpy(g_block[b] + (off & kBlockMask), s, n);
    g_used += n;

    uint32_t i = static_cast<uint32_t>(h) & kSlotMask;
    while (__atomic_load_n(&g_tab[i].h, __ATOMIC_RELAXED)) i = (i + 1) & kSlotMask;
    g_tab[i].off = off;
    g_tab[i].idlen = g_count | (n << 18);
    __atomic_store_n(&g_tab[i].h, h, __ATOMIC_RELEASE);
    return g_count++;
}

// ---------------------------------------------------------------- per-thread state

struct Acc {
    int64_t count = 0, id_sum = 0, amount_sum = 0;
    int64_t amount_min = 0, amount_max = 0;
    uint64_t name_bytes = 0, note_bytes = 0;
};

struct Job {
    const std::vector<uint32_t>* lut = nullptr;
    uint32_t unmatched = 0;
    off_t start = 0, stop = 0;
    bool is_last = false;
    // Name references held between parse and drain. `s` points into the mapped
    // file, which outlives the job; escaped names are drained before the
    // per-record scratch buffer is reused instead.
    struct Pending { uint64_t h; const char* s; uint32_t n; uint32_t c; };
    Pending pend[32];
    uint32_t npend = 0;
    std::vector<Acc> acc;
    std::vector<char> tmp;        // unescape scratch
};

// Drain the batch: the probes are independent of each other, so their table
// misses overlap, and so do the bitmap ORs. Same table, same lock, same result.
void flush_pending(Job& j) {
    const uint32_t n = j.npend;
    uint32_t ids[32];
    for (uint32_t k = 0; k < n; ++k) {
        const Job::Pending& e = j.pend[k];
        uint32_t id = name_lookup(e.h, e.s, e.n);
        if (id == kNotFound) id = name_intern_h(e.h, e.s, e.n);
        ids[k] = id;
    }
    for (uint32_t k = 0; k < n; ++k)
        __atomic_fetch_or(g_seen + static_cast<size_t>(j.pend[k].c) * kSeenWords + (ids[k] >> 6),
                          1ull << (ids[k] & 63), __ATOMIC_RELAXED);
    j.npend = 0;
}

// ---------------------------------------------------------------- CSV record

// 0x80 in every byte of w that holds '"'. Shared by find_quote/count_quotes.
inline uint64_t quote_bits(uint64_t w) {
    const uint64_t x = w ^ 0x2222222222222222ull;
    const uint64_t v = x & 0x7F7F7F7F7F7F7F7Full;
    return ~((v + 0x7F7F7F7F7F7F7F7Full) | x) & 0x8080808080808080ull;
}

// First '"' in [p, end), or nullptr. Fields are short, so an 8-byte SWAR probe
// beats a memchr call per quoted field (measured in PERF.md).
inline const char* find_quote(const char* p, const char* end) {
    while (p + 8 <= end) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        const uint64_t m = quote_bits(w);
        if (m) return p + (static_cast<unsigned>(__builtin_ctzll(m)) >> 3);
        p += 8;
    }
    while (p < end && *p != '"') ++p;
    return p < end ? p : nullptr;
}

inline uint64_t count_quotes_span(const char* p, const char* end) {
    uint64_t n = 0;
    while (p + 8 <= end) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        n += static_cast<uint64_t>(__builtin_popcountll(quote_bits(w)));
        p += 8;
    }
    while (p < end) n += (*p++ == '"');
    return n;
}

// Number of leading ASCII digits in an 8-byte little-endian window.
inline uint32_t ndigits(uint64_t w) {
    const uint64_t x = w ^ 0x3030303030303030ull;
    const uint64_t t = (x + 0x7676767676767676ull) | x;  // high bit iff non-digit
    const uint64_t m = t & 0x8080808080808080ull;
    return m ? static_cast<uint32_t>(__builtin_ctzll(m)) >> 3 : 8;
}

// Value of the first nd (<= 8) ASCII digits of w (little-endian window). The
// digits are pushed into the high bytes so the multiply chain puts each on its
// 10^(7-i) place; bytes past nd shift out. Unit-tested in bench/digits_test.cpp.
inline uint64_t digits8(uint64_t w, uint32_t nd) {
    if (nd < 8) w <<= 8 * (8 - nd);
    uint64_t v = w & 0x0F0F0F0F0F0F0F0Full;
    v = (v * 2561) >> 8;
    v = (v & 0x00FF00FF00FF00FFull) * 6553601ull >> 16;
    return ((v & 0x0000FFFF0000FFFFull) * 42949672960001ull) >> 32;
}

// Parses the ASCII digits at p into v, returns the first non-digit. The 8-byte
// window replaces a serial v = v*10 + d chain (~3 cycles per digit, measured in
// PERF.md); 8+ digit fields fall back to the loop.
inline const char* parse_u64(const char* p, const char* end, uint64_t& v) {
    if (p + 8 <= end) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        const uint32_t nd = ndigits(w);
        if (nd < 8) { v = digits8(w, nd); return p + nd; }
    }
    while (p < end && *p >= '0' && *p <= '9') v = v * 10 + static_cast<uint64_t>(*p++ - '0');
    return p;
}

// Scans one CSV field at p (quoted or plain) and leaves p on the delimiter.
// Reports the unescaped length and the number of "" pairs; false if the buffer
// ends inside the field.
inline bool scan_field(const char*& p, const char* end, const char*& start, uint32_t& len,
                       uint32_t& esc) {
    esc = 0;
    start = p;
    if (p < end && *p == '"') {
        start = ++p;
        for (;;) {
            const char* q = find_quote(p, end);
            if (!q) return false;
            if (q + 1 < end && q[1] == '"') { ++esc; p = q + 2; }
            else { len = static_cast<uint32_t>(q - start) - esc; p = q + 1; return true; }
        }
    }
    while (p < end && *p != ',') ++p;
    len = static_cast<uint32_t>(p - start);
    return p < end;
}

// Returns pointer past the record terminator, or nullptr if the record is
// incomplete in this buffer. `eof` allows a final record without newline.
const char* parse_record(Job& j, const char* p, const char* end, bool eof) {
    if (p + 3 <= end && static_cast<unsigned char>(p[0]) == 0xEF &&
        static_cast<unsigned char>(p[1]) == 0xBB && static_cast<unsigned char>(p[2]) == 0xBF)
        p += 3;
    uint64_t rid = 0, region = 0, whole = 0;
    const char* nstart = p, *tstart = p;
    uint32_t nlen = 0, esc = 0, tlen = 0, tesc = 0;
    p = parse_u64(p, end, rid);
    if (p >= end) return eof ? p : nullptr;
    if (*p++ != ',') return nullptr;
    // name: unescape and hash now, and prefetch the table line while the rest of
    // the record is parsed (hides the first miss of the lookup).
    if (!scan_field(p, end, nstart, nlen, esc)) return eof ? end : nullptr;
    if (esc) {  // "" -> " into the job scratch; raw length is nlen + esc
        const char* e = nstart + nlen + esc;
        j.tmp.resize(static_cast<size_t>(e - nstart));
        char* t = j.tmp.data();
        uint32_t o = 0;
        for (const char* s = nstart; s < e; ++s) {
            t[o++] = *s;
            if (s[0] == '"' && s + 1 < e && s[1] == '"') ++s;
        }
        nstart = t;
        nlen = o;
    }
    const uint64_t nh = fnv(nstart, nlen);
    __builtin_prefetch(&g_tab[static_cast<uint32_t>(nh) & kSlotMask], 0, 3);
    if (p >= end) return eof ? end : nullptr;
    if (*p++ != ',') return nullptr;
    // note: only the unescaped byte length is needed
    if (!scan_field(p, end, tstart, tlen, tesc)) return eof ? end : nullptr;
    if (p >= end) return eof ? end : nullptr;
    if (*p++ != ',') return nullptr;
    p = parse_u64(p, end, region);
    if (p >= end) return eof ? end : nullptr;
    if (*p++ != ',') return nullptr;
    // amount: -?[0-9]+\.[0-9][0-9], exact integer cents
    bool neg = false;
    if (p < end && *p == '-') { neg = true; ++p; }
    p = parse_u64(p, end, whole);
    if (p + 3 > end) return eof ? end : nullptr;
    if (*p != '.' || p[1] < '0' || p[1] > '9' || p[2] < '0' || p[2] > '9') return nullptr;
    int64_t cents = static_cast<int64_t>(whole * 100 + static_cast<uint64_t>(p[1] - '0') * 10 + (p[2] - '0'));
    if (neg) cents = -cents;
    p += 3;
    if (p < end && *p == '\r') ++p;
    if (p < end && *p == '\n') ++p;
    else if (p >= end && !eof) return nullptr;

    // join + aggregate
    uint16_t c = kNoCountry;
    const std::vector<uint32_t>& lut = *j.lut;
    if (region < lut.size()) {
        uint32_t v = lut[static_cast<size_t>(region)];
        if (v) c = static_cast<uint16_t>(v - 1);
    }
    if (c == kNoCountry) c = static_cast<uint16_t>(j.unmatched);
    Acc& a = j.acc[c];
    if (a.count == 0) { a.amount_min = cents; a.amount_max = cents; }
    ++a.count;
    a.id_sum += static_cast<int64_t>(rid);
    a.amount_sum += cents;
    if (cents < a.amount_min) a.amount_min = cents;
    if (cents > a.amount_max) a.amount_max = cents;
    a.name_bytes += nlen;
    a.note_bytes += tlen;
    if (esc) {  // scratch buffer is reused per record, so drain before queueing
        flush_pending(j);
        const uint32_t nid = name_intern_h(nh, nstart, nlen);
        __atomic_fetch_or(g_seen + static_cast<size_t>(c) * kSeenWords + (nid >> 6),
                          1ull << (nid & 63), __ATOMIC_RELAXED);
    } else {
        Job::Pending& pe = j.pend[j.npend];
        pe.h = nh;
        pe.s = nstart;
        pe.n = nlen;
        pe.c = c;
        if (++j.npend == 32) flush_pending(j);
    }
    return p;
}

// ---------------------------------------------------------------- I/O + ranges

// First record boundary at/after `from`: a '\n' at even quote parity, with the
// parity carried in from the caller. Runs until the next terminator, so it is
// typically a few hundred bytes of work.
off_t find_boundary(const char* base, off_t from, int parity, off_t size) {
    const char* p = base + from;
    const char* e = base + size;
    while (p < e) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(e - p)));
        if (!nl) return size;
        parity ^= static_cast<int>(count_quotes_span(p, nl) & 1);
        if (!parity) return static_cast<off_t>(nl - base) + 1;
        p = nl + 1;
    }
    return size;
}

void parse_range(Job& j, const char* p, const char* end, bool eof) {
    while (p < end) {
        const char* next = parse_record(j, p, end, eof);
        if (!next) { flush_pending(j); return; }
        p = next;
    }
    flush_pending(j);
}

// ---------------------------------------------------------------- regions

struct Regions {
    std::vector<std::string> codes;   // country code per id
    std::vector<uint32_t> row_country;
    std::vector<uint32_t> ids;
};

bool load_regions(const char* path, Regions& R) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); return false; }
    std::vector<char> buf(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < buf.size()) {
        ssize_t n = ::pread(fd, buf.data() + got, buf.size() - got, static_cast<off_t>(got));
        if (n <= 0) { ::close(fd); return false; }
        got += static_cast<size_t>(n);
    }
    ::close(fd);

    const char* p = buf.data();
    const char* end = p + buf.size();
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    if (!nl) return false;
    p = nl + 1;

    // 3-byte country code -> id, open addressing over a small table
    constexpr size_t kSlots = 4096;
    std::vector<uint32_t> key(kSlots, 0);   // code | 1<<24 marks used
    std::vector<uint32_t> val(kSlots, 0);
    while (p < end) {
        uint32_t id = 0;
        while (p < end && *p >= '0' && *p <= '9') id = id * 10 + static_cast<uint32_t>(*p++ - '0');
        if (p >= end || *p != ',') return false;
        ++p;
        const char* code = p;
        while (p < end && *p != '\n' && *p != '\r') ++p;
        size_t clen = static_cast<size_t>(p - code);
        char c0 = clen >= 1 ? code[0] : '?', c1 = clen >= 2 ? code[1] : '?',
             c2 = clen >= 3 ? code[2] : '?';
        uint32_t k = (static_cast<uint32_t>(static_cast<unsigned char>(c0)) << 16) |
                     (static_cast<uint32_t>(static_cast<unsigned char>(c1)) << 8) |
                     static_cast<uint32_t>(static_cast<unsigned char>(c2)) | (1u << 24);
        size_t i = (k * 2654435761u) >> 20;  // top 12 bits of a multiplicative hash
        while (key[i] && key[i] != k) i = (i + 1) & (kSlots - 1);
        uint32_t cid;
        if (!key[i]) {
            key[i] = k;
            cid = static_cast<uint32_t>(R.codes.size());
            val[i] = cid + 1;
            R.codes.emplace_back(code, clen);
        } else {
            cid = val[i] - 1;
        }
        R.ids.push_back(id);
        R.row_country.push_back(cid);
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
    }
    return !R.codes.empty();
}

// ---------------------------------------------------------------- threads

template <class F>
void parallel(uint32_t n, F&& f) {
    std::vector<std::thread> ts;
    ts.reserve(n - 1);
    for (uint32_t i = 1; i < n; ++i) ts.emplace_back([&f, i] { f(i); });
    f(0);
    for (std::thread& t : ts) t.join();
}

// ---------------------------------------------------------------- output

char* put_u64(char* o, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n) *o++ = t[--n];
    return o;
}

char* put_cents(char* o, int64_t c) {
    uint64_t u = c < 0 ? static_cast<uint64_t>(-c) : static_cast<uint64_t>(c);
    if (c < 0) *o++ = '-';
    o = put_u64(o, u / 100);
    *o++ = '.';
    uint64_t f = u % 100;
    *o++ = static_cast<char>('0' + f / 10);
    *o++ = static_cast<char>('0' + f % 10);
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    const char* data = "/data";
    const char* out = "/out";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }
    std::string rp(data), op(out), rec(data);
    rp += "/regions.csv";
    op += "/aggregates.txt";
    rec += "/records.csv";

    int fd = ::open(rec.c_str(), O_RDONLY);
    if (fd < 0) { std::fprintf(stderr, "cannot open records.csv\n"); return 1; }
    struct stat st;
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) { std::fprintf(stderr, "records.csv empty\n"); return 1; }
    const off_t size = st.st_size;
    const char* base = static_cast<const char*>(::mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_PRIVATE, fd, 0));
    if (base == MAP_FAILED) { std::fprintf(stderr, "mmap failed\n"); return 1; }

    uint32_t hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 1;
    uint32_t nt = std::min(kThreads, hw);
    if (size < static_cast<off_t>(4) << 20) nt = 1;

    // Phase 1: count quotes per nominal range while regions.csv loads on a
    // helper thread. Neither needs the other's result.
    std::vector<off_t> rstart(nt), rstop(nt);
    for (uint32_t i = 0; i < nt; ++i) {
        rstart[i] = size * i / nt;
        rstop[i] = size * (i + 1) / nt;
    }
    std::vector<uint64_t> q(nt, 0);
    Regions R;
    bool regions_ok = true;
    std::thread region_load([&] { regions_ok = load_regions(rp.c_str(), R); });
    parallel(nt, [&](uint32_t i) {
        q[i] = count_quotes_span(base + rstart[i], base + rstop[i]);
    });
    region_load.join();
    if (!regions_ok) { std::fprintf(stderr, "regions.csv failed\n"); return 1; }
    const uint32_t ncountry = static_cast<uint32_t>(R.codes.size());
    const uint32_t unmatched = ncountry;  // extra last group
    uint32_t max_region = 0;
    for (uint32_t id : R.ids) max_region = std::max(max_region, id);
    std::vector<uint32_t> lut(static_cast<size_t>(max_region) + 1, 0);
    for (size_t k = 0; k < R.ids.size(); ++k) lut[R.ids[k]] = R.row_country[k] + 1;
    std::vector<std::string> codes = std::move(R.codes);
    codes.emplace_back("_UNMATCHED");
    R = Regions();

    g_seen = static_cast<uint64_t*>(
        std::calloc(static_cast<size_t>(ncountry + 1) * kSeenWords, sizeof(uint64_t)));
    if (!g_seen) { std::fprintf(stderr, "bitmap alloc failed\n"); return 1; }

    std::vector<Job> jobs(nt);
    for (uint32_t i = 0; i < nt; ++i) {
        jobs[i].lut = &lut;
        jobs[i].unmatched = unmatched;
        jobs[i].acc.resize(ncountry + 1);
        jobs[i].is_last = (i + 1 == nt);
        jobs[i].start = rstart[i];
        jobs[i].stop = rstop[i];
    }

    std::vector<int> par(nt, 0);
    for (uint32_t i = 1; i < nt; ++i) par[i] = par[i - 1] ^ static_cast<int>(q[i - 1] & 1);

    // Phase 2: move each range start to the first record boundary at/after it.
    parallel(nt, [&](uint32_t i) {
        if (i == 0) {  // BOM, then the header line
            off_t pos = (size >= 3 && static_cast<unsigned char>(base[0]) == 0xEF &&
                         static_cast<unsigned char>(base[1]) == 0xBB &&
                         static_cast<unsigned char>(base[2]) == 0xBF) ? 3 : 0;
            const char* nl = static_cast<const char*>(std::memchr(base + pos, '\n', static_cast<size_t>(size - pos)));
            jobs[0].start = nl ? static_cast<off_t>(nl - base) + 1 : size;
        } else {
            jobs[i].start = find_boundary(base, jobs[i].start, par[i], size);
        }
    });
    // Ranges are half-open [start_i, start_{i+1}); the last reaches EOF.
    for (uint32_t i = 0; i + 1 < nt; ++i)
        jobs[i].stop = std::max(jobs[i].start, jobs[i + 1].start);
    jobs[nt - 1].stop = size;

    // Phase 3: parse. Accumulators are per thread; names go to the one shared table.
    parallel(nt, [&](uint32_t i) {
        if (jobs[i].start < jobs[i].stop || jobs[i].is_last)
            parse_range(jobs[i], base + jobs[i].start, base + jobs[i].stop, jobs[i].is_last);
    });
    ::munmap(const_cast<char*>(base), static_cast<size_t>(size));
    ::close(fd);

    // Phase 4: sum the scalars, then count bits in the shared per-country bitmap.
    std::vector<uint32_t> distinct(ncountry + 1, 0);
    std::vector<Acc> total(ncountry + 1);
    parallel(nt, [&](uint32_t w) {
        uint32_t lo = (ncountry + 1) * w / nt, hi = (ncountry + 1) * (w + 1) / nt;
        for (uint32_t c = lo; c < hi; ++c) {
            Acc& s = total[c];
            for (uint32_t t = 0; t < nt; ++t) {
                const Acc& a = jobs[t].acc[c];
                if (!a.count) continue;
                if (!s.count) { s.amount_min = a.amount_min; s.amount_max = a.amount_max; }
                s.count += a.count;
                s.id_sum += a.id_sum;
                s.amount_sum += a.amount_sum;
                s.amount_min = std::min(s.amount_min, a.amount_min);
                s.amount_max = std::max(s.amount_max, a.amount_max);
                s.name_bytes += a.name_bytes;
                s.note_bytes += a.note_bytes;
            }
            if (!s.count) continue;
            uint64_t bits = 0;
            const uint64_t* row = g_seen + static_cast<size_t>(c) * kSeenWords;
            for (uint32_t k = 0; k < kSeenWords; ++k)
                bits += static_cast<uint64_t>(__builtin_popcountll(row[k]));
            distinct[c] = static_cast<uint32_t>(bits);
        }
    });

    // Phase 5: write, sorted bytewise.
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i <= ncountry; ++i)
        if (total[i].count) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&codes](uint32_t a, uint32_t b) { return codes[a] < codes[b]; });
    std::string outbuf;
    outbuf.reserve(order.size() * 96);
    char line[256];
    for (uint32_t idx : order) {
        const Acc& a = total[idx];
        const uint32_t ndistinct = distinct[idx];
        char* o = line;
        const std::string& code = codes[idx];
        std::memcpy(o, code.data(), code.size());
        o += code.size();
        *o++ = ' ';
        o = put_u64(o, static_cast<uint64_t>(a.count));
        *o++ = ' ';
        o = put_u64(o, static_cast<uint64_t>(a.id_sum));
        *o++ = ' ';
        o = put_cents(o, a.amount_sum);
        *o++ = ' ';
        o = put_cents(o, a.amount_min);
        *o++ = ' ';
        o = put_cents(o, a.amount_max);
        *o++ = ' ';
        o = put_u64(o, ndistinct);
        *o++ = ' ';
        o = put_u64(o, a.name_bytes);
        *o++ = ' ';
        o = put_u64(o, a.note_bytes);
        *o++ = '\n';
        outbuf.append(line, static_cast<size_t>(o - line));
    }
    std::FILE* f = std::fopen(op.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "write failed\n"); return 1; }
    bool ok = std::fwrite(outbuf.data(), 1, outbuf.size(), f) == outbuf.size();
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::fprintf(stderr, "write failed\n"); return 1; }
    return 0;
}
