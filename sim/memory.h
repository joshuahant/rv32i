#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

// Flat memory model for a bare-metal RV32I simulation.
//
// Default base address is 0x80000000 to match the standard riscv-tests linker
// script (rv32ui-p-*).  All physical addresses must fall within
// [base, base + size).
//
// The ELF loader populates the buffer from LOAD segments and locates the
// 'tohost' / 'fromhost' symbols used by riscv-tests to signal pass/fail.

class Memory {
public:
    static constexpr uint32_t DEFAULT_BASE = 0x80000000u;
    static constexpr uint32_t DEFAULT_SIZE = 64u * 1024u * 1024u; // 64 MiB

    explicit Memory(uint32_t base = DEFAULT_BASE,
                    uint32_t size = DEFAULT_SIZE);

    // Load an ELF32 file into memory.  Returns the entry point address.
    // Throws std::runtime_error on any parse error.
    uint32_t load_elf(const std::string& path);

    // Raw byte-level access (used by the cache miss handler / pipeline MEM
    // stage after the cache lookup).
    uint8_t  read8 (uint32_t addr) const;
    uint16_t read16(uint32_t addr) const;
    uint32_t read32(uint32_t addr) const;

    void write8 (uint32_t addr, uint8_t  val);
    void write16(uint32_t addr, uint16_t val);
    void write32(uint32_t addr, uint32_t val);

    // riscv-tests tohost/fromhost handshake addresses.
    uint32_t tohost_addr()   const { return tohost_;   }
    uint32_t fromhost_addr() const { return fromhost_; }
    bool     has_tohost()    const { return tohost_ != 0; }

    uint32_t base() const { return base_; }
    uint32_t size() const { return (uint32_t)mem_.size(); }

private:
    uint32_t             base_;
    std::vector<uint8_t> mem_;
    uint32_t             tohost_   = 0;
    uint32_t             fromhost_ = 0;

    uint8_t* ptr(uint32_t addr);
    const uint8_t* ptr(uint32_t addr) const;
};
