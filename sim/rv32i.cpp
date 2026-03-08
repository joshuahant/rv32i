#include "rv32i.h"
#include <sstream>
#include <iomanip>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static int32_t sext(uint32_t val, int bits) {
    int shift = 32 - bits;
    return (int32_t)((int32_t)(val << shift) >> shift);
}

// ─── decode ───────────────────────────────────────────────────────────────────

DecodedInst decode(uint32_t raw, uint32_t pc) {
    DecodedInst d;
    d.raw    = raw;
    d.pc     = pc;
    d.opcode = raw & 0x7F;
    d.rd     = (raw >>  7) & 0x1F;
    d.rs1    = (raw >> 15) & 0x1F;
    d.rs2    = (raw >> 20) & 0x1F;
    d.funct3 = (raw >> 12) & 0x07;
    d.funct7 = (raw >> 25) & 0x7F;

    switch (d.opcode) {
        case Op::LUI:
        case Op::AUIPC:
            d.type  = InstType::U;
            d.imm   = (int32_t)(raw & 0xFFFFF000u);
            d.valid = true;
            break;

        case Op::JAL:
            d.type = InstType::J;
            d.imm  = sext(
                (((raw >> 31) & 1u) << 20) |
                (((raw >> 12) & 0xFFu) << 12) |
                (((raw >> 20) & 1u) << 11) |
                (((raw >> 21) & 0x3FFu) << 1),
                21);
            d.valid = true;
            break;

        case Op::JALR:
        case Op::LOAD:
        case Op::OP_IMM:
        case Op::MISC_MEM:
        case Op::SYSTEM:
            d.type  = InstType::I;
            d.imm   = sext(raw >> 20, 12);
            d.valid = true;
            break;

        case Op::STORE:
            d.type  = InstType::S;
            d.imm   = sext(((raw >> 25) << 5) | ((raw >> 7) & 0x1F), 12);
            d.valid = true;
            break;

        case Op::BRANCH:
            d.type = InstType::B;
            d.imm  = sext(
                (((raw >> 31) & 1u) << 12) |
                (((raw >>  7) & 1u) << 11) |
                (((raw >> 25) & 0x3Fu) << 5) |
                (((raw >>  8) & 0xFu)  << 1),
                13);
            d.valid = true;
            break;

        case Op::OP:
            d.type  = InstType::R;
            d.imm   = 0;
            d.valid = true;
            break;

        default:
            d.type  = InstType::INVALID;
            d.valid = false;
            break;
    }

    return d;
}

// ─── disasm ───────────────────────────────────────────────────────────────────

std::string DecodedInst::disasm() const {
    if (!valid) return "invalid";

    std::ostringstream o;
    auto rd_s   = reg_name(rd);
    auto rs1_s  = reg_name(rs1);
    auto rs2_s  = reg_name(rs2);

    switch (opcode) {
        case Op::LUI:
            o << "lui " << rd_s << ", " << (imm >> 12); break;
        case Op::AUIPC:
            o << "auipc " << rd_s << ", " << (imm >> 12); break;
        case Op::JAL:
            if (rd == 0)
                o << "j " << std::hex << std::showbase << (pc + imm);
            else
                o << "jal " << rd_s << ", " << std::hex << std::showbase << (pc + imm);
            break;
        case Op::JALR:
            if (rd == 0 && rs1 == 1 && imm == 0)
                o << "ret";
            else
                o << "jalr " << rd_s << ", " << imm << "(" << rs1_s << ")";
            break;
        case Op::BRANCH: {
            const char* mn = (funct3 == F3::BEQ)  ? "beq"  :
                             (funct3 == F3::BNE)  ? "bne"  :
                             (funct3 == F3::BLT)  ? "blt"  :
                             (funct3 == F3::BGE)  ? "bge"  :
                             (funct3 == F3::BLTU) ? "bltu" :
                             (funct3 == F3::BGEU) ? "bgeu" : "b?";
            o << mn << " " << rs1_s << ", " << rs2_s << ", "
              << std::hex << std::showbase << (uint32_t)(pc + imm);
            break;
        }
        case Op::LOAD: {
            const char* mn = (funct3 == F3::LB)  ? "lb"  :
                             (funct3 == F3::LH)  ? "lh"  :
                             (funct3 == F3::LW)  ? "lw"  :
                             (funct3 == F3::LBU) ? "lbu" :
                             (funct3 == F3::LHU) ? "lhu" : "l?";
            o << mn << " " << rd_s << ", " << imm << "(" << rs1_s << ")";
            break;
        }
        case Op::STORE: {
            const char* mn = (funct3 == F3::SB) ? "sb" :
                             (funct3 == F3::SH) ? "sh" :
                             (funct3 == F3::SW) ? "sw" : "s?";
            o << mn << " " << rs2_s << ", " << imm << "(" << rs1_s << ")";
            break;
        }
        case Op::OP_IMM: {
            const char* mn = (funct3 == F3::ADD)  ? "addi"  :
                             (funct3 == F3::SLL)  ? "slli"  :
                             (funct3 == F3::SLT)  ? "slti"  :
                             (funct3 == F3::SLTU) ? "sltiu" :
                             (funct3 == F3::XOR)  ? "xori"  :
                             (funct3 == F3::SR)   ? (funct7 & 0x20 ? "srai" : "srli") :
                             (funct3 == F3::OR)   ? "ori"   :
                             (funct3 == F3::AND)  ? "andi"  : "?i";
            if (funct3 == F3::SLL || funct3 == F3::SR)
                o << mn << " " << rd_s << ", " << rs1_s << ", " << (imm & 0x1F);
            else
                o << mn << " " << rd_s << ", " << rs1_s << ", " << imm;
            break;
        }
        case Op::OP: {
            bool alt = (funct7 == 0x20);
            const char* mn = (funct3 == F3::ADD)  ? (alt ? "sub"  : "add")  :
                             (funct3 == F3::SLL)  ? "sll"  :
                             (funct3 == F3::SLT)  ? "slt"  :
                             (funct3 == F3::SLTU) ? "sltu" :
                             (funct3 == F3::XOR)  ? "xor"  :
                             (funct3 == F3::SR)   ? (alt ? "sra" : "srl") :
                             (funct3 == F3::OR)   ? "or"   :
                             (funct3 == F3::AND)  ? "and"  : "?";
            o << mn << " " << rd_s << ", " << rs1_s << ", " << rs2_s;
            break;
        }
        case Op::MISC_MEM:
            o << "fence"; break;
        case Op::SYSTEM:
            if (imm == 0) o << "ecall";
            else if (imm == 1) o << "ebreak";
            else o << "csr? " << std::hex << imm;
            break;
        default:
            o << "unknown(0x" << std::hex << raw << ")";
    }

    return o.str();
}
