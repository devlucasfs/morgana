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
class x86_64_gnu : public codegen_interface_methods {
private:
    const std::string entrypoint = "_start";
    std::size_t stack_sz = 0;
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

    // ───────────── layout ─────────────

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

    // contents of a slot -> 64-bit register, extended by the signedness of its type
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

    // literal, or the value of a name (a tuple is read as the address of its slot)
    void load_operand(R r, const Operand& op) {
        if( op.literal ) { load_imm(r, parse_lit(op.text)); return; }

        const Entry& e = var(op.text);
        if( e.addr || is_tuple(e.type) ) I("leaq", mem(e.off), rn(r, 8));
        else                             load_slot(r, e);
    }

    // makes the low `size` bytes of r the whole 64-bit value
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

    // ───────────── calls ─────────────

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
        }
    }

public:
    using codegen_interface_methods::codegen_interface_methods;

    void prologue(prologue_t prologue) override {
        stack_sz = 0;
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
        loc_read(op.loc);

        Entry d = make_slot(unsigned_type(op.loc.bits));
        store_reg(RAX, mem(d.off), d.slot_bytes);
        vars[op.dst] = d;
    }

    void loc_store_op(loc_store_op_t op) override {
        load_operand(RAX, op.value);
        loc_write(op.loc);
    }

    void loc_get_op(loc_get_op_t op) override {
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

inline std::size_t x86_64_gnu::size_of(const Ty& t) {
    if( is_tuple(t) ) return tuple_layout(t.tuple);
    if( is_void(t) )  return 0;
    if( auto s = fixed_size(t.kind) ) return *s;
    return 8;   // int, uint, ptr
}

inline std::size_t x86_64_gnu::align_of(const Ty& t) {
    if( is_tuple(t) ) {
        std::size_t a = 1;
        tuple_layout(t.tuple, &a);
        return a;
    }
    std::size_t s = size_of(t);
    return s == 0 ? 1 : s;
}
