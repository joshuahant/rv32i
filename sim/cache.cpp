#include "cache.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static bool is_pow2(uint32_t n) { return n > 0 && (n & (n - 1)) == 0; }

static uint32_t log2i(uint32_t n) {
    uint32_t r = 0;
    while (n >>= 1) r++;
    return r;
}

// ─── CacheStats::print ────────────────────────────────────────────────────────

void CacheStats::print(const std::string& name) const {
    std::cout << "=== " << name << " ===\n";
    std::cout << "  Accesses:          " << accesses
              << "  (reads: " << reads << "  writes: " << writes << ")\n";
    std::cout << "  Hits:              " << hits
              << std::fixed << std::setprecision(2)
              << "  (" << hit_rate() * 100.0 << "%)\n";
    std::cout << "  Misses:            " << misses()
              << "  (" << miss_rate() * 100.0 << "%)\n";
    std::cout << "    Cold (compuls.): " << cold_misses     << "\n";
    std::cout << "    Capacity:        " << capacity_misses << "\n";
    std::cout << "    Conflict:        " << conflict_misses << "\n";
    std::cout << "  Evictions:         " << evictions
              << "  (dirty: " << dirty_evictions << ")\n";
    std::cout << "  Total cycles:      " << total_cycles << "\n";
    if (accesses > 0)
        std::cout << "  Avg cycles/access: "
                  << (double)total_cycles / (double)accesses << "\n";
    std::cout << "\n";
}

// ─── Cache construction ───────────────────────────────────────────────────────

Cache::Cache(const CacheConfig& cfg) : cfg_(cfg) {
    if (!is_pow2(cfg.block_size))
        throw std::invalid_argument(cfg.name + ": block_size must be a power of 2");
    if (cfg.size < cfg.block_size)
        throw std::invalid_argument(cfg.name + ": size must be >= block_size");
    if (!is_pow2(cfg.size))
        throw std::invalid_argument(cfg.name + ": size must be a power of 2");

    uint32_t total_blocks = cfg.size / cfg.block_size;
    ways_ = (cfg.ways == 0) ? total_blocks : cfg.ways; // 0 = fully-assoc

    if (total_blocks % ways_ != 0)
        throw std::invalid_argument(cfg.name + ": total_blocks not divisible by ways");

    num_sets_ = total_blocks / ways_;

    if (!is_pow2(num_sets_))
        throw std::invalid_argument(cfg.name + ": resulting num_sets not a power of 2");

    sets_.assign(num_sets_, std::vector<CacheLine>(ways_));
    fa_.resize(total_blocks);
}

// ─── Address decomposition ────────────────────────────────────────────────────

uint32_t Cache::offset_bits() const { return log2i(cfg_.block_size); }
uint32_t Cache::index_bits()  const { return log2i(num_sets_); }

uint32_t Cache::block_addr(uint32_t addr) const {
    return addr >> offset_bits();
}
uint32_t Cache::set_index(uint32_t addr) const {
    return (addr >> offset_bits()) & (num_sets_ - 1);
}
uint32_t Cache::tag_of(uint32_t addr) const {
    return addr >> (offset_bits() + index_bits());
}

// ─── Way lookup / victim selection ───────────────────────────────────────────

int Cache::find_way(uint32_t set_idx, uint32_t tag) const {
    const auto& s = sets_[set_idx];
    for (uint32_t w = 0; w < ways_; w++)
        if (s[w].valid && s[w].tag == tag) return (int)w;
    return -1;
}

int Cache::find_victim(uint32_t set_idx) {
    auto& s = sets_[set_idx];
    // Prefer invalid line
    for (uint32_t w = 0; w < ways_; w++)
        if (!s[w].valid) return (int)w;
    // All valid: apply replacement policy
    switch (cfg_.repl) {
        case ReplPolicy::LRU:
        case ReplPolicy::FIFO: {
            int victim = 0;
            uint64_t oldest = s[0].order;
            for (uint32_t w = 1; w < ways_; w++)
                if (s[w].order < oldest) { oldest = s[w].order; victim = (int)w; }
            return victim;
        }
        case ReplPolicy::RANDOM:
            return (int)(rand() % ways_);
    }
    return 0;
}

// ─── FA shadow ────────────────────────────────────────────────────────────────

int Cache::fa_find(uint32_t baddr) const {
    for (size_t i = 0; i < fa_.size(); i++)
        if (fa_[i].valid && fa_[i].tag == baddr) return (int)i;
    return -1;
}

int Cache::fa_victim() {
    for (size_t i = 0; i < fa_.size(); i++)
        if (!fa_[i].valid) return (int)i;
    int v = 0; uint64_t oldest = fa_[0].order;
    for (size_t i = 1; i < fa_.size(); i++)
        if (fa_[i].order < oldest) { oldest = fa_[i].order; v = (int)i; }
    return v;
}

bool Cache::fa_access(uint32_t baddr) {
    int idx = fa_find(baddr);
    if (idx >= 0) { fa_[idx].order = fa_clk_++; return true; }
    int v = fa_victim();
    fa_[v] = { true, false, baddr, fa_clk_++ };
    return false;
}

// ─── access ───────────────────────────────────────────────────────────────────

AccessResult Cache::access(uint32_t addr, bool is_write) {
    AccessResult result;
    stats_.accesses++;
    is_write ? stats_.writes++ : stats_.reads++;

    uint32_t sidx  = set_index(addr);
    uint32_t tag   = tag_of(addr);
    uint32_t baddr = block_addr(addr);

    // Always update the FA shadow and seen-set (needed for miss classification).
    bool first_time = (seen_.find(baddr) == seen_.end());
    seen_.insert(baddr);
    bool fa_hit = fa_access(baddr);

    int way = find_way(sidx, tag);

    if (way >= 0) {
        // ── Hit ─────────────────────────────────────────────────────────────
        result.hit    = true;
        result.cycles = cfg_.hit_cycles;
        stats_.hits++;
        stats_.total_cycles += result.cycles;

        if (cfg_.repl == ReplPolicy::LRU)
            sets_[sidx][way].order = clk_++;

        if (is_write && cfg_.write == WritePolicy::WRITE_BACK)
            sets_[sidx][way].dirty = true;

    } else {
        // ── Miss ─────────────────────────────────────────────────────────────
        result.hit    = false;
        result.cycles = cfg_.hit_cycles + cfg_.miss_penalty;
        stats_.total_cycles += result.cycles;

        if (first_time)     stats_.cold_misses++;
        else if (!fa_hit)   stats_.capacity_misses++;
        else                stats_.conflict_misses++;

        // Allocate on: any read, or write with write_alloc
        bool allocate = !is_write || cfg_.write_alloc;
        if (allocate) {
            int victim = find_victim(sidx);
            auto& line = sets_[sidx][victim];

            if (line.valid) {
                result.evicted       = true;
                result.evicted_dirty = line.dirty;
                // Reconstruct block-aligned address from (tag, set_index)
                uint32_t evicted_blk  = (line.tag << index_bits()) | sidx;
                result.evicted_addr   = evicted_blk << offset_bits();
                stats_.evictions++;
                if (line.dirty) stats_.dirty_evictions++;
            }

            line.valid = true;
            line.dirty = is_write && (cfg_.write == WritePolicy::WRITE_BACK);
            line.tag   = tag;
            // LRU and FIFO both use 'order': LRU updates on every hit,
            // FIFO only sets it at install time and never updates it.
            line.order = clk_++;
        }
    }

    return result;
}

// ─── install ──────────────────────────────────────────────────────────────────

void Cache::install(uint32_t addr, bool dirty) {
    uint32_t sidx = set_index(addr);
    uint32_t tag  = tag_of(addr);

    int way = find_way(sidx, tag);
    if (way >= 0) {
        // Already present — update dirty bit and LRU order
        if (dirty) sets_[sidx][way].dirty = true;
        if (cfg_.repl == ReplPolicy::LRU) sets_[sidx][way].order = clk_++;
        return;
    }

    int victim = find_victim(sidx);
    auto& line = sets_[sidx][victim];
    // We don't count evictions here because this install is a hierarchy-driven
    // operation (not a demand access), so the eviction was already counted when
    // the block was first demanded.
    line = { true, dirty, tag, clk_++ };
}

// ─── probe ────────────────────────────────────────────────────────────────────

bool Cache::probe(uint32_t addr) const {
    return find_way(set_index(addr), tag_of(addr)) >= 0;
}

// ─── reset_stats ─────────────────────────────────────────────────────────────

void Cache::reset_stats() {
    stats_  = {};
    seen_.clear();
    for (auto& l : fa_) l = {};
    fa_clk_ = 0;
}

void Cache::print_stats() const { stats_.print(cfg_.name); }

// ─── HierarchyStats::print ────────────────────────────────────────────────────

void HierarchyStats::print() const {
    uint64_t total = reads + writes;
    std::cout << "=== Hierarchy Summary ===\n";
    std::cout << "  Total accesses: " << total
              << "  (reads: " << reads << "  writes: " << writes << ")\n";
    std::cout << "  Total cycles:   " << total_cycles << "\n";
    if (total > 0)
        std::cout << std::fixed << std::setprecision(2)
                  << "  Avg cycles:     " << (double)total_cycles / (double)total << "\n";
    std::cout << "\n";
}

// ─── CacheHierarchy ──────────────────────────────────────────────────────────

CacheHierarchy::CacheHierarchy(std::vector<CacheConfig> configs, uint32_t memory_cycles)
    : mem_cycles_(memory_cycles) {
    if (configs.empty())
        throw std::invalid_argument("CacheHierarchy: at least one level required");
    for (auto& c : configs)
        levels_.emplace_back(c);
}

uint64_t CacheHierarchy::access(uint32_t addr, bool is_write) {
    is_write ? stats_.writes++ : stats_.reads++;
    uint64_t cycles = 0;

    // Track which levels missed so we can install the block top-down on a
    // memory fetch.
    size_t hit_level = levels_.size(); // sentinel = memory

    for (size_t i = 0; i < levels_.size(); i++) {
        AccessResult r = levels_[i].access(addr, is_write);
        cycles += r.cycles;

        if (r.hit) {
            hit_level = i;

            // Write-through: propagate write to next level that has the block.
            if (is_write && levels_[i].config().write == WritePolicy::WRITE_THROUGH) {
                for (size_t j = i + 1; j < levels_.size(); j++) {
                    if (levels_[j].probe(addr)) {
                        // Don't count these as independent accesses in j's stats
                        // — use install() to update state silently.
                        levels_[j].install(addr, false);
                        break;
                    }
                }
            }
            break;
        }

        // Miss at level i: handle any dirty eviction to level i+1.
        if (r.evicted && r.evicted_dirty) {
            if (i + 1 < levels_.size())
                levels_[i + 1].install(r.evicted_addr, /*dirty=*/true);
            // else: dirty eviction from LLC goes to memory (no extra latency
            // modelled here; can add mem_cycles_ if desired).
        }
    }

    // All levels missed → add DRAM latency and install block down from LLC→L1.
    if (hit_level == levels_.size()) {
        cycles += mem_cycles_;
        // levels_[last] already installed the block via access().
        // Install in all higher levels (they already called access() which
        // installed eagerly, so install() is a no-op if present, or installs
        // the block if write_alloc was false and we still want to propagate).
        // This pass ensures coherence for read-miss paths.
        for (int j = (int)levels_.size() - 2; j >= 0; j--)
            levels_[j].install(addr, /*dirty=*/false);
    }

    stats_.total_cycles += cycles;
    return cycles;
}

void CacheHierarchy::reset_stats() {
    stats_ = {};
    for (auto& l : levels_) l.reset_stats();
}

void CacheHierarchy::print_stats() const {
    for (const auto& l : levels_) l.print_stats();
    stats_.print();
}
