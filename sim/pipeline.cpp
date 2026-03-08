#include "pipeline.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cassert>
#include <cstring>

// ─── CoreStats::print ─────────────────────────────────────────────────────────

void CoreStats::print() const {
    std::cout << "=== Core Statistics ===\n";
    std::cout << "  Cycles:              " << cycles       << "\n";
    std::cout << "  Instructions:        " << instructions << "\n";
    std::cout << std::fixed << std::setprecision(3)
              << "  IPC:                 " << ipc()        << "\n";
    std::cout << "  Data stall cycles:   " << data_stalls  << "\n";
    std::cout << "  I$ stall cycles:     " << i_cache_stalls << "\n";
    std::cout << "  D$ stall cycles:     " << d_cache_stalls << "\n";
    std::cout << "  Branches total:      " << branch_total   << "\n";
    std::cout << "  Branches taken:      " << branch_taken   << "\n";
    std::cout << "  Branch mispredicts:  " << branch_mispred << "\n";
    if (branch_total > 0)
        std::cout << "  Branch mispredict %: " << std::fixed << std::setprecision(1)
                  << 100.0 * branch_mispred / branch_total << "%\n";
    std::cout << "\n";
}

// ─── Core construction ────────────────────────────────────────────────────────

Core::Core(Memory& mem, CacheHierarchy& icache, CacheHierarchy& dcache,
           const PipelineConfig& cfg)
    : mem_(mem), icache_(icache), dcache_(dcache), cfg_(cfg) {
    regs_.fill(0);
    if (cfg_.btb_enable) {
        uint32_t sz = cfg_.btb_size;
        if (sz == 0 || (sz & (sz - 1)) != 0)
            throw std::invalid_argument("BTB size must be a power of 2");
        btb_.resize(sz);
    }
}

// ─── ALU ─────────────────────────────────────────────────────────────────────

int32_t Core::alu_op(uint32_t f3, uint32_t f7,
                     int32_t a, int32_t b, uint32_t opcode) const {
    bool is_imm = (opcode == Op::OP_IMM);
    bool alt    = (f7 == 0x20) && !is_imm;          // SUB, SRA (reg ops only)
    bool alt_sh = (f7 == 0x20);                     // SRAI also has f7=0x20

    switch (f3) {
        case F3::ADD:  return alt ? (a - b) : (a + b);
        case F3::SLL:  return a << (b & 0x1F);
        case F3::SLT:  return (a < b) ? 1 : 0;
        case F3::SLTU: return ((uint32_t)a < (uint32_t)b) ? 1 : 0;
        case F3::XOR:  return a ^ b;
        case F3::SR:   return alt_sh ? (a >> (b & 0x1F))
                                     : (int32_t)((uint32_t)a >> (b & 0x1F));
        case F3::OR:   return a | b;
        case F3::AND:  return a & b;
        default:       return 0;
    }
}

// ─── Branch condition ─────────────────────────────────────────────────────────

bool Core::eval_branch(uint32_t f3, int32_t a, int32_t b) {
    switch (f3) {
        case F3::BEQ:  return a == b;
        case F3::BNE:  return a != b;
        case F3::BLT:  return a < b;
        case F3::BGE:  return a >= b;
        case F3::BLTU: return (uint32_t)a < (uint32_t)b;
        case F3::BGEU: return (uint32_t)a >= (uint32_t)b;
        default:       return false;
    }
}

// ─── Forwarding ───────────────────────────────────────────────────────────────
// Priority: EX/MEM (more recent) > MEM/WB.

int32_t Core::forward(uint32_t r, int32_t reg_val) const {
    if (r == 0) return 0;
    // EX/MEM forwarding (instruction two cycles ahead has already computed its ALU result)
    if (ex_mem_.valid && ex_mem_.reg_write && ex_mem_.inst.rd == r)
        return ex_mem_.alu_result;
    // MEM/WB forwarding
    if (mem_wb_.valid && mem_wb_.reg_write && mem_wb_.rd == r)
        return mem_wb_.result;
    return reg_val;
}

// ─── Load-use hazard detection ────────────────────────────────────────────────
// The instruction currently in ID/EX is a LOAD.  The instruction currently in
// IF/ID (about to enter ID) reads the register being loaded.  We must stall
// for one cycle so the loaded value is available in MEM/WB for forwarding.

bool Core::detect_load_use() const {
    if (!id_ex_.valid || !id_ex_.inst.is_load()) return false;
    uint32_t load_rd = id_ex_.inst.rd;
    if (load_rd == 0) return false;

    // Check what rs1/rs2 the IF/ID instruction needs.
    if (!if_id_.valid) return false;
    // Peek at the raw instruction in IF/ID to extract rs1/rs2 without full decode.
    uint32_t raw = if_id_.raw;
    uint32_t rs1 = (raw >> 15) & 0x1F;
    uint32_t rs2 = (raw >> 20) & 0x1F;
    uint32_t op  = raw & 0x7F;
    // Stores use rs1 as base and rs2 as data; branches use both.
    // U-type and J-type don't use rs1/rs2.
    bool uses_rs1 = (op != Op::LUI && op != Op::AUIPC && op != Op::JAL);
    bool uses_rs2 = (op == Op::OP || op == Op::BRANCH || op == Op::STORE);

    return (uses_rs1 && rs1 == load_rd) || (uses_rs2 && rs2 == load_rd);
}

// ─── BTB ─────────────────────────────────────────────────────────────────────

bool Core::btb_predict(uint32_t pc, uint32_t& out_target) const {
    if (!cfg_.btb_enable || btb_.empty()) return false;
    uint32_t idx = (pc >> 2) & (btb_.size() - 1);
    const BTBEntry& e = btb_[idx];
    if (!e.valid || e.tag != pc) return false;
    if (e.ctr < 2) return false; // predict not-taken
    out_target = e.target;
    return true;
}

void Core::btb_update(uint32_t pc, uint32_t target, bool taken) {
    if (!cfg_.btb_enable || btb_.empty()) return;
    uint32_t idx = (pc >> 2) & (btb_.size() - 1);
    BTBEntry& e = btb_[idx];
    e.valid  = true;
    e.tag    = pc;
    e.target = target;
    if (taken)  e.ctr = (e.ctr < 3) ? e.ctr + 1 : 3;
    else        e.ctr = (e.ctr > 0) ? e.ctr - 1 : 0;
}

// ─── Stage: WB ───────────────────────────────────────────────────────────────

void Core::stage_wb(MEM_WB& cur) {
    if (!cur.valid) return;
    if (cur.reg_write) write_reg(cur.rd, cur.result);
    stats_.instructions++;
    if (cfg_.trace) print_trace(cur);
}

// ─── Stage: MEM ──────────────────────────────────────────────────────────────

void Core::stage_mem(EX_MEM& cur, MEM_WB& next) {
    next = {};
    if (!cur.valid) return;

    next.valid     = true;
    next.pc        = cur.pc;
    next.inst      = cur.inst;
    next.reg_write = cur.reg_write;
    next.rd        = cur.inst.rd;
    next.result    = cur.alu_result; // default; overridden on load

    if (cur.inst.is_load()) {
        uint32_t addr = (uint32_t)cur.alu_result;
        // D$ access; result cycles drives stall accounting.
        uint64_t lat = dcache_.read(addr);
        if (lat > 1) { stats_.d_cache_stalls += lat - 1; stats_.cycles += lat - 1; }

        switch (cur.inst.funct3) {
            case F3::LB:  next.result = (int8_t) mem_.read8(addr);  break;
            case F3::LH:  next.result = (int16_t)mem_.read16(addr); break;
            case F3::LW:  next.result = (int32_t)mem_.read32(addr); break;
            case F3::LBU: next.result = (int32_t)mem_.read8(addr);  break;
            case F3::LHU: next.result = (int32_t)mem_.read16(addr); break;
            default: break;
        }

    } else if (cur.inst.is_store()) {
        uint32_t addr = (uint32_t)cur.alu_result;
        // D$ write.
        uint64_t lat = dcache_.write(addr);
        if (lat > 1) { stats_.d_cache_stalls += lat - 1; stats_.cycles += lat - 1; }

        // Forward rs2 value for the store data.
        int32_t store_data = forward(cur.inst.rs2, cur.rs2_val);

        switch (cur.inst.funct3) {
            case F3::SB: mem_.write8 (addr, (uint8_t) store_data); break;
            case F3::SH: mem_.write16(addr, (uint16_t)store_data); break;
            case F3::SW: mem_.write32(addr, (uint32_t)store_data); break;
            default: break;
        }

        // Check tohost write (riscv-tests pass/fail signal).
        if (mem_.has_tohost() && addr == mem_.tohost_addr()) {
            uint32_t val = (uint32_t)store_data;
            if (!stopping_) {
                stopping_     = true;
                pending_exit_ = (val == 1) ? 0 : (int)(val >> 1);
            }
        }
        next.reg_write = false; // stores don't write rd
    }
}

// ─── Stage: EX ───────────────────────────────────────────────────────────────

void Core::stage_ex(ID_EX& cur, EX_MEM& next,
                    bool& branch_flush, uint32_t& branch_target) {
    next = {};
    branch_flush  = false;
    branch_target = 0;
    if (!cur.valid) return;

    const DecodedInst& inst = cur.inst;
    next.valid     = true;
    next.pc        = cur.pc;
    next.inst      = inst;
    next.reg_write = inst.writes_rd();
    next.rs2_val   = forward(inst.rs2, cur.rs2_val);

    // Resolve forwarded operands.
    int32_t a = forward(inst.rs1, cur.rs1_val);
    int32_t b = forward(inst.rs2, cur.rs2_val);

    switch (inst.opcode) {
        case Op::LUI:
            next.alu_result = inst.imm;
            break;

        case Op::AUIPC:
            next.alu_result = (int32_t)((uint32_t)cur.pc + (uint32_t)inst.imm);
            break;

        case Op::JAL:
            next.alu_result  = (int32_t)((uint32_t)cur.pc + 4u); // link address
            branch_target    = (uint32_t)cur.pc + (uint32_t)inst.imm;
            branch_flush     = true;
            stats_.branch_taken++;
            stats_.branch_mispred++; // always-not-taken predicted fall-through
            break;

        case Op::JALR:
            next.alu_result  = (int32_t)((uint32_t)cur.pc + 4u);
            branch_target    = (uint32_t)(a + inst.imm) & ~1u;
            branch_flush     = true;
            stats_.branch_taken++;
            stats_.branch_mispred++;
            break;

        case Op::BRANCH: {
            stats_.branch_total++;
            bool taken   = eval_branch(inst.funct3, a, b);
            uint32_t tgt = (uint32_t)cur.pc + (uint32_t)inst.imm;

            btb_update(cur.pc, tgt, taken);

            // Determine what we predicted in IF.
            uint32_t btb_tgt  = 0;
            bool     predicted = btb_predict(cur.pc, btb_tgt); // true=taken predicted

            bool mispredicted = (taken != predicted) ||
                                (taken && predicted && btb_tgt != tgt);

            if (taken) stats_.branch_taken++;
            if (mispredicted) stats_.branch_mispred++;

            if (mispredicted) {
                branch_flush  = true;
                branch_target = taken ? tgt : (uint32_t)cur.pc + 4u;
            }
            next.reg_write = false; // branches don't write rd
            break;
        }

        case Op::LOAD:
            // EX computes the load address; MEM does the actual access.
            next.alu_result = a + inst.imm;
            break;

        case Op::STORE:
            // EX computes store address; store data forwarding done in MEM.
            next.alu_result = a + inst.imm;
            next.reg_write  = false;
            break;

        case Op::OP_IMM:
            next.alu_result = alu_op(inst.funct3, inst.funct7, a, inst.imm, Op::OP_IMM);
            break;

        case Op::OP:
            next.alu_result = alu_op(inst.funct3, inst.funct7, a, b, Op::OP);
            break;

        case Op::MISC_MEM: // FENCE — treat as NOP in this model
            next.reg_write = false;
            break;

        case Op::SYSTEM:
            // ECALL / EBREAK: trigger a drain — stop fetching but let
            // in-flight instructions (including this one) commit via WB.
            if (!stopping_) {
                stopping_     = true;
                pending_exit_ = (int)reg(10); // a0 = exit code by convention
            }
            next.reg_write = false;
            break;

        default:
            next.reg_write = false;
            break;
    }
}

// ─── Stage: ID ───────────────────────────────────────────────────────────────

void Core::stage_id(IF_ID& cur, ID_EX& next) {
    next = {};
    if (!cur.valid) return;

    DecodedInst inst = decode(cur.raw, cur.pc);

    next.valid   = true;
    next.pc      = cur.pc;
    next.inst    = inst;
    next.rs1_val = reg(inst.rs1); // may be stale; forwarding in EX fixes it
    next.rs2_val = reg(inst.rs2);
}

// ─── Stage: IF ───────────────────────────────────────────────────────────────

void Core::stage_if(uint32_t fetch_pc, IF_ID& next) {
    next = {};

    // I$ access.
    uint64_t lat = icache_.read(fetch_pc);
    if (lat > 1) { stats_.i_cache_stalls += lat - 1; stats_.cycles += lat - 1; }

    next.valid = true;
    next.pc    = fetch_pc;
    try {
        next.raw = mem_.read32(fetch_pc);
    } catch (const std::out_of_range&) {
        // PC out of range → treat as invalid / halt.
        next.valid = false;
        halted_    = true;
        exit_code_ = -1;
    }
}

// ─── tick ─────────────────────────────────────────────────────────────────────
// Computes the next state of all latches from the current state, then commits.

bool Core::tick() {
    if (halted_) return false;
    stats_.cycles++;

    if (cfg_.trace_pipeline) print_pipeline_state();

    // ── Stopping (drain) mode: pipeline is draining after a halt event ────
    // Only retire instructions already past EX (in MEM/WB). Discard anything
    // in ID/EX and IF/ID — those were speculatively fetched and should not
    // be counted (the halt instruction was in EX when stopping_ was set).
    if (stopping_) {
        MEM_WB next_mem_wb = {};
        stage_wb(mem_wb_);
        stage_mem(ex_mem_, next_mem_wb);
        mem_wb_ = next_mem_wb;
        ex_mem_ = {};  // nothing new enters EX during drain
        id_ex_  = {};
        if_id_  = {};

        if (!mem_wb_.valid) {
            halted_    = true;
            exit_code_ = pending_exit_;
            return false;
        }
        return true;
    }

    // ── Detect load-use hazard ────────────────────────────────────────────
    bool load_stall = detect_load_use();
    if (load_stall) stats_.data_stalls++;

    // ── Compute next latch state ──────────────────────────────────────────
    MEM_WB  next_mem_wb = {};
    EX_MEM  next_ex_mem = {};
    ID_EX   next_id_ex  = {};
    IF_ID   next_if_id  = {};

    bool     branch_flush  = false;
    uint32_t branch_target = 0;

    // WB: commits result to register file, counts retired instructions.
    stage_wb(mem_wb_);

    // MEM: loads/stores, tohost check.
    stage_mem(ex_mem_, next_mem_wb);

    // EX: ALU, branch resolution.
    stage_ex(id_ex_, next_ex_mem, branch_flush, branch_target);

    if (!load_stall) {
        // ID: decode, register file read.
        stage_id(if_id_, next_id_ex);

        // IF: fetch next instruction.
        uint32_t fetch_pc = pc_;
        stage_if(fetch_pc, next_if_id);
        pc_ = fetch_pc + 4; // advance PC for next cycle's IF
    } else {
        // Stall: hold IF/ID and PC; insert bubble into ID/EX.
        next_if_id = if_id_;
        next_id_ex = {}; // bubble
    }

    // ── Branch flush (overrides stall for IF/ID and ID/EX) ───────────────
    if (branch_flush) {
        next_if_id = {};
        next_id_ex = {};
        pc_        = branch_target;
    }

    // ── Commit ────────────────────────────────────────────────────────────
    mem_wb_ = next_mem_wb;
    ex_mem_ = next_ex_mem;
    id_ex_  = next_id_ex;
    if_id_  = next_if_id;

    return stats_.cycles < cfg_.max_cycles;
}

// ─── run ─────────────────────────────────────────────────────────────────────

void Core::run() {
    while (tick()) {}
    if (stats_.cycles >= cfg_.max_cycles && !halted_) {
        std::cerr << "[sim] max cycles (" << cfg_.max_cycles << ") reached\n";
        exit_code_ = -2;
    }
}

// ─── Trace helpers ────────────────────────────────────────────────────────────

void Core::print_trace(const MEM_WB& wb) const {
    std::cout << "[" << std::setw(10) << stats_.cycles << "] "
              << std::hex << std::setw(8) << std::setfill('0') << wb.pc
              << std::setfill(' ') << std::dec
              << "  " << wb.inst.disasm();
    if (wb.reg_write && wb.rd != 0)
        std::cout << "  =>  " << reg_name(wb.rd)
                  << " = 0x" << std::hex << wb.result << std::dec;
    std::cout << "\n";
}

void Core::print_pipeline_state() const {
    auto stage_str = [](const char* name, bool valid, uint32_t pc,
                        const std::string& dis) {
        std::cout << "  " << std::setw(4) << name << ": ";
        if (valid)
            std::cout << std::hex << std::setw(8) << std::setfill('0') << pc
                      << std::setfill(' ') << std::dec << "  " << dis;
        else
            std::cout << "<bubble>";
        std::cout << "\n";
    };

    std::cout << "--- cycle " << stats_.cycles << " ---\n";
    stage_str("IF",  if_id_.valid,  if_id_.pc,  "(fetch)");
    stage_str("ID",  id_ex_.valid,  id_ex_.pc,  id_ex_.inst.disasm());
    stage_str("EX",  ex_mem_.valid, ex_mem_.pc, ex_mem_.inst.disasm());
    stage_str("MEM", mem_wb_.valid, mem_wb_.pc, mem_wb_.inst.disasm());
    // WB is mem_wb_ from previous cycle — just show what committed this cycle.
}

// ─── print_stats ─────────────────────────────────────────────────────────────

void Core::print_stats() const { stats_.print(); }

void Core::dump_regs() const {
    std::cout << "=== Register File ===\n";
    for (int i = 0; i < 32; i++) {
        std::cout << std::setw(4) << reg_name(i) << " = "
                  << std::hex << std::setw(8) << std::setfill('0')
                  << (uint32_t)reg(i) << std::dec << std::setfill(' ');
        if ((i & 3) == 3) std::cout << "\n"; else std::cout << "  ";
    }
    std::cout << "\n";
}
