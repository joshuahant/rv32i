#include "memory.h"
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstdio>

// ─── ELF32 structures (little-endian RISC-V) ─────────────────────────────────

struct Elf32_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf32_Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};

struct Elf32_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
};

struct Elf32_Sym {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
};

static constexpr uint32_t PT_LOAD   = 1;
static constexpr uint32_t SHT_SYMTAB= 2;
static constexpr uint32_t SHT_STRTAB= 3;
static const uint8_t ELF_MAGIC[4]   = { 0x7F, 'E', 'L', 'F' };

// ─── Memory ───────────────────────────────────────────────────────────────────

Memory::Memory(uint32_t base, uint32_t size) : base_(base), mem_(size, 0) {}

uint8_t* Memory::ptr(uint32_t addr) {
    if (addr < base_ || addr >= base_ + (uint32_t)mem_.size())
        throw std::out_of_range("Memory address 0x" +
            std::to_string(addr) + " out of range");
    return mem_.data() + (addr - base_);
}

const uint8_t* Memory::ptr(uint32_t addr) const {
    if (addr < base_ || addr >= base_ + (uint32_t)mem_.size())
        throw std::out_of_range("Memory address 0x" +
            std::to_string(addr) + " out of range");
    return mem_.data() + (addr - base_);
}

uint8_t  Memory::read8 (uint32_t a) const { return *ptr(a); }
uint16_t Memory::read16(uint32_t a) const {
    uint16_t v; memcpy(&v, ptr(a), 2); return v;
}
uint32_t Memory::read32(uint32_t a) const {
    uint32_t v; memcpy(&v, ptr(a), 4); return v;
}
void Memory::write8 (uint32_t a, uint8_t  v) { *ptr(a) = v; }
void Memory::write16(uint32_t a, uint16_t v) { memcpy(ptr(a), &v, 2); }
void Memory::write32(uint32_t a, uint32_t v) { memcpy(ptr(a), &v, 4); }

// ─── ELF loader ───────────────────────────────────────────────────────────────

uint32_t Memory::load_elf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open ELF: " + path);

    // Read entire file into a buffer for random access.
    std::vector<uint8_t> buf(std::istreambuf_iterator<char>(f), {});
    if (buf.size() < sizeof(Elf32_Ehdr))
        throw std::runtime_error("ELF file too small");

    // Validate magic.
    if (memcmp(buf.data(), ELF_MAGIC, 4) != 0)
        throw std::runtime_error("Not an ELF file");
    if (buf[4] != 1) // EI_CLASS = ELFCLASS32
        throw std::runtime_error("Not a 32-bit ELF");
    if (buf[5] != 1) // EI_DATA = ELFDATA2LSB
        throw std::runtime_error("Not a little-endian ELF");

    Elf32_Ehdr ehdr;
    memcpy(&ehdr, buf.data(), sizeof(ehdr));

    if (ehdr.e_machine != 0xF3) // EM_RISCV
        throw std::runtime_error("Not a RISC-V ELF");

    // ── Load LOAD segments ────────────────────────────────────────────────
    for (int i = 0; i < ehdr.e_phnum; i++) {
        size_t off = ehdr.e_phoff + i * ehdr.e_phentsize;
        if (off + sizeof(Elf32_Phdr) > buf.size())
            throw std::runtime_error("ELF program header out of bounds");

        Elf32_Phdr ph;
        memcpy(&ph, buf.data() + off, sizeof(ph));
        if (ph.p_type != PT_LOAD) continue;

        // Zero-fill [vaddr, vaddr+memsz) then copy [vaddr, vaddr+filesz).
        for (uint32_t b = 0; b < ph.p_memsz; b++)
            write8(ph.p_vaddr + b, 0);
        for (uint32_t b = 0; b < ph.p_filesz; b++)
            write8(ph.p_vaddr + b, buf[ph.p_offset + b]);
    }

    // ── Find symbol table to locate tohost / fromhost ─────────────────────
    // We need the section header string table first.
    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0) return ehdr.e_entry;

    auto read_shdr = [&](int idx) {
        Elf32_Shdr sh;
        memcpy(&sh, buf.data() + ehdr.e_shoff + idx * ehdr.e_shentsize, sizeof(sh));
        return sh;
    };

    // Find .symtab and its associated .strtab.
    for (int i = 0; i < ehdr.e_shnum; i++) {
        Elf32_Shdr sh = read_shdr(i);
        if (sh.sh_type != SHT_SYMTAB) continue;

        Elf32_Shdr strtab_sh = read_shdr(sh.sh_link);
        if (strtab_sh.sh_type != SHT_STRTAB) continue;

        const char* strtab = reinterpret_cast<const char*>(
            buf.data() + strtab_sh.sh_offset);
        size_t strtab_size = strtab_sh.sh_size;

        uint32_t nsyms = sh.sh_size / sizeof(Elf32_Sym);
        for (uint32_t s = 0; s < nsyms; s++) {
            Elf32_Sym sym;
            memcpy(&sym, buf.data() + sh.sh_offset + s * sizeof(Elf32_Sym),
                   sizeof(sym));
            if (sym.st_name >= strtab_size) continue;
            const char* name = strtab + sym.st_name;
            if (strcmp(name, "tohost")   == 0) tohost_   = sym.st_value;
            if (strcmp(name, "fromhost") == 0) fromhost_ = sym.st_value;
        }
        break; // only one .symtab expected
    }

    return ehdr.e_entry;
}
