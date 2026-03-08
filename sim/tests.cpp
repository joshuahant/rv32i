// Cache simulator test suite
// Run with: make sim-test  (from project root)
//
// Tests are grouped by feature:
//   1. Basic hit/miss
//   2. Replacement policies (LRU, FIFO, Random)
//   3. Miss classification (cold / capacity / conflict)
//   4. Write policies (write-back / write-through)
//   5. Eviction counting (clean and dirty)
//   6. Cycle counting
//   7. Multi-level hierarchy
//   8. RISC-V-flavoured access patterns

#include "cache.h"
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>

// ─── Minimal test framework ───────────────────────────────────────────────────

static int g_total   = 0;
static int g_failed  = 0;
static std::string g_suite;

static void suite(const std::string& name) {
    g_suite = name;
    std::cout << "\n[" << name << "]\n";
}

#define EXPECT_EQ(got, expected) do { \
    g_total++; \
    if ((got) != (expected)) { \
        std::cerr << "  FAIL  " << __func__ << ":" << __LINE__ \
                  << "  expected=" << (expected) << "  got=" << (got) << "\n"; \
        g_failed++; \
    } else { \
        std::cout << "  pass  " << __func__ << ":" << __LINE__ << "\n"; \
    } \
} while(0)

#define EXPECT_TRUE(cond)  EXPECT_EQ(!!(cond), true)
#define EXPECT_FALSE(cond) EXPECT_EQ(!!(cond), false)

// ─── Helper: build a typical L1-like cache ────────────────────────────────────
// 1 KiB, 4-way, 64-byte blocks → 4 sets.
static CacheConfig make_l1(ReplPolicy rp = ReplPolicy::LRU) {
    CacheConfig c;
    c.name       = "L1";
    c.size       = 1024;
    c.block_size = 64;
    c.ways       = 4;
    c.repl       = rp;
    c.write      = WritePolicy::WRITE_BACK;
    c.write_alloc= true;
    c.hit_cycles = 4;
    c.miss_penalty = 0;
    return c;
}

// ─── Helper: addresses that map to set 0 (block addresses 0, 4, 8, 12, …)
// With 1KiB / 64B / 4-way → 4 sets.  set_index = block_addr & 3.
// Blocks 0, 4, 8, 12, 16, ... all map to set 0.
static uint32_t set0_addr(int n) {
    // block n*4 → byte addr n*4*64 = n*256
    return (uint32_t)(n * 4 * 64);
}

// ─── 1. Basic hit/miss ────────────────────────────────────────────────────────

static void test_cold_start_all_miss() {
    Cache c(make_l1());
    for (int i = 0; i < 16; i++) {
        auto r = c.access((uint32_t)i * 64, false);
        EXPECT_FALSE(r.hit);
    }
    EXPECT_EQ(c.stats().misses(), (uint64_t)16);
    EXPECT_EQ(c.stats().hits,     (uint64_t)0);
}

static void test_repeat_access_hits() {
    Cache c(make_l1());
    c.access(0, false);          // cold miss
    auto r = c.access(0, false); // should hit
    EXPECT_TRUE(r.hit);
    EXPECT_EQ(c.stats().hits, (uint64_t)1);
}

static void test_probe() {
    Cache c(make_l1());
    EXPECT_FALSE(c.probe(0));
    c.access(0, false);
    EXPECT_TRUE(c.probe(0));
    // Different cache line
    EXPECT_FALSE(c.probe(64));
}

// ─── 2. Replacement policy ────────────────────────────────────────────────────

static void test_lru_evicts_oldest() {
    // 4-way set-assoc, 4 sets.  Fill set 0 with 4 blocks, then access a 5th.
    // With LRU, the first block (least recently used) should be evicted.
    Cache c(make_l1(ReplPolicy::LRU));

    // Install 4 blocks into set 0 (fills it completely).
    for (int i = 0; i < 4; i++) c.access(set0_addr(i), false);

    // Re-use blocks 1-3 to make block 0 the LRU.
    c.access(set0_addr(1), false);
    c.access(set0_addr(2), false);
    c.access(set0_addr(3), false);

    // Block 0 should be evicted when we bring in block 4.
    auto r = c.access(set0_addr(4), false);
    EXPECT_FALSE(r.hit);
    EXPECT_TRUE(r.evicted);
    EXPECT_EQ(r.evicted_addr, set0_addr(0));
}

static void test_fifo_evicts_oldest_inserted() {
    Cache c(make_l1(ReplPolicy::FIFO));

    // Fill set 0 with blocks 0–3 in order.
    for (int i = 0; i < 4; i++) c.access(set0_addr(i), false);

    // Access 1-3 to make them "recently used" — but FIFO ignores usage order.
    c.access(set0_addr(1), false);
    c.access(set0_addr(2), false);
    c.access(set0_addr(3), false);

    // Block 0 was inserted first → FIFO should evict it.
    auto r = c.access(set0_addr(4), false);
    EXPECT_FALSE(r.hit);
    EXPECT_TRUE(r.evicted);
    EXPECT_EQ(r.evicted_addr, set0_addr(0));
}

static void test_direct_mapped_conflict() {
    CacheConfig cfg;
    cfg.name       = "DM";
    cfg.size       = 1024;
    cfg.block_size = 64;
    cfg.ways       = 1; // direct-mapped
    cfg.repl       = ReplPolicy::LRU;
    cfg.write      = WritePolicy::WRITE_BACK;
    cfg.write_alloc= true;
    cfg.hit_cycles = 4;
    Cache c(cfg);

    // Two addresses that map to the same set (same block modulo 16 sets).
    // With 1KiB / 64B / 1-way → 16 sets.  set = block_addr & 15.
    // block 0 and block 16 both map to set 0.
    uint32_t a = 0;
    uint32_t b = 16 * 64;

    c.access(a, false);
    c.access(b, false); // evicts a
    auto r = c.access(a, false); // evicts b, misses on a
    EXPECT_FALSE(r.hit);
    EXPECT_EQ(c.stats().conflict_misses, (uint64_t)1);
}

// ─── 3. Miss classification ───────────────────────────────────────────────────

static void test_cold_misses_only_on_first_access() {
    Cache c(make_l1());
    const int N = 8;
    // First pass: all cold misses.
    for (int i = 0; i < N; i++) c.access((uint32_t)i * 64, false);
    EXPECT_EQ(c.stats().cold_misses, (uint64_t)N);
    EXPECT_EQ(c.stats().capacity_misses, (uint64_t)0);
    EXPECT_EQ(c.stats().conflict_misses, (uint64_t)0);
}

static void test_capacity_misses() {
    // 1KiB cache, 64B blocks → 16 blocks total.
    // Fully-associative (ways=0) so no conflict misses.
    CacheConfig cfg;
    cfg.name       = "FA";
    cfg.size       = 1024;
    cfg.block_size = 64;
    cfg.ways       = 0; // fully-associative
    cfg.repl       = ReplPolicy::LRU;
    cfg.write      = WritePolicy::WRITE_BACK;
    cfg.write_alloc= true;
    cfg.hit_cycles = 4;
    Cache c(cfg);

    // Access 17 distinct cache lines (more than capacity).
    for (int i = 0; i < 17; i++) c.access((uint32_t)i * 64, false);
    // Re-access line 0 — it was evicted (capacity), no conflict possible.
    c.access(0, false);

    EXPECT_EQ(c.stats().conflict_misses, (uint64_t)0);
    EXPECT_TRUE(c.stats().capacity_misses > 0);
}

static void test_conflict_vs_capacity() {
    // The direct-mapped conflict test above already verifies one conflict miss.
    // Here verify the counts more carefully.
    CacheConfig cfg;
    cfg.name       = "DM2";
    cfg.size       = 1024;
    cfg.block_size = 64;
    cfg.ways       = 1;
    cfg.repl       = ReplPolicy::LRU;
    cfg.write      = WritePolicy::WRITE_BACK;
    cfg.write_alloc= true;
    cfg.hit_cycles = 4;
    Cache c(cfg);

    // 16 sets (direct-mapped). Block 0 and block 16 map to set 0.
    uint32_t a = 0;
    uint32_t b = 16 * 64;

    c.access(a, false); // cold
    c.access(b, false); // cold
    c.access(a, false); // conflict (FA would hit since both fit in FA of same size)
    c.access(b, false); // conflict

    EXPECT_EQ(c.stats().cold_misses,     (uint64_t)2);
    EXPECT_EQ(c.stats().conflict_misses, (uint64_t)2);
    EXPECT_EQ(c.stats().capacity_misses, (uint64_t)0);
}

// ─── 4. Write policies ────────────────────────────────────────────────────────

static void test_writeback_marks_dirty() {
    Cache c(make_l1());
    // Write to block 0 in set 0 (miss → allocate dirty).
    c.access(set0_addr(0), true);
    // Fill the other 3 ways of set 0 with clean reads (LRU order > block 0).
    c.access(set0_addr(1), false);
    c.access(set0_addr(2), false);
    c.access(set0_addr(3), false);
    // Re-touch 1–3 so block 0 becomes the LRU (lowest order).
    c.access(set0_addr(1), false);
    c.access(set0_addr(2), false);
    c.access(set0_addr(3), false);
    // Bring in block 4 → set 0 is full → LRU victim is block 0 (dirty).
    c.access(set0_addr(4), false);
    EXPECT_TRUE(c.stats().dirty_evictions > 0);
}

static void test_write_through_no_dirty() {
    CacheConfig cfg = make_l1();
    cfg.write      = WritePolicy::WRITE_THROUGH;
    cfg.write_alloc= false;
    Cache c(cfg);

    c.access(0, false); // read: install
    c.access(0, true);  // write hit: write-through, no dirty bit set
    // Evict by filling the set.
    for (int i = 1; i <= 4; i++) c.access(set0_addr(i), false);
    EXPECT_EQ(c.stats().dirty_evictions, (uint64_t)0);
}

static void test_write_alloc_on_write_miss() {
    Cache c(make_l1()); // write-back + write_alloc = true
    auto r = c.access(0, true); // write miss → should allocate
    EXPECT_FALSE(r.hit);
    EXPECT_TRUE(c.probe(0)); // block should now be in cache
}

static void test_no_write_alloc_on_write_miss() {
    CacheConfig cfg = make_l1();
    cfg.write_alloc = false;
    Cache c(cfg);
    auto r = c.access(0, true); // write miss → should NOT allocate
    EXPECT_FALSE(r.hit);
    EXPECT_FALSE(c.probe(0)); // block should NOT be in cache
}

// ─── 5. Eviction counting ────────────────────────────────────────────────────

static void test_eviction_count() {
    Cache c(make_l1()); // 4-way, 4 sets
    // Fill set 0 (4 blocks), then force 4 more evictions.
    for (int i = 0; i < 4; i++) c.access(set0_addr(i), false); // fill, no evictions
    for (int i = 4; i < 8; i++) c.access(set0_addr(i), false); // 4 evictions
    EXPECT_EQ(c.stats().evictions, (uint64_t)4);
}

static void test_dirty_eviction_count() {
    Cache c(make_l1());
    // Write to 4 lines in set 0 → all dirty.
    for (int i = 0; i < 4; i++) c.access(set0_addr(i), true);
    // Bring in 4 more → evict the 4 dirty ones.
    for (int i = 4; i < 8; i++) c.access(set0_addr(i), false);
    EXPECT_EQ(c.stats().dirty_evictions, (uint64_t)4);
}

// ─── 6. Cycle counting ───────────────────────────────────────────────────────

static void test_hit_cycles() {
    CacheConfig cfg = make_l1();
    cfg.hit_cycles = 4;
    Cache c(cfg);
    c.access(0, false); // miss (cold)
    c.access(0, false); // hit
    // Total = miss_cycles + hit_cycles = (4+0) + 4 = 8
    EXPECT_EQ(c.stats().total_cycles, (uint64_t)8);
}

static void test_miss_penalty_cycles() {
    CacheConfig cfg = make_l1();
    cfg.hit_cycles   = 4;
    cfg.miss_penalty = 100;
    Cache c(cfg);
    c.access(0, false); // cold miss: 4 + 100 = 104 cycles
    EXPECT_EQ(c.stats().total_cycles, (uint64_t)104);
    c.access(0, false); // hit: 4 cycles
    EXPECT_EQ(c.stats().total_cycles, (uint64_t)108);
}

// ─── 7. Multi-level hierarchy ────────────────────────────────────────────────

static void test_hierarchy_l1_hit_no_l2() {
    CacheConfig l1 = make_l1();
    l1.hit_cycles = 4;
    CacheConfig l2;
    l2.name       = "L2";
    l2.size       = 8192;
    l2.block_size = 64;
    l2.ways       = 8;
    l2.hit_cycles = 12;
    l2.write      = WritePolicy::WRITE_BACK;
    l2.write_alloc= true;

    CacheHierarchy h({l1, l2}, /*mem_cycles=*/200);
    h.read(0); // cold: L1 miss → L2 miss → memory. Cycles = 4 + 12 + 200
    uint64_t c1 = h.stats().total_cycles;
    EXPECT_EQ(c1, (uint64_t)(4 + 12 + 200));

    uint64_t c2 = h.read(0); // L1 hit: cycles = 4
    EXPECT_EQ(c2, (uint64_t)4);
}

static void test_hierarchy_l1_miss_l2_hit() {
    CacheConfig l1 = make_l1();
    l1.hit_cycles = 4;
    CacheConfig l2;
    l2.name       = "L2";
    l2.size       = 8192;
    l2.block_size = 64;
    l2.ways       = 8;
    l2.hit_cycles = 12;
    l2.write      = WritePolicy::WRITE_BACK;
    l2.write_alloc= true;

    CacheHierarchy h({l1, l2}, /*mem_cycles=*/200);

    // First access: both miss → L1 + L2 + memory
    h.read(0);
    // Flush L1 by filling all 4 ways of set 0 with other blocks.
    for (int i = 1; i <= 4; i++) h.read(set0_addr(i));
    // Now L1 no longer has block 0, but L2 does.
    // Next access to 0: L1 miss (4 cycles) + L2 hit (12 cycles) = 16 cycles.
    uint64_t c = h.read(0);
    EXPECT_EQ(c, (uint64_t)(4 + 12));
    // L1 should now have block 0 again.
    EXPECT_TRUE(h.level(0).probe(0));
}

static void test_hierarchy_three_levels() {
    CacheConfig l1, l2, l3;
    l1.name="L1"; l1.size=1024;  l1.block_size=64; l1.ways=4; l1.hit_cycles=4;
    l1.write=WritePolicy::WRITE_BACK; l1.write_alloc=true;
    l2.name="L2"; l2.size=8192;  l2.block_size=64; l2.ways=8; l2.hit_cycles=12;
    l2.write=WritePolicy::WRITE_BACK; l2.write_alloc=true;
    l3.name="L3"; l3.size=65536; l3.block_size=64; l3.ways=16;l3.hit_cycles=40;
    l3.write=WritePolicy::WRITE_BACK; l3.write_alloc=true;

    CacheHierarchy h({l1,l2,l3}, /*mem=*/200);
    uint64_t c = h.read(0); // all miss: 4+12+40+200
    EXPECT_EQ(c, (uint64_t)(4 + 12 + 40 + 200));
    c = h.read(0); // L1 hit: 4
    EXPECT_EQ(c, (uint64_t)4);
}

static void test_hierarchy_dirty_writeback_to_l2() {
    CacheConfig l1 = make_l1();
    l1.hit_cycles = 4;
    CacheConfig l2;
    l2.name="L2"; l2.size=8192; l2.block_size=64; l2.ways=8;
    l2.hit_cycles=12; l2.write=WritePolicy::WRITE_BACK; l2.write_alloc=true;

    CacheHierarchy h({l1, l2}, 200);

    // Write to block 0 (allocates dirty in L1).
    h.write(0);
    // Fill L1 set 0 to force eviction of block 0.
    for (int i = 1; i <= 4; i++) h.read(set0_addr(i));
    // Block 0 was dirty → should have been written back to L2.
    EXPECT_TRUE(h.level(1).probe(0));
}

// ─── 8. RISC-V flavoured patterns ────────────────────────────────────────────

// Simulate a simple instruction fetch loop (sequential, then repeated).
static void test_rv_instruction_fetch_loop() {
    CacheConfig icache;
    icache.name       = "I$";
    icache.size       = 4096; // 4 KiB
    icache.block_size = 64;
    icache.ways       = 2;
    icache.repl       = ReplPolicy::LRU;
    icache.write      = WritePolicy::WRITE_BACK; // read-only in practice
    icache.write_alloc= false;
    icache.hit_cycles = 1;
    Cache c(icache);

    // Fetch 16 instructions (4B each), all within one cache line.
    for (int i = 0; i < 16; i++) c.access((uint32_t)i * 4, false);
    // Second pass: all hits.
    for (int i = 0; i < 16; i++) c.access((uint32_t)i * 4, false);

    EXPECT_EQ(c.stats().cold_misses, (uint64_t)1); // one cache line
    EXPECT_EQ(c.stats().hits, (uint64_t)15 + 16);  // 15 hits first pass + 16 second
}

// Simulate a simple store-then-load pattern (register file spill/fill).
static void test_rv_store_load_pattern() {
    CacheConfig dcache = make_l1();
    dcache.name      = "D$";
    dcache.hit_cycles= 2;
    Cache c(dcache);

    // Store 8 registers (each 4B) to stack frame at address 0x1000.
    for (int i = 0; i < 8; i++) c.access(0x1000 + (uint32_t)i * 4, true);
    // Load them back.
    for (int i = 0; i < 8; i++) c.access(0x1000 + (uint32_t)i * 4, false);

    // All stores and loads land in one 64B cache line.
    EXPECT_EQ(c.stats().cold_misses, (uint64_t)1);
    EXPECT_EQ(c.stats().writes, (uint64_t)8);
    EXPECT_EQ(c.stats().reads,  (uint64_t)8);
    // 15 hits (1 write miss, 7 write hits, 8 read hits).
    EXPECT_EQ(c.stats().hits, (uint64_t)15);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "Cache Simulator Test Suite\n";
    std::cout << std::string(50, '=') << "\n";

    suite("1. Basic hit/miss");
    test_cold_start_all_miss();
    test_repeat_access_hits();
    test_probe();

    suite("2. Replacement policies");
    test_lru_evicts_oldest();
    test_fifo_evicts_oldest_inserted();
    test_direct_mapped_conflict();

    suite("3. Miss classification");
    test_cold_misses_only_on_first_access();
    test_capacity_misses();
    test_conflict_vs_capacity();

    suite("4. Write policies");
    test_writeback_marks_dirty();
    test_write_through_no_dirty();
    test_write_alloc_on_write_miss();
    test_no_write_alloc_on_write_miss();

    suite("5. Eviction counting");
    test_eviction_count();
    test_dirty_eviction_count();

    suite("6. Cycle counting");
    test_hit_cycles();
    test_miss_penalty_cycles();

    suite("7. Multi-level hierarchy");
    test_hierarchy_l1_hit_no_l2();
    test_hierarchy_l1_miss_l2_hit();
    test_hierarchy_three_levels();
    test_hierarchy_dirty_writeback_to_l2();

    suite("8. RISC-V patterns");
    test_rv_instruction_fetch_loop();
    test_rv_store_load_pattern();

    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "Results: " << (g_total - g_failed) << " / " << g_total << " passed";
    if (g_failed == 0)
        std::cout << "  ✓ All tests passed!\n";
    else
        std::cout << "  ✗ " << g_failed << " test(s) FAILED\n";

    return g_failed ? 1 : 0;
}
