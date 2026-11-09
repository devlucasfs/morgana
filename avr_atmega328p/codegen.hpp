#pragma once

#include "src/codegen/backends.hpp"
#include "src/codegen/factory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>

/*x
 * AVR ATmega328P backend (avr-gas syntax, 8-bit RISC, Harvard, 2 KiB SRAM).
 *
 * Design (everything is "memory machine" style, like the x86 backend):
 *   - Every IR value lives in the stack frame; Y (r29:r28) is the frame base ("rbp").
 *     A slot `pos` occupies [Y - pos, Y - pos + size), little endian.
 *     AVR can't do negative displacements, so slots are reached with Z/X = Y - pos.
 *   - Pointers are 2 bytes. Wherever the IR slot is wider (ir_word = 8, the movq of the
 *     x86 backend) the pointer is zero-extended up to the slot size.
 *   - Calling convention (own, NOT avr-gcc's):
 *       * arguments: fixed 8-byte slots, argument i at SP+1+8*i in the caller (an
 *         "outgoing area" reserved at the bottom of the frame), which the callee sees
 *         at Y+5+8*i. Because slots are indexed, `argument()` may be called in any order
 *         and doesn't need the argument size.
 *       * return value: low byte in r18, up to 8 bytes (r18..r25). A tuple (sret) is
 *         written through the pointer in arg 0 and that pointer comes back in r18:r19.
 *       * callee-saved: only Y. Everything else is scratch.
 *   - Scratch registers used by the backend:
 *       r0           temp
 *       r2..r9       operand B       r10..r17   operand A
 *       r18..r25     result / return value / loop counters
 *       r26:r27 (X)  destination pointer / counter    r30:r31 (Z) source pointer
 *     r1 is never touched (no mul), so the zero-reg convention is irrelevant.
 *   - SP is set with the usual cli-safe idiom; the frame size is only known at the end
 *     of the function, so it is a `.set .LFRAMEn` referenced through lo8()/hi8().
 *   - String literals: each function that uses a literal gets a private buffer at the bottom
 *     of its frame (past the IR slots, before the outgoing-argument area), filled where the
 *     literal is used: short ones with ldi/st (no flash at all), long ones with an lpm loop
 *     from .progmem.data (flash only, never copied to SRAM). `string_ptr` yields a normal
 *     SRAM pointer, so ld/st/deref work on it like on any other stack data.
 *     See `strings_on_stack` / `literal_inline_max`.
 *   - `_start` is special-cased: sets SP = RAMEND, copies .data from flash (initialised
 *     globals) and emits a reset vector.
 *     With avr-gcc's normal crt just name your entry `main` and ignore all that.
 *
 * Mapped registers / io (the reg_loc handed to loc_*):
 *   - reg_*  : a CPU register (r0..r31, x/y/z)
 *   - io_*   : the I/O ADDRESS SPACE (0x00..0x3F, the numbers in the datasheet's
 *              "I/O address" column, e.g. PORTB = 0x05). Uses in / out, and
 *              sbi / cbi for a constant single-bit write on 0x00..0x1F.
 *   - mem_*  : the DATA ADDRESS SPACE (the numbers in the "memory address" column,
 *              e.g. PORTB = 0x25, UCSR0A = 0xC0, anything up to 0xFFFF). Uses lds / sts.
 *     A mem_* location reaches the frontend as `io = true` with `name == "mem"`
 *     (reg_loc has no dedicated field for it); see is_mem().
 */
class avr_atmega328p : public assembly_backend {
protected:
    // ── knobs ──
    static constexpr bool        reset_vector = true;     // `jmp _start` in .vectors
    static constexpr bool        init_data    = true;     // copy .data from flash in _start
    // true : literals stay in flash and are copied (lpm) into a stack buffer where they are used
    // false: legacy mode, literals go to .rodata and rely on the linker script / _start copying
    //        .rodata flash -> SRAM (costs permanent SRAM; ld/st can only read it after that copy)
    static constexpr bool        strings_on_stack = true;
    // literals up to this size (incl. NUL) are built with `ldi`/`st` and take no flash at all;
    // bigger ones are copied from flash with an lpm loop (smaller code, ~6 bytes is the break-even)
    static constexpr std::size_t literal_inline_max = 8;
    static constexpr std::size_t ram_end      = 0x08FF;

    // ── ABI constants ──
    static constexpr int         SPL = 0x3d, SPH = 0x3e, SREG = 0x3f;   // I/O addresses
    static constexpr std::size_t slot_bytes = 8;     // one argument slot
    static constexpr std::size_t ir_word    = 8;     // IR pointer slot width
    static constexpr std::size_t arg_base   = 5;     // Y + 5 -> arg 0 (ret addr 2 + saved Y 2 + 1)
    static constexpr int         A = 10, B = 2, R = 18, RET = 18;

    // ── I/O address space limits ──
    static constexpr unsigned long long io_max     = 0x3F;   // in / out reach 0x00..0x3F
    static constexpr unsigned long long io_bit_max = 0x1F;   // sbi / cbi reach 0x00..0x1F

    std::unordered_map<std::string, std::string>  strings;      // literal bytes -> flash label (emitted once per program)
    std::unordered_map<std::string, std::size_t>  fn_strings;   // literal bytes -> end offset of this function's stack buffer
    std::size_t str_bytes = 0;      // total stack-buffer bytes for literals in this function
    std::size_t cur_id    = 0;      // id of the function being emitted
    std::size_t sret_slot = 0;
    std::size_t frame     = 0;
    std::size_t max_out   = 0;      // outgoing argument slots used in this function

    // ── tiny helpers ──

    static std::string r(int n) { return "r" + std::to_string(n); }

    static bool ends_with(const std::string& s, const char* suf) {
        const std::string t = suf;
        return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
    }

    static void check_size(std::size_t s) {
        if( s != 1 && s != 2 && s != 4 && s != 8 )
            throw std::runtime_error("Unsupported operand size: " + std::to_string(s));
    }

    // "-N(%rbp)" -> N     (same textual form the x86 backend uses for stack slots)
    static bool parse_slot(const std::string& s, std::size_t& pos) {
        if( !ends_with(s, "(%rbp)") ) return false;
        std::string b = s.substr(0, s.size() - 6);
        if( b.empty() || b[0] != '-' ) return false;
        b.erase(0, 1);
        if( b.empty() || b.find_first_not_of("0123456789") != std::string::npos ) return false;
        pos = static_cast<std::size_t>(std::strtoull(b.c_str(), nullptr, 10));
        return true;
    }

    // "name(%rip)" -> name   (a global)
    static bool parse_symbol(const std::string& s, std::string& sym) {
        if( !ends_with(s, "(%rip)") ) return false;
        sym = s.substr(0, s.size() - 6);
        return !sym.empty();
    }

    static bool is_imm(const std::string& s) { return !s.empty() && s[0] == '$'; }

    static long long immediate_value(const std::string& src, std::size_t size, bool sign) {
        const std::string digits = (!src.empty() && src[0] == '$') ? src.substr(1) : src;
        long long v = (!digits.empty() && digits[0] == '-')
            ? std::strtoll(digits.c_str(), nullptr, 10)
            : static_cast<long long>(std::strtoull(digits.c_str(), nullptr, 10));

        switch( size ) {
            case 1: return sign ? static_cast<long long>(static_cast<std::int8_t>(v))
                                : static_cast<long long>(static_cast<std::uint8_t>(v));
            case 2: return sign ? static_cast<long long>(static_cast<std::int16_t>(v))
                                : static_cast<long long>(static_cast<std::uint16_t>(v));
            case 4: return sign ? static_cast<long long>(static_cast<std::int32_t>(v))
                                : static_cast<long long>(static_cast<std::uint32_t>(v));
            default: return v;
        }
    }

    static bool is_signed_op(const std::string& op) {
        return op == "sdiv" || op == "srem" || op == "sar"
            || op == "slt" || op == "sle" || op == "sgt" || op == "sge";
    }

    static std::string generate_position(std::size_t pos) {
        return "-" + std::to_string(pos) + "(%rbp)";
    }

    static std::string block_label(const std::string& name) { return ".Lb_" + name; }

    std::string tmp() { return ".Lt" + std::to_string(next_unique_id()); }

    void put_label(const std::string& l) {
        set_tabs(0);
        writeln(l + ":");
        set_tabs(1);
    }

    // ── 16-bit pointer arithmetic on a register pair (lo = 24 / 26 / 28 / 30 ...) ──

    void sub16(int lo, std::size_t v) {
        v &= 0xFFFF;
        if( v == 0 ) return;
        if( v <= 63 && lo >= 24 ) { writeln("sbiw " + r(lo) + ", " + std::to_string(v)); return; }
        if( (v & 0xFF) == 0 )     { writeln("subi " + r(lo + 1) + ", " + std::to_string(v >> 8)); return; }
        writeln("subi " + r(lo)     + ", " + std::to_string(v & 0xFF));
        writeln("sbci " + r(lo + 1) + ", " + std::to_string(v >> 8));
    }

    void add16(int lo, std::size_t v) {
        v &= 0xFFFF;
        if( v == 0 ) return;
        if( v <= 63 && lo >= 24 ) { writeln("adiw " + r(lo) + ", " + std::to_string(v)); return; }
        sub16(lo, (0x10000 - v) & 0xFFFF);
    }

    // pair <- Y - pos        (address of a stack slot)
    void frame_addr(int lo, std::size_t pos) {
        writeln("movw " + r(lo) + ", r28");
        sub16(lo, pos);
    }

    // pair <- SP + off
    void sp_addr(int lo, std::size_t off) {
        writeln("in " + r(lo)     + ", " + std::to_string(SPL));
        writeln("in " + r(lo + 1) + ", " + std::to_string(SPH));
        add16(lo, off);
    }

    // ── register blocks ──

    void set_const(int reg, unsigned byte) {
        if( byte == 0 )        writeln("clr " + r(reg));
        else if( reg >= 16 )   writeln("ldi " + r(reg) + ", " + std::to_string(byte));
        else {
            writeln("ldi r24, " + std::to_string(byte));
            writeln("mov " + r(reg) + ", r24");
        }
    }

    // fill [base+have, base+n) with the zero/sign extension of what's already there
    void extend(int base, std::size_t have, std::size_t n, bool sign) {
        if( have >= n ) return;

        if( !sign ) {
            for( std::size_t j = have; j < n; j++ ) writeln("clr " + r(base + (int)j));
            return;
        }

        const int top = base + (int)have - 1, t = base + (int)have;
        writeln("mov " + r(t) + ", " + r(top));
        writeln("lsl " + r(t));
        writeln("sbc " + r(t) + ", " + r(t));
        for( std::size_t j = have + 1; j < n; j++ ) writeln("mov " + r(base + (int)j) + ", " + r(t));
    }

    // an operand (immediate / stack slot / global) -> n bytes at base (ssz = its own size)
    void load_block(int base, std::size_t n, bool imm, const std::string& src,
                    std::size_t ssz, bool sign) {
        if( n > 8 ) throw std::runtime_error("Unsupported operand size: " + std::to_string(n));
        const std::size_t rd = std::min(ssz, n);

        if( imm ) {
            const std::uint64_t v = static_cast<std::uint64_t>(immediate_value(src, ssz, sign));
            for( std::size_t i = 0; i < n; i++ )
                set_const(base + (int)i, static_cast<unsigned>((v >> (8 * i)) & 0xFF));
            return;
        }

        std::size_t pos = 0;
        std::string sym;

        if( parse_slot(src, pos) ) {
            frame_addr(30, pos);
            for( std::size_t i = 0; i < rd; i++ )
                writeln("ldd " + r(base + (int)i) + ", Z+" + std::to_string(i));
        } else if( parse_symbol(src, sym) ) {
            for( std::size_t i = 0; i < rd; i++ )
                writeln("lds " + r(base + (int)i) + ", " + sym + "+" + std::to_string(i));
        } else {
            throw std::runtime_error("Unsupported operand '" + src + "'");
        }

        extend(base, rd, n, sign);
    }

    // n bytes at base -> slot, truncated / extended to dsz
    void store_result(int base, std::size_t n, std::size_t pos, std::size_t dsz, bool sign) {
        frame_addr(30, pos);

        const std::size_t m = std::min(n, dsz);
        for( std::size_t i = 0; i < m; i++ )
            writeln("std Z+" + std::to_string(i) + ", " + r(base + (int)i));

        if( dsz > n ) {
            if( sign ) {
                writeln("mov r0, " + r(base + (int)n - 1));
                writeln("lsl r0");
                writeln("sbc r0, r0");
            } else {
                writeln("clr r0");
            }
            for( std::size_t j = n; j < dsz; j++ )
                writeln("std Z+" + std::to_string(j) + ", r0");
        }
    }

    // address of a slot / global -> r24:r25
    void load_address(const std::string& src) {
        std::size_t pos = 0;
        std::string sym;

        if( parse_slot(src, pos) )        frame_addr(24, pos);
        else if( parse_symbol(src, sym) ) {
            writeln("ldi r24, lo8(" + sym + ")");
            writeln("ldi r25, hi8(" + sym + ")");
        } else {
            throw std::runtime_error("Cannot take the address of '" + src + "'");
        }
    }

    // r24:r25 (a pointer) -> slot, zero-extended up to `size` (at least 2 bytes)
    void store_pointer(std::size_t pos, std::size_t size) {
        frame_addr(30, pos);
        writeln("std Z+0, r24");
        writeln("std Z+1, r25");

        const std::size_t w = std::max<std::size_t>(size, 2);
        if( w > 2 ) writeln("clr r0");
        for( std::size_t j = 2; j < w; j++ ) writeln("std Z+" + std::to_string(j) + ", r0");
    }

    // pointer stored in a slot -> r24:r25
    void load_pointer(std::size_t pos) {
        frame_addr(30, pos);
        writeln("ld r24, Z");
        writeln("ldd r25, Z+1");
    }

    // copy `size` bytes from (Z) to (X). Clobbers r0, r22:r23, X, Z
    void emit_copy(std::size_t size) {
        if( size == 0 ) return;

        if( size <= 16 ) {
            for( std::size_t i = 0; i < size; i++ ) {
                writeln("ld r0, Z+");
                writeln("st X+, r0");
            }
            return;
        }

        const std::string loop = tmp();
        writeln("ldi r22, " + std::to_string(size & 0xFF));
        writeln("ldi r23, " + std::to_string((size >> 8) & 0xFF));
        put_label(loop);
        writeln("ld r0, Z+");
        writeln("st X+, r0");
        writeln("subi r22, 1");
        writeln("sbci r23, 0");
        writeln("brne " + loop);
    }

    // copy `size` (>= 1) bytes from FLASH (Z, byte address) to SRAM (X) with a loop.
    // Clobbers r0, r22:r23, X, Z
    void emit_copy_flash(std::size_t size) {
        if( size == 0 ) return;

        const std::string loop = tmp();
        writeln("ldi r22, " + std::to_string(size & 0xFF));
        writeln("ldi r23, " + std::to_string((size >> 8) & 0xFF));
        put_label(loop);
        writeln("lpm r0, Z+");
        writeln("st X+, r0");
        writeln("subi r22, 1");
        writeln("sbci r23, 0");
        writeln("brne " + loop);
    }

    // flash (or legacy .rodata) copy of a literal, emitted once per program; returns its label
    const std::string& literal(const std::string& bytes, const char* section) {
        auto it = strings.find(bytes);
        if( it != strings.end() ) return it->second;

        const std::string label = ".Lstr" + std::to_string(next_unique_id());
        it = strings.emplace(bytes, label).first;

        writeln(std::string(".pushsection ") + section);
        writeln(label + ":");

        for( std::size_t i = 0; i <= bytes.size(); i += 16 ) {
            std::string line = ".byte ";
            for( std::size_t j = i; j < i + 16 && j <= bytes.size(); j++ ) {
                unsigned v = j < bytes.size() ? static_cast<unsigned char>(bytes[j]) : 0;
                if( j != i ) line += ", ";
                line += std::to_string(v);
            }
            writeln(line);
        }

        writeln(".balign 2");
        writeln(".popsection");
        return it->second;
    }

    // pair <- address of the literal buffer that ends `end` bytes past the IR locals.
    // The buffer is [Y - (LFLOC + end), Y - (LFLOC + end) + size); LFLOC (the final size of the
    // IR locals) is only known at the epilogue, so it is a `.set` symbol (like .LFRAME).
    // `lo` must be >= 16 (subi/sbci).
    void str_addr(int lo, std::size_t end) {
        const std::string ex = ".LFLOC" + std::to_string(cur_id) + "+" + std::to_string(end);
        writeln("movw " + r(lo) + ", r28");
        writeln("subi " + r(lo)     + ", lo8(" + ex + ")");
        writeln("sbci " + r(lo + 1) + ", hi8(" + ex + ")");
    }

    // block A (8 bytes) -> outgoing argument slot `index`
    void store_out(std::size_t index) {
        max_out = std::max(max_out, index + 1);
        sp_addr(30, 1 + slot_bytes * index);
        for( std::size_t i = 0; i < slot_bytes; i++ )
            writeln("std Z+" + std::to_string(i) + ", " + r(A + (int)i));
    }

    // r24:r25 pointer -> block A (8 bytes, zero-extended)
    void pointer_to_a() {
        writeln("movw " + r(A) + ", r24");
        for( std::size_t i = 2; i < slot_bytes; i++ ) writeln("clr " + r(A + (int)i));
    }

    // ── bit juggling on register blocks ──

    void shift_right(int base, std::size_t n, std::size_t k) {
        const std::size_t kb = k / 8, bits = k % 8;

        if( kb >= n ) { for( std::size_t i = 0; i < n; i++ ) writeln("clr " + r(base + (int)i)); return; }

        if( kb ) {
            for( std::size_t i = 0; i + kb < n; i++ ) writeln("mov " + r(base + (int)i) + ", " + r(base + (int)(i + kb)));
            for( std::size_t i = n - kb; i < n; i++ ) writeln("clr " + r(base + (int)i));
        }
        for( std::size_t t = 0; t < bits; t++ ) {
            writeln("lsr " + r(base + (int)n - 1));
            for( int i = (int)n - 2; i >= 0; i-- ) writeln("ror " + r(base + i));
        }
    }

    void shift_left(int base, std::size_t n, std::size_t k) {
        const std::size_t kb = k / 8, bits = k % 8;

        if( kb >= n ) { for( std::size_t i = 0; i < n; i++ ) writeln("clr " + r(base + (int)i)); return; }

        if( kb ) {
            for( int i = (int)n - 1; i >= (int)kb; i-- ) writeln("mov " + r(base + i) + ", " + r(base + i - (int)kb));
            for( std::size_t i = 0; i < kb; i++ ) writeln("clr " + r(base + (int)i));
        }
        for( std::size_t t = 0; t < bits; t++ ) {
            writeln("lsl " + r(base));
            for( std::size_t i = 1; i < n; i++ ) writeln("rol " + r(base + (int)i));
        }
    }

    void mask_and(int base, std::size_t n, std::uint64_t mask) {
        for( std::size_t i = 0; i < n; i++ ) {
            const unsigned mb = static_cast<unsigned>((mask >> (8 * i)) & 0xFF);
            if( mb == 0xFF ) continue;
            if( mb == 0 )    { writeln("clr " + r(base + (int)i)); continue; }
            writeln("ldi r24, " + std::to_string(mb));
            writeln("and " + r(base + (int)i) + ", r24");
        }
    }

    void neg_block(int base, std::size_t n) {
        const std::string done = tmp();
        for( std::size_t i = 0; i < n; i++ ) writeln("com " + r(base + (int)i));
        for( std::size_t i = 0; i < n; i++ ) {
            writeln("inc " + r(base + (int)i));
            if( i + 1 < n ) writeln("brne " + done);
        }
        put_label(done);
    }

    void chain(const char* first, const char* rest, int d, int s, std::size_t n) {
        for( std::size_t i = 0; i < n; i++ )
            writeln(std::string(i == 0 ? first : rest) + " " + r(d + (int)i) + ", " + r(s + (int)i));
    }

    // unsigned divide: A / B -> quotient in A, remainder in R. Clobbers r26
    void udivmod(std::size_t n) {
        const std::string l1 = tmp(), l2 = tmp(), l3 = tmp();

        for( std::size_t i = 0; i < n; i++ ) writeln("clr " + r(R + (int)i));
        writeln("ldi r26, " + std::to_string(8 * n));

        put_label(l1);
        writeln("lsl " + r(A));
        for( std::size_t i = 1; i < n; i++ ) writeln("rol " + r(A + (int)i));
        for( std::size_t i = 0; i < n; i++ ) writeln("rol " + r(R + (int)i));
        writeln("brcs " + l3);                       // carried out of R: certainly R >= B
        chain("cp", "cpc", R, B, n);
        writeln("brlo " + l2);
        put_label(l3);
        chain("sub", "sbc", R, B, n);
        writeln("inc " + r(A));                      // bit 0 of A is 0 after the lsl
        put_label(l2);
        writeln("dec r26");
        writeln("brne " + l1);
    }

    // ── registers / mapped io ──

    static int reg_index(std::string name) {
        for( auto& c : name ) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if( name.size() >= 2 && name[0] == 'r'
            && name.find_first_not_of("0123456789", 1) == std::string::npos ) {
            const int v = std::atoi(name.c_str() + 1);
            if( v <= 31 ) return v;
        }

        if( name == "xl" || name == "x" ) return 26;
        if( name == "xh" ) return 27;
        if( name == "yl" || name == "y" ) return 28;
        if( name == "yh" ) return 29;
        if( name == "zl" || name == "z" ) return 30;
        if( name == "zh" ) return 31;

        throw std::runtime_error("Unknown register '" + name + "'");
    }

    static std::size_t loc_bytes(const reg_loc& r) {
        if( r.bits != 8 && r.bits != 16 && r.bits != 32 && r.bits != 64 )
            throw std::runtime_error("Unsupported register / io size: " + std::to_string(r.bits));
        return r.bits / 8;
    }

    // mem_*: data address space (lds / sts). Marked by the parser with name == "mem".
    static bool is_mem(const reg_loc& l) { return l.io && l.name == "mem"; }

    // io_*: I/O address space (in / out / sbi / cbi)
    static bool is_io_space(const reg_loc& l) { return l.io && l.name != "mem"; }

    // an I/O-space access of n bytes must fit in 0x00..0x3F
    static void check_io_range(const reg_loc& l, std::size_t n) {
        const unsigned long long last = static_cast<unsigned long long>(l.addr) + n - 1;
        if( last > io_max )
            throw std::runtime_error(
                "io address 0x" + hex(static_cast<unsigned long long>(l.addr)) + " (" + std::to_string(l.bits)
                + " bits) is outside the I/O space 0x00..0x3F. If it is a data-space address "
                  "(the datasheet's memory address, e.g. PORTB = 0x25), use mem_* instead; "
                  "or subtract 0x20 to get the I/O address."
            );
    }

    static std::string hex(unsigned long long v) {
        static const char* d = "0123456789ABCDEF";
        std::string s;
        do { s.insert(s.begin(), d[v & 0xF]); v >>= 4; } while( v );
        return s;
    }

    void copy_regs(int dst, int src, std::size_t n) {
        if( dst == src ) return;
        if( dst < src ) for( std::size_t i = 0; i < n; i++ )           writeln("mov " + r(dst + (int)i) + ", " + r(src + (int)i));
        else            for( int i = (int)n - 1; i >= 0; i-- )          writeln("mov " + r(dst + i) + ", " + r(src + i));
    }

    // register / mapped address -> block A. 16-bit io: low byte first
    void loc_read(const reg_loc& l) {
        const std::size_t n = loc_bytes(l);

        if( is_mem(l) ) {
            for( std::size_t i = 0; i < n; i++ )
                writeln("lds " + r(A + (int)i) + ", " + std::to_string(static_cast<unsigned long long>(l.addr) + i));
        } else if( is_io_space(l) ) {
            check_io_range(l, n);
            for( std::size_t i = 0; i < n; i++ )
                writeln("in " + r(A + (int)i) + ", " + std::to_string(static_cast<unsigned long long>(l.addr) + i));
        } else {
            const int base = reg_index(l.name);
            if( base + (int)n > 32 ) throw std::runtime_error("Register '" + l.name + "' doesn't fit in " + std::to_string(l.bits) + " bits");
            copy_regs(A, base, n);
        }
    }

    // block A -> register / mapped address. 16-bit io: high byte first (TCNT1, ICR1...)
    void loc_write(const reg_loc& l) {
        const std::size_t n = loc_bytes(l);

        if( is_mem(l) ) {
            for( int i = (int)n - 1; i >= 0; i-- )
                writeln("sts " + std::to_string(static_cast<unsigned long long>(l.addr) + i) + ", " + r(A + i));
        } else if( is_io_space(l) ) {
            check_io_range(l, n);
            for( int i = (int)n - 1; i >= 0; i-- )
                writeln("out " + std::to_string(static_cast<unsigned long long>(l.addr) + i) + ", " + r(A + i));
        } else {
            const int base = reg_index(l.name);
            if( base + (int)n > 32 ) throw std::runtime_error("Register '" + l.name + "' doesn't fit in " + std::to_string(l.bits) + " bits");
            copy_regs(base, A, n);
        }
    }

    static std::uint64_t bit_mask(std::size_t width) {
        return width >= 64 ? ~0ULL : ((1ULL << width) - 1);
    }

    void load_value_block(int base, std::size_t n, const syscall_arg& v) {
        if( v.lea ) {
            load_address(v.src);
            if( n == 1 ) { writeln("mov " + r(base) + ", r24"); return; }
            writeln("movw " + r(base) + ", r24");
            for( std::size_t i = 2; i < n; i++ ) writeln("clr " + r(base + (int)i));
            return;
        }
        load_block(base, n, v.imm, v.src, v.imm ? n : v.size, false);
    }

public:
    explicit avr_atmega328p(const std::string& output) : assembly_backend(output) {}

    std::size_t alignment() override { return 1; }

    std::size_t align_stack(std::size_t size) { return size; }

    void extern_fn(symbols& sym, std::string& name) override {
        writeln(".extern " + name);
    }

    void prologue(symbols& sym, std::string& name, std::size_t id) override {
        sret_slot = 0;
        frame     = 0;
        max_out   = 0;
        str_bytes = 0;
        cur_id    = id;
        fn_strings.clear();

        emptylines(1);

        if( reset_vector && name == "_start" ) {
            writeln(".pushsection .vectors,\"ax\",@progbits");
            writeln("jmp _start");
            writeln(".popsection");
        }

        writeln(".globl " + name);
        writeln(".type " + name + ", @function");
        writeln(name + ":");
        writeln(".LFP" + std::to_string(id) + ":");

        set_tabs(1);

        if( name == "_start" ) {
            writeln("ldi r28, lo8(" + std::to_string(ram_end) + ")");
            writeln("ldi r29, hi8(" + std::to_string(ram_end) + ")");
            writeln("out " + std::to_string(SPH) + ", r29");
            writeln("out " + std::to_string(SPL) + ", r28");

            if( init_data ) {
                const std::string loop = tmp(), check = tmp();
                writeln("ldi r17, hi8(__data_end)");
                writeln("ldi r26, lo8(__data_start)");
                writeln("ldi r27, hi8(__data_start)");
                writeln("ldi r30, lo8(__data_load_start)");
                writeln("ldi r31, hi8(__data_load_start)");
                writeln("rjmp " + check);
                put_label(loop);
                writeln("lpm r0, Z+");
                writeln("st X+, r0");
                put_label(check);
                writeln("cpi r26, lo8(__data_end)");
                writeln("cpc r27, r17");
                writeln("brne " + loop);
            }
        } else {
            writeln("push r28");
            writeln("push r29");
            writeln("in r28, " + std::to_string(SPL));
            writeln("in r29, " + std::to_string(SPH));
        }

        // SP = Y - FRAME   (FRAME is .set in the epilogue)
        const std::string f = ".LFRAME" + std::to_string(id);
        writeln("in r24, " + std::to_string(SPL));
        writeln("in r25, " + std::to_string(SPH));
        writeln("subi r24, lo8(" + f + ")");
        writeln("sbci r25, hi8(" + f + ")");
        writeln("in r0, " + std::to_string(SREG));
        writeln("cli");
        writeln("out " + std::to_string(SPH) + ", r25");
        writeln("out " + std::to_string(SREG) + ", r0");
        writeln("out " + std::to_string(SPL) + ", r24");
    }

    void epilogue(symbols& sym, std::string& name, std::size_t id) override {
        // outgoing area [SP+1, SP+8*max_out] must not touch the lowest local byte
        // frame layout, top to bottom:  [IR slots: frame][literal buffers: str_bytes][outgoing args]
        const std::size_t total = frame + str_bytes + (max_out ? slot_bytes * max_out + 1 : 0);

        constexpr std::size_t sram_bytes = ram_end - 0x100 + 1;     // 0x0100..RAMEND = 2 KiB
        if( total >= sram_bytes )
            throw std::runtime_error("The frame of '" + name + "' needs " + std::to_string(total)
                + " bytes, but the ATmega328P only has " + std::to_string(sram_bytes) + " bytes of SRAM");

        set_tabs(1);
        writeln(".set .LFLOC" + std::to_string(id) + ", " + std::to_string(frame));
        writeln(".set .LFRAME" + std::to_string(id) + ", " + std::to_string(total));

        set_tabs(0);
        writeln(".LFE" + std::to_string(id) + ":");
        set_tabs(1);

        if( name == "_start" ) {
            writeln("cli");
            writeln("rjmp .-2");
        } else {
            writeln("in r0, " + std::to_string(SREG));
            writeln("cli");
            writeln("out " + std::to_string(SPH) + ", r29");
            writeln("out " + std::to_string(SREG) + ", r0");
            writeln("out " + std::to_string(SPL) + ", r28");
            writeln("pop r29");
            writeln("pop r28");
            writeln("ret");
        }

        set_tabs(0);
        writeln(".size " + name + ", . - " + name);
    }

    // A tuple argument is passed by pointer: call with `lea = true` and `src` = the tuple's slot.
    void argument(symbols& sym, bool lea, std::string& src, std::size_t index, bool sret) override {
        if( sret ) index++;

        if( lea ) {
            load_address(src);
            pointer_to_a();
        } else {
            load_block(A, slot_bytes, is_imm(src), src, slot_bytes, false);
        }

        store_out(index);
    }

    void call(symbols& sym, std::string& label) override {
        writeln("call " + label);
    }

    void collect_ret(symbols& sym, std::size_t pos, std::size_t size, bool sret) override {
        if( sret ) return;
        if( size > 8 ) throw std::runtime_error("Unsupported operand size: " + std::to_string(size));

        frame_addr(30, pos);
        for( std::size_t i = 0; i < size; i++ )
            writeln("std Z+" + std::to_string(i) + ", " + r(RET + (int)i));
    }

    void load(symbols& sym, std::size_t pos) override {
        load_pointer(pos);
    }

    void store(symbols& sym, std::size_t size, bool lea, std::string& src, std::size_t pos) override {
        if( lea ) {
            load_address(src);
            store_pointer(pos, size);
            return;
        }

        check_size(size);
        load_block(A, size, is_imm(src), src, size, false);
        store_result(A, size, pos, size, false);
    }

    void store_ptr(symbols& sym, std::size_t size, std::size_t ptr_pos, bool ptr_lea,
                   bool lea, std::string& src) override {
        // X = destination pointer
        if( ptr_lea ) {
            frame_addr(26, ptr_pos);
        } else {
            frame_addr(30, ptr_pos);
            writeln("ld r26, Z");
            writeln("ldd r27, Z+1");
        }

        if( lea ) {
            load_address(src);
            writeln("st X+, r24");
            writeln("st X+, r25");
            if( size > 2 ) writeln("clr r0");
            for( std::size_t j = 2; j < size; j++ ) writeln("st X+, r0");
            return;
        }

        check_size(size);
        load_block(A, size, is_imm(src), src, size, false);
        for( std::size_t i = 0; i < size; i++ ) writeln("st X+, " + r(A + (int)i));
    }

    void sret(symbols& sym, std::size_t pos) override {
        frame_addr(24, pos);
        pointer_to_a();
        store_out(0);
    }

    void save_sret(symbols& sym, std::size_t pos) override {
        sret_slot = pos;

        frame_addr(30, 0);
        add16(30, arg_base);
        writeln("ld r24, Z");
        writeln("ldd r25, Z+1");
        store_pointer(pos, ir_word);
    }

    void ret_value(symbols& sym, std::size_t pos, std::size_t size, bool tuple, std::size_t id) override {
        if( tuple ) {
            if( sret_slot == 0 )
                throw std::runtime_error("ret of a tuple in a function without an sret slot");

            load_pointer(sret_slot);                 // r24:r25 = sret pointer
            writeln("movw r26, r24");
            frame_addr(30, pos);
            emit_copy(size);
            writeln("movw " + r(RET) + ", r24");    // return the pointer in r18:r19
        } else {
            if( size > 8 ) throw std::runtime_error("Unsupported operand size: " + std::to_string(size));

            frame_addr(30, pos);
            for( std::size_t i = 0; i < size; i++ )
                writeln("ldd " + r(RET + (int)i) + ", Z+" + std::to_string(i));
        }

        writeln("jmp .LFE" + std::to_string(id));
    }

    void param(symbols& sym, std::size_t index, std::size_t pos,
               std::size_t size, bool sret, bool tuple) override {
        if( sret ) index++;

        const std::size_t off = arg_base + slot_bytes * index;

        if( tuple ) {
            frame_addr(30, 0);
            add16(30, off);
            writeln("ld r24, Z");
            writeln("ldd r25, Z+1");
            writeln("movw r30, r24");              // Z = tuple pointer
            frame_addr(26, pos);                   // X = our slot
            emit_copy(size);
            return;
        }

        check_size(size);

        writeln("movw r30, r28");
        add16(30, off);
        for( std::size_t i = 0; i < size; i++ )
            writeln("ldd " + r(A + (int)i) + ", Z+" + std::to_string(i));

        store_result(A, size, pos, size, false);
    }

    void alloca(std::size_t size) override {
        frame += align_stack(size);
    }

    void copy_bytes(symbols& sym, std::size_t src_pos, std::size_t size, std::size_t dst_pos) override {
        frame_addr(30, src_pos);
        frame_addr(26, dst_pos);
        emit_copy(size);
    }

    void ret_int(symbols& sym, std::string& data, std::size_t id) override {
        const std::string d = (!data.empty() && data[0] == '$') ? data.substr(1) : data;
        const std::uint64_t v = (!d.empty() && d[0] == '-')
            ? static_cast<std::uint64_t>(std::strtoll(d.c_str(), nullptr, 10))
            : std::strtoull(d.c_str(), nullptr, 10);

        for( int i = 0; i < 8; i++ )
            set_const(RET + i, static_cast<unsigned>((v >> (8 * i)) & 0xFF));

        writeln("jmp .LFE" + std::to_string(id));
    }

    void syscall(symbols& sym, const std::string& number, const std::vector<syscall_arg>& args, std::size_t pos) override {
        throw std::runtime_error("syscall is not available on AVR (there is no OS); use mapped io / registers");
    }

    void deref(symbols& sym, std::size_t src_pos, bool via_lea,
               std::size_t size, std::size_t dst_pos) override {
        if( via_lea ) {
            frame_addr(30, src_pos);
        } else {
            load_pointer(src_pos);
            writeln("movw r30, r24");
        }

        frame_addr(26, dst_pos);
        emit_copy(size);
    }

    void string_ptr(symbols& sym, const std::string& bytes, std::size_t pos) override {
        // legacy: literal in .rodata, rely on the linker script copying it to SRAM
        if( !strings_on_stack ) {
            const std::string& l = literal(bytes, ".rodata");
            writeln("ldi r24, lo8(" + l + ")");
            writeln("ldi r25, hi8(" + l + ")");
            store_pointer(pos, ir_word);
            return;
        }

        // a stack buffer in this function (shared by equal literals), filled every time this
        // point runs, so it also works inside loops / branches
        const std::size_t size = bytes.size() + 1;
        auto fs = fn_strings.find(bytes);

        if( fs == fn_strings.end() ) {
            str_bytes += size;
            fs = fn_strings.emplace(bytes, str_bytes).first;
        }

        const std::size_t end = fs->second;

        str_addr(26, end);                                      // X = buffer

        if( size <= literal_inline_max ) {
            // ldi + st = same code size as lpm + st, but no Z setup, no flash data, fewer cycles
            int last = -1;
            for( std::size_t i = 0; i < size; i++ ) {
                const int v = i < bytes.size() ? static_cast<unsigned char>(bytes[i]) : 0;
                if( v != last ) { writeln("ldi r22, " + std::to_string(v)); last = v; }
                writeln("st X+, r22");
            }
        } else {
            // .progmem.data stays in flash (avr-ld puts it in .text)
            const std::string& l = literal(bytes, ".progmem.data,\"a\",@progbits");
            writeln("ldi r30, lo8(" + l + ")");                // Z = flash byte address (lpm)
            writeln("ldi r31, hi8(" + l + ")");
            emit_copy_flash(size);
        }

        str_addr(24, end);                                      // r24:r25 = buffer (an SRAM pointer)
        store_pointer(pos, ir_word);
    }

    void label(const std::string& name) override {
        set_tabs(0);
        writeln(block_label(name) + ":");
        set_tabs(1);
    }

    // `jmp` instead of `rjmp`: functions can be bigger than rjmp's +-4 KiB
    void jump(const std::string& name) override {
        writeln("jmp " + block_label(name));
    }

    void branch(std::size_t pos, std::size_t size, bool nonzero, const std::string& name) override {
        const std::string skip = tmp();

        frame_addr(30, pos);
        writeln("ld r24, Z");
        for( std::size_t i = 1; i < size; i++ ) {
            writeln("ldd r25, Z+" + std::to_string(i));
            writeln("or r24, r25");
        }
        writeln("tst r24");

        writeln(std::string(nonzero ? "breq " : "brne ") + skip);   // short hop over the long jmp
        writeln("jmp " + block_label(name));
        put_label(skip);
    }

    void binary(symbols& sym, const std::string& op, std::size_t size,
                bool a_imm, const std::string& a,
                bool b_imm, const std::string& b,
                std::size_t dst_pos, std::size_t dst_size) override {
        check_size(size);
        const std::size_t n = size;
        const bool sign = is_signed_op(op);

        load_block(A, n, a_imm, a, n, sign);
        load_block(B, n, b_imm, b, n, sign);

        int res = A;
        std::size_t rn = n;

        if( op == "add" )      chain("add", "adc", A, B, n);
        else if( op == "sub" ) chain("sub", "sbc", A, B, n);
        else if( op == "and" ) chain("and", "and", A, B, n);
        else if( op == "or" )  chain("or",  "or",  A, B, n);
        else if( op == "xor" ) chain("eor", "eor", A, B, n);
        else if( op == "shl" || op == "shr" || op == "sar" ) {
            const std::string loop = tmp(), done = tmp();

            writeln("tst " + r(B));                       // count = low byte of B
            writeln("breq " + done);
            put_label(loop);

            if( op == "shl" ) {
                writeln("lsl " + r(A));
                for( std::size_t i = 1; i < n; i++ ) writeln("rol " + r(A + (int)i));
            } else {
                writeln(std::string(op == "shr" ? "lsr " : "asr ") + r(A + (int)n - 1));
                for( int i = (int)n - 2; i >= 0; i-- ) writeln("ror " + r(A + i));
            }

            writeln("dec " + r(B));
            writeln("brne " + loop);
            put_label(done);
        }
        else if( op == "mul" ) {
            const std::string l1 = tmp(), l2 = tmp();

            for( std::size_t i = 0; i < n; i++ ) writeln("clr " + r(R + (int)i));
            writeln("ldi r26, " + std::to_string(8 * n));

            put_label(l1);
            writeln("lsr " + r(B + (int)n - 1));          // shift B right, LSB -> carry
            for( int i = (int)n - 2; i >= 0; i-- ) writeln("ror " + r(B + i));
            writeln("brcc " + l2);
            chain("add", "adc", R, A, n);
            put_label(l2);
            writeln("lsl " + r(A));
            for( std::size_t i = 1; i < n; i++ ) writeln("rol " + r(A + (int)i));
            writeln("dec r26");
            writeln("brne " + l1);

            res = R;
        }
        else if( op == "udiv" || op == "urem" ) {
            udivmod(n);
            res = (op == "udiv") ? A : R;
        }
        else if( op == "sdiv" || op == "srem" ) {
            // r27: bit0 = quotient is negative, bit1 = remainder is negative (sign of A)
            const std::string pa = tmp(), pb = tmp(), skip = tmp();

            writeln("clr r27");
            writeln("tst " + r(A + (int)n - 1));
            writeln("brpl " + pa);
            writeln("ori r27, 3");
            neg_block(A, n);
            put_label(pa);

            writeln("tst " + r(B + (int)n - 1));
            writeln("brpl " + pb);
            writeln("ldi r26, 1");
            writeln("eor r27, r26");
            neg_block(B, n);
            put_label(pb);

            udivmod(n);

            const bool quot = op == "sdiv";
            writeln(std::string("sbrs r27, ") + (quot ? "0" : "1"));
            writeln("rjmp " + skip);
            neg_block(quot ? A : R, n);
            put_label(skip);

            res = quot ? A : R;
        }
        else {
            bool swap = false;
            const char* br = nullptr;

            if( op == "eq" )       { swap = false; br = "breq"; }
            else if( op == "ne" )  { swap = false; br = "brne"; }
            else if( op == "slt" ) { swap = false; br = "brlt"; }
            else if( op == "sge" ) { swap = false; br = "brge"; }
            else if( op == "sgt" ) { swap = true;  br = "brlt"; }
            else if( op == "sle" ) { swap = true;  br = "brge"; }
            else if( op == "ult" ) { swap = false; br = "brlo"; }
            else if( op == "uge" ) { swap = false; br = "brsh"; }
            else if( op == "ugt" ) { swap = true;  br = "brlo"; }
            else if( op == "ule" ) { swap = true;  br = "brsh"; }
            else throw std::runtime_error("Unknown binary operation '" + op + "'");

            const std::string yes = tmp();

            chain("cp", "cpc", swap ? B : A, swap ? A : B, n);
            writeln("ldi r24, 1");                        // ldi doesn't touch SREG
            writeln(std::string(br) + " " + yes);
            writeln("ldi r24, 0");
            put_label(yes);

            res = 24;
            rn  = 1;
        }

        store_result(res, rn, dst_pos, dst_size, false);
    }

    void cast(symbols& sym, std::size_t src_pos, std::size_t src_size, bool src_signed,
              std::size_t dst_pos, std::size_t dst_size) override {
        check_size(src_size);

        load_block(A, src_size, false, generate_position(src_pos), src_size, src_signed);
        store_result(A, src_size, dst_pos, dst_size, src_signed);
    }

    void loc_load(symbols& sym, const reg_loc& l, std::size_t pos) override {
        const std::size_t n = loc_bytes(l);
        loc_read(l);
        store_result(A, n, pos, n, false);
    }

    void loc_store(symbols& sym, const reg_loc& l, const syscall_arg& value) override {
        load_value_block(A, loc_bytes(l), value);
        loc_write(l);
    }

    void loc_get(symbols& sym, const reg_loc& l, std::size_t lo, std::size_t width,
                 std::size_t dst_pos, std::size_t dst_size) override {
        if( width == 0 || lo + width > l.bits )
            throw std::runtime_error("The slice doesn't fit in " + std::to_string(l.bits) + " bits");

        const std::size_t n = loc_bytes(l);

        loc_read(l);
        shift_right(A, n, lo);
        if( width < 64 ) mask_and(A, n, bit_mask(width));
        store_result(A, n, dst_pos, dst_size, false);
    }

    void loc_set(symbols& sym, const reg_loc& l, std::size_t lo, std::size_t width,
                 const syscall_arg& value) override {
        if( width == 0 || lo + width > l.bits )
            throw std::runtime_error("The slice doesn't fit in " + std::to_string(l.bits) + " bits");

        // Constant single-bit write on the low I/O space: sbi / cbi.
        // One instruction, atomic (no read-modify-write window for an interrupt),
        // and it doesn't touch any register or SREG. This is what turns a digital pin on/off.
        if( is_io_space(l) && l.bits == 8 && width == 1
            && static_cast<unsigned long long>(l.addr) <= io_bit_max
            && value.imm && !value.lea ) {
            const bool one = (immediate_value(value.src, 1, false) & 1) != 0;
            writeln(std::string(one ? "sbi " : "cbi ")
                    + std::to_string(static_cast<unsigned long long>(l.addr)) + ", " + std::to_string(lo));
            return;
        }

        const std::size_t n = loc_bytes(l);
        const std::uint64_t m = bit_mask(width);

        loc_read(l);                                   // current contents -> A
        mask_and(A, n, ~(m << lo));                    // clear the slice

        load_value_block(B, n, value);                 // new bits, masked and shifted -> B
        if( width < 64 ) mask_and(B, n, m);
        shift_left(B, n, lo);

        for( std::size_t i = 0; i < n; i++ ) writeln("or " + r(A + (int)i) + ", " + r(B + (int)i));

        loc_write(l);
    }

    // `gs()` = word address, which is what icall wants
    void function_address(symbols& sym, const std::string& name, std::size_t pos) override {
        writeln("ldi r24, lo8(gs(" + name + "))");
        writeln("ldi r25, hi8(gs(" + name + "))");
        store_pointer(pos, ir_word);
    }

    void call_indirect(symbols& sym, bool lea, const std::string& src) override {
        std::size_t pos = 0;
        std::string sym_name;

        if( lea ) {
            load_address(src);
            writeln("movw r30, r24");
        } else if( parse_slot(src, pos) ) {
            load_pointer(pos);
            writeln("movw r30, r24");
        } else if( parse_symbol(src, sym_name) ) {
            writeln("lds r30, " + sym_name);
            writeln("lds r31, " + sym_name + "+1");
        } else {
            throw std::runtime_error("Unsupported call target '" + src + "'");
        }

        writeln("icall");
    }
};
