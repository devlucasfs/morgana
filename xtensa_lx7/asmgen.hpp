#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "src/codegen/interface.hpp"

/*
 * Xtensa LX7 (ESP32-S3) / GNU as backend, CALL0 ABI (no register windows).
 *
 * The parser only hands over NAMES and TYPES. Everything physical lives here:
 *
 *   - sizes / alignment of every type (int, uint and ptr are 4 bytes: this is a 32-bit
 *     machine; tuples use natural alignment, fields in order, size rounded up to the
 *     biggest alignment). i64 / u64 are 8 bytes, 8-aligned;
 *   - 64-bit scalars live in REGISTER PAIRS (low word, high word):
 *         first operand  = (a2, a3)     second operand = (a4, a5)     result = (a2, a3)
 *         scratch for the 64-bit paths: a6, a7, a9
 *     add / sub / mul / and / or / xor / shifts / comparisons are open-coded; div / rem call
 *     libgcc (__divdi3, __udivdi3, __moddi3, __umoddi3), which take and return (a2,a3) /
 *     (a4,a5) in CALL0. `mul` needs the MUL32H option (muluh), the LX7 of the ESP32-S3 has it.
 *     A 32-bit value used where a 64-bit one is expected is extended by ITS signedness;
 *     the other way around (64 -> 32) is only allowed through `cast` (it keeps the low word);
 *   - Xtensa loads/stores only take UNSIGNED immediate offsets (l32i: 0..1020, l16: 0..510,
 *     l8: 0..255), so there is no "negative rbp" frame. Instead a15 is the frame pointer and
 *     points to the LOW end of the frame; every slot lives at a15 + off (off > 0).
 *         a15+0  : saved a0 (return address)
 *         a15+4  : saved caller's a15
 *         a15+8… : slots
 *     The frame size is only known at the end of the function, so the prologue uses
 *     `movi a8, .LFRAME<n>` and the epilogue `.set`s it. Offsets out of the immediate
 *     range are materialised in a11 automatically.
 *   - CALL0 calling convention: a2..a7 carry the arguments, the rest goes to the stack at
 *     sp+0, sp+4, ... at the call. Return in a2 (a2:a3 for 64 bits). a0 = return address,
 *     a1 = sp, a12..a15 are callee-saved. A 64-bit argument takes two consecutive words and
 *     starts on an EVEN word (a2/a4/a6, or an 8-aligned stack offset); an argument that does
 *     not fit entirely in registers goes to the stack, and so does everything after it.
 *     A function that returns a tuple gets a hidden pointer in a2 (sret). A tuple ARGUMENT is
 *     passed by pointer (the callee copies it, so it still behaves like a value).
 *     The signature of a direct call comes from the `extern` / `unfold` / definition seen
 *     before it; for `call_indirect` it is inferred from the operands (a literal is 32 bits).
 *
 * Scratch registers (never live across a statement):
 *     a2  accumulator / result        a3  second operand (32-bit paths)
 *     a4  address                     a8  memory-to-memory copy
 *     a10 call target / loc_set value a11 address scratch used by the offset-range fixups
 *
 * How a name is read:
 *   - a normal slot (alloc / call result / load / ...) is read as its CONTENTS, extended to
 *     32 (or 64) bits by the signedness of its type;
 *   - the result of `ptr_to` is read as the ADDRESS of the slot it points to;
 *   - deref / storep / call_indirect follow the parser's `via_lea` flag: true means "the
 *     pointer is the address of the slot", false means "the pointer is stored in the slot".
 *
 * Loc spaces:
 *   - reg / sreg: "a0".."a15" (plain mov / extui), "sr.<name>" (rsr/wsr, e.g. sr.ccount,
 *     sr.ps, sr.vecbase), "ur.<name>" (rur/wur). For `sreg` the "sr." prefix is optional.
 *     32 bits for special registers.
 *   - mem: absolute 32-bit address, fenced with `memw` (peripherals). Whole 64-bit
 *     mem_load / mem_store are two 32-bit accesses (low word first).
 *   - io : Xtensa has no port I/O, so io_* is the same as mem_* (the address is an absolute
 *          32-bit address, e.g. 0x60000000.. for the ESP32-S3 peripherals).
 *          vreg: no SIMD here, so it throws.
 *
 * Needs the 32-bit mul/div and sign-extend options, which the LX7 in the ESP32-S3 has.
 * Branches are relaxed by the assembler (bnez etc. have a short range), calls use call0 /
 * callx0 and so every function is aligned to 4 bytes.
 */
class xtensa_lx7 : public codegen_interface_methods {
public:
    const std::string entrypoint = "app_main";
private:
    std::size_t stack_sz = 0;
    std::string current;
    int func = 0;

    // ───────────── per-name bookkeeping ─────────────

    struct Entry {
        Ty          type;         // type of the value as seen by the expressions
        long        off  = 0;     // the slot starts at off(a15)
        bool        addr = false; // `ptr_to` result: reads as the address of the slot
        std::size_t slot_bytes = 0;
    };

    std::unordered_map<std::string, Entry>            vars;      // reset at every function
    std::unordered_map<std::string, std::string>      fn_alias;  // `move g, f` where f is a function
    std::unordered_map<std::string, std::vector<Ty>>  sigs;      // function name -> parameter types
    std::unordered_set<std::string>                   externs;
    std::unordered_map<std::string, int>              strings;   // literal bytes -> .LS<n>
    int nstrings = 0;

    // current function
    Ty                 fn_ret;
    bool               fn_sret = false;
    long               sret_off = 0;
    std::vector<Entry> param_slots;

    enum R { A0, A1, A2, A3, A4, A5, A6, A7, A8, A9, A10, A11, A12, A13, A14, A15 };

    static constexpr R ACC = A2;   // accumulator / return value
    static constexpr R SEC = A3;   // second operand
    static constexpr R ADR = A4;   // address
    static constexpr R TMP = A8;   // copy temporary
    static constexpr R TGT = A10;  // call target / loc_set value
    static constexpr R SCR = A11;  // offset-range scratch

    static constexpr long SAVE_AREA = 8;   // saved a0 + saved a15

    static std::size_t align_up(std::size_t v, std::size_t a) {
        return (v + a - 1) & ~(a - 1);
    }

    static std::size_t size_of(const Ty& t);
    static std::size_t align_of(const Ty& t);

    // an i64 / u64 scalar (lives in a register pair)
    static bool wide(const Ty& t) {
        return ! is_tuple(t) && ! is_void(t) && ! is_vec(t) && size_of(t) == 8;
    }

    static std::size_t tuple_layout(const morgana_tuple& t, std::size_t* align = nullptr,
                                    std::vector<std::size_t>* offsets = nullptr) {
        std::size_t off = 0, max_al = 1;

        for( const auto& el : t.elements ) {
            Ty e = make_ty(el.first, el.second);
            std::size_t a = align_of(e);

            off = align_up(off, a);
            if( offsets ) offsets->push_back(off);
            off += size_of(e);
            max_al = std::max(max_al, a);
        }

        if( align ) *align = max_al;
        return align_up(off, max_al);
    }

    static bool is_signed_kind(morgana_type k) {
        #define X(m, str, sgn, size) if( k == morgana_type::m ) return int(sgn) == 1;
        TYPE_SET(X)
        #undef X
        return false;
    }

    static std::string rn(R r) { return "a" + std::to_string(static_cast<int>(r)); }

    static std::string n(long long v) { return std::to_string(v); }

    static std::string strip(const std::string& s) {
        return (! s.empty() && (s[0] == '%' || s[0] == '$')) ? s.substr(1) : s;
    }

    static bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

    static std::uint64_t width_mask(std::size_t bits) {
        return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
    }

    static long long parse_lit(const std::string& s) {
        try { return std::stoll(s); }
        catch( ... ) { return static_cast<long long>(std::stoull(s)); }
    }

    // one instruction: `mnemonic a, b, c, d`
    void I(const std::string& mn, const std::string& a = "", const std::string& b = "",
           const std::string& c = "", const std::string& d = "") {
        std::string s = mn;
        const std::string* ops[4] = { &a, &b, &c, &d };
        bool first = true;

        for( const std::string* o : ops ) {
            if( o->empty() ) break;
            s += first ? " " : ", ";
            s += *o;
            first = false;
        }

        emit("%s\n", s.c_str());
    }

    std::string label_of(const std::string& name) const {
        return ".L" + std::to_string(func) + "_" + name;
    }

    std::size_t normalized_stacksz() {
        return (stack_sz + 15) & ~static_cast<std::size_t>(15);
    }

    long alloca_bytes(std::size_t size, std::size_t align) {
        if( align == 0 ) align = 1;
        stack_sz = align_up(stack_sz, align);
        long off = static_cast<long>(stack_sz);
        stack_sz += size;
        return off;
    }

    Entry make_slot(const Ty& t) {
        Entry e;
        e.type       = t;
        e.slot_bytes = size_of(t);
        e.off        = alloca_bytes(e.slot_bytes, align_of(t));
        return e;
    }

    const Entry& var(const std::string& name) const {
        auto it = vars.find(name);
        if( it == vars.end() )
            throw std::runtime_error("codegen: unknown name '" + name + "' in " + current);
        return it->second;
    }

    std::string resolve(std::string name) const {
        for( auto it = fn_alias.find(name); it != fn_alias.end(); it = fn_alias.find(name) )
            name = it->second;
        return name;
    }

    // ───────────── memory access (unsigned immediate offsets only) ─────────────

    static void need_scalar(std::size_t size) {
        if( size > 4 )
            throw std::runtime_error("codegen: a 64-bit value can't be used as a 32-bit one here "
                                     "(use `cast` to truncate it)");
    }

    // brings `off(base)` inside the instruction's immediate range, using a11 if needed
    void addr_fix(std::size_t size, std::string& base, long& off) {
        long limit = size == 1 ? 255 : size == 2 ? 510 : 1020;
        long sz    = static_cast<long>(size);

        if( off >= 0 && off <= limit && off % sz == 0 ) return;

        I("movi", rn(SCR), n(off));
        I("add",  rn(SCR), rn(SCR), base);
        base = rn(SCR);
        off  = 0;
    }

    void ld(R dst, std::size_t size, bool sg, std::string base, long off) {
        need_scalar(size);
        addr_fix(size, base, off);

        switch( size ) {
            case 1:
                I("l8ui", rn(dst), base, n(off));
                if( sg ) I("sext", rn(dst), rn(dst), "7");
                break;
            case 2:
                I(sg ? "l16si" : "l16ui", rn(dst), base, n(off));
                break;
            default:
                I("l32i", rn(dst), base, n(off));
                break;
        }
    }

    void st(R src, std::size_t size, std::string base, long off) {
        need_scalar(size);
        addr_fix(size, base, off);

        I(size == 1 ? "s8i" : size == 2 ? "s16i" : "s32i", rn(src), base, n(off));
    }

    // 64 bits = two words, low word first (little endian)
    void ld64(R lo, R hi, const std::string& base, long off) {
        ld(lo, 4, false, base, off);
        ld(hi, 4, false, base, off + 4);
    }

    void st64(R lo, R hi, const std::string& base, long off) {
        st(lo, 4, base, off);
        st(hi, 4, base, off + 4);
    }

    void load_slot(R r, const Entry& e) {
        ld(r, size_of(e.type), is_signed_kind(e.type.kind), "a15", e.off);
    }

    void store_slot(R r, const Entry& e, std::size_t size) {
        st(r, size, "a15", e.off);
    }

    // r = a15 + off
    void lea(R r, long off) {
        if( off >= -128 && off <= 127 ) {
            I("addi", rn(r), "a15", n(off));
        } else {
            I("movi", rn(r), n(off));
            I("add",  rn(r), rn(r), "a15");
        }
    }

    void adj_sp(long delta) {
        if( delta == 0 ) return;

        if( delta >= -128 && delta <= 127 ) {
            I("addi", "a1", "a1", n(delta));
        } else {
            I("movi", rn(SCR), n(delta));
            I("add",  "a1", "a1", rn(SCR));
        }
    }

    void load_imm(R r, long long v) {
        if( v < INT32_MIN || v > 0xFFFFFFFFLL )
            throw std::runtime_error("codegen: literal doesn't fit in 32 bits on xtensa");

        I("movi", rn(r), n(static_cast<std::int32_t>(static_cast<std::uint32_t>(v))));
    }

    void load_operand(R r, const Operand& op) {
        if( op.literal ) { load_imm(r, parse_lit(op.text)); return; }

        const Entry& e = var(op.text);
        if( e.addr || is_tuple(e.type) ) lea(r, e.off);
        else                             load_slot(r, e);
    }

    // hi = the sign (or zero) extension of lo
    void extend_hi(R hi, R lo, bool sg) {
        if( sg ) I("srai", rn(hi), rn(lo), "31");
        else     I("movi", rn(hi), "0");
    }

    // (lo, hi) = a 64-bit value: a literal, a 64-bit slot, or a narrower one (extended by its type)
    void load_operand64(R lo, R hi, const Operand& op) {
        if( op.literal ) {
            std::uint64_t u = static_cast<std::uint64_t>(parse_lit(op.text));
            I("movi", rn(lo), n(static_cast<std::int32_t>(static_cast<std::uint32_t>(u))));
            I("movi", rn(hi), n(static_cast<std::int32_t>(static_cast<std::uint32_t>(u >> 32))));
            return;
        }

        const Entry& e = var(op.text);

        if( e.addr || is_tuple(e.type) ) {   // an address: 32 bits, zero extended
            lea(lo, e.off);
            I("movi", rn(hi), "0");
            return;
        }

        if( wide(e.type) ) { ld64(lo, hi, "a15", e.off); return; }

        load_slot(lo, e);
        extend_hi(hi, lo, is_signed_kind(e.type.kind));
    }

    void normalize(R r, std::size_t size, bool sgn) {
        if( size >= 4 ) return;

        if( sgn ) I("sext", rn(r), rn(r), size == 1 ? "7" : "15");
        else      I("extui", rn(r), rn(r), "0", size == 1 ? "8" : "16");
    }

    // memory -> memory through a8. `al` is the alignment of the data (never read more
    // than that at once: Xtensa faults on unaligned l32i/l16)
    void copy(const std::string& sb, long sd, const std::string& db, long dd,
              std::size_t nbytes, std::size_t al) {
        if( al == 0 || al > 4 ) al = 4;

        std::size_t done = 0;

        while( done < nbytes ) {
            std::size_t left  = nbytes - done;
            std::size_t chunk = (left >= 4 && al >= 4) ? 4 : (left >= 2 && al >= 2) ? 2 : 1;

            ld(TMP, chunk, false, sb, sd + static_cast<long>(done));
            st(TMP, chunk, db, dd + static_cast<long>(done));
            done += chunk;
        }
    }

    void local_label(const char* l) {
        bool old = into;
        into = false;
        emit("%s:\n", l);
        into = old;
    }

    // ───────────── calling convention ─────────────

    struct ArgLoc {
        bool reg  = true;    // in registers (first word is ARG[w]) or on the stack
        int  w    = 0;       // register word index (0 = a2)
        long soff = 0;       // stack offset (sp+soff at the call / frame+soff in the callee)
        bool wide = false;
    };

    static std::vector<ArgLoc> arg_layout(const std::vector<Ty>& types, bool sret) {
        std::vector<ArgLoc> out;
        int  w = sret ? 1 : 0;
        long so = 0;
        bool spill = false;

        for( const Ty& t : types ) {
            ArgLoc a;
            a.wide = wide(t);
            const int words = a.wide ? 2 : 1;

            if(! spill && a.wide ) w = (w + 1) & ~1;

            if(! spill && w + words <= 6 ) {
                a.reg = true;
                a.w   = w;
                w    += words;
            } else {
                spill = true;
                a.reg = false;
                if( a.wide ) so = static_cast<long>(align_up(static_cast<std::size_t>(so), 8));
                a.soff = so;
                so += 4 * words;
            }

            out.push_back(a);
        }

        return out;
    }

    Ty operand_ty(const Operand& op) const {
        if( op.literal ) return make_ty(morgana_type::mint);

        const Entry& e = var(op.text);
        if( e.addr || is_tuple(e.type) ) return make_ty(morgana_type::mptr);
        return e.type;
    }

    void call_common(const std::string& dst, const Ty& ret, const std::vector<Operand>& args,
                     bool sret, const std::string* direct, const Entry* target, bool target_lea) {
        static const R ARG[6] = { A2, A3, A4, A5, A6, A7 };

        // parameter types: the declared signature if it is a direct call, the operands otherwise
        const std::vector<Ty>* sig = nullptr;
        if( direct ) {
            auto it = sigs.find(resolve(*direct));
            if( it != sigs.end() ) sig = &it->second;
        }

        std::vector<Ty> pt;
        for( std::size_t k = 0; k < args.size(); k++ )
            pt.push_back(sig && k < sig->size() ? (*sig)[k] : operand_ty(args[k]));

        const std::vector<ArgLoc> locs = arg_layout(pt, sret);

        Entry d;
        const bool has_ret = ! is_void(ret);
        if( has_ret ) d = make_slot(ret);

        long stack_bytes = 0;
        for( const ArgLoc& l : locs )
            if(! l.reg ) stack_bytes = std::max(stack_bytes, l.soff + (l.wide ? 8L : 4L));

        const long area = static_cast<long>(align_up(static_cast<std::size_t>(stack_bytes), 16));

        // stack arguments live at sp+0, sp+4, ... when the call is made
        adj_sp(-area);

        for( std::size_t k = 0; k < args.size(); k++ ) {
            if( locs[k].reg ) continue;

            if( locs[k].wide ) {
                load_operand64(A2, A3, args[k]);
                st64(A2, A3, "a1", locs[k].soff);
            } else {
                load_operand(ACC, args[k]);
                st(ACC, 4, "a1", locs[k].soff);
            }
        }

        if( target ) {
            if( target_lea ) lea(TGT, target->off);
            else             ld(TGT, 4, false, "a15", target->off);
        }

        for( std::size_t k = 0; k < args.size(); k++ ) {
            if(! locs[k].reg ) continue;

            if( locs[k].wide ) load_operand64(ARG[locs[k].w], ARG[locs[k].w + 1], args[k]);
            else               load_operand(ARG[locs[k].w], args[k]);
        }

        if( sret ) lea(A2, d.off);

        if( direct ) I("call0", resolve(*direct));
        else         I("callx0", rn(TGT));

        adj_sp(area);

        if( has_ret && ! is_tuple(ret) ) {
            if( wide(ret) ) st64(A2, A3, "a15", d.off);
            else            store_slot(ACC, d, size_of(ret));
        }
        if(! dst.empty() && has_ret ) vars[dst] = d;
    }

    // ───────────── 64-bit arithmetic: (a2,a3) op (a4,a5) -> (a2,a3) ─────────────

    void binary64(const binary_op_t& op) {
        const std::string& o = op.op;

        load_operand64(A2, A3, op.a);
        load_operand64(A4, A5, op.b);

        bool cmp = false;

        if( o == "add" ) {
            I("add", "a6", "a2", "a4");
            I("bgeu", "a6", "a4", "1f");        // no carry
            I("addi", "a3", "a3", "1");
            local_label("1");
            I("add", "a3", "a3", "a5");
            I("mov", "a2", "a6");
        }
        else if( o == "sub" ) {
            I("sub", "a6", "a2", "a4");
            I("bgeu", "a2", "a4", "1f");        // no borrow
            I("addi", "a3", "a3", "-1");
            local_label("1");
            I("sub", "a3", "a3", "a5");
            I("mov", "a2", "a6");
        }
        else if( o == "mul" ) {
            I("mull",  "a6", "a2", "a4");       // low word
            I("muluh", "a7", "a2", "a4");
            I("mull",  "a9", "a2", "a5");
            I("add",   "a7", "a7", "a9");
            I("mull",  "a9", "a3", "a4");
            I("add",   "a7", "a7", "a9");
            I("mov", "a2", "a6");
            I("mov", "a3", "a7");
        }
        else if( o == "and" || o == "or" || o == "xor" ) {
            I(o, "a2", "a2", "a4");
            I(o, "a3", "a3", "a5");
        }
        else if( o == "shl" ) {
            I("movi", "a6", "63");
            I("and", "a4", "a4", "a6");
            I("bgeui", "a4", "32", "1f");
            I("ssl", "a4");                     // SAR = 32 - n
            I("src", "a3", "a3", "a2");
            I("sll", "a2", "a2");
            I("j", "2f");
            local_label("1");
            I("addi", "a4", "a4", "-32");
            I("ssl", "a4");
            I("sll", "a3", "a2");
            I("movi", "a2", "0");
            local_label("2");
        }
        else if( o == "shr" || o == "sar" ) {
            const bool arith = o == "sar";
            I("movi", "a6", "63");
            I("and", "a4", "a4", "a6");
            I("bgeui", "a4", "32", "1f");
            I("ssr", "a4");                     // SAR = n
            I("src", "a2", "a3", "a2");
            I(arith ? "sra" : "srl", "a3", "a3");
            I("j", "2f");
            local_label("1");
            I("addi", "a4", "a4", "-32");
            I("ssr", "a4");
            I(arith ? "sra" : "srl", "a2", "a3");
            if( arith ) I("srai", "a3", "a3", "31");
            else        I("movi", "a3", "0");
            local_label("2");
        }
        else if( o == "sdiv" || o == "udiv" || o == "srem" || o == "urem" ) {
            // libgcc: (a2,a3) / (a4,a5) -> (a2,a3), exactly where we already have them
            const char* fn = o == "sdiv" ? "__divdi3"  : o == "udiv" ? "__udivdi3"
                           : o == "srem" ? "__moddi3"  : "__umoddi3";
            I("call0", fn);
        }
        else if( o == "eq" || o == "ne" ) {
            cmp = true;
            I("xor", "a6", "a2", "a4");
            I("xor", "a7", "a3", "a5");
            I("or",  "a6", "a6", "a7");
            I("movi", "a7", "1");
            I(o == "eq" ? "beqz" : "bnez", "a6", "1f");
            I("movi", "a7", "0");
            local_label("1");
            I("mov", "a6", "a7");
        }
        else if( o == "slt" || o == "sle" || o == "sgt" || o == "sge"
              || o == "ult" || o == "ule" || o == "ugt" || o == "uge" ) {
            cmp = true;

            // everything is `X < Y`, swapped for gt / le and negated for le / ge
            const bool sg   = o[0] == 's';
            const bool swap = o == "sgt" || o == "sle" || o == "ugt" || o == "ule";
            const bool neg  = o == "sle" || o == "sge" || o == "ule" || o == "uge";

            const std::string x0 = swap ? "a4" : "a2", x1 = swap ? "a5" : "a3";
            const std::string y0 = swap ? "a2" : "a4", y1 = swap ? "a3" : "a5";

            I("movi", "a6", "1");
            I("beq", x1, y1, "1f");
            I(sg ? "blt" : "bltu", x1, y1, "3f");     // the high words decide
            I("movi", "a6", "0");
            I("j", "3f");
            local_label("1");
            I("bltu", x0, y0, "3f");                  // equal high words: low words, unsigned
            I("movi", "a6", "0");
            local_label("3");

            if( neg ) {
                I("movi", "a7", "1");
                I("xor", "a6", "a6", "a7");
            }
        }
        else {
            throw std::runtime_error("codegen: unknown binary operation '" + o + "'");
        }

        Entry d = make_slot(op.result);

        if( cmp ) st(A6, d.slot_bytes, "a15", d.off);   // u8
        else      st64(A2, A3, "a15", d.off);

        vars[op.dst] = d;
    }

    // ───────────── locations (registers / memory / io) ─────────────

    void loc_read(const Loc& l) {
        if( l.space == Loc::Space::vreg )
            throw std::runtime_error("codegen: vector registers don't exist on xtensa lx7");

        if( l.bits > 32 )
            throw std::runtime_error("codegen: locations can't be 64 bits on xtensa "
                                     "(only a whole mem_load / mem_store)");

        const std::size_t bytes = l.bits / 8;

        switch( l.space ) {
            case Loc::Space::reg:
            case Loc::Space::sreg: {
                std::string nm = strip(l.name);
                const bool special = l.space == Loc::Space::sreg || starts(nm, "sr.") || starts(nm, "ur.");

                if( special ) {
                    if( starts(nm, "ur.") ) {
                        I("rur." + nm.substr(3), "a2");
                    } else {
                        if( starts(nm, "sr.") ) nm = nm.substr(3);
                        I("rsr." + nm, "a2");
                    }
                }
                else if( l.bits == 32 )      I("mov", "a2", nm);
                else                         I("extui", "a2", nm, "0", n(l.bits));
                break;
            }

            case Loc::Space::io:    // no port I/O on xtensa: io_* is memory-mapped, same as mem_*
            case Loc::Space::mem:
                if( l.addr > 0xFFFFFFFFULL )
                    throw std::runtime_error("codegen: address doesn't fit in 32 bits on xtensa");
                I("memw");
                load_imm(ADR, static_cast<long long>(l.addr));
                ld(ACC, bytes, false, "a4", 0);
                break;

            case Loc::Space::vreg:
                break;   // handled above
        }
    }

    void loc_write(const Loc& l) {
        if( l.space == Loc::Space::vreg )
            throw std::runtime_error("codegen: vector registers don't exist on xtensa lx7");

        if( l.bits > 32 )
            throw std::runtime_error("codegen: locations can't be 64 bits on xtensa "
                                     "(only a whole mem_load / mem_store)");

        const std::size_t bytes = l.bits / 8;

        switch( l.space ) {
            case Loc::Space::reg:
            case Loc::Space::sreg: {
                std::string nm = strip(l.name);
                const bool special = l.space == Loc::Space::sreg || starts(nm, "sr.") || starts(nm, "ur.");

                if( special ) {
                    if( starts(nm, "ur.") ) {
                        I("wur." + nm.substr(3), "a2");
                    } else {
                        if( starts(nm, "sr.") ) nm = nm.substr(3);
                        I("wsr." + nm, "a2");
                        I("isync");
                    }
                } else if( l.bits == 32 ) {
                    I("mov", nm, "a2");
                } else {
                    // partial write: keep the upper bits of the register
                    std::uint64_t mask = width_mask(l.bits);
                    load_imm(ADR, static_cast<long long>(~mask & 0xFFFFFFFFULL));
                    I("and", "a4", nm, "a4");
                    load_imm(SEC, static_cast<long long>(mask));
                    I("and", "a2", "a2", "a3");
                    I("or", nm, "a4", "a2");
                }
                break;
            }

            case Loc::Space::io:    // no port I/O on xtensa: io_* is memory-mapped, same as mem_*
            case Loc::Space::mem:
                if( l.addr > 0xFFFFFFFFULL )
                    throw std::runtime_error("codegen: address doesn't fit in 32 bits on xtensa");
                I("memw");
                load_imm(ADR, static_cast<long long>(l.addr));
                st(ACC, bytes, "a4", 0);
                I("memw");
                break;

            case Loc::Space::vreg:
                break;   // handled above
        }
    }

public:
    using codegen_interface_methods::codegen_interface_methods;

    void prologue(prologue_t prologue) override {
        stack_sz = SAVE_AREA;
        current  = prologue.identifier;
        vars.clear();
        param_slots.clear();
        sigs[prologue.identifier] = prologue.params;

        const char *id = c(current);
        const std::string frame = ".LFRAME" + std::to_string(func);

        into = false;
        emit(".text\n"
             ".balign 4\n"
             ".globl %s\n"
             ".type %s, @function\n"
             "%s:\n"
             ".LFP%d:\n",
             repeat3(id),
             func);

        into = true;

        const bool entry = current == entrypoint;

        if( entry ) {
            I("movi", "a8", "-16");
            I("and",  "a1", "a1", "a8");
        }

        I("movi", "a8", frame);
        I("sub",  "a1", "a1", "a8");

        if(! entry ) {
            I("s32i", "a0",  "a1", "0");
            I("s32i", "a15", "a1", "4");
        }

        I("mov", "a15", "a1");

        static const R ARG[6] = { A2, A3, A4, A5, A6, A7 };

        fn_ret  = prologue.ret;
        fn_sret = is_tuple(fn_ret);

        if( fn_sret ) {
            sret_off = alloca_bytes(4, 4);
            st(A2, 4, "a15", sret_off);
        }

        const std::vector<ArgLoc> locs = arg_layout(prologue.params, fn_sret);

        for( std::size_t k = 0; k < prologue.params.size(); k++ ) {
            const Ty& p = prologue.params[k];
            const ArgLoc& l = locs[k];
            Entry e = make_slot(p);

            if( l.reg ) {
                if( l.wide )          st64(ARG[l.w], ARG[l.w + 1], "a15", e.off);
                else if( is_tuple(p) ) copy(rn(ARG[l.w]), 0, "a15", e.off, e.slot_bytes, align_of(p));
                else                   store_slot(ARG[l.w], e, e.slot_bytes);
            } else {
                // incoming stack arguments sit just above this function's frame
                I("movi", rn(TGT), frame);
                I("add",  rn(TGT), rn(TGT), "a15");

                if( l.wide ) {
                    ld64(A2, A3, rn(TGT), l.soff);
                    st64(A2, A3, "a15", e.off);
                } else {
                    ld(A2, 4, false, rn(TGT), l.soff);

                    if( is_tuple(p) ) copy("a2", 0, "a15", e.off, e.slot_bytes, align_of(p));
                    else              store_slot(A2, e, e.slot_bytes);
                }
            }

            param_slots.push_back(e);
        }
    }

    void epilogue(epilogue_t epilogue) override {
        const char *id = c(current);

        into = false;
        emit(".set .LFRAME%d, %zu\n", func, normalized_stacksz());
        emit(".LFE%d:\n", func);

        into = true;

        if( current == entrypoint ) {
            // nothing to return to on bare metal: park the core
            into = false;
            emit(".LHALT%d:\n", func);
            into = true;
            I("j", ".LHALT" + std::to_string(func));
        } else {
            I("movi", "a8", ".LFRAME" + std::to_string(func));
            I("add",  "a8", "a15", "a8");          // a8 = caller's sp
            I("l32i", "a0",  "a15", "0");
            I("l32i", "a15", "a15", "4");
            I("mov",  "a1",  "a8");
            I("ret");
        }

        into = false;
        emit(".size %s, .-%s\n\n", repeat2(id));

        func++;
    }

    void extern_op(extern_op_t op) override {
        externs.insert(op.name);
        sigs[op.name] = op.params;

        bool old = into;
        into = false;
        emit(".extern %s\n", c(op.name));
        into = old;
    }

    void unfold_op(unfold_op_t op) override {
        sigs[op.name] = op.params;
    }

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

            I("movi", "a2", fn);      // absolute address through the literal pool
            store_slot(ACC, d, 4);
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
                lea(ACC, s.off);
                st(ACC, 4, "a15", d.off + to);
            } else {
                copy("a15", s.off, "a15", d.off + to, size_of(el), align_of(el));
            }
        }

        vars[op.dst] = d;
    }

    // <dst> = extract <type> <tuple>, <index>
    void extract_op(extract_op_t op) override {
        const Entry& s = var(op.src);

        std::vector<std::size_t> offs;
        tuple_layout(s.type.tuple, nullptr, &offs);

        Entry d = make_slot(op.type);
        copy("a15", s.off + static_cast<long>(offs[op.index]), "a15", d.off,
             d.slot_bytes, align_of(op.type));
        vars[op.dst] = d;
    }

    void deref_op(deref_op_t op) override {
        const Entry& src = var(op.src);

        if( op.via_lea ) lea(ACC, src.off);
        else             ld(ACC, 4, false, "a15", src.off);

        Entry d = make_slot(op.type);
        copy("a2", 0, "a15", d.off, d.slot_bytes, align_of(op.type));
        vars[op.dst] = d;
    }

    void store_op(store_op_t op) override {
        const Entry t = var(op.target);
        const std::size_t nb = t.slot_bytes;
        const bool scalar = nb == 1 || nb == 2 || nb == 4;

        if(! op.value.literal ) {
            const Entry& s = var(op.value.text);

            if( is_tuple(s.type) && ! s.addr ) {
                copy("a15", s.off, "a15", t.off, std::min(nb, size_of(s.type)),
                     std::min(align_of(t.type), align_of(s.type)));
                return;
            }
        }

        if( nb == 8 && ! is_tuple(t.type) ) {    // i64 / u64 slot
            load_operand64(A2, A3, op.value);
            st64(A2, A3, "a15", t.off);
            return;
        }

        if(! scalar ) throw std::runtime_error("codegen: can't store a scalar into '" + op.target + "'");

        load_operand(ACC, op.value);
        store_slot(ACC, t, nb);
    }

    void storep_op(storep_op_t op) override {
        const Entry& p = var(op.ptr);

        if( wide(op.type) ) {
            load_operand64(A2, A3, op.value);

            if( op.via_lea ) lea(ADR, p.off);
            else             ld(ADR, 4, false, "a15", p.off);

            st64(A2, A3, "a4", 0);
            return;
        }

        load_operand(ACC, op.value);

        if( op.via_lea ) lea(ADR, p.off);
        else             ld(ADR, 4, false, "a15", p.off);

        st(ACC, size_of(op.type), "a4", 0);
    }

    void str_op(str_op_t op) override {
        auto it = strings.find(op.bytes);

        if( it == strings.end() ) {
            it = strings.emplace(op.bytes, nstrings++).first;

            bool old = into;
            into = false;

            emit(".pushsection .rodata\n.LS%d:\n", it->second);

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
        I("movi", "a2", ".LS" + std::to_string(it->second));
        store_slot(ACC, d, 4);
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
            ld(ACC, 4, false, "a15", sret_off);
            copy("a15", s.off, "a2", 0, size_of(fn_ret), align_of(fn_ret));
        } else if( wide(fn_ret) ) {
            load_operand64(A2, A3, op.value);        // returned in a2:a3
        } else {
            load_operand(ACC, op.value);
        }

        I("j", ".LFE" + std::to_string(func));
    }

    // Linux/Xtensa convention: number in a2, arguments in a6, a3, a4, a5, a8, a9,
    // result in a2. (On bare metal the `syscall` instruction just raises the SYSCALL
    // exception, so a handler has to implement it.)
    void syscall_op(syscall_op_t op) override {
        static const R ARG[6] = { A6, A3, A4, A5, A8, A9 };

        if( op.args.size() > 6 )
            throw std::runtime_error("codegen: syscall takes at most 6 arguments");

        for( std::size_t k = op.args.size(); k > 0; ) {
            --k;
            load_operand(ARG[k], op.args[k]);
        }

        load_imm(A2, parse_lit(op.number));
        I("syscall");

        Entry d = make_slot(make_ty(morgana_type::mptr));   // 32-bit result
        store_slot(ACC, d, 4);
        vars[op.dst] = d;
    }

    void binary_op(binary_op_t op) override {
        if( wide(op.operand) ) { binary64(op); return; }

        static const std::unordered_set<std::string> signed_ops   = { "sdiv", "srem", "slt", "sle", "sgt", "sge", "sar" };
        static const std::unordered_set<std::string> unsigned_ops = { "udiv", "urem", "ult", "ule", "ugt", "uge", "shr", "eq", "ne" };

        struct Cmp { const char* br; bool swap; };
        static const std::unordered_map<std::string, Cmp> cc = {
            { "eq",  { "beq",  false } }, { "ne",  { "bne",  false } },
            { "slt", { "blt",  false } }, { "sge", { "bge",  false } },
            { "sgt", { "blt",  true  } }, { "sle", { "bge",  true  } },
            { "ult", { "bltu", false } }, { "uge", { "bgeu", false } },
            { "ugt", { "bltu", true  } }, { "ule", { "bgeu", true  } },
        };

        const std::string& o = op.op;
        const std::size_t size = size_of(op.operand);
        const bool shift = o == "shl" || o == "shr" || o == "sar";

        load_operand(ACC, op.a);
        load_operand(SEC, op.b);

        if( signed_ops.count(o) || unsigned_ops.count(o) ) {
            bool sg = signed_ops.count(o) > 0;
            normalize(ACC, size, sg);
            if(! shift ) normalize(SEC, size, sg);
        }

        if( o == "add" )       I("add",  "a2", "a2", "a3");
        else if( o == "sub" )  I("sub",  "a2", "a2", "a3");
        else if( o == "mul" )  I("mull", "a2", "a2", "a3");
        else if( o == "and" )  I("and",  "a2", "a2", "a3");
        else if( o == "or" )   I("or",   "a2", "a2", "a3");
        else if( o == "xor" )  I("xor",  "a2", "a2", "a3");
        else if( o == "shl" )  { I("ssl", "a3"); I("sll", "a2", "a2"); }
        else if( o == "shr" )  { I("ssr", "a3"); I("srl", "a2", "a2"); }
        else if( o == "sar" )  { I("ssr", "a3"); I("sra", "a2", "a2"); }
        else if( o == "sdiv" ) I("quos", "a2", "a2", "a3");
        else if( o == "srem" ) I("rems", "a2", "a2", "a3");
        else if( o == "udiv" ) I("quou", "a2", "a2", "a3");
        else if( o == "urem" ) I("remu", "a2", "a2", "a3");
        else {
            auto it = cc.find(o);
            if( it == cc.end() ) throw std::runtime_error("codegen: unknown binary operation '" + o + "'");

            // no setcc on Xtensa: a4 = 1, skip the `a4 = 0` when the condition holds
            I("movi", "a4", "1");
            if( it->second.swap ) I(it->second.br, "a3", "a2", "1f");
            else                  I(it->second.br, "a2", "a3", "1f");
            I("movi", "a4", "0");
            local_label("1");
            I("mov", "a2", "a4");
        }

        Entry d = make_slot(op.result);
        store_slot(ACC, d, d.slot_bytes);
        vars[op.dst] = d;
    }

    void cast_op(cast_op_t op) override {
        Entry d = make_slot(op.to);

        if( wide(op.to) ) {
            // anything -> 64 bits: extended by the signedness of the source
            load_operand64(A2, A3, Operand{ op.src, false });
            st64(A2, A3, "a15", d.off);
        } else if( wide(op.from) ) {
            // 64 bits -> narrower: keep the low word (stored with the size of the destination)
            const Entry& s = var(op.src);
            ld(ACC, 4, false, "a15", s.off);
            store_slot(ACC, d, d.slot_bytes);
        } else {
            load_operand(ACC, Operand{ op.src, false });
            store_slot(ACC, d, d.slot_bytes);
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
        I("j", label_of(op.target));
    }

    void branch_op(branch_op_t op) override {
        const Entry& e = var(op.cond);

        if( wide(e.type) && ! e.addr ) {         // nonzero when either word is
            ld64(A2, A3, "a15", e.off);
            I("or", "a2", "a2", "a3");
        } else {
            load_operand(ACC, Operand{ op.cond, false });
        }

        I(op.nonzero ? "bnez" : "beqz", "a2", label_of(op.target));
    }

    void loc_load_op(loc_load_op_t op) override {
        if( op.loc.bits == 64 ) {
            if( op.loc.space != Loc::Space::mem && op.loc.space != Loc::Space::io )
                throw std::runtime_error("codegen: only mem_load / io_load can be 64 bits on xtensa");
            if( op.loc.addr > 0xFFFFFFFFULL )
                throw std::runtime_error("codegen: address doesn't fit in 32 bits on xtensa");

            I("memw");
            load_imm(ADR, static_cast<long long>(op.loc.addr));
            ld64(A2, A3, "a4", 0);

            Entry d = make_slot(unsigned_type(64));
            st64(A2, A3, "a15", d.off);
            vars[op.dst] = d;
            return;
        }

        loc_read(op.loc);

        Entry d = make_slot(unsigned_type(op.loc.bits));
        store_slot(ACC, d, d.slot_bytes);
        vars[op.dst] = d;
    }

    void loc_store_op(loc_store_op_t op) override {
        if( op.loc.bits == 64 ) {
            if( op.loc.space != Loc::Space::mem && op.loc.space != Loc::Space::io )
                throw std::runtime_error("codegen: only mem_store / io_store can be 64 bits on xtensa");
            if( op.loc.addr > 0xFFFFFFFFULL )
                throw std::runtime_error("codegen: address doesn't fit in 32 bits on xtensa");

            load_operand64(A2, A3, op.value);
            I("memw");
            load_imm(ADR, static_cast<long long>(op.loc.addr));
            st64(A2, A3, "a4", 0);
            I("memw");
            return;
        }

        load_operand(ACC, op.value);
        loc_write(op.loc);
    }

    void loc_get_op(loc_get_op_t op) override {
        loc_read(op.loc);

        if( op.width <= 16 && op.width > 0 ) {
            I("extui", "a2", "a2", n(op.lo), n(op.width));
        } else {
            if( op.lo ) {
                I("ssai", n(op.lo));
                I("srl", "a2", "a2");
            }
            if( op.width < 32 ) {
                load_imm(SEC, static_cast<long long>(width_mask(op.width)));
                I("and", "a2", "a2", "a3");
            }
        }

        Entry d = make_slot(op.type);
        store_slot(ACC, d, d.slot_bytes);
        vars[op.dst] = d;
    }

    void loc_set_op(loc_set_op_t op) override {
        load_operand(TGT, op.value);

        if( op.width < 32 ) {
            load_imm(SEC, static_cast<long long>(width_mask(op.width)));
            I("and", "a10", "a10", "a3");
        }
        if( op.lo ) I("slli", "a10", "a10", n(op.lo));

        loc_read(op.loc);

        std::uint64_t field = (width_mask(op.width) << op.lo) & 0xFFFFFFFFULL;
        std::uint64_t keep  = ~field & width_mask(op.loc.bits) & 0xFFFFFFFFULL;

        load_imm(SEC, static_cast<long long>(keep));
        I("and", "a2", "a2", "a3");
        I("or",  "a2", "a2", "a10");

        loc_write(op.loc);
    }
};

inline std::size_t xtensa_lx7::size_of(const Ty& t) {
    if( is_tuple(t) ) return tuple_layout(t.tuple);
    if( is_vec(t) )   return t.bits / 8;
    if( is_void(t) )  return 0;
    if( auto s = fixed_size(t.kind) ) return *s;
    return 4;   // int, uint, ptr (32-bit machine)
}

inline std::size_t xtensa_lx7::align_of(const Ty& t) {
    if( is_tuple(t) ) {
        std::size_t a = 1;
        tuple_layout(t.tuple, &a);
        return a;
    }
    std::size_t s = size_of(t);
    return s == 0 ? 1 : s;
}
