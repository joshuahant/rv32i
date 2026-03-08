#pragma once
#include <cstdint>
#include <string>

// ─── Opcodes (bits 6:0) ───────────────────────────────────────────────────────
namespace Op {
    constexpr uint32_t LUI      = 0x37;
    constexpr uint32_t AUIPC    = 0x17;
    constexpr uint32_t JAL      = 0x6F;
    constexpr uint32_t JALR     = 0x67;
    constexpr uint32_t BRANCH   = 0x63;
    constexpr uint32_t LOAD     = 0x03;
    constexpr uint32_t STORE    = 0x23;
    constexpr uint32_t OP_IMM   = 0x13;
    constexpr uint32_t OP       = 0x33;
    constexpr uint32_t MISC_MEM = 0x0F; // FENCE
    constexpr uint32_t SYSTEM   = 0x73; // ECALL / EBREAK / CSR
}

// ─── funct3 values ───────────────────────────────────────────────────────────
namespace F3 {
    // Branch
    constexpr uint32_t BEQ  = 0; constexpr uint32_t BNE  = 1;
    constexpr uint32_t BLT  = 4; constexpr uint32_t BGE  = 5;
    constexpr uint32_t BLTU = 6; constexpr uint32_t BGEU = 7;
    // Load
    constexpr uint32_t LB = 0; constexpr uint32_t LH  = 1;
    constexpr uint32_t LW = 2; constexpr uint32_t LBU = 4;
    constexpr uint32_t LHU = 5;
    // Store
    constexpr uint32_t SB = 0; constexpr uint32_t SH = 1; constexpr uint32_t SW = 2;
    // ALU (OP / OP_IMM)
    constexpr uint32_t ADD  = 0; constexpr uint32_t SLL  = 1;
    constexpr uint32_t SLT  = 2; constexpr uint32_t SLTU = 3;
    constexpr uint32_t XOR  = 4; constexpr uint32_t SR   = 5;
    constexpr uint32_t OR   = 6; constexpr uint32_t AND  = 7;
}

// ─── Instruction format type ─────────────────────────────────────────────────
enum class InstType { R, I, S, B, U, J, INVALID };

// ─── Decoded instruction ─────────────────────────────────────────────────────
struct DecodedInst {
    uint32_t raw    = 0;
    uint32_t pc     = 0;
    uint32_t opcode = 0;
    uint32_t rd     = 0;
    uint32_t rs1    = 0;
    uint32_t rs2    = 0;
    uint32_t funct3 = 0;
    uint32_t funct7 = 0;
    int32_t  imm    = 0;
    InstType type   = InstType::INVALID;
    bool     valid  = false;

    bool is_load()   const { return valid && opcode == Op::LOAD;   }
    bool is_store()  const { return valid && opcode == Op::STORE;  }
    bool is_branch() const { return valid && opcode == Op::BRANCH; }
    bool is_jump()   const { return valid && (opcode == Op::JAL || opcode == Op::JALR); }

    // True if this instruction writes to rd (and rd matters).
    bool writes_rd() const {
        if (!valid || rd == 0) return false;
        return opcode != Op::STORE && opcode != Op::BRANCH && opcode != Op::MISC_MEM;
    }

    std::string disasm() const;
};

// Decode a 32-bit instruction word.
DecodedInst decode(uint32_t raw, uint32_t pc);

// ABI register names (zero, ra, sp, …, t6).
inline const char* reg_name(uint32_t r) {
    static const char* n[] = {
        "zero","ra","sp","gp","tp","t0","t1","t2",
        "s0",  "s1","a0","a1","a2","a3","a4","a5",
        "a6",  "a7","s2","s3","s4","s5","s6","s7",
        "s8",  "s9","s10","s11","t3","t4","t5","t6"
    };
    return (r < 32) ? n[r] : "??";
}
