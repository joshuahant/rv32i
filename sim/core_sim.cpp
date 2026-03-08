// RV32I Core Simulator — entry point
//
// Usage:
//   ./build/core_sim [options] <elf>
//
// Options:
//   --trace              Print each retired instruction
//   --trace-pipeline     Print per-cycle pipeline state
//   --btb [N]            Enable BTB with N entries (default 256)
//   --max-cycles N       Stop after N cycles (default 100M)
//   --mem-base  ADDR     Memory base address in hex (default 0x80000000)
//   --mem-size  BYTES    Memory size in bytes (default 67108864 = 64 MiB)
//
// Cache defaults (configurable via future flags):
//   L1-I$: 32 KiB, 4-way, 64 B blocks, 1-cycle hit,  WRITE_BACK
//   L1-D$: 32 KiB, 4-way, 64 B blocks, 4-cycle hit,  WRITE_BACK
//   L2:    256 KiB,8-way, 64 B blocks, 12-cycle hit,  WRITE_BACK
//   DRAM:  200 cycles
//
// Exit codes:
//   0  pass (tohost == 1 or ecall/ebreak with a0 == 0)
//   N  fail (riscv-tests: failing test number N; ecall: a0 value)
//  -1  memory fault
//  -2  max cycles exceeded

#include "pipeline.h"
#include "memory.h"
#include "cache.h"
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options] <elf-file>\n"
              << "  --trace              Instruction-level trace\n"
              << "  --trace-pipeline     Per-cycle pipeline state\n"
              << "  --btb [N]            Enable BTB (N entries, default 256)\n"
              << "  --max-cycles N       Max simulation cycles (default 100M)\n"
              << "  --mem-base ADDR      Memory base in hex (default 0x80000000)\n"
              << "  --mem-size BYTES     Memory size in bytes (default 64M)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    PipelineConfig  pcfg;
    uint32_t        mem_base = Memory::DEFAULT_BASE;
    uint32_t        mem_size = Memory::DEFAULT_SIZE;
    const char*     elf_path = nullptr;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--trace")          == 0) pcfg.trace          = true;
        else if (strcmp(argv[i], "--trace-pipeline") == 0) pcfg.trace_pipeline = true;
        else if (strcmp(argv[i], "--btb")            == 0) {
            pcfg.btb_enable = true;
            if (i + 1 < argc && argv[i+1][0] != '-')
                pcfg.btb_size = (uint32_t)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--max-cycles") == 0 && i + 1 < argc)
            pcfg.max_cycles = (uint64_t)strtoull(argv[++i], nullptr, 10);
        else if (strcmp(argv[i], "--mem-base")   == 0 && i + 1 < argc)
            mem_base = (uint32_t)strtoul(argv[++i], nullptr, 16);
        else if (strcmp(argv[i], "--mem-size")   == 0 && i + 1 < argc)
            mem_size = (uint32_t)strtoul(argv[++i], nullptr, 10);
        else if (argv[i][0] != '-')
            elf_path = argv[i];
        else { std::cerr << "Unknown option: " << argv[i] << "\n"; usage(argv[0]); return 1; }
    }

    if (!elf_path) { usage(argv[0]); return 1; }

    // ── Memory ────────────────────────────────────────────────────────────
    Memory mem(mem_base, mem_size);
    uint32_t entry = 0;
    try {
        entry = mem.load_elf(elf_path);
    } catch (const std::exception& e) {
        std::cerr << "ELF load error: " << e.what() << "\n";
        return 1;
    }
    std::cout << "[sim] Loaded " << elf_path
              << "  entry=0x" << std::hex << entry << std::dec << "\n";
    if (mem.has_tohost())
        std::cout << "[sim] tohost @ 0x" << std::hex << mem.tohost_addr()
                  << std::dec << "\n";

    // ── Cache hierarchy ───────────────────────────────────────────────────
    // L1-I$ → L2 → DRAM
    CacheConfig l1i_cfg;
    l1i_cfg.name       = "L1-I$";
    l1i_cfg.size       = 32 * 1024;
    l1i_cfg.block_size = 64;
    l1i_cfg.ways       = 4;
    l1i_cfg.repl       = ReplPolicy::LRU;
    l1i_cfg.write      = WritePolicy::WRITE_BACK;
    l1i_cfg.write_alloc= true;
    l1i_cfg.hit_cycles = 1;

    CacheConfig l1d_cfg;
    l1d_cfg.name       = "L1-D$";
    l1d_cfg.size       = 32 * 1024;
    l1d_cfg.block_size = 64;
    l1d_cfg.ways       = 4;
    l1d_cfg.repl       = ReplPolicy::LRU;
    l1d_cfg.write      = WritePolicy::WRITE_BACK;
    l1d_cfg.write_alloc= true;
    l1d_cfg.hit_cycles = 4;

    CacheConfig l2_cfg;
    l2_cfg.name       = "L2";
    l2_cfg.size       = 256 * 1024;
    l2_cfg.block_size = 64;
    l2_cfg.ways       = 8;
    l2_cfg.repl       = ReplPolicy::LRU;
    l2_cfg.write      = WritePolicy::WRITE_BACK;
    l2_cfg.write_alloc= true;
    l2_cfg.hit_cycles = 12;

    CacheHierarchy icache({l1i_cfg, l2_cfg}, /*mem_cycles=*/200);
    CacheHierarchy dcache({l1d_cfg, l2_cfg}, /*mem_cycles=*/200);

    // ── Core ──────────────────────────────────────────────────────────────
    Core core(mem, icache, dcache, pcfg);
    core.set_pc(entry);

    std::cout << "[sim] Starting simulation...\n\n";
    core.run();

    // ── Results ───────────────────────────────────────────────────────────
    std::cout << "\n";
    int code = core.exit_code();
    if (code == 0)
        std::cout << "[sim] PASS\n\n";
    else if (code == -2)
        std::cout << "[sim] TIMEOUT (max cycles reached)\n\n";
    else if (code == -1)
        std::cout << "[sim] FAULT (memory out of range)\n\n";
    else
        std::cout << "[sim] FAIL  (test " << code << ")\n\n";

    core.dump_regs();
    core.print_stats();
    icache.print_stats();
    dcache.print_stats();

    return (code == 0) ? 0 : 1;
}
