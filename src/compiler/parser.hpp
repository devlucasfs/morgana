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
#include <string>
#include <variant>

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

inline bool is_number(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
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

// ret <integer>
inline void parse_ret(Lexer& lex, symbols& sym, codegen& asmgen) {
    auto data = next_token(lex, "return value");

    if(! is_number(data) )
        CompilerOutputs::fatal("ret only supports integer literals for now, received '" + data + "'");

    asmgen.ret_int(sym, data);
}

// }
inline void parse_close(symbols& sym, codegen& asmgen) {
    sym.pop_scope();
    asmgen.epilogue(sym);
}

// <name> = <statement>
inline void parse_assignment(const std::string& name, Lexer& lex, symbols& sym, codegen& asmgen) {
    auto statement = next_token(lex, "statement");

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

            if(! std::holds_alternative<borrowed>(val) )
                CompilerOutputs::fatal("ptr_to only works on values returned by a call: '" + target + "'");

            auto borrow = std::get<borrowed>(val);
            borrow.allocated.kind = types::mptr;
            borrow.allocated.recalculate();
            sym.push(name, allocation(borrow.allocated, borrow.pos));
            return;
        }

        case hash("move"):
        case hash("load"): {
            auto source = next_token(lex, "value name");
            sym.push(name, lookup_or_fatal(sym, source, "Value"));
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

        default:
            CompilerOutputs::fatal("Unknown statement '" + statement + "' after '='");
    }
}

inline void parse_function(Lexer& lex, symbols& sym, codegen& asmgen) {
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

    sym.push(name, subroutine(ret, params));

    assert(lex, {"{"});
    sym.push_scope();
    asmgen.prologue(sym, name);
}

inline void parser(Lexer& lex, std::string output, std::string extensor) {
    target config {
        .architecture = X86_64,
        .convention   = GNU,
        .output       = output,
    };

    auto backend = assembly_factory::create(config);
    codegen asmgen(std::move(backend));
    symbols sym;

    while( auto token = lex.next() ) {
        switch( hash(*token) ) {
            case hash("extern"): parse_extern(lex, sym, asmgen);    continue;
            case hash("unfold"): parse_unfold(lex, sym, asmgen);    continue;
            case hash("call"):   parse_call_void(lex, sym, asmgen); continue;
            case hash("store"):  parse_store(lex, sym, asmgen);     continue;
            case hash("ret"):    parse_ret(lex, sym, asmgen);       continue;
            case hash("}"):      parse_close(sym, asmgen);          continue;

            default: {
                auto sign = lex.peek();
                if( sign && *sign == "=" ) {
                    lex.next();
                    parse_assignment(*token, lex, sym, asmgen);
                    continue;
                }

                parse_function(lex, sym, asmgen);
                continue;
            }
        }
    }
}
