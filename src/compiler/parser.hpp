#pragma once

#include "./lexer.hpp"
#include "./symbols.hpp"
#include "./backends/factory.hpp"
#include "./codegen/backends.hpp"
#include "./codegen/asmgen.hpp"
#include "parser/blocks.hpp"
#include "parser/types.hpp"
#include <algorithm>
#include <cctype>
#include <initializer_list>
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
        "add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or",
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

// }
inline void parse_close(symbols& sym, codegen& asmgen) {
    sym.pop_scope();
    asmgen.epilogue(sym);
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


            size_t p = asmgen.alloca(8);
            // sym.push(name, borrowed(ret, p));

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
        asmgen.param(sym, i, p, args[i].type.size, sret);
        sym.push(args[i].name, borrowed(args[i].type, p));
    }

    return args;
}

inline void parser(Lexer& lex, std::string output, std::string extensor) {
    target config {
        .architecture = X86_64,
        .convention   = GNU,
        .output       = output,
    };

    auto backend = assembly_factory::create(config);
    std::optional<subroutine> current;
    std::vector<destroyer_arg> destroyer_args;
    codegen asmgen(std::move(backend));
    symbols sym;

    while( auto token = lex.next() ) {
        switch( hash(*token) ) {
            case hash("extern"): parse_extern(lex, sym, asmgen);    continue;
            case hash("unfold"): parse_unfold(lex, sym, asmgen);    continue;
            case hash("call"):   parse_call_void(lex, sym, asmgen); continue;
            case hash("store"):  parse_store(lex, sym, asmgen);     continue;
            case hash("ret"):    parse_ret(lex, sym, asmgen);       continue;
            case hash("br"):     parse_br(lex, sym, asmgen);        continue;
            case hash("brez"):   parse_branch(lex, sym, asmgen, false); continue;
            case hash("brnz"):   parse_branch(lex, sym, asmgen, true);  continue;
            case hash("@_"):     destroyer_args = parse_destroyer(lex, sym, asmgen, current); continue;
            case hash("}"):      parse_close(sym, asmgen);          continue;

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
}
