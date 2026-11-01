// Speed Demon 2026, Track D (Traverse): exact directed shortest paths.
// The graph is mmap'ed once and parsed in newline-aligned chunks. Worker-local
// edge buckets build CSR without atomics on rows; a parallel BFS relabels nodes
// for locality. Queries are grouped by source and answered by early-exit Dijkstra
// with a monotone radix heap. Zero means unseen in the complemented distance map;
// a uint32 search that exceeds its range is rerun with uint64 distances.
//
// Runtime paths: solution --data /data --out /out

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <fcntl.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* map_file(const std::string& path, size_t& len) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) exit(1);
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); exit(1); }
    len = (size_t)st.st_size;
    void* p = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) exit(1);
    return (const char*)p;
}
static inline bool skip(const char*& p, const char* e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p < e;
}
// Reads an unsigned decimal and skips the whitespace separating tokens.
static inline uint32_t num(const char*& p, const char* e) {
    if (!skip(p, e)) return 0;
    uint32_t x = 0;
    while (p < e && *p >= '0' && *p <= '9') x = x * 10 + (uint32_t)(*p++ - '0');
    return x;
}
template <class F> static void par(unsigned T, F f) {
    std::vector<std::thread> ts;
    for (unsigned i = 0; i < T; ++i) ts.emplace_back(f, i);
    for (auto& t : ts) t.join();
}
struct Arc { uint32_t v, w; };
static uint32_t* g_off;
static Arc* g_arc;

// Radix-heap Dijkstra. Queries sharing a source share one search and stop after
// every requested target has settled. A uint32 pass handles ordinary distances;
// groups that exceed it are rerun with uint64 distances.
struct Group { uint32_t source; size_t first, last; };
template <class D> struct Search {
    using K = std::conditional_t<sizeof(D) == 4, uint64_t, unsigned __int128>;
    size_t bytes;
    D* dist; // complemented distance, zero means unseen
    std::vector<uint32_t> touched;
    std::vector<K> bucket[65];
    D last = 0;
    size_t count = 0;
    explicit Search(uint32_t n) : bytes(size_t(n) * sizeof(D)) {
        dist = (D*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (dist == MAP_FAILED) exit(1);
    }
    ~Search() { munmap(dist, bytes); }
    void push(D d, uint32_t v) {
        unsigned b = d == last ? 0 : 64 - __builtin_clzll(uint64_t(d ^ last));
        bucket[b].push_back(K(d) << 32 | v);
        ++count;
    }
    bool run(const Group& g, const std::vector<size_t>& todo,
             const std::vector<uint32_t>& qs, std::vector<int64_t>& ans) {
        size_t remaining = g.last - g.first;
        for (size_t i = g.first; i < g.last; ++i) ans[todo[i]] = -1;
        dist[g.source] = ~D(0);
        touched.push_back(g.source);
        last = 0;
        push(0, g.source);
        bool overflow = false;
        while (count && remaining) {
            if (bucket[0].empty()) {
                size_t b = 1;
                while (bucket[b].empty()) ++b;
                last = D(*std::min_element(bucket[b].begin(), bucket[b].end()) >> 32);
                for (K k : bucket[b]) --count, push(D(k >> 32), uint32_t(k));
                bucket[b].clear();
            }
            D d = D(bucket[0].back() >> 32);
            uint32_t u = uint32_t(bucket[0].back());
            bucket[0].pop_back();
            --count;
            if (!bucket[0].empty()) __builtin_prefetch(&g_arc[g_off[uint32_t(bucket[0].back())]]);
            if (d != (D)~dist[u]) continue;
            if (g.last - g.first == 1) {
                size_t qi = todo[g.first];
                if (u == qs[2 * qi + 1]) { ans[qi] = (int64_t)d; --remaining; }
            } else {
                auto it = std::lower_bound(todo.begin() + g.first, todo.begin() + g.last, u,
                    [&](size_t qi, uint32_t target) { return qs[2 * qi + 1] < target; });
                while (it != todo.begin() + g.last && qs[2 * *it + 1] == u) {
                    ans[*it] = (int64_t)d;
                    ++it;
                    --remaining;
                }
            }
            if (!remaining) break;
            for (uint32_t k = g_off[u]; k < g_off[u + 1]; ++k) {
                uint64_t nd = (uint64_t)d + g_arc[k].w;
                uint32_t v = g_arc[k].v;
                if (nd >= (D)~D(0)) { overflow = true; break; }
                if ((D)~nd > dist[v]) {
                    if (!dist[v]) touched.push_back(v);
                    dist[v] = ~nd;
                    push(D(nd), v);
                }
            }
            if (overflow) break;
        }
        for (uint32_t v : touched) dist[v] = 0;
        for (auto& b : bucket) b.clear();
        touched.clear();
        count = 0;
        return overflow;
    }
};
struct alignas(64) JobCursor { std::atomic<size_t> next{0}; };
template <class D>
static void solve(unsigned T, uint32_t n, const std::vector<size_t>& jobs,
                  const std::vector<Group>& groups, const std::vector<size_t>& todo,
                  const std::vector<uint32_t>& qs, std::vector<int64_t>& ans,
                  std::vector<uint8_t>* overflow) {
    std::vector<JobCursor> next(T);
    for (unsigned i = 0; i < T; ++i) next[i].next.store(jobs.size() * i / T, std::memory_order_relaxed);
    par(T, [&](unsigned i) {
        Search<D> search(n);
        for (unsigned k = 0; k < T; ++k) {
            unsigned b = (i + k) % T;
            size_t limit = jobs.size() * (b + 1) / T;
            for (size_t j; (j = next[b].next.fetch_add(1, std::memory_order_relaxed)) < limit;) {
                size_t job = jobs[j];
                bool over = search.run(groups[job], todo, qs, ans);
                if (overflow) (*overflow)[job] = over;
                else if (over) abort();
            }
        }
    });
}
int main(int argc, char** argv) {
    std::string data = "/data", out = "/out";
    for (int i = 1; i + 1 < argc; i += 2) (std::strcmp(argv[i], "--data") ? out : data) = argv[i + 1];
    unsigned T = std::max(1u, std::thread::hardware_concurrency());
#if defined(__linux__)
    cpu_set_t cs;
    if (!sched_getaffinity(0, sizeof cs, &cs)) T = std::max(1, CPU_COUNT(&cs));
#endif
    if (const char* env = getenv("SD_T")) { unsigned x = (unsigned)atoi(env); if (x) T = x; }
    size_t glen, qlen;
    const char* g = map_file(data + "/graph.txt", glen);
    const char* p = g;
    uint32_t n = num(p, g + glen);
    uint32_t m = num(p, g + glen);
    const char* body = p;
    const char* gend = g + glen;
    std::vector<const char*> cut(T + 1, gend);
    for (unsigned i = 0; i < T; ++i) {
        const char* c = body + (gend - body) * i / T;
        cut[i] = c;
        if (i > 0) {
            const char* r = (const char*)memchr(c, '\n', gend - c);
            cut[i] = r ? r + 1 : gend;
        }
    }
    struct E { uint32_t u, v, w; };
    std::vector<std::vector<E>> B(T * T);
    par(T, [&](unsigned i) {
        for (unsigned j = 0; j < T; ++j) B[i * T + j].reserve(m / T / T * 9 / 8 + 64);
        const char* q = cut[i];
        const char* e = cut[i + 1];
        while (skip(q, e)) {
            uint32_t u = num(q, e), v = num(q, e), w = num(q, e);
            B[i * T + (uint64_t)u * T / n].push_back({u, v, w});
        }
    });
    munmap((void*)g, glen);
    uint32_t* off = (uint32_t*)calloc(n + 1, 4);
    Arc* arc = (Arc*)malloc(sizeof(Arc) * (size_t)m);
    unsigned char* hasin = (unsigned char*)calloc(n, 1);  // has an in-edge from another node
    std::vector<uint64_t> rb(T + 1);
    for (unsigned j = 0; j < T; ++j)
        for (unsigned i = 0; i < T; ++i) rb[j + 1] = (i ? rb[j + 1] : rb[j]) + B[i * T + j].size();
    par(T, [&](unsigned j) {  // CSR rows of nodes [ceil(n*j/T), ceil(n*(j+1)/T))
        for (unsigned i = 0; i < T; ++i)
            for (const E& e : B[i * T + j]) ++off[e.u];
        uint32_t end = rb[j];
        for (uint32_t u = ((uint64_t)n * j + T - 1) / T; u < ((uint64_t)n * (j + 1) + T - 1) / T; ++u)
            off[u] = end += off[u];  // end of u's row; the fill below walks it back to the start
        for (unsigned i = 0; i < T; ++i) {
            for (const E& e : B[i * T + j]) {
                arc[--off[e.u]] = {e.v, e.w};
                if (e.u != e.v) __atomic_store_n(&hasin[e.v], 1, __ATOMIC_RELAXED);
            }
            std::vector<E>().swap(B[i * T + j]);
        }
    });
    off[n] = m;
    std::vector<uint32_t> perm(n, UINT32_MAX);  // old id -> new id
    {
        std::vector<std::vector<uint32_t>> Q(T);  // region i, in BFS order
        std::vector<uint64_t> nb(T + 1), eb(T + 1);
        par(T, [&](unsigned i) {
            auto& q = Q[i];
            q.reserve((n + T - 1) / T * 2);
            uint64_t e = 0;
            auto claim = [&](uint32_t v) {
                uint32_t x = UINT32_MAX;
                if (__atomic_load_n(&perm[v], __ATOMIC_RELAXED) == x &&
                    __atomic_compare_exchange_n(&perm[v], &x, i, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    q.push_back(v);
            };
            size_t h = 0;
            for (uint32_t r = (uint64_t)n * i / T; r < (uint64_t)n * (i + 1) / T; ++r)
                for (claim(r); h < q.size(); ++h) {
                    if (h + 24 < q.size()) __builtin_prefetch(&off[q[h + 24]]);
                    if (h + 16 < q.size()) __builtin_prefetch(&arc[off[q[h + 16]]]);
                    if (h + 8 < q.size())
                        for (uint32_t x = q[h + 8], k = off[x]; k < off[x + 1]; ++k) __builtin_prefetch(&perm[arc[k].v]);
                    uint32_t u = q[h];
                    e += off[u + 1] - off[u];
                    for (uint32_t k = off[u]; k < off[u + 1]; ++k) claim(arc[k].v);
                }
            nb[i + 1] = q.size(), eb[i + 1] = e;
        });
        for (unsigned i = 0; i < T; ++i) nb[i + 1] += nb[i], eb[i + 1] += eb[i];
        uint32_t* noff = (uint32_t*)malloc(4 * (size_t)(n + 1));
        Arc* narc = (Arc*)malloc(sizeof(Arc) * (size_t)m);
        unsigned char* nin = (unsigned char*)malloc(n);
        par(T, [&](unsigned i) {
            for (size_t j = 0; j < Q[i].size(); ++j) perm[Q[i][j]] = nb[i] + j;
        });
        par(T, [&](unsigned i) {
            const auto& q = Q[i];
            uint32_t o = eb[i];
            for (size_t j = 0; j < q.size(); ++j) {
                if (j + 32 < q.size()) __builtin_prefetch(&off[q[j + 32]]);
                if (j + 16 < q.size()) __builtin_prefetch(&arc[off[q[j + 16]]]);
                if (j + 8 < q.size())
                    for (uint32_t y = q[j + 8], k = off[y]; k < off[y + 1]; ++k) __builtin_prefetch(&perm[arc[k].v]);
                uint32_t u = q[j], x = nb[i] + j;
                noff[x] = o, nin[x] = hasin[u];
                for (uint32_t k = off[u]; k < off[u + 1]; ++k) narc[o++] = {perm[arc[k].v], arc[k].w};
            }
        });
        noff[n] = m;
        free(arc), free(off), free(hasin);
        g_off = noff, g_arc = narc, hasin = nin;
    }
    const char* q = map_file(data + "/queries.txt", qlen);
    std::vector<uint32_t> qs;
    for (const char* e = q + qlen; skip(q, e);) qs.push_back(perm[num(q, e)]);
    munmap((void*)q, qlen);
    std::vector<uint32_t>().swap(perm);
    size_t nq = qs.size() / 2;
    std::vector<int64_t> ans(nq);
    std::vector<size_t> todo;
    for (size_t i = 0; i < nq; ++i) {
        uint32_t s = qs[2 * i], t = qs[2 * i + 1];
        if (s == t) ans[i] = 0;
        else if (!hasin[t] || g_off[s] == g_off[s + 1]) ans[i] = -1;
        else todo.push_back(i);
    }
    free(hasin);
    std::sort(todo.begin(), todo.end(), [&](size_t a, size_t b) {
        if (qs[2 * a] != qs[2 * b]) return qs[2 * a] < qs[2 * b];
        return qs[2 * a + 1] < qs[2 * b + 1];
    });
    std::vector<Group> groups;
    for (size_t i = 0; i < todo.size();) {
        size_t j = i + 1;
        uint32_t s = qs[2 * todo[i]];
        while (j < todo.size() && qs[2 * todo[j]] == s) ++j;
        groups.push_back({s, i, j});
        i = j;
    }
    std::vector<size_t> jobs(groups.size());
    for (size_t i = 0; i < jobs.size(); ++i) jobs[i] = i;
    std::vector<uint8_t> overflow(groups.size(), 0);
    solve<uint32_t>(T, n, jobs, groups, todo, qs, ans, &overflow);
    jobs.clear();
    for (size_t i = 0; i < overflow.size(); ++i) if (overflow[i]) jobs.push_back(i);
    solve<uint64_t>(T, n, jobs, groups, todo, qs, ans, nullptr);
    std::vector<char> res;
    res.reserve(nq * 18);
    for (int64_t a : ans) {
        if (a < 0) { res.push_back('-'); res.push_back('1'); }
        else {
            char digits[24]; unsigned k = 0;
            uint64_t x = (uint64_t)a;
            do { digits[k++] = char('0' + x % 10); x /= 10; } while (x);
            while (k) res.push_back(digits[--k]);
        }
        res.push_back('\n');
    }
    int fd = open((out + "/distances.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;
    size_t pos = 0;
    while (pos < res.size()) {
        ssize_t wrote = write(fd, res.data() + pos, res.size() - pos);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return 1;
        }
        pos += (size_t)wrote;
    }
    return close(fd) != 0;
}
