#pragma once

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "src/codegen/interface.hpp"

/*
 * AVR ATmega328P / GNU as (avr-as) backend.
 *
 * The parser only hands over NAMES and TYPES. Everything physical lives here:
 *
 *   - sizes of every type: i8/u8 = 1, i16/u16 = 2, i32/u32 = 4, i64/u64 = 8 and
 *     int, uint and ptr are 2 bytes (like avr-gcc). Nothing is aligned (alignment 1), tuples
 *     are packed in field order;
 *   - one slot per name, addressed as `Y+off` (Y = r29:r28 is the frame pointer and points at
 *     the BOTTOM of the frame, slots grow upwards from Y+1). `ldd/std` only reach Y+63, so a
 *     slot that doesn't fit is reached through a pointer computed in X/Z;
 *   - frame size is only known at the end of the function: the prologue uses
 *     `subi r28, lo8(.LFRAME<n>)` and the epilogue `.set`s it;
 *   - calling convention = avr-gcc's, restricted to r25..r18 for register arguments:
 *       * arguments are allocated from r25 downwards, each one rounded up to an even number of
 *         bytes (1 byte -> r24, 2 -> r25:r24, 4 -> r25:r22, 8 -> r25:r18); an argument that
 *         doesn't fit in r25..r18 goes to the stack (pushed right to left, little endian,
 *         caller pops);
 *       * return value in the same registers (1 byte -> r24, 2 -> r25:r24, ...);
 *       * a tuple RETURN gets a hidden pointer as first argument (sret); a tuple ARGUMENT is
 *         passed by pointer (the callee copies it, so it still behaves like a value);
 *       * the callee only keeps Y (r29:r28) and r1 (always zero). Everything else is scratch:
 *         r0, r18..r27, r30, r31. r2..r17 are NEVER touched.
 *     NOTE: the caller doesn't see the type of a literal argument. When the callee was already
 *     emitted its parameter sizes are used; otherwise a literal is passed as `int` (2 bytes),
 *     widened to 4/8 bytes only if the value needs it.
 *
 * How a name is read:
 *   - a normal slot (alloc / call result / load / ...) is read as its CONTENTS, extended to
 *     the width of the operation by the signedness of its type;
 *   - the result of `ptr_to` is read as the ADDRESS of the slot it points to (2 bytes);
 *   - deref / storep / call_indirect follow the parser's `via_lea` flag: true means "the
 *     pointer is the address of the slot", false means "the pointer is stored in the slot".
 *
 * Other AVR specifics:
 *   - the entry function (`_start`) sets up r1, SREG, SP (RAMEND = 0x08FF), copies `.data` from
 *     flash to SRAM (string literals live in `.data`) and ends in an infinite loop. A vector
 *     table with the reset vector pointing to it is emitted in `.vectors`;
 *   - function pointers (`ptr_to` of a function) are WORD addresses (what `icall` wants);
 *   - mul / div / rem (8, 16 and 32 bits) call avr-libgcc. 64-bit mul/div/rem is not supported;
 *   - syscall doesn't exist on bare metal: use the loc ops. Loc spaces are:
 *       reg -> a register ("r24", "x", "y", "z"), wider locs use the following registers;
 *       mem -> absolute data-space address (lds/sts);
 *       io  -> I/O-space address 0..63 (in/out). Extended I/O (>= 0x60) is `mem`.
 *
 *   assemble & link:
 *       avr-gcc -mmcu=atmega328p -nostartfiles out.s -o out.elf
 */
class avr_atmega328p : public codegen_interface_methods {
public:
    const std::string entrypoint = "_start";
private:
    std::size_t stack_sz = 0;
    std::string current;
    int func    = 0;
    int nlabels = 0;

    // ───────────── per-name bookkeeping ─────────────

    struct Entry {
        Ty          type;         // type of the value as seen by the expressions
        long        off  = 0;     // the slot starts at Y+off
        bool        addr = false; // `ptr_to` result: reads as the address of the slot
        std::size_t slot_bytes = 0;
    };

    struct ArgLoc {
        bool        reg  = false;
        int         base = 0;     // first (lowest) register when `reg`
        std::size_t wire = 0;     // bytes the argument takes (always even)
    };

    std::unordered_map<std::string, Entry>                    vars;      // reset at every function
    std::unordered_map<std::string, std::string>              fn_alias;  // `move g, f` where f is a function
    std::unordered_set<std::string>                           externs;
    std::unordered_map<std::string, int>                      strings;   // literal bytes -> .LS<n>
    std::unordered_map<std::string, std::vector<std::size_t>> fn_sigs;   // parameter sizes on the wire
    int nstrings = 0;

    // current function
    Ty                 fn_ret;
    bool               fn_sret = false;
    long               sret_off = 0;
    std::vector<Entry> param_slots;

    // ───────────── small helpers ─────────────

    static std::size_t size_of(const Ty& t);
    static std::size_t align_of(const Ty& t);

    static std::size_t even(std::size_t n) {
        return n < 2 ? 2 : (n + 1) & ~static_cast<std::size_t>(1);
    }

    static std::size_t tuple_layout(const morgana_tuple& t, std::size_t* align = nullptr,
                                    std::vector<std::size_t>* offsets = nullptr) {
        std::size_t off = 0, max_al = 1;

        for( const auto& el : t.elements ) {
            Ty e = make_ty(el.first, el.second);
            std::size_t a = align_of(e);

            off = (off + a - 1) & ~(a - 1);
            if( offsets ) offsets->push_back(off);
            off += size_of(e);
            max_al = std::max(max_al, a);
        }

        if( align ) *align = max_al;
        return (off + max_al - 1) & ~(max_al - 1);
    }

    static bool is_signed_kind(morgana_type k) {
        #define X(m, str, sgn, size) if( k == morgana_type::m ) return int(sgn) == 1;
        TYPE_SET(X)
        #undef X
        return false;
    }

    static std::string rg(int r)                       { return "r" + std::to_string(r); }
    static std::string rk(int base, std::size_t k)     { return rg(base + static_cast<int>(k)); }
    static std::string num(long long v)                { return std::to_string(v); }
    static std::string ypl(long off, std::size_t k = 0) {
        return "Y+" + std::to_string(off + static_cast<long>(k));
    }

    // can `ldd/std` reach the `n` bytes starting at Y+off?
    static bool reach(long off, std::size_t n) {
        return n == 0 || off + static_cast<long>(n) - 1 <= 63;
    }

    static std::uint64_t width_mask(std::size_t bits) {
        return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
    }

    static long long parse_lit(const std::string& s) {
        try { return std::stoll(s); }
        catch( ... ) { return static_cast<long long>(std::stoull(s)); }
    }

    static int reg_of(std::string n) {
        if(! n.empty() && n[0] == '%' ) n.erase(0, 1);
        for( auto& ch : n ) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        if( n == "x" ) return 26;
        if( n == "y" ) return 28;
        if( n == "z" ) return 30;

        if( n.size() >= 2 && n[0] == 'r' ) {
            try {
                int v = std::stoi(n.substr(1));
                if( v >= 0 && v < 32 ) return v;
            } catch( ... ) {}
        }

        throw std::runtime_error("codegen: unknown AVR register '" + n + "'");
    }

    // one instruction: `mnemonic a, b`
    void I(const std::string& mn, const std::string& a = "", const std::string& b = "") {
        std::string s = mn;
        if(! a.empty() ) s += " " + a;
        if(! b.empty() ) s += ", " + b;
        emit("%s\n", s.c_str());
    }

    void L(const std::string& name) {
        bool old = into;
        into = false;
        emit("%s:\n", c(name));
        into = old;
    }

    std::string new_label() {
        return ".LX" + std::to_string(func) + "_" + std::to_string(nlabels++);
    }

    std::string label_of(const std::string& name) const {
        return ".L" + std::to_string(func) + "_" + name;
    }

    std::size_t normalized_stacksz() {
        return stack_sz;
    }

    // slots grow upwards from Y+1
    long alloca_bytes(std::size_t size) {
        long off = static_cast<long>(stack_sz) + 1;
        stack_sz += size;
        return off;
    }

    Entry make_slot(const Ty& t) {
        Entry e;
        e.type       = t;
        e.slot_bytes = size_of(t);
        e.off        = alloca_bytes(e.slot_bytes);
        return e;
    }

    const Entry& var(const std::string& n) const {
        auto it = vars.find(n);
        if( it == vars.end() )
            throw std::runtime_error("codegen: unknown name '" + n + "' in " + current);
        return it->second;
    }

    std::string resolve(std::string name) const {
        for( auto it = fn_alias.find(name); it != fn_alias.end(); it = fn_alias.find(name) )
            name = it->second;
        return name;
    }

    // ───────────── addresses and bytes (these clobber SREG) ─────────────

    // register pair (lo, lo+1) <- Y + disp
    void addr_to_pair(int lo, long disp) {
        I("movw", rg(lo), "r28");
        if( disp ) {
            long c16 = (-disp) & 0xFFFF;
            I("subi", rg(lo),     num(c16 & 0xFF));
            I("sbci", rg(lo + 1), num((c16 >> 8) & 0xFF));
        }
    }

    // regs base.. <- n bytes of the slot at Y+off (far slots go through Z)
    void load_slot_bytes(int base, std::size_t n, long off) {
        if( reach(off, n) ) {
            for( std::size_t k = 0; k < n; k++ ) I("ldd", rk(base, k), ypl(off, k));
        } else {
            addr_to_pair(30, off);
            for( std::size_t k = 0; k < n; k++ ) I("ld", rk(base, k), "Z+");
        }
    }

    // n bytes of the slot at Y+off <- regs base..
    void store_slot_bytes(int base, std::size_t n, long off) {
        if( reach(off, n) ) {
            for( std::size_t k = 0; k < n; k++ ) I("std", ypl(off, k), rk(base, k));
        } else {
            addr_to_pair(30, off);
            for( std::size_t k = 0; k < n; k++ ) I("st", "Z+", rk(base, k));
        }
    }

    // pair lo (26 = X, 30 = Z) <- 2-byte pointer stored in the slot at Y+off.
    // A far slot uses the other pair as a helper.
    void load_ptr_slot(int lo, long off) {
        if( reach(off, 2) ) {
            I("ldd", rg(lo),     ypl(off));
            I("ldd", rg(lo + 1), ypl(off, 1));
        } else {
            int tmp = lo == 30 ? 26 : 30;
            std::string p = tmp == 26 ? "X" : "Z";
            addr_to_pair(tmp, off);
            I("ld", rg(lo),     p + "+");
            I("ld", rg(lo + 1), p);
        }
    }

    // regs base..base+n-1 <- value of the operand, truncated / extended to n bytes (n <= 8).
    // Clobbers Z (and SREG).
    void load_operand_bytes(int base, std::size_t n, const Operand& op) {
        if( n == 0 ) return;
        if( n > 8 ) throw std::runtime_error("codegen: value wider than 8 bytes in a register");

        if( op.literal ) {
            long long v = parse_lit(op.text);
            for( std::size_t k = 0; k < n; k++ ) I("ldi", rk(base, k), num((v >> (8 * k)) & 0xFF));
            return;
        }

        const Entry& e = var(op.text);

        if( e.addr || is_tuple(e.type) ) {          // value = address of the slot
            addr_to_pair(30, e.off);
            for( std::size_t k = 0; k < n; k++ ) {
                if( k == 0 )      I("mov", rk(base, k), "r30");
                else if( k == 1 ) I("mov", rk(base, k), "r31");
                else              I("ldi", rk(base, k), "0");
            }
            return;
        }

        const std::size_t m = std::min(e.slot_bytes, n);
        const bool sg = is_signed_kind(e.type.kind);

        load_slot_bytes(base, m, e.off);

        if( n > m ) {
            if( sg && m > 0 ) {
                I("mov", rk(base, m), rk(base, m - 1));
                I("lsl", rk(base, m));
                I("sbc", rk(base, m), rk(base, m));
                for( std::size_t k = m + 1; k < n; k++ ) I("mov", rk(base, k), rk(base, m));
            } else {
                for( std::size_t k = m; k < n; k++ ) I("ldi", rk(base, k), "0");
            }
        }
    }

    // slot of `d` <- the n bytes in regs base.., truncated / extended (by `sg`) to the slot size
    void store_regs(const Entry& d, int base, std::size_t n, bool sg) {
        const std::size_t m = std::min(d.slot_bytes, n);
        store_slot_bytes(base, m, d.off);

        if( d.slot_bytes > m ) {
            if( sg && m > 0 ) {
                I("mov", "r26", rk(base, m - 1));
                I("lsl", "r26");
                I("sbc", "r26", "r26");
            } else {
                I("ldi", "r26", "0");
            }
            for( std::size_t k = m; k < d.slot_bytes; k++ ) store_slot_bytes(26, 1, d.off + static_cast<long>(k));
        }
    }

    // ───────────── memory -> memory ─────────────

    // X = source, Z = destination, copies n bytes and advances both
    void block_copy(std::size_t n) {
        if( n <= 8 ) {
            for( std::size_t k = 0; k < n; k++ ) { I("ld", "r0", "X+"); I("st", "Z+", "r0"); }
            return;
        }

        const bool wide = n > 255;
        const std::string top = new_label();

        if( wide ) I("push", "r24");
        I("push", "r25");

        if( wide ) { I("ldi", "r24", num(n & 0xFF)); I("ldi", "r25", num((n >> 8) & 0xFF)); }
        else         I("ldi", "r25", num(n));

        L(top);
        I("ld", "r0", "X+");
        I("st", "Z+", "r0");
        if( wide ) I("sbiw", "r24", "1");
        else       I("dec", "r25");
        I("brne", top);

        I("pop", "r25");
        if( wide ) I("pop", "r24");
    }

    void copy_slot_slot(long soff, long doff, std::size_t n) {
        if( n <= 8 && reach(soff, n) && reach(doff, n) ) {
            for( std::size_t k = 0; k < n; k++ ) { I("ldd", "r0", ypl(soff, k)); I("std", ypl(doff, k), "r0"); }
            return;
        }
        addr_to_pair(26, soff);
        addr_to_pair(30, doff);
        block_copy(n);
    }

    // source pointer already in X
    void copy_ptrX_to_slot(long doff, std::size_t n) {
        if( n <= 8 && reach(doff, n) ) {
            for( std::size_t k = 0; k < n; k++ ) { I("ld", "r0", "X+"); I("std", ypl(doff, k), "r0"); }
            return;
        }
        addr_to_pair(30, doff);
        block_copy(n);
    }

    // destination pointer already in Z
    void copy_slot_to_ptrZ(long soff, std::size_t n) {
        if( n <= 8 && reach(soff, n) ) {
            for( std::size_t k = 0; k < n; k++ ) { I("ldd", "r0", ypl(soff, k)); I("st", "Z+", "r0"); }
            return;
        }
        addr_to_pair(26, soff);
        block_copy(n);
    }

    // SP += n
    void sp_pop(long n) {
        if( n <= 4 ) {
            for( long k = 0; k < n; k++ ) I("pop", "r0");
            return;
        }

        I("in", "r26", "0x3d");
        I("in", "r27", "0x3e");

        if( n <= 63 ) {
            I("adiw", "r26", num(n));
        } else {
            long c16 = (-n) & 0xFFFF;
            I("subi", "r26", num(c16 & 0xFF));
            I("sbci", "r27", num((c16 >> 8) & 0xFF));
        }

        I("in", "r0", "0x3f");
        I("cli");
        I("out", "0x3e", "r27");
        I("out", "0x3f", "r0");
        I("out", "0x3d", "r26");
    }

    // ───────────── calling convention ─────────────

    static std::vector<ArgLoc> plan_args(const std::vector<std::size_t>& wires) {
        std::vector<ArgLoc> out;
        int top = 26;

        for( std::size_t w : wires ) {
            ArgLoc a;
            a.wire = w;

            if( top - static_cast<int>(w) >= 18 ) {
                top -= static_cast<int>(w);
                a.reg  = true;
                a.base = top;
            }

            out.push_back(a);
        }

        return out;
    }

    // size on the wire of an argument whose callee isn't known
    std::size_t arg_wire(const Operand& op) const {
        if( op.literal ) {
            long long v = parse_lit(op.text);
            if( v >= -32768 && v <= 65535 )                       return 2;
            if( v >= INT32_MIN && v <= static_cast<long long>(UINT32_MAX) ) return 4;
            return 8;
        }

        const Entry& e = var(op.text);
        if( e.addr || is_tuple(e.type) ) return 2;
        return even(e.slot_bytes);
    }

    void call_common(const std::string& dst, const Ty& ret, const std::vector<Operand>& args,
                     bool sret, const std::string* direct, const Entry* target, bool target_lea) {
        Entry d;
        const bool has_ret = ! is_void(ret);
        if( has_ret ) d = make_slot(ret);

        std::string fn;
        const std::vector<std::size_t>* sig = nullptr;

        if( direct ) {
            fn = resolve(*direct);
            auto it = fn_sigs.find(fn);
            if( it != fn_sigs.end() ) sig = &it->second;
        }

        std::vector<std::size_t> wires;
        if( sret ) wires.push_back(2);
        for( std::size_t k = 0; k < args.size(); k++ )
            wires.push_back(sig && k < sig->size() ? (*sig)[k] : arg_wire(args[k]));

        const std::vector<ArgLoc> plan = plan_args(wires);
        const std::size_t sh = sret ? 1 : 0;

        // stack arguments, last one first (little endian: the LSB ends up at the lowest address)
        long pushed = 0;
        for( std::size_t k = args.size(); k-- > 0; ) {
            const ArgLoc& a = plan[k + sh];
            if( a.reg ) continue;

            load_operand_bytes(18, a.wire, args[k]);
            for( std::size_t b = a.wire; b-- > 0; ) I("push", rk(18, b));
            pushed += static_cast<long>(a.wire);
        }

        // register arguments
        if( sret ) addr_to_pair(plan[0].base, d.off);

        for( std::size_t k = 0; k < args.size(); k++ ) {
            const ArgLoc& a = plan[k + sh];
            if( a.reg ) load_operand_bytes(a.base, a.wire, args[k]);
        }

        if( target ) {
            if( target_lea ) addr_to_pair(30, target->off);
            else             load_ptr_slot(30, target->off);
            I("icall");
        } else {
            I("call", fn);
        }

        if( pushed ) sp_pop(pushed);

        if( has_ret && ! is_tuple(ret) ) {
            std::size_t n = size_of(ret);
            store_slot_bytes(26 - static_cast<int>(even(n)), n, d.off);
        }

        if(! dst.empty() && has_ret ) vars[dst] = d;
    }

    // ───────────── accumulator r18.. helpers ─────────────

    void acc_shl_const(std::size_t N, unsigned n) {
        const std::size_t q = n / 8;
        const unsigned    r = n % 8;

        for( std::size_t k = N; k-- > 0; ) {
            if( k < q )   I("ldi", rk(18, k), "0");
            else if( q )  I("mov", rk(18, k), rk(18, k - q));
        }

        for( unsigned i = 0; i < r; i++ ) {
            I("lsl", "r18");
            for( std::size_t k = 1; k < N; k++ ) I("rol", rk(18, k));
        }
    }

    void acc_shr_const(std::size_t N, unsigned n) {
        const std::size_t q = n / 8;
        const unsigned    r = n % 8;

        for( std::size_t k = 0; k < N; k++ ) {
            if( k + q >= N ) I("ldi", rk(18, k), "0");
            else if( q )     I("mov", rk(18, k), rk(18, k + q));
        }

        for( unsigned i = 0; i < r; i++ ) {
            I("lsr", rk(18, N - 1));
            for( std::size_t k = N - 1; k-- > 0; ) I("ror", rk(18, k));
        }
    }

    // keep only the low `width` bits
    void acc_mask(std::size_t N, std::size_t width) {
        for( std::size_t k = 0; k < N; k++ ) {
            long b = static_cast<long>(width) - 8 * static_cast<long>(k);

            if( b >= 8 )     continue;
            if( b <= 0 )     I("ldi",  rk(18, k), "0");
            else             I("andi", rk(18, k), num((1L << b) - 1));
        }
    }

    // ───────────── loc (registers / memory / I/O) ─────────────

    static std::size_t loc_bytes(const Loc& l) {
        std::size_t n = l.bits / 8;
        if( n == 0 || n > 8 ) throw std::runtime_error("codegen: loc must be 8, 16, 32 or 64 bits wide");
        return n;
    }

    void loc_rd(int dst, const Loc& l, std::size_t k) {
        switch( l.space ) {
            case Loc::Space::reg: {
                int r = reg_of(l.name) + static_cast<int>(k);
                if( r > 31 ) throw std::runtime_error("codegen: loc runs past r31");
                I("mov", rg(dst), rg(r));
                break;
            }
            case Loc::Space::mem:
                I("lds", rg(dst), num(static_cast<long long>(l.addr) + static_cast<long long>(k)));
                break;
            case Loc::Space::io:
                if( static_cast<long long>(l.addr) + static_cast<long long>(k) > 63 )
                    throw std::runtime_error("codegen: io_* address must be 0..63 on AVR (use mem_* for extended I/O)");
                I("in", rg(dst), num(static_cast<long long>(l.addr) + static_cast<long long>(k)));
                break;
        }
    }

    void loc_wr(int src, const Loc& l, std::size_t k) {
        switch( l.space ) {
            case Loc::Space::reg: {
                int r = reg_of(l.name) + static_cast<int>(k);
                if( r > 31 ) throw std::runtime_error("codegen: loc runs past r31");
                I("mov", rg(r), rg(src));
                break;
            }
            case Loc::Space::mem:
                I("sts", num(static_cast<long long>(l.addr) + static_cast<long long>(k)), rg(src));
                break;
            case Loc::Space::io:
                if( static_cast<long long>(l.addr) + static_cast<long long>(k) > 63 )
                    throw std::runtime_error("codegen: io_* address must be 0..63 on AVR (use mem_* for extended I/O)");
                I("out", num(static_cast<long long>(l.addr) + static_cast<long long>(k)), rg(src));
                break;
        }
    }

    // r18.. <- the loc (little endian)
    void loc_read(const Loc& l) {
        const std::size_t N = loc_bytes(l);

        if( l.space == Loc::Space::reg && reg_of(l.name) < 18 ) {
            for( std::size_t k = 0; k < N; k++ ) loc_rd(18 + static_cast<int>(k), l, k);
        } else if( l.space == Loc::Space::reg ) {
            for( std::size_t k = N; k-- > 0; ) loc_rd(18 + static_cast<int>(k), l, k);   // may overlap r18..
        } else {
            for( std::size_t k = 0; k < N; k++ ) loc_rd(18 + static_cast<int>(k), l, k);
        }
    }

    // the loc <- r18..
    void loc_write(const Loc& l) {
        const std::size_t N = loc_bytes(l);

        if( l.space == Loc::Space::reg && reg_of(l.name) > 18 ) {
            for( std::size_t k = N; k-- > 0; ) loc_wr(18 + static_cast<int>(k), l, k);   // may overlap r18..
        } else {
            for( std::size_t k = 0; k < N; k++ ) loc_wr(18 + static_cast<int>(k), l, k);
        }
    }

    void emit_vectors(const char* entry_id) {
        bool old = into;
        into = false;

        emit(".section .vectors,\"ax\",@progbits\n"
             ".global __vectors\n"
             "__vectors:\n"
             "jmp %s\n"
             ".rept 25\n"
             "jmp __bad_interrupt\n"
             ".endr\n"
             ".weak __bad_interrupt\n\n",
             entry_id);

        into = old;
    }

public:
    using codegen_interface_methods::codegen_interface_methods;

    void prologue(prologue_t prologue) override {
        stack_sz = 0;
        current  = prologue.identifier;
        vars.clear();
        param_slots.clear();

        const char *id = c(current);
        const std::string f = std::to_string(func);
        const bool is_entry = current == entrypoint;

        if( is_entry ) emit_vectors(id);

        into = false;
        emit(".text\n"
             ".globl %s\n"
             ".type %s, @function\n"
             "%s:\n"
             ".LFP%d:\n",
             repeat3(id),
             func);

        into = true;

        if( is_entry ) {
            I("clr", "r1");
            I("out", "0x3f", "r1");
            I("ldi", "r28", "0xFF");                 // RAMEND = 0x08FF
            I("ldi", "r29", "0x08");
            I("out", "0x3e", "r29");
            I("out", "0x3d", "r28");

            // .data: flash -> SRAM (what avr-libc's __do_copy_data does)
            const std::string cp = new_label(), chk = new_label();

            I("ldi", "r26", "lo8(__data_start)");
            I("ldi", "r27", "hi8(__data_start)");
            I("ldi", "r30", "lo8(__data_load_start)");
            I("ldi", "r31", "hi8(__data_load_start)");
            I("ldi", "r16", "hi8(__data_end)");
            I("rjmp", chk);
            L(cp);
            I("lpm", "r0", "Z+");
            I("st", "X+", "r0");
            L(chk);
            I("cpi", "r26", "lo8(__data_end)");
            I("cpc", "r27", "r16");
            I("brne", cp);
        } else {
            I("push", "r28");
            I("push", "r29");
        }

        // Y = SP; Y -= frame; SP = Y   (slots live at Y+1 .. Y+frame)
        I("in", "r28", "0x3d");
        I("in", "r29", "0x3e");
        I("subi", "r28", "lo8(.LFRAME" + f + ")");
        I("sbci", "r29", "hi8(.LFRAME" + f + ")");
        I("in", "r0", "0x3f");
        I("cli");
        I("out", "0x3e", "r29");
        I("out", "0x3f", "r0");
        I("out", "0x3d", "r28");

        fn_ret  = prologue.ret;
        fn_sret = is_tuple(fn_ret);

        std::vector<std::size_t> wires;
        if( fn_sret ) wires.push_back(2);
        for( const auto& p : prologue.params ) wires.push_back(is_tuple(p) ? 2 : even(size_of(p)));

        fn_sigs[current] = std::vector<std::size_t>(wires.begin() + (fn_sret ? 1 : 0), wires.end());

        const std::vector<ArgLoc> plan = plan_args(wires);
        std::size_t idx = 0;

        if( fn_sret ) {
            sret_off = alloca_bytes(2);
            store_slot_bytes(plan[0].base, 2, sret_off);
            idx = 1;
        }

        for( const auto& p : prologue.params ) param_slots.push_back(make_slot(p));

        // parameters that arrive in registers
        bool any_stack = false;

        for( std::size_t k = 0; k < prologue.params.size(); k++ ) {
            const ArgLoc& a = plan[idx + k];
            const Entry&  e = param_slots[k];

            if(! a.reg ) { any_stack = true; continue; }

            if( is_tuple(prologue.params[k]) ) {
                I("movw", "r26", rg(a.base));
                copy_ptrX_to_slot(e.off, e.slot_bytes);
            } else {
                store_slot_bytes(a.base, e.slot_bytes, e.off);
            }
        }

        // parameters that arrive on the stack: they start at (frame top) + 5
        // (return address 2 + saved Y 2 + the free byte SP points to). r22:r23 is the cursor.
        if( any_stack ) {
            I("movw", "r26", "r28");
            I("ldi", "r24", "lo8(.LFRAME" + f + "+5)");
            I("ldi", "r25", "hi8(.LFRAME" + f + "+5)");
            I("add", "r26", "r24");
            I("adc", "r27", "r25");
            I("movw", "r22", "r26");

            for( std::size_t k = 0; k < prologue.params.size(); k++ ) {
                const ArgLoc& a = plan[idx + k];
                const Entry&  e = param_slots[k];

                if( a.reg ) continue;

                I("movw", "r26", "r22");

                if( is_tuple(prologue.params[k]) ) {
                    I("ld", "r24", "X+");
                    I("ld", "r25", "X+");
                    I("movw", "r22", "r26");
                    I("movw", "r26", "r24");
                    copy_ptrX_to_slot(e.off, e.slot_bytes);
                } else {
                    copy_ptrX_to_slot(e.off, e.slot_bytes);
                    if( a.wire > e.slot_bytes ) I("adiw", "r26", num(static_cast<long long>(a.wire - e.slot_bytes)));
                    I("movw", "r22", "r26");
                }
            }
        }
    }

    void epilogue(epilogue_t) override {
        const char *id = c(current);
        const std::size_t frame = normalized_stacksz();

        into = false;
        emit(".set .LFRAME%d, %zu\n", func,  frame);
        emit(".LFE%d:\n", func);

        into = true;

        if( current == entrypoint ) {
            const std::string halt = ".LHALT" + std::to_string(func);

            I("cli");
            L(halt);
            I("rjmp", halt);
        } else {
            if( frame ) {
                long c16 = -static_cast<long>(frame) & 0xFFFF;
                I("subi", "r28", num(c16 & 0xFF));
                I("sbci", "r29", num((c16 >> 8) & 0xFF));
            }

            I("in", "r0", "0x3f");
            I("cli");
            I("out", "0x3e", "r29");
            I("out", "0x3f", "r0");
            I("out", "0x3d", "r28");
            I("pop", "r29");
            I("pop", "r28");
            I("ret");
        }

        into = false;
        emit(".size %s, .-%s\n\n", repeat2(id));

        func++;
    }

    void extern_op(extern_op_t op) override {
        externs.insert(op.name);

        bool old = into;
        into = false;
        emit(".extern %s\n", c(op.name));
        into = old;
    }

    void unfold_op(unfold_op_t) override {}

    void destroyer_op(destroyer_op_t op) override {
        for( std::size_t k = 0; k < op.args.size() && k < param_slots.size(); k++ )
            vars[op.args[k].name] = param_slots[k];
    }

    void alloc_op(alloc_op_t op) override {
        vars[op.dst] = make_slot(op.type);
    }

    void ptr_to_op(ptr_to_op_t op) override {
        const Ty ptr = make_ty(morgana_type::mptr);

        if( op.function ) {
            std::string fn = resolve(op.target);
            Entry d = make_slot(ptr);

            // word address: this is what `icall` expects
            I("ldi", "r24", "lo8(gs(" + fn + "))");
            I("ldi", "r25", "hi8(gs(" + fn + "))");
            store_slot_bytes(24, 2, d.off);
            vars[op.dst] = d;
            return;
        }

        Entry e = var(op.target);
        e.type = ptr;
        e.addr = true;
        vars[op.dst] = e;
    }

    void move_op(move_op_t op) override {
        auto it = vars.find(op.src);
        if( it != vars.end() ) { vars[op.dst] = it->second; return; }

        fn_alias[op.dst] = resolve(op.src);
    }

    void load_op(load_op_t op) override {
        Entry e = var(op.src);
        e.addr = false;
        vars[op.dst] = e;
    }

    void pack_op(pack_op_t op) override {
        Entry d = make_slot(op.type);

        std::vector<std::size_t> offs;
        tuple_layout(op.type.tuple, nullptr, &offs);

        for( std::size_t k = 0; k < op.elements.size(); k++ ) {
            const auto& decl = op.type.tuple.elements[k];
            Ty el = make_ty(decl.first, decl.second);
            const Entry& s = var(op.elements[k]);
            long to = static_cast<long>(offs[k]);

            if( s.addr ) {
                addr_to_pair(24, s.off);
                store_slot_bytes(24, 2, d.off + to);
            } else {
                copy_slot_slot(s.off, d.off + to, size_of(el));
            }
        }

        vars[op.dst] = d;
    }

    void deref_op(deref_op_t op) override {
        const Entry& src = var(op.src);

        if( op.via_lea ) addr_to_pair(26, src.off);
        else             load_ptr_slot(26, src.off);

        Entry d = make_slot(op.type);
        copy_ptrX_to_slot(d.off, d.slot_bytes);
        vars[op.dst] = d;
    }

    void store_op(store_op_t op) override {
        const Entry t = var(op.target);
        const std::size_t n = t.slot_bytes;
        const bool scalar = n == 1 || n == 2 || n == 4 || n == 8;

        if(! op.value.literal ) {
            const Entry& s = var(op.value.text);

            if( is_tuple(s.type) && ! s.addr ) {
                copy_slot_slot(s.off, t.off, std::min(n, size_of(s.type)));
                return;
            }
        }

        if(! scalar ) throw std::runtime_error("codegen: can't store a scalar into '" + op.target + "'");

        load_operand_bytes(18, n, op.value);
        store_slot_bytes(18, n, t.off);
    }

    void storep_op(storep_op_t op) override {
        const Entry& p = var(op.ptr);
        const std::size_t n = size_of(op.type);

        if( op.via_lea ) addr_to_pair(26, p.off);
        else             load_ptr_slot(26, p.off);

        if(! op.value.literal ) {
            const Entry& s = var(op.value.text);

            if( is_tuple(s.type) && ! s.addr ) {
                I("movw", "r30", "r26");
                copy_slot_to_ptrZ(s.off, n);
                return;
            }
        }

        load_operand_bytes(18, n, op.value);
        for( std::size_t k = 0; k < n; k++ ) I("st", "X+", rk(18, k));
    }

    void str_op(str_op_t op) override {
        auto it = strings.find(op.bytes);

        if( it == strings.end() ) {
            it = strings.emplace(op.bytes, nstrings++).first;

            bool old = into;
            into = false;

            // SRAM: the entry function copies .data from flash at boot
            emit(".pushsection .data\n.LS%d:\n", it->second);

            if(! op.bytes.empty() ) {
                std::string line = ".byte ";
                for( std::size_t k = 0; k < op.bytes.size(); k++ ) {
                    if( k ) line += ",";
                    line += std::to_string(static_cast<unsigned>(static_cast<unsigned char>(op.bytes[k])));
                }
                emit("%s\n", line.c_str());
            }

            emit(".popsection\n");
            into = old;
        }

        Entry d = make_slot(make_ty(morgana_type::mptr));
        const std::string s = ".LS" + std::to_string(it->second);

        I("ldi", "r24", "lo8(" + s + ")");
        I("ldi", "r25", "hi8(" + s + ")");
        store_slot_bytes(24, 2, d.off);
        vars[op.dst] = d;
    }

    void call_op(call_op_t op) override {
        call_common(op.dst, op.ret, op.args, op.sret, &op.callee, nullptr, false);
    }

    void call_indirect_op(call_indirect_op_t op) override {
        Entry t = var(op.target);
        call_common(op.dst, op.ret, op.args, op.sret, nullptr, &t, op.via_lea);
    }

    void ret_op(ret_op_t op) override {
        if( fn_sret ) {
            if( op.value.literal )
                throw std::runtime_error("codegen: a tuple function can't return a literal");

            const Entry& s = var(op.value.text);
            load_ptr_slot(30, sret_off);
            copy_slot_to_ptrZ(s.off, size_of(fn_ret));
        } else if(! is_void(fn_ret) ) {
            const std::size_t w = even(size_of(fn_ret));
            load_operand_bytes(26 - static_cast<int>(w), w, op.value);
        }

        I("jmp", ".LFE" + std::to_string(func));
    }

    void syscall_op(syscall_op_t) override {
        throw std::runtime_error("codegen: syscall doesn't exist on avr_atmega328p (bare metal); use the loc ops");
    }

    void binary_op(binary_op_t op) override {
        const std::string& o = op.op;
        const std::size_t N = size_of(op.operand);

        if( N == 0 || N > 8 )
            throw std::runtime_error("codegen: unsupported operand size for '" + o + "'");

        Entry d = make_slot(op.result);
        const bool rsg = is_signed_kind(op.result.kind);

        if( o == "shl" || o == "shr" || o == "sar" ) {
            load_operand_bytes(18, N, op.a);
            load_operand_bytes(26, 1, op.b);                 // shift count

            const std::string top = new_label(), end = new_label();

            I("tst", "r26");
            I("breq", end);
            L(top);

            if( o == "shl" ) {
                I("lsl", "r18");
                for( std::size_t k = 1; k < N; k++ ) I("rol", rk(18, k));
            } else {
                I(o == "shr" ? "lsr" : "asr", rk(18, N - 1));
                for( std::size_t k = N - 1; k-- > 0; ) I("ror", rk(18, k));
            }

            I("dec", "r26");
            I("brne", top);
            L(end);

            store_regs(d, 18, N, rsg);
        }
        else if( o == "mul" || o == "sdiv" || o == "udiv" || o == "srem" || o == "urem" ) {
            // avr-libgcc: mul  A,B -> A      (A: r24 / r25:r24 / r25:r22,  B: r22 / r23:r22 / r21:r18)
            //             div  quotient and remainder come back in fixed registers
            const std::size_t M = N <= 1 ? 1 : N <= 2 ? 2 : N <= 4 ? 4 : 0;
            if(! M ) throw std::runtime_error("codegen: 64-bit mul/div/rem isn't supported on AVR");

            const int ab = M == 4 ? 22 : 24;
            const int bb = M == 4 ? 18 : 22;

            load_operand_bytes(ab, M, op.a);
            load_operand_bytes(bb, M, op.b);

            if( o == "mul" ) {
                I("call", M == 1 ? "__mulqi3" : M == 2 ? "__mulhi3" : "__mulsi3");
                store_regs(d, ab, M, rsg);
            } else {
                static const char* names[2][3] = {
                    { "__udivmodqi4", "__udivmodhi4", "__udivmodsi4" },
                    { "__divmodqi4",  "__divmodhi4",  "__divmodsi4"  },
                };

                const bool sgn  = o[0] == 's';
                const bool quot = o[1] == 'd';
                const int  idx  = M == 1 ? 0 : M == 2 ? 1 : 2;
                const int  qb   = M == 1 ? 24 : M == 2 ? 22 : 18;   // quotient
                const int  rb   = M == 1 ? 25 : M == 2 ? 24 : 22;   // remainder

                I("call", names[sgn ? 1 : 0][idx]);
                store_regs(d, quot ? qb : rb, M, rsg);
            }
        }
        else {
            static const std::unordered_set<std::string> cmp_ops = { "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge" };
            static const std::unordered_set<std::string> alu_ops = { "add", "sub", "and", "or", "xor" };

            const bool cmp = cmp_ops.count(o) > 0;
            if(! cmp && ! alu_ops.count(o) ) throw std::runtime_error("codegen: unknown binary operation '" + o + "'");

            // A in r18.. ; B is streamed byte by byte so the carry survives (ld/ldi/mov keep SREG)
            load_operand_bytes(18, N, op.a);

            enum { LIT, ADDR, NEAR, FAR } kind = LIT;
            long long   lit  = 0;
            long        boff = 0;
            std::size_t m    = 0;
            bool        sg   = false;

            if( op.b.literal ) {
                kind = LIT;
                lit  = parse_lit(op.b.text);
            } else {
                const Entry& e = var(op.b.text);

                if( e.addr || is_tuple(e.type) ) {
                    kind = ADDR;
                    addr_to_pair(26, e.off);
                } else {
                    boff = e.off;
                    m    = std::min(e.slot_bytes, N);
                    sg   = is_signed_kind(e.type.kind);

                    if( N > m && sg && m > 0 ) {              // sign byte in r27
                        load_slot_bytes(27, 1, e.off + static_cast<long>(m) - 1);
                        I("lsl", "r27");
                        I("sbc", "r27", "r27");
                    }

                    if( reach(e.off, m) ) kind = NEAR;
                    else { kind = FAR; addr_to_pair(30, e.off); }
                }
            }

            auto src = [&](std::size_t k) -> std::string {
                if( kind == LIT )  { I("ldi", "r26", num((lit >> (8 * k)) & 0xFF)); return "r26"; }
                if( kind == ADDR ) return k == 0 ? "r26" : k == 1 ? "r27" : "r1";
                if( k >= m )       return sg ? "r27" : "r1";

                if( kind == NEAR ) I("ldd", "r0", ypl(boff, k));
                else               I("ld", "r0", "Z+");
                return "r0";
            };

            for( std::size_t k = 0; k < N; k++ ) {
                const std::string s = src(k);
                const std::string a = rk(18, k);

                if( o == "add" )      I(k ? "adc" : "add", a, s);
                else if( o == "sub" ) I(k ? "sbc" : "sub", a, s);
                else if( o == "and" ) I("and", a, s);
                else if( o == "or" )  I("or",  a, s);
                else if( o == "xor" ) I("eor", a, s);
                else                  I(k ? "cpc" : "cp", a, s);
            }

            if( cmp ) {
                // flags are those of A - B (carry chain): ldi doesn't touch them
                const std::string t = new_label();

                if( o == "eq" )       { I("ldi", "r18", "1"); I("breq", t); I("ldi", "r18", "0"); }
                else if( o == "ne" )  { I("ldi", "r18", "1"); I("brne", t); I("ldi", "r18", "0"); }
                else if( o == "slt" ) { I("ldi", "r18", "1"); I("brlt", t); I("ldi", "r18", "0"); }
                else if( o == "sge" ) { I("ldi", "r18", "1"); I("brge", t); I("ldi", "r18", "0"); }
                else if( o == "ult" ) { I("ldi", "r18", "1"); I("brlo", t); I("ldi", "r18", "0"); }
                else if( o == "uge" ) { I("ldi", "r18", "1"); I("brsh", t); I("ldi", "r18", "0"); }
                else if( o == "sgt" ) { I("ldi", "r18", "0"); I("breq", t); I("brlt", t); I("ldi", "r18", "1"); }
                else if( o == "ugt" ) { I("ldi", "r18", "0"); I("breq", t); I("brlo", t); I("ldi", "r18", "1"); }
                else if( o == "sle" ) { I("ldi", "r18", "1"); I("breq", t); I("brlt", t); I("ldi", "r18", "0"); }
                else                  { I("ldi", "r18", "1"); I("breq", t); I("brlo", t); I("ldi", "r18", "0"); }   // ule

                L(t);
                store_regs(d, 18, 1, false);
            } else {
                store_regs(d, 18, N, rsg);
            }
        }

        vars[op.dst] = d;
    }

    void cast_op(cast_op_t op) override {
        Entry d = make_slot(op.to);

        if( d.slot_bytes ) {
            load_operand_bytes(18, d.slot_bytes, Operand{ op.src, false });
            store_slot_bytes(18, d.slot_bytes, d.off);
        }

        vars[op.dst] = d;
    }

    void label_op(label_op_t op) override {
        bool old = into;
        into = false;
        emit("%s:\n", c(label_of(op.name)));
        into = old;
    }

    void br_op(br_op_t op) override {
        I("jmp", label_of(op.target));
    }

    void branch_op(branch_op_t op) override {
        const Entry& e = var(op.cond);
        const std::size_t n = std::min<std::size_t>(e.addr || is_tuple(e.type) ? 2 : e.slot_bytes, 8);

        if( n == 0 ) throw std::runtime_error("codegen: branch on an empty value");

        load_operand_bytes(18, n, Operand{ op.cond, false });

        if( n == 1 ) I("tst", "r18");
        else         for( std::size_t k = 1; k < n; k++ ) I("or", "r18", rk(18, k));

        // jmp reaches everywhere, the conditional branches don't: skip over it
        const std::string skip = new_label();

        I(op.nonzero ? "breq" : "brne", skip);
        I("jmp", label_of(op.target));
        L(skip);
    }

    void loc_load_op(loc_load_op_t op) override {
        loc_read(op.loc);

        Entry d = make_slot(unsigned_type(op.loc.bits));
        store_regs(d, 18, loc_bytes(op.loc), false);
        vars[op.dst] = d;
    }

    void loc_store_op(loc_store_op_t op) override {
        load_operand_bytes(18, loc_bytes(op.loc), op.value);
        loc_write(op.loc);
    }

    void loc_get_op(loc_get_op_t op) override {
        const std::size_t N = loc_bytes(op.loc);

        loc_read(op.loc);
        acc_shr_const(N, static_cast<unsigned>(op.lo));
        acc_mask(N, op.width);

        Entry d = make_slot(op.type);
        store_regs(d, 18, N, false);
        vars[op.dst] = d;
    }

    void loc_set_op(loc_set_op_t op) override {
        const std::size_t N = loc_bytes(op.loc);

        // r18.. <- (value & width_mask) << lo
        load_operand_bytes(18, N, op.value);
        acc_mask(N, op.width);
        acc_shl_const(N, static_cast<unsigned>(op.lo));

        std::uint64_t field = width_mask(op.width) << op.lo;
        std::uint64_t keep  = ~field & width_mask(op.loc.bits);

        // read-modify-write, one byte at a time
        for( std::size_t k = 0; k < N; k++ ) {
            loc_rd(26, op.loc, k);
            I("andi", "r26", num((keep >> (8 * k)) & 0xFF));
            I("or", "r26", rk(18, k));
            loc_wr(26, op.loc, k);
        }
    }
};

inline std::size_t avr_atmega328p::size_of(const Ty& t) {
    if( is_tuple(t) ) return tuple_layout(t.tuple);
    if( is_void(t) )  return 0;
    if( auto s = fixed_size(t.kind) ) return *s;
    return 2;   // int, uint, ptr
}

inline std::size_t avr_atmega328p::align_of(const Ty&) {
    return 1;   // AVR has no alignment requirements
}
