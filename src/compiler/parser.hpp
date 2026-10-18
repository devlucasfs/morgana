#pragma once

#include "./lexer.hpp"
#include "./symbols.hpp"
#include "./backends/factory.hpp"
#include "./codegen/backends.hpp"
#include "./codegen/asmgen.hpp"
#include "parser/blocks.hpp"
#include "parser/types.hpp"
#include <string>
#include <initializer_list>

std::string assert(Lexer& lex, std::initializer_list<std::string> expected, std::string extra="") {
    auto tk = lex.next();
    for( const auto& str : expected ) if( tk && *tk == str ) return str;

    std::string message = "Expected ";

    bool first = true;
    for( const auto& str : expected ) {
        if(! first ) message += " or ";
        message += "'" + str + "'";
        first = false;
    }

    message += " and was received '" + *tk + "' instead." + (!extra.empty() ? ("\n~ " + extra) : "");

    CompilerOutputs::fatal(message);
    return "";
}

void parser(Lexer& lex, std::string output, std::string extensor) {
    target config {
        .architecture = X86_64,
        .convention = GNU,
        .output = output,
    };

    auto backend = assembly_factory::create(config);
    codegen asmgen(std::move(backend));
    symbols sym;

    size_t pos = 0;
    while( auto token = lex.next() ) {
        switch(hash(*token)) {
            case hash("extern"): {
                auto ret = types::get_from(lex);
                auto data = lex.next();
                asmgen.extern_fn(sym, *data);
                assert(lex, {"("});
                auto types = types::get_sequence(lex, ")");
                sym.push(*data, subroutine(ret, types));
                continue;
            }

            case hash("call"): {
                auto data = lex.next();
                auto fn = sym.lookup(*data);
                if(! fn.has_value() ) CompilerOutputs::fatal("Function " + *data + " not fond");

                auto sub = std::get<subroutine>(fn.value());
                if( sub.ret.kind != types::mvoid ) CompilerOutputs::fatal("The function " + *data + " have a return type. You need to colect it.");

                assert(lex, {"("});
                auto args = get_block(lex, ")");
                if( sub.args.size() != args.size() ) CompilerOutputs::fatal("Was expected " + std::to_string(sub.args.size()) + " arguments, but was received " + std::to_string(args.size()));
                for( int i = args.size() - 1; i >= 0; i-- ) {
                    const auto& arg = args[i];
                    asmgen.argument(sym, sub, arg, i, false);
                }

                asmgen.call(sym, *data);
                continue;
            }

            case hash("store"): {
                auto data = lex.next();
                auto alloc = sym.lookup(*data);
                if(
                    (! alloc.has_value() )
                ||  (! std::holds_alternative<allocation>(alloc.value()) )
                ) CompilerOutputs::fatal("Allocation " + *data + " not fond");

                auto value = lex.next();
                asmgen.store(sym, std::get<allocation>(alloc.value()), *value);
            }

            case hash("}"): {
                sym.pop_scope();
                asmgen.epilogue(sym);
                pos = 0;
                continue;
            }

            default: {
                auto sign = lex.peek();
                if( sign && *sign == "=" ) {
                    lex.next();
                    auto statement = lex.next();
                    switch(hash(*statement)) {
                        case hash("alloc"): {
                            auto type = types::get_from(lex);
                            pos += type.size;
                            sym.push(*token, allocation(type, pos));
                        } continue;

                        case hash("ptr_to"): {
                            auto data = lex.next();
                            auto value = sym.lookup(*data);
                            if(! value.has_value() ) CompilerOutputs::fatal("Value " + *data + " not fond");

                            auto val = value.value();
                            if( std::holds_alternative<borrowed>(val) ) {
                                auto borrow = std::get<borrowed>(val);
                                borrow.allocated.kind = types::mptr;
                                borrow.allocated.recalculate();
                                sym.push(*token, allocation(borrow.allocated, borrow.pos));
                            }
                        } continue;

                        case hash("call"): {
                            auto data = lex.next();
                            auto fn = sym.lookup(*data);
                            if(! fn.has_value() ) CompilerOutputs::fatal("Function " + *data + " not fond");

                            auto sub = std::get<subroutine>(fn.value());
                            if( sub.ret.kind == types::mvoid ) CompilerOutputs::fatal("The function " + *data + " have not a return type. You do not need to colect it.");

                            pos += sub.ret.size;
                            sym.push(*token, borrowed(sub.ret, pos));

                            bool tuple = sub.ret.kind == types::mtuple;
                            if( tuple ) {
                                asmgen.sret(sym, pos);
                            }

                            assert(lex, {"("});
                            auto args = get_block(lex, ")");
                            if( sub.args.size() != args.size() ) CompilerOutputs::fatal("Was expected " + std::to_string(sub.args.size()) + " arguments, but was received " + std::to_string(args.size()));
                            for( int i = args.size() - 1; i >= 0; i-- ) {
                                const auto& arg = args[i];
                                asmgen.argument(sym, sub, arg, i, tuple);
                            }

                            asmgen.call(sym, *data);
                            asmgen.collect_ret(sym, pos);
                            continue;
                        }

                        default: continue;
                    }
                }

                lex.back();
                auto ret = types::get_from(lex);
                auto data = lex.next();
                assert(lex, {"("});
                auto types = types::get_sequence(lex, ")");
                sym.push(*data, subroutine(ret, types));
                assert(lex, {"{"});
                sym.push_scope();
                asmgen.prologue(sym, *data);
            }
        }
    }
}
