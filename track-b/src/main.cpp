// Speed Demon 2026 - Track B (Index)
//
// Reads /data/docs.txt and /data/queries.txt, writes /out/results.txt.
// Follows specs/b-index.md exactly:
//   * token = maximal run of ASCII [a-z0-9] after ASCII lowercasing; every
//     other byte (including every byte >= 0x80) is a separator
//   * BOOL applies AND/OR/NOT strictly left to right, NOT is binary
//   * PHRASE requires consecutive token positions inside one document
//   * output: count, sum of ids, then min(10, count) smallest ids
//
// The queries are read first, so only the terms they mention are indexed:
// document postings for every query term, token positions only for terms
// that occur in a phrase.  Index construction is a two-pass parallel CSR
// build (count, then fill): no per-term vectors, no reallocation, and exact
// memory.  docs.txt is memory mapped read-only.
//
// Build: g++ -O3 -std=c++17 -DNDEBUG -march=native -flto -pthread -static -s
// Run:   ./runner /data /out

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace {

constexpr uint32_t kNoTerm = 0xFFFFFFFFu;
constexpr uint32_t kMaxTermLen = 32;


// ---------------------------------------------------------------- utilities

uint8_t g_tok[256];

void init_token_table() {
    for (int i = 0; i < 256; ++i) {
        unsigned c = static_cast<unsigned>(i);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            g_tok[i] = static_cast<uint8_t>(c);
        else
            g_tok[i] = 0;
    }
}

inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

inline uint64_t hash_bytes(const char* s, uint32_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(n) * 0x9e3779b97f4a7c15ULL);
    while (n >= 8) {
        uint64_t k;
        std::memcpy(&k, s, 8);
        h ^= k;
        h = (h << 27) | (h >> 37);
        h *= 0x9e3779b97f4a7c15ULL;
        s += 8;
        n -= 8;
    }
    if (n) {
        uint64_t k = 0;
        std::memcpy(&k, s, n);
        h ^= k;
        h *= 0x9e3779b97f4a7c15ULL;
    }
    return mix64(h);
}

template <typename U>
inline void append_uint(std::string& out, U v) {
    char b[32];
    int n = 0;
    do {
        b[n++] = static_cast<char>('0' + static_cast<char>(v % 10));
        v /= 10;
    } while (v);
    while (n) out.push_back(b[--n]);
}

// -------------------------------------------------------------- term table

// Open-addressing string -> dense id map.  Keys live in one arena and slots
// store offsets, so the table never moves a key.  Read-only after setup.
class TermTable {
public:
    struct Slot {
        uint64_t h;
        uint32_t id;
        uint32_t off;
        uint32_t len;
        uint32_t pad;
    };

    TermTable() { reset(1u << 12); }

    uint32_t size() const { return count_; }
    uint32_t max_len() const { return max_len_; }

    uint32_t find(const char* s, uint32_t n) const {
        return find_h(s, n, hash_bytes(s, n));
    }

    uint32_t find_h(const char* s, uint32_t n, uint64_t h) const {
        uint32_t i = static_cast<uint32_t>(h) & mask_;
        for (;;) {
            const Slot& sl = slots_[i];
            if (!sl.len) return kNoTerm;
            if (sl.h == h && sl.len == n && std::memcmp(arena_.data() + sl.off, s, n) == 0)
                return sl.id;
            i = (i + 1) & mask_;
        }
    }

    uint32_t insert(const char* s, uint32_t n) {
        if ((static_cast<size_t>(count_) + 1) * 2 > slots_.size()) grow();
        uint64_t h = hash_bytes(s, n);
        uint32_t i = static_cast<uint32_t>(h) & mask_;
        for (;;) {
            Slot& sl = slots_[i];
            if (!sl.len) {
                sl.h = h;
                sl.id = count_;
                sl.off = static_cast<uint32_t>(arena_.size());
                sl.len = n;
                sl.pad = 0;
                arena_.insert(arena_.end(), s, s + n);
                if (n > max_len_) max_len_ = n;
                return count_++;
            }
            if (sl.h == h && sl.len == n && std::memcmp(arena_.data() + sl.off, s, n) == 0)
                return sl.id;
            i = (i + 1) & mask_;
        }
    }

private:
    void reset(size_t n) {
        slots_.assign(n, Slot{0, 0, 0, 0, 0});
        mask_ = static_cast<uint32_t>(n - 1);
        count_ = 0;
        arena_.reserve(4u << 20);
    }

    void grow() {
        std::vector<Slot> ns(slots_.size() * 2, Slot{0, 0, 0, 0, 0});
        uint32_t nmask = static_cast<uint32_t>(ns.size() - 1);
        for (const Slot& sl : slots_) {
            if (!sl.len) continue;
            uint32_t i = static_cast<uint32_t>(sl.h) & nmask;
            while (ns[i].len) i = (i + 1) & nmask;
            ns[i] = sl;
        }
        slots_.swap(ns);
        mask_ = nmask;
    }

    std::vector<Slot> slots_;
    std::vector<char> arena_;
    uint32_t mask_ = 0;
    uint32_t count_ = 0;
    uint32_t max_len_ = 0;
};

// --------------------------------------------------------------- file input

bool read_whole_file(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    if (sz < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(sz));
    size_t got = sz ? std::fread(&out[0], 1, static_cast<size_t>(sz), f) : 0;
    std::fclose(f);
    return got == static_cast<size_t>(sz);
}

struct MappedFile {
    const char* data = nullptr;
    size_t size = 0;
    bool open(const std::string& path) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st;
        if (::fstat(fd, &st) != 0 || st.st_size < 0) { ::close(fd); return false; }
        size = static_cast<size_t>(st.st_size);
        if (size == 0) { ::close(fd); return true; }
        void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        data = static_cast<const char*>(p);
        return true;
    }
};

// --------------------------------------------------------------- query data

struct Query {
    uint32_t kind;  // 0 = BOOL, 1 = PHRASE
    uint32_t toff;  // first term id in terms[]
    uint32_t ooff;  // first op code in ops[]
    uint32_t n;     // term count
};

enum : uint8_t { OP_AND = 0, OP_OR = 1, OP_NOT = 2 };

// Parse queries in a single pass: each term token is inserted once, then
// immediately resolved to its dense id while the flat query representation is
// built.  This keeps the grammar walk and the duplicate lookup pass in sync.
uint32_t parse_queries(const std::string& qb, TermTable& table, std::vector<Query>& queries,
                      std::vector<uint32_t>& terms, std::vector<uint8_t>& ops, std::vector<uint8_t>& need_pos) {
    const char* p = qb.data();
    const char* e = p + qb.size();
    queries.reserve(qb.size() / 30 + 16);
    terms.reserve(qb.size() / 4);
    ops.reserve(qb.size() / 8);
    while (p < e) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(e - p)));
        if (!nl) nl = e;
        size_t len = static_cast<size_t>(nl - p);
        Query qu{};
        qu.toff = static_cast<uint32_t>(terms.size());
        qu.ooff = static_cast<uint32_t>(ops.size());
        if (len >= 5 && std::memcmp(p, "BOOL ", 5) == 0) {
            qu.kind = 0;
            const char* q = p + 5;
            bool is_term = true;
            for (const char* r = q;; ++r) {
                if (r == nl || *r == ' ') {
                    if (r > q) {
                        const uint32_t n = static_cast<uint32_t>(r - q);
                        if (is_term) {
                            terms.push_back(table.insert(q, n));
                        } else if (std::memcmp(q, "AND", 3) == 0) {
                            ops.push_back(OP_AND);
                        } else if (std::memcmp(q, "OR", 2) == 0) {
                            ops.push_back(OP_OR);
                        } else {
                            ops.push_back(OP_NOT);
                        }
                    }
                    is_term = !is_term;
                    q = r + 1;
                    if (r == nl) break;
                }
            }
            qu.n = static_cast<uint32_t>(terms.size()) - qu.toff;
        } else if (len >= 8 && std::memcmp(p, "PHRASE \"", 8) == 0) {
            qu.kind = 1;
            const char* q = p + 8;
            const char* end = nl;
            if (end > q && end[-1] == '"') --end;
            while (q < end) {
                const char* s = q;
                while (q < end && *q != ' ') ++q;
                if (q > s) terms.push_back(table.insert(s, static_cast<uint32_t>(q - s)));
                if (q < end) ++q;
            }
            qu.n = static_cast<uint32_t>(terms.size()) - qu.toff;
        } else {
            qu.kind = 0;
            qu.n = 0;
        }
        queries.push_back(qu);
        p = nl + 1;
    }
    need_pos.resize(table.size() + 1, 0);
    for (const Query& qu : queries) {
        if (qu.kind != 1) continue;
        for (uint32_t i = 0; i < qu.n; ++i) {
            uint32_t t = terms[qu.toff + i];
            if (t < need_pos.size()) need_pos[t] = 1;
        }
    }
    return static_cast<uint32_t>(table.size());
}

// ------------------------------------------------------------------- index

// One term's sorted posting list, borrowed from the CSR arrays.
struct List {
    const uint32_t* p = nullptr;
    uint32_t n = 0;
    bool empty() const { return n == 0; }
};

inline bool same_list(List a, List b) { return a.p == b.p && a.n == b.n; }

const uint32_t kZero = 0;
const List kEmptyList{&kZero, 0};

struct Index {
    std::unique_ptr<uint32_t[]> post_data;
    std::unique_ptr<uint32_t[]> pos_data;
    std::unique_ptr<uint32_t[]> pstart_data;
    std::vector<uint32_t> post_off;    // nterms + 1
    std::vector<uint32_t> pos_off;     // nterms + 1
    std::vector<uint32_t> pstart_off;  // nterms + 1

    List list(uint32_t t) const {
        return List{post_data.get() + post_off[t], post_off[t + 1] - post_off[t]};
    }
    // Position slice of posting i of term t.
    void positions(uint32_t t, uint32_t i, const uint32_t*& p, uint32_t& n) const {
        const uint32_t* ps = pstart_data.get() + pstart_off[t] + i;
        p = pos_data.get() + ps[0];
        n = ps[1] - ps[0];
    }
};

struct Chunk {
    const char* b = nullptr;
    const char* e = nullptr;
    uint32_t d0 = 0;
};

uint32_t parse_id_at(const char* p, const char* e) {
    uint32_t v = 0;
    while (p < e && *p >= '0' && *p <= '9') {
        v = v * 10 + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    return v;
}

// Split the corpus on line boundaries, roughly equally by bytes.  Document ids
// are dense, so the id printed at a chunk's first line is its starting id.
void make_chunks(const char* data, size_t size, unsigned nchunks, std::vector<Chunk>& chunks) {
    const char* end = data ? data + size : data;
    std::vector<const char*> bounds(nchunks + 1);
    bounds[0] = data;
    bounds[nchunks] = end;
    for (unsigned k = 1; k < nchunks; ++k) {
        if (!data) {
            bounds[k] = end;
            continue;
        }
        const char* target = data + size * k / nchunks;
        const char* nl = static_cast<const char*>(std::memchr(target, '\n', static_cast<size_t>(end - target)));
        bounds[k] = nl ? nl + 1 : end;
    }
    chunks.resize(nchunks);
    for (unsigned k = 0; k < nchunks; ++k) {
        chunks[k].b = bounds[k];
        chunks[k].e = bounds[k + 1] < bounds[k] ? bounds[k] : bounds[k + 1];
        chunks[k].d0 = (bounds[k] < end) ? parse_id_at(bounds[k], end) : 0;
    }
}

// Feed (term id, document id, token position) for every corpus token that is
// one of the query terms.  Every token still advances the position counter.
// on_doc runs once per document so callers can record document boundaries.
// A term longer than 32 bytes needs the slow path, but a whole run longer than
// 32 bytes is not a plausible query term, so the branch stays out of the fast
// loop.
template <typename D, typename F>
void scan_chunk(const Chunk& c, const TermTable& table, bool long_terms, D&& on_doc, F&& on_token) {
    const char* p = c.b;
    const char* const e = c.e;
    uint32_t doc = c.d0;
    while (p < e) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(e - p)));
        if (!nl) nl = e;
        on_doc(doc);
        const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(nl - p)));
        const char* q = tab ? tab + 1 : nl;
        uint32_t pos = 0;
        while (q < nl) {
            unsigned char ch = static_cast<unsigned char>(*q);
            unsigned char lc = g_tok[ch];
            if (!lc) {
                ++q;
                continue;
            }
            const char* ts = q;
            char buf[kMaxTermLen];
            uint32_t n = 0;
            do {
                if (n < kMaxTermLen) buf[n] = static_cast<char>(lc);
                ++n;
                ++q;
                if (q < nl) {
                    ch = static_cast<unsigned char>(*q);
                    lc = g_tok[ch];
                } else {
                    lc = 0;
                }
            } while (lc);
            if (n <= kMaxTermLen) {
                uint64_t h = hash_bytes(buf, n);
                uint32_t tid = table.find_h(buf, n, h);
                if (tid != kNoTerm) on_token(tid, doc, pos);
            } else if (long_terms) {
                thread_local std::string long_tok;
                long_tok.resize(n);
                for (uint32_t i = 0; i < n; ++i) long_tok[i] = static_cast<char>(g_tok[static_cast<unsigned char>(ts[i])]);
                uint64_t h = hash_bytes(long_tok.data(), n);
                uint32_t tid = table.find_h(long_tok.data(), n, h);
                if (tid != kNoTerm) on_token(tid, doc, pos);
            }
            ++pos;
        }
        ++doc;
        p = nl < e ? nl + 1 : e;
    }
}

void build_index(const char* data, size_t size, const TermTable& table, const std::vector<uint8_t>& need_pos,
                 unsigned nthreads, Index& idx) {
    const uint32_t nterms = table.size();
    const uint32_t N = nterms + 1;  // + sentinel for absent terms
    const bool long_terms = table.max_len() > kMaxTermLen;
    idx.post_off.resize(N + 1);
    idx.pos_off.resize(N + 1);
    idx.pstart_off.resize(N + 1);

    std::vector<Chunk> chunks;
    make_chunks(data, size, nthreads, chunks);

    // One private counter row per thread, so pass 1 needs no atomics.
    std::vector<uint32_t> cnt_df(static_cast<size_t>(nthreads) * N, 0);
    std::vector<uint32_t> cnt_occ(static_cast<size_t>(nthreads) * N, 0);
    std::vector<uint32_t> last(static_cast<size_t>(nthreads) * N, kNoTerm);

    // Pass 1 tokenizes the corpus once and records, per thread, one packed
    // (term id, position) word per query-term occurrence.  A word whose high
    // half is kNoTerm carries the document id and starts a new document.
    // Pass 2 replays those words to scatter the CSR arrays, so the corpus is
    // never tokenized twice.
    const uint64_t kDocMark = static_cast<uint64_t>(kNoTerm) << 32;
    std::vector<std::vector<uint64_t>> streams(nthreads);

    {
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (unsigned u = 0; u < nthreads; ++u) {
            threads.emplace_back([&, u]() {
                uint32_t* cdf = cnt_df.data() + static_cast<size_t>(u) * N;
                uint32_t* cocc = cnt_occ.data() + static_cast<size_t>(u) * N;
                uint32_t* clast = last.data() + static_cast<size_t>(u) * N;
                uint32_t localmax = 0;
                std::vector<uint64_t>& stream = streams[u];
                stream.reserve((static_cast<size_t>(chunks[u].e - chunks[u].b) >> 3) + 64);
                scan_chunk(
                    chunks[u], table, long_terms,
                    [&](uint32_t doc) { stream.push_back(kDocMark | doc); },
                    [&](uint32_t tid, uint32_t doc, uint32_t pos) {
                        if (clast[tid] != doc) {
                            clast[tid] = doc;
                            ++cdf[tid];
                        }
                        if (need_pos[tid]) ++cocc[tid];
                        stream.push_back((static_cast<uint64_t>(tid) << 32) | pos);
                        if (pos > localmax) localmax = pos;
                    });
            });
        }
        for (auto& th : threads) th.join();
    }

    std::vector<uint32_t> df(N, 0);
    std::vector<uint32_t> occ(N, 0);
    for (uint32_t t = 0; t < N; ++t) {
        uint32_t d = 0;
        uint32_t o = 0;
        for (unsigned u = 0; u < nthreads; ++u) {
            d += cnt_df[static_cast<size_t>(u) * N + t];
            o += cnt_occ[static_cast<size_t>(u) * N + t];
        }
        df[t] = d;
        occ[t] = o;
    }

    uint64_t total_post = 0, total_pos = 0, total_pstart = 0;
    for (uint32_t t = 0; t < N; ++t) {
        idx.post_off[t] = static_cast<uint32_t>(total_post);
        idx.pos_off[t] = static_cast<uint32_t>(total_pos);
        idx.pstart_off[t] = static_cast<uint32_t>(total_pstart);
        total_post += df[t];
        total_pos += need_pos[t] ? occ[t] : 0;
        total_pstart += need_pos[t] ? static_cast<uint64_t>(df[t]) + 1 : 0;
    }
    idx.post_off[N] = static_cast<uint32_t>(total_post);
    idx.pos_off[N] = static_cast<uint32_t>(total_pos);
    idx.pstart_off[N] = static_cast<uint32_t>(total_pstart);

    // Turn per-thread counts into per-thread write offsets, in place.
    for (uint32_t t = 0; t < N; ++t) {
        uint32_t rp = idx.post_off[t];
        uint32_t ro = idx.pos_off[t];
        for (unsigned u = 0; u < nthreads; ++u) {
            size_t k = static_cast<size_t>(u) * N + t;
            uint32_t d = cnt_df[k];
            uint32_t o = cnt_occ[k];
            cnt_df[k] = rp;
            cnt_occ[k] = ro;
            rp += d;
            ro += o;
        }
    }

    idx.post_data.reset(new uint32_t[total_post ? total_post : 1]);
    idx.pos_data.reset(new uint32_t[total_pos ? total_pos : 1]);
    idx.pstart_data.reset(new uint32_t[total_pstart ? total_pstart : 1]);

    std::fill(last.begin(), last.end(), kNoTerm);

    {
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (unsigned u = 0; u < nthreads; ++u) {
            threads.emplace_back([&, u]() {
                uint32_t* sp = cnt_df.data() + static_cast<size_t>(u) * N;
                uint32_t* pp = cnt_occ.data() + static_cast<size_t>(u) * N;
                uint32_t* clast = last.data() + static_cast<size_t>(u) * N;
                uint32_t* post = idx.post_data.get();
                uint32_t* posd = idx.pos_data.get();
                uint32_t* pst = idx.pstart_data.get();
                const uint32_t* poff = idx.post_off.data();
                const uint32_t* psoff = idx.pstart_off.data();
                uint32_t doc = kNoTerm;
                for (uint64_t rec : streams[u]) {
                    uint32_t tid = static_cast<uint32_t>(rec >> 32);
                    if (tid == kNoTerm) {
                        doc = static_cast<uint32_t>(rec);
                        continue;
                    }
                    if (clast[tid] != doc) {
                        clast[tid] = doc;
                        uint32_t o = sp[tid]++;
                        post[o] = doc;
                        if (need_pos[tid]) pst[psoff[tid] + (o - poff[tid])] = pp[tid];
                    }
                    if (need_pos[tid]) posd[pp[tid]++] = static_cast<uint32_t>(rec);
                }
            });
        }
        for (auto& th : threads) th.join();
    }

    for (uint32_t t = 0; t < N; ++t) {
        if (need_pos[t]) idx.pstart_data.get()[idx.pstart_off[t] + df[t]] = idx.pos_off[t + 1];
    }

}

// --------------------------------------------------------------- set algebra

// True if any of the eight sorted values at p equals v.  Used to compare one
// element of the driving list against a block of the other list at a time.
inline bool any_eq8(const uint32_t* p, uint32_t v) {
#if defined(__aarch64__)
    uint32x4_t m0 = vceqq_u32(vld1q_u32(p), vdupq_n_u32(v));
    uint32x4_t m1 = vceqq_u32(vld1q_u32(p + 4), vdupq_n_u32(v));
    return vmaxvq_u32(vorrq_u32(m0, m1)) != 0;
#elif defined(__SSE2__)
    __m128i vv = _mm_set1_epi32(static_cast<int>(v));
    __m128i m = _mm_or_si128(
        _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), vv),
        _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 4)), vv));
    return _mm_movemask_epi8(m) != 0;
#else
    for (int k = 0; k < 8; ++k)
        if (p[k] == v) return true;
    return false;
#endif
}

// Sorted intersection.  The smaller side drives; a large size gap uses
// binary search per element instead of a full merge.
void intersect_into(List a, List b, std::vector<uint32_t>& out) {
    out.clear();
    if (a.n > b.n) std::swap(a, b);
    out.reserve(a.n);
    if (a.n == 0) return;
    if (static_cast<uint64_t>(a.n) * 32 < b.n) {
        uint32_t j = 0;
        for (uint32_t i = 0; i < a.n; ++i) {
            uint32_t x = a.p[i];
            const uint32_t* base = b.p + j;
            const uint32_t* end = b.p + b.n;
            const uint32_t* hit = std::lower_bound(base, end, x);
            if (hit != end && *hit == x) {
                out.push_back(x);
                j = static_cast<uint32_t>(hit - b.p) + 1;
                if (j >= b.n) break;
            } else {
                j = static_cast<uint32_t>(hit - b.p);
                if (j >= b.n) break;
            }
        }
        return;
    }
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < a.n && j + 8 <= b.n) {
        uint32_t x = a.p[i];
        uint32_t last = b.p[j + 7];
        if (x > last) {
            j += 8;
            continue;
        }
        if (any_eq8(b.p + j, x)) out.push_back(x);
        if (x == last) j += 8;
        ++i;
    }
    while (i < a.n && j < b.n) {
        uint32_t x = a.p[i];
        uint32_t y = b.p[j];
        if (x == y) {
            out.push_back(x);
            ++i;
            ++j;
        } else if (x < y) {
            ++i;
        } else {
            ++j;
        }
    }
}

void union_into(List a, List b, std::vector<uint32_t>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(a.n) + b.n);
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < a.n && j < b.n) {
        uint32_t x = a.p[i];
        uint32_t y = b.p[j];
        if (x < y) {
            out.push_back(x);
            ++i;
        } else if (y < x) {
            out.push_back(y);
            ++j;
        } else {
            out.push_back(x);
            ++i;
            ++j;
        }
    }
    while (i < a.n) out.push_back(a.p[i++]);
    while (j < b.n) out.push_back(b.p[j++]);
}

void difference_into(List a, List b, std::vector<uint32_t>& out) {
    out.clear();
    if (a.n == 0) return;
    out.reserve(a.n);
    if (b.n == 0) {
        out.assign(a.p, a.p + a.n);
        return;
    }
    if (static_cast<uint64_t>(a.n) * 32 < b.n) {
        uint32_t j = 0;
        for (uint32_t i = 0; i < a.n; ++i) {
            uint32_t x = a.p[i];
            const uint32_t* hit = std::lower_bound(b.p + j, b.p + b.n, x);
            if (hit == b.p + b.n || *hit != x) out.push_back(x);
            j = static_cast<uint32_t>(hit - b.p);
            if (j >= b.n) {
                out.insert(out.end(), a.p + i + 1, a.p + a.n);
                break;
            }
        }
        return;
    }
    uint32_t i = 0;
    uint32_t j = 0;
    for (; i < a.n; ++i) {
        uint32_t x = a.p[i];
        while (j < b.n && b.p[j] < x) ++j;
        if (j == b.n) {
            out.insert(out.end(), a.p + i, a.p + a.n);
            break;
        }
        if (b.p[j] != x) out.push_back(x);
    }
}

// ------------------------------------------------------------ query workers

struct ThreadCtx {
    std::vector<uint32_t> buf_a;
    std::vector<uint32_t> buf_b;
    std::vector<uint32_t> cand_a;
    std::vector<uint32_t> cand_b;
    std::string out;
};


void format_result(List r, std::string& out) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r.n; ++i) sum += r.p[i];
    append_uint(out, r.n);
    out.push_back(' ');
    append_uint(out, sum);
    uint32_t m = std::min<uint32_t>(10, r.n);
    for (uint32_t i = 0; i < m; ++i) {
        out.push_back(' ');
        append_uint(out, r.p[i]);
    }
    out.push_back('\n');
}

void run_bool(const Query& qu, const std::vector<uint32_t>& terms, const std::vector<uint8_t>& ops,
              const Index& idx, ThreadCtx& ctx, std::string& out) {
    List cur = idx.list(terms[qu.toff]);
    std::vector<uint32_t>* w = &ctx.buf_a;
    std::vector<uint32_t>* w2 = &ctx.buf_b;
    uint32_t k = 1;
    while (k < qu.n) {
        uint8_t op = ops[qu.ooff + k - 1];
        if (op == OP_NOT) {
            List rhs = idx.list(terms[qu.toff + k]);
            ++k;
            if (cur.empty() || rhs.empty()) continue;
            if (same_list(cur, rhs)) {
                cur = kEmptyList;
                continue;
            }
            difference_into(cur, rhs, *w);
            cur = List{w->data(), static_cast<uint32_t>(w->size())};
            std::swap(w, w2);
            continue;
        }
        // A maximal run of one associative operator can be evaluated in any
        // order, so start from the smallest posting lists.
        List operands[16];
        uint32_t m = 0;
        operands[m++] = cur;
        while (k < qu.n && ops[qu.ooff + k - 1] == op) {
            operands[m++] = idx.list(terms[qu.toff + k]);
            ++k;
        }
        for (uint32_t i = 1; i < m; ++i) {
            List v = operands[i];
            uint32_t j = i;
            while (j > 0 && operands[j - 1].n > v.n) {
                operands[j] = operands[j - 1];
                --j;
            }
            operands[j] = v;
        }
        // A buffered accumulator is evaluated no later than the second step, so
        // the output rotation cannot overwrite it while it is still an operand.
        if (cur.n != 0 && (cur.p == ctx.buf_a.data() || cur.p == ctx.buf_b.data())) {
            uint32_t p = 0;
            while (p < m && operands[p].p != cur.p) ++p;
            if (p >= 2) std::swap(operands[1], operands[p]);
        }
        cur = operands[0];
        if (op == OP_AND) {
            for (uint32_t i = 1; i < m && !cur.empty(); ++i) {
                List rhs = operands[i];
                if (rhs.empty() || same_list(cur, rhs)) continue;
                intersect_into(cur, rhs, *w);
                cur = List{w->data(), static_cast<uint32_t>(w->size())};
                std::swap(w, w2);
            }
        } else {
            for (uint32_t i = 1; i < m; ++i) {
                List rhs = operands[i];
                if (rhs.empty() || same_list(cur, rhs)) continue;
                if (cur.empty()) {
                    cur = rhs;
                    continue;
                }
                union_into(cur, rhs, *w);
                cur = List{w->data(), static_cast<uint32_t>(w->size())};
                std::swap(w, w2);
            }
        }
    }
    format_result(cur, out);
}

void run_phrase(const Query& qu, const std::vector<uint32_t>& terms, const Index& idx, ThreadCtx& ctx,
                std::string& out) {
    uint32_t n = qu.n;
    if (n == 0) {
        format_result(kEmptyList, out);
        return;
    }
    const uint32_t* qt = terms.data() + qu.toff;
    List ql[16];
    uint32_t order[16];
    uint32_t j = 0;
    uint32_t best = UINT32_MAX;
    for (uint32_t o = 0; o < n; ++o) {
        order[o] = o;
        ql[o] = idx.list(qt[o]);
        if (ql[o].empty()) {
            format_result(kEmptyList, out);
            return;
        }
        if (ql[o].n < best) {
            best = ql[o].n;
            j = o;
        }
    }
    // Intersect in increasing posting-list size order.
    for (uint32_t i = 1; i < n; ++i) {
        uint32_t key = order[i];
        uint32_t ks = ql[key].n;
        uint32_t m = i;
        while (m > 0 && ql[order[m - 1]].n > ks) {
            order[m] = order[m - 1];
            --m;
        }
        order[m] = key;
    }

    List cur = ql[j];
    std::vector<uint32_t>* w = &ctx.cand_a;
    std::vector<uint32_t>* w2 = &ctx.cand_b;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t o = order[i];
        if (o == j) continue;
        intersect_into(cur, ql[o], *w);
        cur = List{w->data(), static_cast<uint32_t>(w->size())};
        std::swap(w, w2);
        if (cur.empty()) break;
    }
    if (cur.empty()) {
        format_result(kEmptyList, out);
        return;
    }

    // Positional verification.
    const uint32_t* sl[16];
    uint32_t sln[16];
    std::vector<uint32_t> matches;
    matches.reserve(cur.n);
    for (uint32_t ci = 0; ci < cur.n; ++ci) {
        uint32_t doc = cur.p[ci];
        bool present = true;
        for (uint32_t o = 0; o < n; ++o) {
            const uint32_t* end = ql[o].p + ql[o].n;
            const uint32_t* hit = std::lower_bound(ql[o].p, end, doc);
            if (hit == end || *hit != doc) {
                present = false;
                break;
            }
            idx.positions(qt[o], static_cast<uint32_t>(hit - ql[o].p), sl[o], sln[o]);
        }
        if (!present) continue;
        bool found = false;
        for (uint32_t pi = 0; pi < sln[j]; ++pi) {
            uint32_t p = sl[j][pi];
            if (p < j) continue;
            uint32_t start = p - j;
            bool ok = true;
            for (uint32_t o = 0; o < n; ++o) {
                if (o == j) continue;
                uint32_t need = start + o;
                if (!std::binary_search(sl[o], sl[o] + sln[o], need)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                found = true;
                break;
            }
        }
        if (found) matches.push_back(doc);
    }
    format_result(List{matches.data(), static_cast<uint32_t>(matches.size())}, out);
}

void worker(const std::vector<Query>& queries, const std::vector<uint32_t>& terms,
            const std::vector<uint8_t>& ops, const Index& idx, size_t lo, size_t hi, ThreadCtx& ctx) {
    ctx.out.clear();
    ctx.out.reserve((hi - lo) * 40 + 64);
    for (size_t qi = lo; qi < hi; ++qi) {
        const Query& qu = queries[qi];
        if (qu.kind == 0)
            run_bool(qu, terms, ops, idx, ctx, ctx.out);
        else
            run_phrase(qu, terms, idx, ctx, ctx.out);
    }
}

unsigned clamp_threads(const char* env_name, unsigned def, unsigned maxv) {
    unsigned v = def;
    if (const char* env = std::getenv(env_name)) {
        unsigned e = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        if (e > 0) v = e;
    }
    return std::max(1u, std::min(v, maxv));
}

// The harness pins a cpuset; hardware_concurrency() reports host CPUs, so
// count the affinity mask instead where the platform exposes one.
unsigned available_cpus() {
#if defined(__linux__)
    cpu_set_t set;
    if (::sched_getaffinity(0, sizeof(set), &set) == 0) {
        unsigned n = 0;
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) ++n;
        if (n) return n;
    }
#endif
    unsigned hw = std::thread::hardware_concurrency();
    return hw ? hw : 1;
}

}  // namespace

int main(int argc, char** argv) {

    std::string data_dir = argc > 1 ? argv[1] : "/data";
    std::string out_dir = argc > 2 ? argv[2] : "/out";

    init_token_table();

    std::string qb;
    if (!read_whole_file(data_dir + "/queries.txt", qb)) {
        std::fprintf(stderr, "cannot open %s/queries.txt\n", data_dir.c_str());
        return 1;
    }

    TermTable table;
    std::vector<Query> queries;
    std::vector<uint32_t> terms;
    std::vector<uint8_t> ops;
    std::vector<uint8_t> need_pos;
    uint32_t nterms = parse_queries(qb, table, queries, terms, ops, need_pos);
    need_pos.resize(nterms + 1, 0);
    qb.clear();
    qb.shrink_to_fit();


    MappedFile docs_file;
    if (!docs_file.open(data_dir + "/docs.txt")) {
        std::fprintf(stderr, "cannot open %s/docs.txt\n", data_dir.c_str());
        return 1;
    }

    unsigned hw = available_cpus();
    unsigned build_threads = clamp_threads("SD_BUILD_THREADS", std::min(hw, 12u), 64u);

    Index idx;
    build_index(docs_file.data, docs_file.size, table, need_pos, build_threads, idx);

    unsigned nthreads = clamp_threads("SD_THREADS", std::min(hw, 12u), 64u);
    if (nthreads > queries.size()) nthreads = static_cast<unsigned>(queries.size());
    if (nthreads == 0) nthreads = 1;

    std::vector<ThreadCtx> ctxs(nthreads);
    std::vector<std::thread> threads;
    threads.reserve(nthreads);
    size_t nq = queries.size();
    for (unsigned t = 0; t < nthreads; ++t) {
        size_t lo = nq * t / nthreads;
        size_t hi = nq * (t + 1) / nthreads;
        threads.emplace_back([&, t, lo, hi]() { worker(queries, terms, ops, idx, lo, hi, ctxs[t]); });
    }
    for (auto& th : threads) th.join();

    std::string path = out_dir + "/results.txt";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot create %s\n", path.c_str());
        return 1;
    }
    for (unsigned t = 0; t < nthreads; ++t) {
        const std::string& s = ctxs[t].out;
        if (!s.empty()) std::fwrite(s.data(), 1, s.size(), f);
    }
    if (std::fclose(f) != 0) return 1;
    _exit(0);

    return 0;
}
