#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <stdexcept>

// ─── Enums ────────────────────────────────────────────────────────────────────

enum class ReplPolicy  { LRU, FIFO, RANDOM };
enum class WritePolicy { WRITE_BACK, WRITE_THROUGH };

// ─── Configuration ────────────────────────────────────────────────────────────

struct CacheConfig {
    std::string  name        = "L?";
    uint32_t     size        = 32768; // total bytes
    uint32_t     block_size  = 64;    // bytes per block/line
    uint32_t     ways        = 4;     // 1=direct-mapped, 0=fully-associative
    ReplPolicy   repl        = ReplPolicy::LRU;
    WritePolicy  write       = WritePolicy::WRITE_BACK;
    bool         write_alloc = true;  // allocate on write miss?
    uint32_t     hit_cycles  = 4;     // cycles when this level hits
    uint32_t     miss_penalty= 0;     // extra cycles added to this level on miss
                                      // (usually 0; set on LLC only if desired)
};

// ─── Statistics ───────────────────────────────────────────────────────────────

struct CacheStats {
    uint64_t accesses        = 0;
    uint64_t reads           = 0;
    uint64_t writes          = 0;
    uint64_t hits            = 0;
    uint64_t cold_misses     = 0; // compulsory: first ever access to block
    uint64_t capacity_misses = 0; // would hit in FA cache of same size
    uint64_t conflict_misses = 0; // FA would hit, but set is full (limited assoc)
    uint64_t evictions       = 0;
    uint64_t dirty_evictions = 0;
    uint64_t total_cycles    = 0;

    uint64_t misses()   const { return cold_misses + capacity_misses + conflict_misses; }
    double   hit_rate() const { return accesses ? (double)hits / accesses : 0.0; }
    double   miss_rate()const { return accesses ? (double)misses() / accesses : 0.0; }

    void print(const std::string& name) const;
};

// ─── Internal structures ──────────────────────────────────────────────────────

struct CacheLine {
    bool     valid = false;
    bool     dirty = false;
    uint32_t tag   = 0;
    uint64_t order = 0; // LRU: last-use timestamp; FIFO: insertion timestamp
};

struct AccessResult {
    bool     hit           = false;
    bool     evicted       = false;
    bool     evicted_dirty = false;
    uint32_t evicted_addr  = 0; // block-aligned byte address of evicted line
    uint32_t cycles        = 0;
};

// ─── Cache ────────────────────────────────────────────────────────────────────

class Cache {
public:
    explicit Cache(const CacheConfig& cfg);

    // Perform a read or write access. Returns hit/miss info and any eviction.
    // On a miss with write_alloc (or any read), the new block is installed
    // immediately and the evicted line (if any) is reported in the result.
    AccessResult access(uint32_t addr, bool is_write);

    // Install a block from a lower level (used by CacheHierarchy on writeback
    // or after a miss is resolved at a lower level).
    void install(uint32_t addr, bool dirty = false);

    // Returns true if addr is currently cached (no side-effects).
    bool probe(uint32_t addr) const;

    void reset_stats();
    void print_stats() const;

    const CacheStats&  stats()  const { return stats_; }
    const CacheConfig& config() const { return cfg_;   }

private:
    CacheConfig cfg_;
    CacheStats  stats_;

    uint32_t num_sets_;
    uint32_t ways_;
    std::vector<std::vector<CacheLine>> sets_; // [set_idx][way]
    uint64_t clk_ = 0;

    // ── Miss classification helpers ──────────────────────────────────────────
    // seen_: block addresses ever accessed (detects cold misses)
    std::unordered_set<uint32_t> seen_;
    // FA shadow: fully-associative cache with the same total block count.
    // Updated on every access so we can distinguish capacity vs conflict misses.
    std::vector<CacheLine> fa_;
    uint64_t fa_clk_ = 0;

    // ── Address helpers ──────────────────────────────────────────────────────
    uint32_t offset_bits() const;
    uint32_t index_bits()  const;
    uint32_t block_addr(uint32_t addr)  const; // addr >> offset_bits
    uint32_t set_index(uint32_t addr)   const;
    uint32_t tag_of(uint32_t addr)      const;

    // ── Set operations ───────────────────────────────────────────────────────
    int find_way(uint32_t set_idx, uint32_t tag) const; // -1 if not found
    int find_victim(uint32_t set_idx);

    // ── FA shadow operations ─────────────────────────────────────────────────
    bool fa_access(uint32_t baddr); // returns true on FA hit; always installs
    int  fa_find(uint32_t baddr) const;
    int  fa_victim();
};

// ─── Hierarchy ────────────────────────────────────────────────────────────────

struct HierarchyStats {
    uint64_t reads        = 0;
    uint64_t writes       = 0;
    uint64_t total_cycles = 0;
    void print() const;
};

// CacheHierarchy chains N levels of Cache. On a miss at level i, level i+1
// is consulted. Dirty evictions from level i are written to level i+1.
// If all levels miss, memory_cycles are added (representing DRAM latency).
class CacheHierarchy {
public:
    CacheHierarchy(std::vector<CacheConfig> configs, uint32_t memory_cycles = 200);

    uint64_t access(uint32_t addr, bool is_write);
    uint64_t read (uint32_t addr) { return access(addr, false); }
    uint64_t write(uint32_t addr) { return access(addr, true);  }

    void reset_stats();
    void print_stats() const;

    Cache&  level(size_t i)     { return levels_[i]; }
    size_t  num_levels()  const { return levels_.size(); }
    const HierarchyStats& stats() const { return stats_; }

private:
    std::vector<Cache> levels_;
    uint32_t           mem_cycles_;
    HierarchyStats     stats_;
};
