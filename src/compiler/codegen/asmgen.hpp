#pragma once

#include "../parser/types.hpp"
#include "../symbols.hpp"
#include "./backends.hpp"
#include "backends.hpp"
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <regex>
#include <string>
#include <utility>
#include <variant>
#include <memory>

struct codegen {
    size_t FUNC = 0;
    std::string current_function, last_call;

    size_t pos = 0;
    size_t reserved = 0;

    std::unique_ptr<assembly_backend> backend;
    codegen(std::unique_ptr<assembly_backend> b)
        : backend(std::move(b)) {}

    backend_void(extern_fn, (symbols& sym, std::string data) , sym, data);

    static size_t align_up(size_t v, size_t a) {
        return (v + a - 1) & ~(a - 1);
    }

    static size_t alignment_of(size_t size) {
        if( size >= 16 ) return 16;
        size_t a = 1;
        while( a < size ) a <<= 1;
        return a;
    }

    size_t alloca(std::size_t size) {
        if( size == 0 ) return pos;

        pos = align_up(pos + size, alignment_of(size));

        if( pos > reserved ) {
            size_t delta = align_up(pos - reserved, backend->alignment());
            backend->alloca(delta);
            reserved += delta;
        }
        return pos;
    }

    std::pair<bool, std::string> _load_data(symbols& sym, std::string value, types::type type) {
        bool lea = false;
        std::string src = "unknown";

        std::regex number("\\d*");
        if( std::regex_match(value, number) ) {
            return { lea, "$" + value };
        }

        auto symbol = sym.lookup(value);
        if(! symbol.has_value() ) CompilerOutputs::fatal("Invalid data informed");

        size_t pos = 0;
        types::type t;
        if( std::holds_alternative<allocation>(symbol.value()) )
        {   auto a = std::get<allocation>(symbol.value());
            lea = true, pos = a.pos, t = a.allocated;
        }

        if( std::holds_alternative<borrowed>(symbol.value()) )
        {   auto b = std::get<borrowed>(symbol.value());
            lea = false, pos = b.pos, t = b.allocated;
        }

        if( t.kind != type.kind )
            CompilerOutputs::fatal("Was expected receive a " + type.kto_string() + " but was received " + t.kto_string());

        src = "-" + std::to_string(pos) + "(%rbp)";

        return { lea, src };
    }

    void argument(symbols& sym, subroutine& sub, std::string arg, size_t i, bool sret) {
        auto [lea, src] = _load_data(sym, arg, sub.args[i]);
        backend->argument(sym, lea, src, i, sret);
    }

    void prologue(symbols& sym, std::string fn) {
        current_function = fn;
        pos = reserved = 0;
        backend->prologue(sym, fn, FUNC);
    }

    void epilogue(symbols& sym) {
        backend->epilogue(sym, current_function, FUNC++);
        pos = reserved = 0;
    }

    void call(symbols& sym, std::string data) {
        last_call = data;
        backend->call(sym, data);
    }

    void collect_ret(symbols& sym, size_t p) {
        auto fn = sym.lookup(last_call);
        auto sub = std::get<subroutine>(fn.value());
        backend->collect_ret(sym, p, sub.ret.size, sub.ret.kind == types::mtuple);
    }

    void sret(symbols& sym, size_t p) {
        backend->sret(sym, p);
    }

    void store(symbols& sym, allocation alloc, std::string value) {
        auto [lea, src] = _load_data(sym, value, alloc.allocated);
        backend->store(sym, alloc.allocated.size, lea, src, alloc.pos);
    }

    void ret_int(symbols& sym, std::string& data) {
        backend->ret_int(sym, data, FUNC);
    }
};
