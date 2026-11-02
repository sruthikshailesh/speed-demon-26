// Speed Demon 2026 - Track B (Index)
// High-Performance Parallel Inverted Index & Query Engine
// Author: Speed Demon Performance Engineering Team
//
// Complies with specs/b-index.md:
//   * Token: maximal run of ASCII [a-z0-9] after ASCII lowercasing; every
//     other byte (including >= 0x80) is a separator.
//   * BOOL applies AND/OR/NOT strictly left to right; NOT is binary.
//   * PHRASE requires consecutive token positions within a single document.
//   * Output: <count> <sum_of_ids> [<up to 10 smallest doc ids>]\n

#include <algorithm>
#include <atomic>
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

uint8_t g_tok[256];

void init_token_table() {
    for (int i = 0; i < 256; ++i) {
        unsigned c = static_cast<unsigned>(i);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        g_tok[i] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? static_cast<uint8_t>(c) : 0;
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
        h = ((h ^ k) << 27) | ((h ^ k) >> 37);
        h *= 0x9e3779b97f4a7c15ULL;
        s += 8;
        n -= 8;
    }
    if (n) {
        uint64_t k = 0;
        std::memcpy(&k, s, n);
        h = (h ^ k) * 0x9e3779b97f4a7c15ULL;
    }
    return mix64(h);
}

template <typename U>
inline char* write_uint(char* p, U v) {
    char b[32];
    int n = 0;
    do {
        b[n++] = static_cast<char>('0' + static_cast<char>(v % 10));
        v /= 10;
    } while (v);
    while (n) *p++ = b[--n];
    return p;
}

// Compact 16-byte Slot structure for TermTable (exactly 4 slots per 64-byte cache line)
class TermTable {
public:
    struct alignas(16) Slot {
        uint64_t h;
        uint32_t id;
        uint32_t off; // upper 8 bits: len, lower 24 bits: arena offset
    };

    TermTable() { reset(1u << 12); }

    uint32_t size() const { return count_; }
    uint32_t min_len() const { return min_len_; }
    uint32_t max_len() const { return max_len_; }

    inline uint32_t find_h(const char* s, uint32_t n, uint64_t h) const {
        uint32_t i = static_cast<uint32_t>(h) & mask_;
        for (;;) {
            const Slot& sl = slots_[i];
            if (!sl.h) return kNoTerm;
            if (sl.h == h && (sl.off >> 24) == n) {
                if (std::memcmp(arena_.data() + (sl.off & 0x00FFFFFFu), s, n) == 0)
                    return sl.id;
            }
            i = (i + 1) & mask_;
        }
    }

    uint32_t insert(const char* s, uint32_t n) {
        if ((static_cast<size_t>(count_) + 1) * 2 > slots_.size()) grow();
        uint64_t h = hash_bytes(s, n);
        if (!h) h = 1;
        uint32_t i = static_cast<uint32_t>(h) & mask_;
        for (;;) {
            Slot& sl = slots_[i];
            if (!sl.h) {
                sl.h = h;
                sl.id = count_;
                sl.off = (n << 24) | (static_cast<uint32_t>(arena_.size()) & 0x00FFFFFFu);
                arena_.insert(arena_.end(), s, s + n);
                if (n < min_len_) min_len_ = n;
                if (n > max_len_) max_len_ = n;
                return count_++;
            }
            if (sl.h == h && (sl.off >> 24) == n) {
                if (std::memcmp(arena_.data() + (sl.off & 0x00FFFFFFu), s, n) == 0)
                    return sl.id;
            }
            i = (i + 1) & mask_;
        }
    }

private:
    void reset(size_t n) {
        slots_.assign(n, Slot{0, 0, 0});
        mask_ = static_cast<uint32_t>(n - 1);
        count_ = 0;
        arena_.reserve(4u << 20);
        min_len_ = UINT32_MAX;
        max_len_ = 0;
    }

    void grow() {
        std::vector<Slot> ns(slots_.size() * 2, Slot{0, 0, 0});
        uint32_t nmask = static_cast<uint32_t>(ns.size() - 1);
        for (const Slot& sl : slots_) {
            if (!sl.h) continue;
            uint32_t i = static_cast<uint32_t>(sl.h) & nmask;
            while (ns[i].h) i = (i + 1) & nmask;
            ns[i] = sl;
        }
        slots_.swap(ns);
        mask_ = nmask;
    }

    std::vector<Slot> slots_;
    std::vector<char> arena_;
    uint32_t mask_ = 0;
    uint32_t count_ = 0;
    uint32_t min_len_ = UINT32_MAX;
    uint32_t max_len_ = 0;
};

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

struct Query {
    uint32_t kind;  // 0 = BOOL, 1 = PHRASE
    uint32_t toff;  // first term id in terms[]
    uint32_t ooff;  // first op code in ops[]
    uint32_t n;     // term count
};

enum : uint8_t { OP_AND = 0, OP_OR = 1, OP_NOT = 2 };

inline uint32_t insert_normalized_token(const char* s, uint32_t n, TermTable& table) {
    char buf[128];
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t c = g_tok[static_cast<unsigned char>(s[i])];
        if (c) {
            if (len < sizeof(buf)) buf[len] = static_cast<char>(c);
            ++len;
        }
    }
    if (len == 0) return kNoTerm;
    if (len <= sizeof(buf)) return table.insert(buf, len);
    std::string long_tok;
    long_tok.reserve(len);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t c = g_tok[static_cast<unsigned char>(s[i])];
        if (c) long_tok.push_back(static_cast<char>(c));
    }
    return table.insert(long_tok.data(), static_cast<uint32_t>(long_tok.size()));
}

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
                            terms.push_back(insert_normalized_token(q, n, table));
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
                while (q < end && !g_tok[static_cast<unsigned char>(*q)]) ++q;
                const char* s = q;
                while (q < end && g_tok[static_cast<unsigned char>(*q)]) ++q;
                if (q > s) {
                    terms.push_back(insert_normalized_token(s, static_cast<uint32_t>(q - s), table));
                }
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
            if (t != kNoTerm && t < need_pos.size()) need_pos[t] = 1;
        }
    }
    return static_cast<uint32_t>(table.size());
}

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
    std::vector<uint32_t> post_off;
    std::vector<uint32_t> pos_off;
    std::vector<uint32_t> pstart_off;

    inline List list(uint32_t t) const {
        if (t == kNoTerm) return kEmptyList;
        return List{post_data.get() + post_off[t], post_off[t + 1] - post_off[t]};
    }
    inline void positions(uint32_t t, uint32_t i, const uint32_t*& p, uint32_t& n) const {
        if (t == kNoTerm) { p = &kZero; n = 0; return; }
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

void make_chunks(const char* data, size_t size, unsigned nchunks, std::vector<Chunk>& chunks) {
    chunks.resize(nchunks);
    if (!data || size == 0) {
        for (unsigned k = 0; k < nchunks; ++k) {
            chunks[k].b = data;
            chunks[k].e = data;
            chunks[k].d0 = 0;
        }
        return;
    }
    const char* end = data + size;
    std::vector<const char*> bounds(nchunks + 1);
    bounds[0] = data;
    bounds[nchunks] = end;

    for (unsigned k = 1; k < nchunks; ++k) {
        const char* target = data + (size * k) / nchunks;
        if (target == data || *(target - 1) == '\n') {
            bounds[k] = target;
        } else {
            const char* nl = static_cast<const char*>(std::memchr(target, '\n', static_cast<size_t>(end - target)));
            bounds[k] = nl ? nl + 1 : end;
        }
    }
    for (unsigned k = 1; k <= nchunks; ++k) {
        if (bounds[k] < bounds[k - 1]) bounds[k] = bounds[k - 1];
    }
    for (unsigned k = 0; k < nchunks; ++k) {
        chunks[k].b = bounds[k];
        chunks[k].e = bounds[k + 1];
        chunks[k].d0 = (bounds[k] < end) ? parse_id_at(bounds[k], end) : 0;
    }
}

template <typename D, typename F>
inline void scan_chunk(const Chunk& c, const TermTable& table, bool long_terms,
                       uint32_t min_tlen, uint32_t max_tlen, D&& on_doc, F&& on_token) {
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

            if (n >= min_tlen && n <= max_tlen) {
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
    const uint32_t N = nterms + 1;
    const bool long_terms = table.max_len() > kMaxTermLen;
    const uint32_t min_tlen = table.min_len();
    const uint32_t max_tlen = table.max_len();

    idx.post_off.resize(N + 1);
    idx.pos_off.resize(N + 1);
    idx.pstart_off.resize(N + 1);

    std::vector<Chunk> chunks;
    make_chunks(data, size, nthreads, chunks);

    // Pass 1: exact counts without any intermediate streams
    std::vector<uint32_t> cnt_df(static_cast<size_t>(nthreads) * N, 0);
    std::vector<uint32_t> cnt_occ(static_cast<size_t>(nthreads) * N, 0);
    std::vector<uint32_t> last(static_cast<size_t>(nthreads) * N, kNoTerm);

    {
        std::vector<std::thread> threads;
        threads.reserve(nthreads);
        for (unsigned u = 0; u < nthreads; ++u) {
            threads.emplace_back([&, u]() {
                uint32_t* cdf = cnt_df.data() + static_cast<size_t>(u) * N;
                uint32_t* cocc = cnt_occ.data() + static_cast<size_t>(u) * N;
                uint32_t* clast = last.data() + static_cast<size_t>(u) * N;
                scan_chunk(
                    chunks[u], table, long_terms, min_tlen, max_tlen,
                    [&](<uint32_t /*doc*/>) {},
                    [&](<uint32_t tid, uint32_t doc, uint32_t /*pos*/>) {
                        if (clast[tid] != doc) {
                            clast[tid] = doc;
                            ++cdf[tid];
                        }
                        if (need_pos[tid]) ++cocc[tid];
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

    // Pass 2: fill CSR arrays directly without any intermediate memory
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
                scan_chunk(
                    chunks[u], table, long_terms, min_tlen, max_tlen,
                    [&](<uint32_t /*doc*/>) {},
                    [&](<uint32_t tid, uint32_t doc, uint32_t pos>) {
                        if (clast[tid] != doc) {
                            clast[tid] = doc;
                            uint32_t o = sp[tid]++;
                            post[o] = doc;
                            if (need_pos[tid]) pst[psoff[tid] + (o - poff[tid])] = pp[tid];
                        }
                        if (need_pos[tid]) posd[pp[tid]++] = pos;
                    });
            });
        }
        for (auto& th : threads) th.join();
    }

    for (uint32_t t = 0; t < N; ++t) {
        if (need_pos[t]) idx.pstart_data.get()[idx.pstart_off[t] + df[t]] = idx.pos_off[t + 1];
    }
}

// Galloping lower bound search
inline uint32_t gallop_lower_bound(const uint32_t* p, uint32_t n, uint32_t val) {
    if (n == 0 || p[0] >= val) return 0;
    if (p[n - 1] < val) return n;

    uint32_t low = 0;
    uint32_t step = 1;
    while (low + step < n && p[low + step] < val) {
        low += step;
        step <<= 1;
    }
    uint32_t high = std::min(n, low + step + 1);

    while (low < high) {
        uint32_t mid = low + ((high - low) >> 1);
        if (p[mid] < val)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

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

void intersect_into(List a, List b, std::vector<uint32_t>& out) {
    out.clear();
    if (a.n > b.n) std::swap(a, b);
    if (a.n == 0) return;
    out.reserve(a.n);

    if (static_cast<uint64_t>(a.n) * 8 <= b.n) {
        uint32_t j = 0;
        for (uint32_t i = 0; i < a.n; ++i) {
            uint32_t x = a.p[i];
            uint32_t rem = b.n - j;
            uint32_t offset = gallop_lower_bound(b.p + j, rem, x);
            j += offset;
            if (j >= b.n) break;
            if (b.p[j] == x) {
                out.push_back(x);
                ++j;
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
    if (static_cast<uint64_t>(a.n) * 8 <= b.n) {
        uint32_t j = 0;
        for (uint32_t i = 0; i < a.n; ++i) {
            uint32_t x = a.p[i];
            uint32_t rem = b.n - j;
            uint32_t offset = gallop_lower_bound(b.p + j, rem, x);
            j += offset;
            if (j >= b.n) {
                out.insert(out.end(), a.p + i, a.p + a.n);
                break;
            }
            if (b.p[j] != x) {
                out.push_back(x);
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

struct ThreadCtx {
    std::vector<uint32_t> buf_a;
    std::vector<uint32_t> buf_b;
    std::vector<uint32_t> cand_a;
    std::vector<uint32_t> cand_b;

    std::vector<List> operands;
    std::vector<List> ql;
    std::vector<uint32_t> order;
    std::vector<const uint32_t*> sl;
    std::vector<uint32_t> sln;
    std::vector<uint32_t> cursors;
    std::vector<uint32_t> matches;
};

inline void format_result_fast(List r, std::string& out) {
    char buf[256];
    char* p = buf;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r.n; ++i) sum += r.p[i];
    p = write_uint(p, r.n);
    *p++ = ' ';
    p = write_uint(p, sum);
    uint32_t m = std::min<uint32_t>(10, r.n);
    for (uint32_t i = 0; i < m; ++i) {
        *p++ = ' ';
        p = write_uint(p, r.p[i]);
    }
    *p++ = '\n';
    out.append(buf, static_cast<size_t>(p - buf));
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
            if (cur.empty()) continue;
            if (rhs.empty()) continue;
            if (same_list(cur, rhs)) {
                cur = kEmptyList;
                continue;
            }
            difference_into(cur, rhs, *w);
            cur = List{w->data(), static_cast<uint32_t>(w->size())};
            std::swap(w, w2);
            continue;
        }

        ctx.operands.clear();
        ctx.operands.push_back(cur);
        while (k < qu.n && ops[qu.ooff + k - 1] == op) {
            ctx.operands.push_back(idx.list(terms[qu.toff + k]));
            ++k;
        }
        uint32_t m = static_cast<uint32_t>(ctx.operands.size());
        for (uint32_t i = 1; i < m; ++i) {
            List v = ctx.operands[i];
            uint32_t j = i;
            while (j > 0 && ctx.operands[j - 1].n > v.n) {
                ctx.operands[j] = ctx.operands[j - 1];
                --j;
            }
            ctx.operands[j] = v;
        }

        if (cur.n != 0 && (cur.p == ctx.buf_a.data() || cur.p == ctx.buf_b.data())) {
            uint32_t p = 0;
            while (p < m && ctx.operands[p].p != cur.p) ++p;
            if (p >= 2) std::swap(ctx.operands[1], ctx.operands[p]);
        }
        cur = ctx.operands[0];
        if (op == OP_AND) {
            for (uint32_t i = 1; i < m; ++i) {
                if (cur.empty()) break;
                List rhs = ctx.operands[i];
                if (rhs.empty()) {
                    cur = kEmptyList;
                    break;
                }
                if (same_list(cur, rhs)) continue;
                intersect_into(cur, rhs, *w);
                cur = List{w->data(), static_cast<uint32_t>(w->size())};
                std::swap(w, w2);
            }
        } else {
            for (uint32_t i = 1; i < m; ++i) {
                List rhs = ctx.operands[i];
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
    format_result_fast(cur, out);
}

void run_phrase(const Query& qu, const std::vector<uint32_t>& terms, const Index& idx, ThreadCtx& ctx,
                std::string& out) {
    uint32_t n = qu.n;
    if (n == 0) {
        format_result_fast(kEmptyList, out);
        return;
    }
    const uint32_t* qt = terms.data() + qu.toff;
    ctx.ql.resize(n);
    ctx.order.resize(n);
    uint32_t j = 0;
    uint32_t best = UINT32_MAX;
    for (uint32_t o = 0; o < n; ++o) {
        ctx.order[o] = o;
        ctx.ql[o] = idx.list(qt[o]);
        if (ctx.ql[o].empty()) {
            format_result_fast(kEmptyList, out);
            return;
        }
        if (ctx.ql[o].n < best) {
            best = ctx.ql[o].n;
            j = o;
        }
    }

    for (uint32_t i = 1; i < n; ++i) {
        uint32_t key = ctx.order[i];
        uint32_t ks = ctx.ql[key].n;
        uint32_t m = i;
        while (m > 0 && ctx.ql[ctx.order[m - 1]].n > ks) {
            ctx.order[m] = ctx.order[m - 1];
            --m;
        }
        ctx.order[m] = key;
    }

    List cur = ctx.ql[j];
    std::vector<uint32_t>* w = &ctx.cand_a;
    std::vector<uint32_t>* w2 = &ctx.cand_b;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t o = ctx.order[i];
        if (o == j) continue;
        intersect_into(cur, ctx.ql[o], *w);
        cur = List{w->data(), static_cast<uint32_t>(w->size())};
        std::swap(w, w2);
        if (cur.empty()) break;
    }
    if (cur.empty()) {
        format_result_fast(kEmptyList, out);
        return;
    }

    ctx.sl.resize(n);
    ctx.sln.resize(n);
    ctx.cursors.assign(n, 0);
    ctx.matches.clear();
    ctx.matches.reserve(cur.n);

    for (uint32_t ci = 0; ci < cur.n; ++ci) {
        uint32_t doc = cur.p[ci];
        uint32_t min_sln = UINT32_MAX;
        uint32_t anchor = j;

        for (uint32_t o = 0; o < n; ++o) {
            uint32_t& c = ctx.cursors[o];
            const uint32_t* p = ctx.ql[o].p;
            uint32_t lim = ctx.ql[o].n;
            while (c < lim && p[c] < doc) ++c;
            idx.positions(qt[o], c, ctx.sl[o], ctx.sln[o]);
            if (ctx.sln[o] < min_sln) {
                min_sln = ctx.sln[o];
                anchor = o;
            }
        }

        const uint32_t* anchor_pos = ctx.sl[anchor];
        uint32_t anchor_cnt = ctx.sln[anchor];
        bool found = false;

        for (uint32_t pi = 0; pi < anchor_cnt; ++pi) {
            uint32_t p = anchor_pos[pi];
            if (p < anchor) continue;
            uint32_t start = p - anchor;
            bool ok = true;

            for (uint32_t o = 0; o < n; ++o) {
                if (o == anchor) continue;
                uint32_t need = start + o;
                const uint32_t* spos = ctx.sl[o];
                uint32_t scnt = ctx.sln[o];

                if (scnt <= 8) {
                    bool hit = false;
                    for (uint32_t k = 0; k < scnt; ++k) {
                        if (spos[k] == need) { hit = true; break; }
                        if (spos[k] > need) break;
                    }
                    if (!hit) { ok = false; break; }
                } else {
                    if (!std::binary_search(spos, spos + scnt, need)) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) {
                found = true;
                break;
            }
        }
        if (found) ctx.matches.push_back(doc);
    }
    format_result_fast(List{ctx.matches.data(), static_cast<uint32_t>(ctx.matches.size())}, out);
}

unsigned clamp_threads(const char* env_name, unsigned def, unsigned maxv) {
    unsigned v = def;
    if (const char* env = std::getenv(env_name)) {
        unsigned e = static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        if (e > 0) v = e;
    }
    return std::max(1u, std::min(v, maxv));
}

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

    size_t nq = queries.size();
    const size_t kTasks = (nq > 0) ? std::min<size_t>(nq, std::max<size_t>(nthreads * 4, 32)) : 1;
    std::vector<std::string> task_outputs(kTasks);
    std::atomic<size_t> next_task{0};

    std::vector<ThreadCtx> ctxs(nthreads);
    std::vector<std::thread> threads;
    threads.reserve(nthreads);

    for (unsigned t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t]() {
            ThreadCtx& ctx = ctxs[t];
            for (;;) {
                size_t task_id = next_task.fetch_add(1, std::memory_order_relaxed);
                if (task_id >= kTasks) break;
                size_t lo = nq * task_id / kTasks;
                size_t hi = nq * (task_id + 1) / kTasks;
                std::string& out = task_outputs[task_id];
                out.reserve((hi - lo) * 32 + 64);
                for (size_t qi = lo; qi < hi; ++qi) {
                    const Query& qu = queries[qi];
                    if (qu.kind == 0)
                        run_bool(qu, terms, ops, idx, ctx, out);
                    else
                        run_phrase(qu, terms, idx, ctx, out);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    std::string path = out_dir + "/results.txt";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot create %s\n", path.c_str());
        return 1;
    }
    for (size_t k = 0; k < kTasks; ++k) {
        const std::string& s = task_outputs[k];
        if (!s.empty()) std::fwrite(s.data(), 1, s.size(), f);
    }
    if (std::fclose(f) != 0) return 1;
    _exit(0);

    return 0;
}
