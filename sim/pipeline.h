#pragma once
#include "rv32i.h"
#include "memory.h"
#include "cache.h"
#include <array>
#include <vector>
#include <string>

// ─── Pipeline latches ─────────────────────────────────────────────────────────
// Each latch holds the output of the stage that writes it.
// 'valid = false' means the slot holds a bubble (NOP).

struct IF_ID {
    bool     valid = false;
    uint32_t pc    = 0;
    uint32_t raw   = 0;
};

struct ID_EX {
    bool        valid   = false;
    uint32_t    pc      = 0;
    DecodedInst inst    = {};
    int32_t     rs1_val = 0; // value read from register file (may be stale; forwarding fixes it)
    int32_t     rs2_val = 0;
};

struct EX_MEM {
    bool        valid          = false;
    uint32_t    pc             = 0;
    DecodedInst inst           = {};
    int32_t     alu_result     = 0;
    int32_t     rs2_val        = 0;  // store data (forwarded)
    bool        branch_taken   = false;
    uint32_t    branch_target  = 0;
    bool        reg_write      = false;
};

struct MEM_WB {
    bool        valid     = false;
    uint32_t    pc        = 0;
    DecodedInst inst      = {};
    int32_t     result    = 0;  // ALU result or loaded value
    bool        reg_write = false;
    uint32_t    rd        = 0;
};

// ─── Statistics ───────────────────────────────────────────────────────────────

struct CoreStats {
    uint64_t cycles            = 0;
    uint64_t instructions      = 0; // retired (WB stage)
    uint64_t data_stalls       = 0; // load-use stall cycles
    uint64_t branch_total      = 0;
    uint64_t branch_taken      = 0;
    uint64_t branch_mispred    = 0; // taken but predicted not-taken (or vice-versa with BTB)
    uint64_t i_cache_stalls    = 0; // extra cycles from I$ misses
    uint64_t d_cache_stalls    = 0; // extra cycles from D$ misses

    double ipc() const {
        return cycles ? (double)instructions / (double)cycles : 0.0;
    }
    void print() const;
};

// ─── BTB entry ────────────────────────────────────────────────────────────────

struct BTBEntry {
    bool     valid  = false;
    uint32_t tag    = 0;    // full PC tag
    uint32_t target = 0;
    uint8_t  ctr    = 1;   // 2-bit saturating counter; 0-1 = not-taken, 2-3 = taken
};

// ─── Pipeline configuration ───────────────────────────────────────────────────

struct PipelineConfig {
    bool     trace          = false; // print retired instruction each cycle
    bool     trace_pipeline = false; // print full pipeline state each cycle
    bool     btb_enable     = false; // enable branch target buffer
    uint32_t btb_size       = 256;  // entries (must be power of 2)
    uint64_t max_cycles     = 400'000'000ULL;
};

// ─── Core ─────────────────────────────────────────────────────────────────────

class Core {
public:
    // icache / dcache are separate CacheHierarchy instances (I$ and D$).
    Core(Memory& mem,
         CacheHierarchy& icache,
         CacheHierarchy& dcache,
         const PipelineConfig& cfg = {});

    // Advance the pipeline by one clock cycle.
    // Returns false when the simulation should stop (halt / tohost / max cycles).
    bool tick();

    // Run until halt or cfg.max_cycles.
    void run();

    // Initialise PC (call before run()).
    void set_pc(uint32_t pc) { pc_ = pc; }

    // Architectural state inspection.
    int32_t  reg(uint32_t r) const { return (r == 0) ? 0 : regs_[r]; }
    uint32_t pc()            const { return pc_; }

    bool halted()    const { return halted_; }
    int  exit_code() const { return exit_code_; }

    const CoreStats& stats() const { return stats_; }
    void print_stats() const;

    // Print register file in a 4-column layout.
    void dump_regs() const;

private:
    Memory&           mem_;
    CacheHierarchy&   icache_;
    CacheHierarchy&   dcache_;
    PipelineConfig    cfg_;
    CoreStats         stats_;

    // Architectural state
    uint32_t                 pc_       = 0;
    std::array<int32_t, 32>  regs_     = {};
    bool                     halted_   = false;
    int                      exit_code_= 0;

    // When a halt is triggered (ECALL / EBREAK / tohost write) we enter
    // "stopping" mode: stop fetching new instructions but let the pipeline
    // drain so in-flight instructions commit and update the register file.
    bool stopping_     = false;
    int  pending_exit_ = 0;

    // Pipeline latches (current state)
    IF_ID  if_id_  = {};
    ID_EX  id_ex_  = {};
    EX_MEM ex_mem_ = {};
    MEM_WB mem_wb_ = {};

    // BTB
    std::vector<BTBEntry> btb_;

    // ── Stage functions ───────────────────────────────────────────────────
    // Each writes into a "next" latch passed by reference.
    // Returns false if the stage should be treated as a stall this cycle.
    void stage_wb (MEM_WB& cur);
    void stage_mem(EX_MEM& cur, MEM_WB& next);
    void stage_ex (ID_EX&  cur, EX_MEM& next, bool& branch_flush, uint32_t& branch_target);
    void stage_id (IF_ID&  cur, ID_EX&  next);
    void stage_if (uint32_t fetch_pc, IF_ID& next);

    // Detect load-use hazard. Returns true → insert stall.
    bool detect_load_use() const;

    // Return the correctly forwarded value for register r.
    // Falls back to reg_val (from register file) if no forwarding applies.
    int32_t forward(uint32_t r, int32_t reg_val) const;

    // ALU operation. opcode distinguishes OP vs OP_IMM for SUB/SRA disambiguation.
    int32_t alu_op(uint32_t funct3, uint32_t funct7,
                   int32_t a, int32_t b, uint32_t opcode) const;

    // Branch condition evaluation.
    static bool eval_branch(uint32_t funct3, int32_t rs1, int32_t rs2);

    // BTB helpers.
    bool btb_predict(uint32_t pc, uint32_t& out_target) const;
    void btb_update (uint32_t pc, uint32_t target, bool taken);

    void write_reg(uint32_t rd, int32_t val) { if (rd != 0) regs_[rd] = val; }

    // Trace helpers.
    void print_trace(const MEM_WB& wb) const;
    void print_pipeline_state() const;
};
