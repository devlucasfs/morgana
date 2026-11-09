#pragma once

#include "../symbols.hpp"
#include "../codegen/factory.hpp"
#include "../hash.hpp"
#include "../target.hpp"

#include "./codegen/backends.hpp"
#include "./codegen/asmgen.hpp"
#include "./lexer.hpp"
#include "./blocks.hpp"
#include "./types.hpp"

#include <algorithm>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

using param_type = std::decay_t<decltype(std::declval<subroutine>().args[0])>;

struct destroyer_arg {
    std::string name;
    param_type  type;
};

inline std::string assert(Lexer& lex, std::initializer_list<std::string> expected, std::string extra = "") {
    auto tk = lex.next();
    if( tk ) {
        for( const auto& str : expected ) if( *tk == str ) return str;
    }

    std::string message = "Expected ";

    bool first = true;
    for( const auto& str : expected ) {
        if(! first ) message += " or ";
        message += "'" + str + "'";
        first = false;
    }

    message += " and was received '" + (tk ? *tk : std::string("<end of file>")) + "' instead.";
    if(! extra.empty() ) message += "\n~ " + extra;

    CompilerOutputs::fatal(message);
    return "";
}

inline std::string next_token(Lexer& lex, const char* expected) {
    auto tk = lex.next();
    if(! tk ) CompilerOutputs::fatal(std::string("Unexpected end of file, expected ") + expected);
    return *tk;
}

// `123` or `-123`
inline bool is_number(const std::string& s) {
    return is_integer_literal(s);
}

inline bool is_zero_literal(const std::string& s) {
    return s.find_first_not_of("-0") == std::string::npos;
}

inline auto lookup_or_fatal(symbols& sym, const std::string& name, const char* kind) {
    auto value = sym.lookup(name);
    if(! value.has_value() ) CompilerOutputs::fatal(std::string(kind) + " " + name + " not found");
    return value.value();
}

inline subroutine lookup_subroutine(symbols& sym, const std::string& name) {
    auto value = lookup_or_fatal(sym, name, "Function");
    if( std::holds_alternative<subroutine>(value) )
        return std::get<subroutine>(value);

    if( std::holds_alternative<unfold>(value) ) {
        auto data = std::get<unfold>(value);
        return subroutine(data.ret, data.args);
    }

    CompilerOutputs::fatal("'" + name + "' is not a function");
}

inline std::string decode_literal(const std::string& token) {
    if( token.empty() || token[0] != 'h' || (token.size() - 1) % 2 != 0 )
        CompilerOutputs::fatal("Invalid string literal '" + token + "'");

    auto hex = [&](char c) -> int {
        if( c >= '0' && c <= '9' ) return c - '0';
        if( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
        if( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
        CompilerOutputs::fatal("Invalid string literal '" + token + "'");
        return 0;
    };

    std::string bytes;
    for( size_t i = 1; i < token.size(); i += 2 )
        bytes.push_back(static_cast<char>(hex(token[i]) * 16 + hex(token[i + 1])));

    return bytes;
}

// ───────────── binary operations ─────────────

// <name> = <op> <a>, <b>
inline bool is_binary_op(const std::string& op) {
    static const std::unordered_set<std::string> ops = {
        "add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor",
        "shl", "shr", "sar",
        "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
    };
    return ops.count(op) > 0;
}

// comparisons produce a 1-byte boolean
inline bool is_comparison_op(const std::string& op) {
    static const std::unordered_set<std::string> ops = {
        "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
    };
    return ops.count(op) > 0;
}

// <name> = <op> <a>, <b>   (a and b: names or integer literals, at least one must be a name)
inline void parse_binary(const std::string& name, const std::string& op,
                         Lexer& lex, symbols& sym, codegen& asmgen) {
    auto a = next_token(lex, "left operand");
    assert(lex, {","}, "A binary operation needs two operands: <name> = " + op + " a, b");
    auto b = next_token(lex, "right operand");

    auto ta = asmgen.operand_type(sym, a);
    auto tb = asmgen.operand_type(sym, b);

    if(! ta && !tb )
        CompilerOutputs::fatal("'" + op + "' needs at least one named operand");

    types::type operand = ta ? *ta : *tb;

    if( operand.kind == types::mtuple )
        CompilerOutputs::fatal("'" + op + "' can't operate on a tuple");

    if( ta && tb && ta->size != tb->size )
        CompilerOutputs::fatal("The operands of '" + op + "' must have the same size");

    // the result has the type of the operands (a comparison keeps the kind, but is 1 byte)
    types::type result = operand;
    if( is_comparison_op(op) ) result.size = 1;

    size_t p = asmgen.alloca(result.size);
    sym.push(name, borrowed(result, p));
    asmgen.binary(sym, op, operand.size, a, b, p, result.size);
}

// ───────────── control flow ─────────────

// ::name::   (the lexer reads it as a single token)
inline bool is_label_token(const std::string& t) {
    return t.size() > 4 && t.compare(0, 2, "::") == 0 && t.compare(t.size() - 2, 2, "::") == 0;
}

inline void parse_label(const std::string& token, symbols& sym, codegen& asmgen) {
    asmgen.label(sym, token.substr(2, token.size() - 4));
}

// br <label>
inline void parse_br(Lexer& lex, symbols& sym, codegen& asmgen) {
    asmgen.jump(sym, next_token(lex, "label name"));
}

// brez <cond>, <label>   |   brnz <cond>, <label>
inline void parse_branch(Lexer& lex, symbols& sym, codegen& asmgen, bool nonzero) {
    auto cond = next_token(lex, "condition value");
    assert(lex, {","}, "A branch needs a label: brez cond, label");
    auto target = next_token(lex, "label name");

    if( is_number(cond) ) {
        bool taken = nonzero ? !is_zero_literal(cond) : is_zero_literal(cond);
        if( taken ) asmgen.jump(sym, target);
        return;
    }

    auto value = lookup_or_fatal(sym, cond, "Value");

    types::type t;
    size_t pos = 0;

    if( std::holds_alternative<borrowed>(value) ) {
        auto b = std::get<borrowed>(value);
        t = b.allocated, pos = b.pos;
    }
    else if( std::holds_alternative<allocation>(value) ) {
        auto a = std::get<allocation>(value);
        t = a.allocated, pos = a.pos;
    }
    else {
        CompilerOutputs::fatal("'" + cond + "' can't be used as a branch condition");
    }

    if( t.kind == types::mtuple )
        CompilerOutputs::fatal("A branch condition can't be a tuple: '" + cond + "'");

    asmgen.branch(sym, pos, t.size, nonzero, target);
}

// "(" arg, arg, ... ")" # argument emissor
inline void emit_arguments(Lexer& lex, symbols& sym, codegen& asmgen, subroutine& sub, bool sret) {
    assert(lex, {"("});
    auto args = get_block(lex, ")");

    if( sub.args.size() != args.size() )
        CompilerOutputs::fatal(
            "Was expected " + std::to_string(sub.args.size())
            + " arguments, but was received " + std::to_string(args.size())
        );

    for( size_t i = args.size(); i-- > 0; )
        asmgen.argument(sym, sub, args[i], i, sret);
}

// extern <type> <name>(<types>)
inline void parse_extern(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto ret  = types::get_from(lex);
    auto name = next_token(lex, "function name");

    asmgen.extern_fn(sym, name);
    assert(lex, {"("});
    auto params = types::get_sequence(lex, ")");
    sym.push(name, subroutine(ret, params));
}

// unfold <type> <name>(<types>)
// (a declaration only: if the function is never cited, nothing is emitted and it is not an error)
inline void parse_unfold(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto ret  = types::get_from(lex);
    auto name = next_token(lex, "function name");

    assert(lex, {"("});
    auto params = types::get_sequence(lex, ")");
    sym.push(name, unfold(ret, params));
}

// call <fn>(<args>) # void functions
inline void parse_call_void(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto name = next_token(lex, "function name");
    auto sub  = lookup_subroutine(sym, name);

    if( sub.ret.kind != types::mvoid )
        CompilerOutputs::fatal("The function " + name + " has a return type. You need to collect it.");

    emit_arguments(lex, sym, asmgen, sub, false);
    asmgen.call(sym, name);
}

// store <alloc> <value>
inline void parse_store(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto name  = next_token(lex, "allocation name");
    auto value = lookup_or_fatal(sym, name, "Allocation");

    if(! std::holds_alternative<allocation>(value) )
        CompilerOutputs::fatal("Allocation " + name + " not found");

    asmgen.store(sym, std::get<allocation>(value), next_token(lex, "value"));
}

// ret <integer> | ret <name>
inline void parse_ret(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto data = next_token(lex, "return value");

    if( is_number(data) ) {
        asmgen.ret_int(sym, data);
        return;
    }

    auto value = lookup_or_fatal(sym, data, "Value");

    if( std::holds_alternative<borrowed>(value) ) {
        auto b = std::get<borrowed>(value);
        asmgen.ret_value(sym, b.pos, b.allocated.size, b.allocated.kind == types::mtuple);
        return;
    }

    if( std::holds_alternative<allocation>(value) ) {
        auto a = std::get<allocation>(value);
        asmgen.ret_value(sym, a.pos, a.allocated.size, a.allocated.kind == types::mtuple);
        return;
    }

    CompilerOutputs::fatal("'" + data + "' can't be returned");
}

// call_indirect <ptr>(<args>)    # void only: the signature isn't known
inline void parse_call_indirect(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto target = next_token(lex, "function pointer name");

    assert(lex, {"("});
    auto args = get_block(lex, ")");

    for( size_t i = args.size(); i-- > 0; )
        asmgen.argument_untyped(sym, args[i], i);

    asmgen.call_indirect(sym, target);
}

// storep <type> <ptr>, <value>     (*ptr = value, `type` is how wide the write is)
inline void parse_storep(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto type   = types::get_from(lex);
    auto target = next_token(lex, "pointer name");
    assert(lex, {","}, "storep needs a value: storep type ptr, value");
    auto value  = next_token(lex, "value");

    if( type.kind == types::mtuple || type.size == 0 )
        CompilerOutputs::fatal("storep only writes scalar values");

    auto val = lookup_or_fatal(sym, target, "Value");

    types::type ptr_t;
    size_t pos   = 0;
    bool via_lea = false;

    if( std::holds_alternative<borrowed>(val) ) {
        auto b = std::get<borrowed>(val);
        ptr_t = b.allocated, pos = b.pos, via_lea = false;
    }
    else if( std::holds_alternative<allocation>(val) ) {
        auto a = std::get<allocation>(val);
        ptr_t = a.allocated, pos = a.pos, via_lea = true;
    }
    else {
        CompilerOutputs::fatal("storep needs a pointer value: '" + target + "'");
    }

    if( ptr_t.kind != types::mptr )
        CompilerOutputs::fatal(
            "storep needs a pointer, but '" + target + "' is a " + ptr_t.kto_string()
        );

    asmgen.store_ptr(sym, pos, via_lea, type, value);
}

// <name> = pack <tuple type> <a>, <b>, ...
// Each element is copied into its place inside a new tuple slot. Elements must be names
// (a constant goes through alloc + store first). Layout: natural alignment, in order.
inline void parse_pack(const std::string& name, Lexer& lex, symbols& sym, codegen& asmgen) {
    auto type = types::get_from(lex);

    if( type.kind != types::mtuple )
        CompilerOutputs::fatal("pack needs a tuple type, but received a " + type.kto_string());

    struct field { size_t pos, size, offset; };
    std::vector<field> fields;

    size_t offset = 0, max_align = 1;

    while( true ) {
        auto value = next_token(lex, "tuple element");

        if( is_number(value) )
            CompilerOutputs::fatal("pack can't take the literal " + value + ": use alloc + store first");

        auto val = lookup_or_fatal(sym, value, "Value");

        types::type t;
        size_t pos = 0;

        if( std::holds_alternative<borrowed>(val) ) {
            auto b = std::get<borrowed>(val);
            t = b.allocated, pos = b.pos;
        }
        else if( std::holds_alternative<allocation>(val) ) {
            auto a = std::get<allocation>(val);
            t = a.allocated, pos = a.pos;
        }
        else {
            CompilerOutputs::fatal("'" + value + "' can't be used as a tuple element");
        }

        if( t.size == 0 )
            CompilerOutputs::fatal("pack can't take a zero-sized element: '" + value + "'");

        bool scalar = t.size == 1 || t.size == 2 || t.size == 4 || t.size == 8;

        if( t.kind != types::mtuple && !scalar )
            CompilerOutputs::fatal("Unsupported tuple element size: " + std::to_string(t.size));

        // a nested tuple's real alignment isn't known here: assumes its size, capped at 8
        size_t align = scalar ? t.size : std::min<size_t>(8, codegen::alignment_of(t.size));

        offset = codegen::align_up(offset, align);
        fields.push_back({ pos, t.size, offset });
        offset += t.size;
        max_align = std::max(max_align, align);

        auto sep = lex.peek();
        if(! sep || *sep != "," ) break;
        lex.next();
    }

    // accepts the layout with or without trailing padding; anything else means the
    // elements don't match the declared tuple type
    size_t padded = codegen::align_up(offset, max_align);
    if( type.size != offset && type.size != padded )
        CompilerOutputs::fatal(
            "The elements of pack take " + std::to_string(offset) + " bytes, but the tuple type takes "
            + std::to_string(type.size)
        );

    size_t p = asmgen.alloca(type.size);
    sym.push(name, borrowed(type, p));

    for( const auto& f : fields )
        asmgen.copy_slot(sym, f.pos, f.size, p - f.offset);
}

// }
inline void parse_close(symbols& sym, codegen& asmgen) {
    sym.pop_scope();
    asmgen.epilogue(sym);
}

// ───────────── registers and mapped io ─────────────
//   <n> = reg_load <name>, <bits>              reg_store <name>, <bits>, <value>
//   <n> = reg_get  <name>, <bits>, <lo>, <w>   reg_set   <name>, <bits>, <lo>, <w>, <value>
//
//   io_*  : the same, with an address in the I/O SPACE (0x00..0x3F on AVR: the datasheet's
//           "I/O address" column, PORTB = 0x05). The backend uses in / out / sbi / cbi.
//   mem_* : the same, with an address in the DATA SPACE (the datasheet's memory address,
//           PORTB = 0x25, UCSR0A = 0xC0, ...). The backend uses lds / sts.
//
//   A mem_* location is a reg_loc with `io = true` and `name = "mem"`: the backends that
//   care about the difference (AVR) read it from there; the others see an address.

enum class loc_kind { reg, io, mem };

inline loc_kind kind_of(const std::string& statement) {
    if( statement.rfind("io_",  0) == 0 ) return loc_kind::io;
    if( statement.rfind("mem_", 0) == 0 ) return loc_kind::mem;
    return loc_kind::reg;
}

inline size_t parse_count(Lexer& lex, const char* what) {
    auto t = next_token(lex, what);
    if(! is_number(t) || t[0] == '-' || t.size() > 9 )
        CompilerOutputs::fatal(std::string("Expected ") + what + " (a non-negative number), but received '" + t + "'");
    return std::stoul(t);
}

inline reg_loc parse_location(Lexer& lex, loc_kind kind) {
    reg_loc r;
    r.io = kind != loc_kind::reg;

    const bool addressed = kind != loc_kind::reg;
    auto head = next_token(lex, kind == loc_kind::io  ? "io address"
                              : kind == loc_kind::mem ? "memory address"
                                                      : "register name");

    if( addressed ) {
        // the lexer splits `0x25` into `0` + `x25`: glue the pieces back until the comma
        if( head[0] == '0' ) {
            while( true ) {
                auto nx = lex.peek();
                if(! nx || *nx == "," ) break;
                head += *lex.next();
            }
        }

        if( head.size() < 3 || head.size() > 18 || head[0] != '0' || (head[1] != 'x' && head[1] != 'X')
            || head.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos )
            CompilerOutputs::fatal("Expected a hexadecimal address (0x...), but received '" + head + "'");
        r.addr = std::stoull(head.substr(2), nullptr, 16);

        if( kind == loc_kind::mem ) r.name = "mem";
    } else {
        r.name = head;
    }

    assert(lex, {","}, "A register / io access needs its size in bits: name, bits");
    r.bits = parse_count(lex, "the size in bits");

    if( r.bits != 8 && r.bits != 16 && r.bits != 32 && r.bits != 64 )
        CompilerOutputs::fatal("The size must be 8, 16, 32 or 64 bits, but received " + std::to_string(r.bits));

    return r;
}

// `, lo, width`
inline std::pair<size_t, size_t> parse_slice(Lexer& lex, const reg_loc& r) {
    assert(lex, {","}, "A slice needs lo and width: name, bits, lo, width");
    size_t lo = parse_count(lex, "the first bit of the slice");
    assert(lex, {","}, "A slice needs lo and width: name, bits, lo, width");
    size_t width = parse_count(lex, "the width of the slice");

    if( width == 0 || lo + width > r.bits )
        CompilerOutputs::fatal(
            "The slice " + std::to_string(lo) + "+" + std::to_string(width)
            + " doesn't fit in " + std::to_string(r.bits) + " bits"
        );

    return { lo, width };
}

inline types::type unsigned_type(size_t bytes) {
    types::type t;
    switch( bytes ) {
        case 1:  t.kind = types::mu8;  break;
        case 2:  t.kind = types::mu16; break;
        case 4:  t.kind = types::mu32; break;
        case 8: t.kind = types::mu64; break;
        default: t.kind = types::musize; break;
    }
    t.size = bytes;
    return t;
}

inline size_t slice_bytes(size_t width) {
    return width <= 8 ? 1 : width <= 16 ? 2 : width <= 32 ? 4 : 8;
}

// reg_store <name>, <bits>, <value>   |   io_store <addr>, <bits>, <value>   |   mem_store ...
inline void parse_loc_store(Lexer& lex, symbols& sym, codegen& asmgen, loc_kind kind) {
    auto loc = parse_location(lex, kind);
    assert(lex, {","}, "A store needs a value: name, bits, value");
    asmgen.loc_store(sym, loc, next_token(lex, "value"));
}

// reg_set <name>, <bits>, <lo>, <width>, <value>   |   io_set ...   |   mem_set ...
inline void parse_loc_set(Lexer& lex, symbols& sym, codegen& asmgen, loc_kind kind) {
    auto loc = parse_location(lex, kind);
    auto [lo, width] = parse_slice(lex, loc);
    assert(lex, {","}, "A set needs a value: name, bits, lo, width, value");
    asmgen.loc_set(sym, loc, lo, width, next_token(lex, "value"));
}

// <name> = <statement>
inline void parse_assignment(const std::string& name, Lexer& lex, symbols& sym, codegen& asmgen) {
    auto statement = next_token(lex, "statement");

    if( is_binary_op(statement) ) {
        parse_binary(name, statement, lex, sym, asmgen);
        return;
    }

    switch( hash(statement) ) {
        case hash("alloc"): {
            auto type = types::get_from(lex);
            size_t p  = asmgen.alloca(type.size);
            sym.push(name, allocation(type, p));
            return;
        }

        case hash("ptr_to"): {
            auto target = next_token(lex, "value name");
            auto val    = lookup_or_fatal(sym, target, "Value");

            if( std::holds_alternative<unfold>(val) )
                   CompilerOutputs::fatal("'" + target + "' is only declared with `unfold`; define it before taking its address");

            if( std::holds_alternative<subroutine>(val) ) {
                types::type ptr_t;
                ptr_t.kind = types::mptr;
                ptr_t.recalculate();

                size_t p = asmgen.alloca(ptr_t.size);
                sym.push(name, borrowed(ptr_t, p));
                asmgen.function_address(sym, target, p);   // cites the function
                return;
            }

            types::type t;
            size_t pos = 0;

            if( std::holds_alternative<borrowed>(val) ) {
                auto b = std::get<borrowed>(val);
                t = b.allocated, pos = b.pos;
            }
            else if( std::holds_alternative<allocation>(val) ) {
                auto a = std::get<allocation>(val);
                t = a.allocated, pos = a.pos;
            }
            else {
                CompilerOutputs::fatal(
                    "ptr_to only works on allocations and values returned by a call: '" + target + "'"
                );
            }

            t.kind = types::mptr;
            t.recalculate();
            sym.push(name, allocation(t, pos));
            return;
        }

        // `move` preserves the symbol category; it only introduces another name.
        case hash("move"): {
            auto source = next_token(lex, "value name");
            sym.push(name, lookup_or_fatal(sym, source, "Value"));
            return;
        }

        // <name> = pack <tuple type> <a>, <b>, ...
        case hash("pack"): {
            parse_pack(name, lex, sym, asmgen);
            return;
        }

        // `load` treats an allocation as the value stored in that stack slot,
        // rather than as the address of the slot. No copy is needed because
        // `borrowed` represents a value already stored at that position.
        case hash("load"): {
            auto source = next_token(lex, "value name");
            auto value  = lookup_or_fatal(sym, source, "Value");

            if( std::holds_alternative<borrowed>(value) ) {
                sym.push(name, std::get<borrowed>(value));
                return;
            }

            if( std::holds_alternative<allocation>(value) ) {
                auto a = std::get<allocation>(value);
                sym.push(name, borrowed(a.allocated, a.pos));
                return;
            }

            CompilerOutputs::fatal("'" + source + "' can't be loaded as a value");
            return;
        }

        case hash("call"): {
            auto fn  = next_token(lex, "function name");
            auto sub = lookup_subroutine(sym, fn);

            if( sub.ret.kind == types::mvoid )
                CompilerOutputs::fatal("The function " + fn + " has no return type. You do not need to collect it.");

            size_t p = asmgen.alloca(sub.ret.size);
            sym.push(name, borrowed(sub.ret, p));

            bool tuple = sub.ret.kind == types::mtuple;
            if( tuple ) asmgen.sret(sym, p);

            emit_arguments(lex, sym, asmgen, sub, tuple);

            asmgen.call(sym, fn);
            asmgen.collect_ret(sym, p);
            return;
        }

        // <name> = call_indirect <ret type> <ptr>(<args>)
        // The signature isn't known, so the return type is written in the instruction.
        // A tuple return goes through sret, exactly like a direct `call`.
        case hash("call_indirect"): {
            auto ret    = types::get_from(lex);
            auto target = next_token(lex, "function pointer name");

            if( ret.kind == types::mvoid )
                CompilerOutputs::fatal("call_indirect returns void: don't collect it, use `call_indirect " + target + "(...)`");

            assert(lex, {"("});
            auto args = get_block(lex, ")");

            size_t p = asmgen.alloca(ret.size);
            sym.push(name, borrowed(ret, p));

            bool tuple = ret.kind == types::mtuple;
            if( tuple ) asmgen.sret(sym, p);

            for( size_t i = args.size(); i-- > 0; )
                asmgen.argument_untyped(sym, args[i], i, tuple);

            asmgen.call_indirect(sym, target);
            asmgen.collect_ret(sym, p, ret);
            return;
        }

        case hash("deref"): {
            auto type   = types::get_from(lex);
            auto source = next_token(lex, "pointer name");
            auto val    = lookup_or_fatal(sym, source, "Value");

            if( type.size == 0 )
                CompilerOutputs::fatal("deref can't read a zero-sized type");

            types::type ptr_t;
            size_t src_pos = 0;
            bool via_lea   = false;

            if( std::holds_alternative<borrowed>(val) ) {
                auto b = std::get<borrowed>(val);
                ptr_t = b.allocated, src_pos = b.pos, via_lea = false;
            }
            else if( std::holds_alternative<allocation>(val) ) {
                auto a = std::get<allocation>(val);
                ptr_t = a.allocated, src_pos = a.pos, via_lea = true;
            }
            else {
                CompilerOutputs::fatal("deref needs a pointer value: '" + source + "'");
            }

            if( ptr_t.kind != types::mptr )
                CompilerOutputs::fatal(
                    "deref needs a pointer, but '" + source + "' is a " + ptr_t.kto_string()
                );

            size_t p = asmgen.alloca(type.size);
            sym.push(name, borrowed(type, p));
            asmgen.deref(sym, src_pos, via_lea, type.size, p);
            return;
        }

        case hash("syscall"): {
            auto number = next_token(lex, "syscall number");
            if(! is_number(number) )
                CompilerOutputs::fatal("Expected a syscall number, but received '" + number + "'");

            std::vector<std::string> values;
            while( true ) {
                auto sep = lex.peek();
                if(! sep || *sep != "," ) break;
                lex.next();
                values.push_back(next_token(lex, "syscall argument"));
            }

            if( values.size() > 6 )
                CompilerOutputs::fatal("A syscall takes at most 6 arguments");

            types::type ret;
            ret.kind = types::misize;
            ret.size = 8;

            size_t p = asmgen.alloca(ret.size);
            sym.push(name, borrowed(ret, p));

            asmgen.syscall(sym, number, values, p);
            return;
        }

        // <name> = str h<hex bytes>
        case hash("str"): {
            auto bytes = decode_literal(next_token(lex, "string literal"));

            types::type ptr_t;
            ptr_t.kind = types::mptr;
            ptr_t.recalculate();

            size_t p = asmgen.alloca(ptr_t.size);
            sym.push(name, borrowed(ptr_t, p));
            asmgen.string_ptr(sym, bytes, p);
            return;
        }

        // <name> = cast <type> <value>   (integer <-> integer, ptr <-> ptr, ptr <-> 64-bit)
        case hash("cast"): {
            auto type   = types::get_from(lex);
            auto source = next_token(lex, "value name");
            auto val    = lookup_or_fatal(sym, source, "Value");

            types::type from;
            size_t src_pos = 0;

            if( std::holds_alternative<borrowed>(val) ) {
                auto b = std::get<borrowed>(val);
                from = b.allocated, src_pos = b.pos;
            }
            else if( std::holds_alternative<allocation>(val) ) {
                auto a = std::get<allocation>(val);
                from = a.allocated, src_pos = a.pos;
            }
            else {
                CompilerOutputs::fatal("cast needs a value: '" + source + "'");
            }

            if( from.kind == types::mtuple || type.kind == types::mtuple || type.size == 0 )
                CompilerOutputs::fatal("cast only converts scalar values");

            // signed source -> sign extension; unsigned source and pointers -> zero extension
            bool src_signed = from.kto_string().rfind("mi", 0) == 0;

            size_t p = asmgen.alloca(type.size);
            sym.push(name, borrowed(type, p));
            asmgen.cast(sym, src_pos, from.size, src_signed, p, type.size);
            return;
        }

        // <n> = reg_load <name>, <bits>   |   <n> = io_load <addr>, <bits>   |   <n> = mem_load <addr>, <bits>
        case hash("reg_load"):
        case hash("io_load"):
        case hash("mem_load"): {
            auto loc = parse_location(lex, kind_of(statement));

            auto t = unsigned_type(loc.bits / 8);
            size_t p = asmgen.alloca(t.size);
            sym.push(name, borrowed(t, p));
            asmgen.loc_load(sym, loc, p);
            return;
        }

        // <n> = reg_get <name>, <bits>, <lo>, <width>   |   <n> = io_get ...   |   <n> = mem_get ...
        case hash("reg_get"):
        case hash("io_get"):
        case hash("mem_get"): {
            auto loc = parse_location(lex, kind_of(statement));
            auto [lo, width] = parse_slice(lex, loc);

            auto t = unsigned_type(slice_bytes(width));
            size_t p = asmgen.alloca(t.size);
            sym.push(name, borrowed(t, p));
            asmgen.loc_get(sym, loc, lo, width, p, t.size);
            return;
        }

        default:
            CompilerOutputs::fatal("Unknown statement '" + statement + "' after '='");
    }
}

inline subroutine parse_function(Lexer& lex, symbols& sym, codegen& asmgen) {
    lex.back();
    auto ret  = types::get_from(lex);
    auto name = next_token(lex, "function name");

    assert(lex, {"("});
    auto params = types::get_sequence(lex, ")");

    if( auto data = sym.lookup(name); data.has_value() ) {
        if(! std::holds_alternative<unfold>(data.value()) )
            CompilerOutputs::fatal(name + " is already defined.");

        auto fwd = std::get<unfold>(data.value());
        bool same = fwd.ret.kind == ret.kind
                 && fwd.ret.size == ret.size
                 && fwd.args.size() == params.size();
        for( size_t i = 0; same && i < params.size(); i++ )
            same = fwd.args[i].kind == params[i].kind && fwd.args[i].size == params[i].size;

        if(! same )
            CompilerOutputs::fatal("Definition of " + name + " does not match its unfold declaration.");

        sym.erase(name);
    }

    subroutine sub(ret, params);
    sym.push(name, sub);

    assert(lex, {"{"});
    sym.push_scope();
    asmgen.prologue(sym, name);
    return sub;
}

// @_ : <arg>, <arg>, ...
inline std::vector<destroyer_arg> parse_destroyer(Lexer& lex, symbols& sym, codegen& asmgen,
                                                  const std::optional<subroutine>& current) {
    if(! current )
        CompilerOutputs::fatal("A destroyer '@_' must be declared inside a function.");

    assert(lex, {":"}, "A destroyer needs its arguments: @_ : self, ...");

    std::vector<destroyer_arg> args;

    while( true ) {
        auto name = next_token(lex, "argument name");

        if( name == "," || name == ":" || name == "{" || name == "}" || name == "=" )
            CompilerOutputs::fatal("Expected an argument name in the destroyer, but received '" + name + "'");

        for( const auto& a : args )
            if( a.name == name )
                CompilerOutputs::fatal("Duplicated argument '" + name + "' in the destroyer");

        if( args.size() >= current->args.size() )
            CompilerOutputs::fatal(
                "The destroyer has more arguments than the function, which takes "
                + std::to_string(current->args.size())
            );

        args.push_back({ name, current->args[args.size()] });

        auto sep = lex.peek();
        if(! sep || *sep != "," ) break;
        lex.next();
    }

    if( args.size() != current->args.size() )
        CompilerOutputs::fatal(
            "The destroyer needs " + std::to_string(current->args.size())
            + " arguments (one per function parameter), but received " + std::to_string(args.size())
        );

    bool sret = current->ret.kind == types::mtuple;

    for( size_t i = 0; i < args.size(); i++ ) {
        if( sym.lookup(args[i].name).has_value() )
            CompilerOutputs::fatal("'" + args[i].name + "' is already defined.");

        size_t p = asmgen.alloca(args[i].type.size);
        asmgen.param(sym, i, p, args[i].type.size, sret, args[i].type.kind == types::mtuple);
        sym.push(args[i].name, borrowed(args[i].type, p));
    }

    return args;
}

inline target_t parser(Lexer& lex, std::string output, std::string arch_os, bool use_ram = false) {
    target_t config;
    gen_target(&config, arch_os);

    config.output = output;

    codegen::backend_factory factory = [config](const std::string& out) -> std::unique_ptr<assembly_backend> {
        target_t c = config;
        c.output = out;
        return assembly_factory::create(c);
    };

    std::optional<subroutine> current;
    std::vector<destroyer_arg> destroyer_args;
    codegen asmgen(std::move(factory), output, use_ram);
    symbols sym;

    while( auto token = lex.next() ) {
        switch( hash(*token) ) {
            case hash("extern"): parse_extern(lex, sym, asmgen);    continue;
            case hash("unfold"): parse_unfold(lex, sym, asmgen);    continue;
            case hash("call"):   parse_call_void(lex, sym, asmgen); continue;
            case hash("store"):  parse_store(lex, sym, asmgen);     continue;
            case hash("storep"): parse_storep(lex, sym, asmgen);    continue;
            case hash("ret"):    parse_ret(lex, sym, asmgen);       continue;
            case hash("br"):     parse_br(lex, sym, asmgen);        continue;
            case hash("brez"):   parse_branch(lex, sym, asmgen, false); continue;
            case hash("brnz"):   parse_branch(lex, sym, asmgen, true);  continue;
            case hash("@_"):     destroyer_args = parse_destroyer(lex, sym, asmgen, current); continue;
            case hash("}"):      parse_close(sym, asmgen);          continue;
            case hash("call_indirect"): parse_call_indirect(lex, sym, asmgen); continue;
            case hash("reg_store"): parse_loc_store(lex, sym, asmgen, loc_kind::reg); continue;
            case hash("io_store"):  parse_loc_store(lex, sym, asmgen, loc_kind::io);  continue;
            case hash("mem_store"): parse_loc_store(lex, sym, asmgen, loc_kind::mem); continue;
            case hash("reg_set"):   parse_loc_set(lex, sym, asmgen, loc_kind::reg);   continue;
            case hash("io_set"):    parse_loc_set(lex, sym, asmgen, loc_kind::io);    continue;
            case hash("mem_set"):   parse_loc_set(lex, sym, asmgen, loc_kind::mem);   continue;

            default: {
                if( is_label_token(*token) ) {
                    parse_label(*token, sym, asmgen);
                    continue;
                }

                auto sign = lex.peek();
                if( sign && *sign == "=" ) {
                    lex.next();
                    parse_assignment(*token, lex, sym, asmgen);
                    continue;
                }

                current = parse_function(lex, sym, asmgen);
                continue;
            }
        }
    }

    asmgen.finalize();

    return config;
}
