#pragma once


#define EXTENSORS(E) \
E("x86_64-linux", x86_64_linux, x86_64_linux_behaviour, "_start")       \
E("avr_atmega328p", avr_atmega328p, avr_atmega328p_behaviour, "_start") \
E("xtensa_lx7", xtensa_lx7, xtensa_lx7_behaviour, "app_main") 

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
 * x86_64 / GNU as (AT&T) backend.
 *
 * The parser only hands over NAMES and TYPES. Everything physical lives here:
 *
 *   - sizes / alignment of every type (int, uint and ptr are 8 bytes; tuples use natural
 *     alignment, fields in order, size rounded up to the biggest alignment);
 *   - one rbp-relative stack slot per name. The frame size is only known at the end of the
 *     function, so the prologue uses `subq $.LFRAME<n>, %rsp` and the epilogue `.set`s it;
 *   - System V calling convention: rdi, rsi, rdx, rcx, r8, r9, then the stack. Return in rax.
 *     A function that returns a tuple gets a hidden pointer in rdi (sret). A tuple ARGUMENT
 *     is passed by pointer (the callee copies it, so it still behaves like a value).
 *
 * How a name is read:
 *   - a normal slot (alloc / call result / load / ...) is read as its CONTENTS, extended to
 *     64 bits by the signedness of its type;
 *   - the result of `ptr_to` is read as the ADDRESS of the slot it points to;
 *   - deref / storep / call_indirect follow the parser's `via_lea` flag: true means "the
 *     pointer is the address of the slot", false means "the pointer is stored in the slot".
 */
class x86_64_linux : public codegen_interface_methods {
public:
    const std::string entrypoint = "_start";
private:
    std::size_t stack_sz = 0;
    long vscratch = 0;
    std::string current;
    int func = 0;

    // ───────────── per-name bookkeeping ─────────────

    struct Entry {
        Ty          type;         // type of the value as seen by the expressions
        long        off  = 0;     // the slot starts at -off(%rbp)
        bool        addr = false; // `ptr_to` result: reads as the address of the slot
        std::size_t slot_bytes = 0;
    };

    std::unordered_map<std::string, Entry>       vars;      // reset at every function
    std::unordered_map<std::string, std::string> fn_alias;  // `move g, f` where f is a function
    std::unordered_set<std::string>              externs;
    std::unordered_map<std::string, int>         strings;   // literal bytes -> .LS<n>
    int nstrings = 0;

    // current function
    Ty                 fn_ret;
    bool               fn_sret = false;
    long               sret_off = 0;
    std::vector<Entry> param_slots;

    enum R { RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11 };

    static std::size_t align_up(std::size_t v, std::size_t a) {
        return (v + a - 1) & ~(a - 1);
    }

    static std::size_t size_of(const Ty& t);
    static std::size_t align_of(const Ty& t);

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

    static std::string rn(R r, std::size_t size) {
        static const char* t[][4] = {
            { "%al",   "%ax",   "%eax",  "%rax" },
            { "%cl",   "%cx",   "%ecx",  "%rcx" },
            { "%dl",   "%dx",   "%edx",  "%rdx" },
            { "%sil",  "%si",   "%esi",  "%rsi" },
            { "%dil",  "%di",   "%edi",  "%rdi" },
            { "%r8b",  "%r8w",  "%r8d",  "%r8"  },
            { "%r9b",  "%r9w",  "%r9d",  "%r9"  },
            { "%r10b", "%r10w", "%r10d", "%r10" },
            { "%r11b", "%r11w", "%r11d", "%r11" },
        };
        return t[r][size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3];
    }

    static std::string sfx(std::size_t size) {
        return size == 1 ? "b" : size == 2 ? "w" : size == 4 ? "l" : "q";
    }

    static std::string imm(long long v) { return "$" + std::to_string(v); }

    static std::string mem(long off, long delta = 0) {
        return std::to_string(-off + delta) + "(%rbp)";
    }

    static std::string rname(const std::string& n) {
        return (! n.empty() && n[0] == '%') ? n : "%" + n;
    }

    static std::uint64_t width_mask(std::size_t bits) {
        return bits >= 64 ? ~0ULL : ((1ULL << bits) - 1);
    }

    static long long parse_lit(const std::string& s) {
        try { return std::stoll(s); }
        catch( ... ) { return static_cast<long long>(std::stoull(s)); }
    }

    // one instruction: `mnemonic a, b`
    void I(const std::string& mn, const std::string& a = "", const std::string& b = "") {
        std::string s = mn;
        if(! a.empty() ) s += " " + a;
        if(! b.empty() ) s += ", " + b;
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
        stack_sz = align_up(stack_sz + size, align);
        return static_cast<long>(stack_sz);
    }

    Entry make_slot(const Ty& t) {
        Entry e;
        e.type       = t;
        e.slot_bytes = size_of(t);
        e.off        = alloca_bytes(e.slot_bytes, align_of(t));
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

    void load_slot(R r, const Entry& e) {
        std::size_t sz = size_of(e.type);
        bool sg = is_signed_kind(e.type.kind);
        std::string m = mem(e.off);

        switch( sz ) {
            case 1:  I(sg ? "movsbq" : "movzbq", m, rn(r, 8)); break;
            case 2:  I(sg ? "movswq" : "movzwq", m, rn(r, 8)); break;
            case 4:  if( sg ) I("movslq", m, rn(r, 8)); else I("movl", m, rn(r, 4)); break;
            default: I("movq", m, rn(r, 8)); break;
        }
    }

    void load_imm(R r, long long v) {
        if( v == 0 )                                  I("xorl", rn(r, 4), rn(r, 4));
        else if( v >= INT32_MIN && v <= INT32_MAX )   I("movq", imm(v), rn(r, 8));
        else                                          I("movabsq", imm(v), rn(r, 8));
    }

    void load_operand(R r, const Operand& op) {
        if( op.literal ) { load_imm(r, parse_lit(op.text)); return; }

        const Entry& e = var(op.text);
        if( e.addr || is_tuple(e.type) ) I("leaq", mem(e.off), rn(r, 8));
        else                             load_slot(r, e);
    }

    void normalize(R r, std::size_t size, bool sgn) {
        if( size >= 8 ) return;

        if( sgn ) {
            if( size == 1 )      I("movsbq", rn(r, 1), rn(r, 8));
            else if( size == 2 ) I("movswq", rn(r, 2), rn(r, 8));
            else                 I("movslq", rn(r, 4), rn(r, 8));
        } else {
            if( size == 1 )      I("movzbl", rn(r, 1), rn(r, 4));
            else if( size == 2 ) I("movzwl", rn(r, 2), rn(r, 4));
            else                 I("movl",   rn(r, 4), rn(r, 4));
        }
    }

    void store_reg(R r, const std::string& dst, std::size_t size) {
        I("mov" + sfx(size), rn(r, size), dst);
    }

    // memory -> memory through %r10: `n` bytes from disp(base) to disp(base)
    void copy(const std::string& sb, long sd, const std::string& db, long dd, std::size_t n) {
        std::size_t done = 0;

        while( done < n ) {
            std::size_t left  = n - done;
            std::size_t chunk = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;

            I("mov" + sfx(chunk), std::to_string(sd + static_cast<long>(done)) + "(" + sb + ")", rn(R10, chunk));
            I("mov" + sfx(chunk), rn(R10, chunk), std::to_string(dd + static_cast<long>(done)) + "(" + db + ")");
            done += chunk;
        }
    }

    void call_common(const std::string& dst, const Ty& ret, const std::vector<Operand>& args,
                     bool sret, const std::string* direct, const Entry* target, bool target_lea) {
        static const R ARG[6] = { RDI, RSI, RDX, RCX, R8, R9 };

        Entry d;
        const bool has_ret = ! is_void(ret);
        if( has_ret ) d = make_slot(ret);

        const std::size_t shift = sret ? 1 : 0;
        const std::size_t nregs = 6 - shift;
        const std::size_t nstack = args.size() > nregs ? args.size() - nregs : 0;
        const std::size_t pad = (nstack & 1) ? 8 : 0;

        if( pad ) I("subq", "$8", "%rsp");

        for( std::size_t k = args.size(); k > nregs; ) {
            --k;
            load_operand(RAX, args[k]);
            I("pushq", "%rax");
        }

        if( target ) I(target_lea ? "leaq" : "movq", mem(target->off), "%r11");

        for( std::size_t k = 0; k < args.size() && k < nregs; k++ )
            load_operand(ARG[k + shift], args[k]);

        if( sret ) I("leaq", mem(d.off), "%rdi");

        I("xorl", "%eax", "%eax");

        if( direct ) {
            std::string fn = resolve(*direct);
            I("call", externs.count(fn) ? fn + "@PLT" : fn);
        } else {
            I("call", "*%r11");
        }

        if( nstack + pad ) I("addq", imm(static_cast<long long>(nstack * 8 + pad)), "%rsp");

        if( has_ret && ! is_tuple(ret) ) store_reg(RAX, mem(d.off), size_of(ret));
        if(! dst.empty() && has_ret ) vars[dst] = d;
    }

    long scratch() {
        if(! vscratch ) vscratch = alloca_bytes(64, 16);
        return vscratch;
    }

    // ───────────── SIMD (vreg) ─────────────
    // Every access is unaligned (movups / vmovups): a slot is only guaranteed 16-byte aligned.
    // 64-bit vregs are MMX (%mmN): the program is responsible for `emms`.

    std::string vreg_name(const Loc& l) {
        const std::string want = l.bits == 64  ? "%mm"
                               : l.bits == 128 ? "%xmm"
                               : l.bits == 256 ? "%ymm" : "%zmm";
        std::string n = rname(l.name);

        if( n.size() <= want.size() || n.compare(0, want.size(), want) != 0
            || ! std::isdigit(static_cast<unsigned char>(n[want.size()])) )
            throw std::runtime_error("codegen: '" + l.name + "' is not a "
                                     + std::to_string(l.bits) + "-bit vector register");
        return n;
    }

    // register -> memory
    void vec_spill(const Loc& l, const std::string& m) {
        std::string r = vreg_name(l);
        if( l.bits == 64 )       I("movq",    r, m);
        else if( l.bits == 128 ) I("movups",  r, m);
        else                     I("vmovups", r, m);
    }

    // memory -> register
    void vec_fill(const Loc& l, const std::string& m) {
        std::string r = vreg_name(l);
        if( l.bits == 64 )       I("movq",    m, r);
        else if( l.bits == 128 ) I("movups",  m, r);
        else                     I("vmovups", m, r);
    }

    void vec_zero(const Loc& l) {
        std::string r = vreg_name(l);
        if( l.bits == 64 )       I("pxor",   r, r);
        else if( l.bits == 128 ) I("pxor",   r, r);
        else if( l.bits == 256 ) I("vpxor",  r + ", " + r, r);
        else                     I("vpxord", r + ", " + r, r);   // AVX-512F
    }

    static std::string sreg_name(const std::string& s) {
        std::string n = s;
        if(! n.empty() && n[0] == '%' ) n.erase(0, 1);
        for( auto& ch : n ) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return n;
    }

    static bool is_flags(const std::string& n) { return n == "rflags" || n == "eflags" || n == "flags"; }
    static bool is_seg(const std::string& n) {
        return n == "cs" || n == "ds" || n == "es" || n == "fs" || n == "gs" || n == "ss";
    }
    static bool is_cr(const std::string& n) {
        return n.size() == 3 && n[0] == 'c' && n[1] == 'r'
            && (n[2] == '0' || n[2] == '2' || n[2] == '3' || n[2] == '4' || n[2] == '8');
    }

    void sreg_check(const std::string& n, std::size_t bits) {
        bool ok;
        if( is_flags(n) )                          ok = bits == 16 || bits == 32 || bits == 64;
        else if( n == "mxcsr" )                    ok = bits == 32;
        else if( is_seg(n) )                       ok = bits == 16;
        else if( n == "fsbase" || n == "gsbase" || n == "xcr0" || is_cr(n) ) ok = bits == 64;
        else throw std::runtime_error("codegen: unknown special register '" + n + "'");

        if(! ok )
            throw std::runtime_error("codegen: '" + n + "' can't be accessed with " + std::to_string(bits) + " bits");
    }

    void sreg_read(const Loc& l) {
        std::string n = sreg_name(l.name);
        sreg_check(n, l.bits);

        if( is_flags(n) )         { I("pushfq"); I("popq", "%rax"); }
        else if( n == "mxcsr" )   { long s = scratch(); I("stmxcsr", mem(s)); I("movl", mem(s), "%eax"); }
        else if( n == "fsbase" )  I("rdfsbase", "%rax");
        else if( n == "gsbase" )  I("rdgsbase", "%rax");
        else if( is_seg(n) )      I("movl", "%" + n, "%eax");
        else if( is_cr(n) )       I("movq", "%" + n, "%rax");
        else /* xcr0 */           {
            I("xorl", "%ecx", "%ecx");
            I("xgetbv");
            I("shlq", "$32", "%rdx");
            I("orq",  "%rdx", "%rax");
        }

        normalize(RAX, l.bits / 8, false);
    }

    void sreg_write(const Loc& l) {
        std::string n = sreg_name(l.name);
        sreg_check(n, l.bits);

        if( is_flags(n) )         { I("pushq", "%rax"); I("popfq"); }
        else if( n == "mxcsr" )   { long s = scratch(); I("movl", "%eax", mem(s)); I("ldmxcsr", mem(s)); }
        else if( n == "fsbase" )  I("wrfsbase", "%rax");
        else if( n == "gsbase" )  I("wrgsbase", "%rax");
        else if( is_seg(n) ) {
            if( n == "cs" ) throw std::runtime_error("codegen: cs can't be written with mov");
            I("movw", "%ax", "%" + n);
        }
        else if( is_cr(n) )       I("movq", "%rax", "%" + n);
        else  {
            I("movq", "%rax", "%rdx");
            I("shrq", "$32", "%rdx");
            I("xorl", "%ecx", "%ecx");
            I("xsetbv");
        }
    }

    void loc_read(const Loc& l) {
        switch( l.space ) {
            case Loc::Space::reg:
                if( l.bits == 8 )       I("movzbl", rname(l.name), "%eax");
                else if( l.bits == 16 ) I("movzwl", rname(l.name), "%eax");
                else if( l.bits == 32 ) I("movl",   rname(l.name), "%eax");
                else                    I("movq",   rname(l.name), "%rax");
                break;

            case Loc::Space::mem:
                I("movabsq", imm(static_cast<long long>(l.addr)), "%rdx");
                if( l.bits == 8 )       I("movzbl", "(%rdx)", "%eax");
                else if( l.bits == 16 ) I("movzwl", "(%rdx)", "%eax");
                else if( l.bits == 32 ) I("movl",   "(%rdx)", "%eax");
                else                    I("movq",   "(%rdx)", "%rax");
                break;

            case Loc::Space::io:
                if( l.bits == 64 ) throw std::runtime_error("codegen: io_* can't be 64 bits on x86");
                I("movl", imm(static_cast<long long>(l.addr)), "%edx");
                if( l.bits == 8 )       { I("xorl", "%eax", "%eax"); I("inb", "%dx", "%al"); }
                else if( l.bits == 16 ) { I("xorl", "%eax", "%eax"); I("inw", "%dx", "%ax"); }
                else                    I("inl", "%dx", "%eax");
                break;

            case Loc::Space::sreg: sreg_read(l);  break;
            case Loc::Space::vreg:
                throw std::runtime_error("codegen: a vector register can't be read as a scalar");
        }
    }

    void loc_write(const Loc& l) {
        const std::size_t bytes = l.bits / 8;

        switch( l.space ) {
            case Loc::Space::reg:
                I("mov" + sfx(bytes), rn(RAX, bytes), rname(l.name));
                break;

            case Loc::Space::mem:
                I("movabsq", imm(static_cast<long long>(l.addr)), "%rdx");
                I("mov" + sfx(bytes), rn(RAX, bytes), "(%rdx)");
                break;

            case Loc::Space::io:
                if( l.bits == 64 ) throw std::runtime_error("codegen: io_* can't be 64 bits on x86");
                I("movl", imm(static_cast<long long>(l.addr)), "%edx");
                I("out" + sfx(bytes), rn(RAX, bytes), "%dx");
                break;

            case Loc::Space::sreg: sreg_write(l); break;
            case Loc::Space::vreg:
                throw std::runtime_error("codegen: a vector register can't be written as a scalar");
        }
    }

public:
    using codegen_interface_methods::codegen_interface_methods;

    void prologue(prologue_t prologue) override {
        stack_sz = 0, vscratch = 0;
        current  = prologue.identifier;
        vars.clear();
        param_slots.clear();

        const char *id = c(current);

        into = false;
        emit(".text\n"
             ".globl %s\n"
             ".type %s, @function\n"
             "%s:\n"
             ".LFP%d:\n",
             repeat3(id),
             func);

        into = true;

        if( current == entrypoint ) {
            emit("xorl %%ebp, %%ebp\n"
                 "andq $-16, %%rsp\n"
                 "movq %%rsp, %%rbp\n");
        } else {
            emit("pushq %%rbp\n"
                 "movq %%rsp, %%rbp\n");
        }

        emit("subq $.LFRAME%d, %%rsp\n", func);

        static const R ARG[6] = { RDI, RSI, RDX, RCX, R8, R9 };

        fn_ret  = prologue.ret;
        fn_sret = is_tuple(fn_ret);

        std::size_t rp = 0;

        if( fn_sret ) {
            sret_off = alloca_bytes(8, 8);
            I("movq", "%rdi", mem(sret_off));
            rp = 1;
        }

        for( const auto& p : prologue.params ) {
            Entry e = make_slot(p);

            if( rp < 6 ) {
                if( is_tuple(p) ) copy(rn(ARG[rp], 8), 0, "%rbp", -e.off, e.slot_bytes);
                else              store_reg(ARG[rp], mem(e.off), e.slot_bytes);
            } else {
                long src = 16 + 8 * static_cast<long>(rp - 6);
                I("movq", std::to_string(src) + "(%rbp)", "%rax");

                if( is_tuple(p) ) copy("%rax", 0, "%rbp", -e.off, e.slot_bytes);
                else              store_reg(RAX, mem(e.off), e.slot_bytes);
            }

            param_slots.push_back(e);
            rp++;
        }
    }

    void epilogue(epilogue_t epilogue) override {
        const char *id = c(current);

        into = false;
        emit(".set .LFRAME%d, %zu\n", func,  normalized_stacksz());
        emit(".LFE%d:\n", func);

        into = true;

        if( current == entrypoint ) {
            emit("movq %%rax, %%rdi\n"
                 "movl $60, %%eax\n"
                 "syscall\n");
        } else {
            emit("leave\n"
                 "ret\n");
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

            if( externs.count(fn) ) I("movq", fn + "@GOTPCREL(%rip)", "%rax");
            else                    I("leaq", fn + "(%rip)", "%rax");

            store_reg(RAX, mem(d.off), 8);
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
                I("leaq", mem(s.off), "%rax");
                store_reg(RAX, mem(d.off, to), 8);
            } else {
                copy("%rbp", -s.off, "%rbp", -d.off + to, size_of(el));
            }
        }

        vars[op.dst] = d;
    }

    void deref_op(deref_op_t op) override {
        const Entry& src = var(op.src);
        I(op.via_lea ? "leaq" : "movq", mem(src.off), "%rax");

        Entry d = make_slot(op.type);
        copy("%rax", 0, "%rbp", -d.off, d.slot_bytes);
        vars[op.dst] = d;
    }

    void store_op(store_op_t op) override {
        const Entry t = var(op.target);
        const std::size_t n = t.slot_bytes;
        const bool scalar = n == 1 || n == 2 || n == 4 || n == 8;

        if(! op.value.literal ) {
            const Entry& s = var(op.value.text);

            if( is_tuple(s.type) && ! s.addr ) {
                copy("%rbp", -s.off, "%rbp", -t.off, std::min(n, size_of(s.type)));
                return;
            }
        }

        if(! scalar ) throw std::runtime_error("codegen: can't store a scalar into '" + op.target + "'");

        load_operand(RAX, op.value);
        store_reg(RAX, mem(t.off), n);
    }

    void storep_op(storep_op_t op) override {
        const Entry& p = var(op.ptr);

        load_operand(RAX, op.value);
        I(op.via_lea ? "leaq" : "movq", mem(p.off), "%rdx");
        store_reg(RAX, "(%rdx)", size_of(op.type));
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
        I("leaq", ".LS" + std::to_string(it->second) + "(%rip)", "%rax");
        store_reg(RAX, mem(d.off), 8);
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
            I("movq", mem(sret_off), "%rax");
            copy("%rbp", -s.off, "%rax", 0, size_of(fn_ret));
        } else {
            load_operand(RAX, op.value);
        }

        I("jmp", ".LFE" + std::to_string(func));
    }

    void syscall_op(syscall_op_t op) override {
        static const R ARG[6] = { RDI, RSI, RDX, R10, R8, R9 };

        for( std::size_t k = 0; k < op.args.size(); k++ )
            load_operand(ARG[k], op.args[k]);

        load_imm(RAX, parse_lit(op.number));
        I("syscall");

        Entry d = make_slot(make_ty(morgana_type::mi64));
        store_reg(RAX, mem(d.off), 8);
        vars[op.dst] = d;
    }

    void binary_op(binary_op_t op) override {
        static const std::unordered_set<std::string> signed_ops   = { "sdiv", "srem", "slt", "sle", "sgt", "sge", "sar" };
        static const std::unordered_set<std::string> unsigned_ops = { "udiv", "urem", "ult", "ule", "ugt", "uge", "shr", "eq", "ne" };

        const std::string& o = op.op;
        const std::size_t size = size_of(op.operand);
        const bool shift = o == "shl" || o == "shr" || o == "sar";

        load_operand(RAX, op.a);
        load_operand(RCX, op.b);

        if( signed_ops.count(o) || unsigned_ops.count(o) ) {
            bool sg = signed_ops.count(o) > 0;
            normalize(RAX, size, sg);
            if(! shift ) normalize(RCX, size, sg);
        }

        if( o == "add" )       I("addq", "%rcx", "%rax");
        else if( o == "sub" )  I("subq", "%rcx", "%rax");
        else if( o == "mul" )  I("imulq", "%rcx", "%rax");
        else if( o == "and" )  I("andq", "%rcx", "%rax");
        else if( o == "or" )   I("orq",  "%rcx", "%rax");
        else if( o == "xor" )  I("xorq", "%rcx", "%rax");
        else if( o == "shl" )  I("shlq", "%cl", "%rax");
        else if( o == "shr" )  I("shrq", "%cl", "%rax");
        else if( o == "sar" )  I("sarq", "%cl", "%rax");
        else if( o == "sdiv" || o == "srem" ) {
            I("cqto");
            I("idivq", "%rcx");
            if( o == "srem" ) I("movq", "%rdx", "%rax");
        }
        else if( o == "udiv" || o == "urem" ) {
            I("xorl", "%edx", "%edx");
            I("divq", "%rcx");
            if( o == "urem" ) I("movq", "%rdx", "%rax");
        }
        else {
            static const std::unordered_map<std::string, std::string> cc = {
                { "eq", "sete" },  { "ne", "setne" },
                { "slt", "setl" }, { "sle", "setle" }, { "sgt", "setg" }, { "sge", "setge" },
                { "ult", "setb" }, { "ule", "setbe" }, { "ugt", "seta" }, { "uge", "setae" },
            };

            auto it = cc.find(o);
            if( it == cc.end() ) throw std::runtime_error("codegen: unknown binary operation '" + o + "'");

            I("cmpq", "%rcx", "%rax");
            I(it->second, "%al");
        }

        Entry d = make_slot(op.result);
        store_reg(RAX, mem(d.off), d.slot_bytes);
        vars[op.dst] = d;
    }

    void cast_op(cast_op_t op) override {
        load_operand(RAX, Operand{ op.src, false });

        Entry d = make_slot(op.to);
        store_reg(RAX, mem(d.off), d.slot_bytes);
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
        load_operand(RAX, Operand{ op.cond, false });
        I("testq", "%rax", "%rax");
        I(op.nonzero ? "jnz" : "jz", label_of(op.target));
    }

    void loc_load_op(loc_load_op_t op) override {
        if( op.loc.space == Loc::Space::vreg ) {
            Entry d = make_slot(make_vec(op.loc.bits));
            vec_spill(op.loc, mem(d.off));
            vars[op.dst] = d;
            return;
        }

        loc_read(op.loc);
        Entry d = make_slot(unsigned_type(op.loc.bits));
        store_reg(RAX, mem(d.off), d.slot_bytes);
        vars[op.dst] = d;
    }

    void loc_store_op(loc_store_op_t op) override {
        if( op.loc.space == Loc::Space::vreg ) {
            if( op.value.literal ) vec_zero(op.loc);
            else                   vec_fill(op.loc, mem(var(op.value.text).off));
            return;
        }

        load_operand(RAX, op.value);
        loc_write(op.loc);
    }

    void loc_get_op(loc_get_op_t op) override {
        if( op.loc.space == Loc::Space::vreg ) {
            const long s  = scratch();
            const long at = static_cast<long>(op.lo / 8);
            const std::size_t n = op.width / 8;

            vec_spill(op.loc, mem(s));
            Entry d = make_slot(op.type);

            if( n <= 8 ) {
                if( n == 1 )      I("movzbl", mem(s, at), "%eax");
                else if( n == 2 ) I("movzwl", mem(s, at), "%eax");
                else if( n == 4 ) I("movl",   mem(s, at), "%eax");
                else              I("movq",   mem(s, at), "%rax");
                store_reg(RAX, mem(d.off), d.slot_bytes);
            } else {
                copy("%rbp", -s + at, "%rbp", -d.off, n);
            }

            vars[op.dst] = d;
            return;
        }

        loc_read(op.loc);

        if( op.lo ) I("shrq", imm(static_cast<long long>(op.lo)), "%rax");
        if( op.width < 64 ) {
            load_imm(RCX, static_cast<long long>(width_mask(op.width)));
            I("andq", "%rcx", "%rax");
        }

        Entry d = make_slot(op.type);
        store_reg(RAX, mem(d.off), d.slot_bytes);
        vars[op.dst] = d;
    }

    void loc_set_op(loc_set_op_t op) override {
        if( op.loc.space == Loc::Space::vreg ) {
            const long s  = scratch();
            const long at = static_cast<long>(op.lo / 8);
            const std::size_t n = op.width / 8;

            vec_spill(op.loc, mem(s));

            if( n <= 8 ) {
                load_operand(RAX, op.value);
                store_reg(RAX, mem(s, at), n);
            } else if( op.value.literal ) {
                for( std::size_t k = 0; k < n; k += 8 )
                    I("movq", "$0", mem(s, at + static_cast<long>(k)));
            } else {
                copy("%rbp", -var(op.value.text).off, "%rbp", -s + at, n);
            }

            vec_fill(op.loc, mem(s));
            return;
        }

        load_operand(R11, op.value);

        if( op.width < 64 ) {
            load_imm(RCX, static_cast<long long>(width_mask(op.width)));
            I("andq", "%rcx", "%r11");
        }
        if( op.lo ) I("shlq", imm(static_cast<long long>(op.lo)), "%r11");

        loc_read(op.loc);

        std::uint64_t field = width_mask(op.width) << op.lo;
        std::uint64_t keep  = ~field & width_mask(op.loc.bits);

        load_imm(RCX, static_cast<long long>(keep));
        I("andq", "%rcx", "%rax");
        I("orq",  "%r11", "%rax");

        loc_write(op.loc);
    }
};

inline std::size_t x86_64_linux::size_of(const Ty& t) {
    if( is_tuple(t) ) return tuple_layout(t.tuple);
    if( is_void(t) )  return 0;
    if( auto s = fixed_size(t.kind) ) return *s;
    return 8;   // int, uint, ptr
}

inline std::size_t x86_64_linux::align_of(const Ty& t) {
    if( is_tuple(t) ) {
        std::size_t a = 1;
        tuple_layout(t.tuple, &a);
        return a;
    }
    std::size_t s = size_of(t);
    return s == 0 ? 1 : s;
} 
#include "src/codegen/interface.hpp"
#include "src/externs.hpp"

#include "samar.hpp"

class x86_64_linux_behaviour : public behaviour_interface_methods {
public:
    using behaviour_interface_methods::behaviour_interface_methods;

    void build(char *sake, std::filesystem::path path, std::string output, std::string extra) override {
        auto _asm     = path / "asm.s";
        auto  obj     = path / "m.o";
        auto  bin     = path / "bin";
        auto  archive = path / "archive.a";
        auto  shared  = path / "shared.so";

        SamarState *samar = __samar_load(sake);

        __samar_push_arg   (samar, (char*) "asm",     (char*) _asm.c_str());
        __samar_push_arg   (samar, (char*) "obj",     (char*) obj.c_str());
        __samar_push_arg   (samar, (char*) "bin",     (char*) bin.c_str());
        __samar_push_arg   (samar, (char*) "archive", (char*) archive.c_str());
        __samar_push_arg   (samar, (char*) "shared",  (char*) shared.c_str());
        __samar_call_runner(samar, (char*) ("x86_64::linux::" + runner(output)).c_str());
    }
};  

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
#include "samar.hpp"
#include "src/codegen/interface.hpp"
#include "src/externs.hpp"

class avr_atmega328p_behaviour : public behaviour_interface_methods {
    using behaviour_interface_methods::behaviour_interface_methods;

    void build(char *sake, std::filesystem::path path, std::string output, std::string extra) override {
        auto path_elf  = path / "avr.elf",
             path_hex  = path / "avr.hex",
             path_file = path / "asm.s";

        auto elf  = path_elf.string(),
             hex  = path_hex.string(),
             file = path_file.string();

        SamarState *samar = __samar_load(sake);
        auto runner = "avr_atmega328p::" + extra + "::native_bin";

        __samar_push_arg   (samar, (char*) "elf",  (char*) elf.c_str());
        __samar_push_arg   (samar, (char*) "hex",  (char*) hex.c_str());
        __samar_push_arg   (samar, (char*) "file", (char*) file.c_str());
        __samar_call_runner(samar, (char*) runner.c_str());
    }

    void upload(char *sake, std::filesystem::path path, std::string baudrate, std::string device, std::string mcu="arduino") override {
        auto path_hex = path / "avr.hex";
        auto hex = path_hex.string();

        baudrate = (baudrate.empty()) ? "115200" : baudrate;
        device = (device.empty()) ? "/dev/ttyUSB0" : device;

        SamarState *samar = __samar_load(sake);
        auto runner = "avr_atmega328p::" + mcu + "::upload";

        __samar_push_arg   (samar, (char*) "device",   (char*) device.c_str());
        __samar_push_arg   (samar, (char*) "baudrate", (char*) baudrate.c_str());
        __samar_push_arg   (samar, (char*) "hex",      (char*) hex.c_str());
        __samar_call_runner(samar, (char*) runner.c_str());
    }
};  

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
#include "src/codegen/interface.hpp"
#include "src/externs.hpp"
#include <filesystem>

#include "samar.hpp"

class xtensa_lx7_behaviour : public behaviour_interface_methods {
    using behaviour_interface_methods::behaviour_interface_methods;

    void build(char *sake, std::filesystem::path path, std::string output, std::string extra="esp32") override {
        auto path_file = path / "asm.s";
        auto file = path_file.string();

        SamarState *samar = __samar_load(sake);
        auto identifier = ("xtensa_lx7::" + extra + "::" + runner(output));

        __samar_push_arg   (samar, (char*) "target", (char*) path.string().c_str());
        __samar_push_arg   (samar, (char*) "file",   (char*) file.c_str());
        __samar_call_runner(samar, (char*) identifier.c_str());
    }

    void upload(char *sake, std::filesystem::path path, std::string baudrate, std::string device, std::string mcu="esp32") override {
        auto path_idf = path / "carla-idf";
        auto idf = path_idf.string();

        baudrate = (baudrate.empty()) ? "115200" : baudrate;
        device = (device.empty()) ? "/dev/ttyUSB0" : device;

        SamarState *samar = __samar_load(sake);
        auto runner = "xtensa_lx7::" + mcu + "::upload";

        __samar_push_arg   (samar, (char*) "folder",   (char*) idf.c_str());
        __samar_push_arg   (samar, (char*) "device",   (char*) device.c_str());
        __samar_push_arg   (samar, (char*) "baudrate", (char*) baudrate.c_str());
        __samar_call_runner(samar, (char*) runner.c_str());
    }
};
