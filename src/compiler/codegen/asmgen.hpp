#pragma once

#include "../parser/types.hpp"
#include "../symbols.hpp"
#include "./backends.hpp"
#include "backends.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

inline subroutine lookup_subroutine(symbols& sym, const std::string& name);

/// `123` or `-123`
inline bool is_integer_literal(const std::string& s) {
    size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
    return i < s.size() && std::all_of(s.begin() + i, s.end(), [](unsigned char c) { return std::isdigit(c); });
}

struct codegen {
    size_t FUNC = 0;
    std::string current_function, last_call;

    size_t pos = 0;
    size_t reserved = 0;

    std::unique_ptr<assembly_backend> backend;
    codegen(std::unique_ptr<assembly_backend> b)
        : backend(std::move(b)) {}

    backend_void(extern_fn, (symbols& sym, std::string data) , sym, data);
    backend_void(save_sret, (symbols& sym, std::size_t pos) , sym, pos);
    backend_void(param, (symbols& sym, std::size_t index, std::size_t pos, std::size_t size, bool sret), sym, index, pos, size, sret);
    backend_void(deref, (symbols& sym, std::size_t src_pos, bool via_lea, std::size_t size, std::size_t dst_pos), sym, src_pos, via_lea, size, dst_pos);
    backend_void(string_ptr, (symbols& sym, const std::string& bytes, std::size_t pos), sym, bytes, pos);
    backend_void(cast, (symbols& sym, std::size_t src_pos, std::size_t src_size, bool src_signed, std::size_t dst_pos, std::size_t dst_size), sym, src_pos, src_size, src_signed, dst_pos, dst_size);

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

        if( is_integer_literal(value) ) {
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

        if( lookup_subroutine(sym, fn).ret.kind == types::mtuple ) {
            size_t p = alloca(8);
            save_sret(sym, p);
        }
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
        auto sub = lookup_subroutine(sym, last_call);
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

    void ret_value(symbols& sym, std::size_t pos, std::size_t size, bool tuple) {
        backend->ret_value(sym, pos, size, tuple, FUNC);
    }

    void syscall(symbols& sym, const std::string& number,
                 const std::vector<std::string>& values, size_t p) {
        std::vector<syscall_arg> args;

        for( const auto& v : values ) {
            syscall_arg a;

            if( is_integer_literal(v) ) {
                a.imm = true;
                a.src = "$" + v;
                args.push_back(a);
                continue;
            }

            auto symbol = sym.lookup(v);
            if(! symbol.has_value() ) CompilerOutputs::fatal("Value " + v + " not found");

            if( std::holds_alternative<allocation>(symbol.value()) ) {
                auto al = std::get<allocation>(symbol.value());
                a.lea  = true;
                a.size = al.allocated.size;
                a.src  = "-" + std::to_string(al.pos) + "(%rbp)";
            }
            else if( std::holds_alternative<borrowed>(symbol.value()) ) {
                auto b = std::get<borrowed>(symbol.value());
                a.size = b.allocated.size;
                a.src  = "-" + std::to_string(b.pos) + "(%rbp)";
            }
            else {
                CompilerOutputs::fatal("'" + v + "' can't be used as a syscall argument");
            }

            args.push_back(a);
        }

        backend->syscall(sym, number, args, p);
    }


    void label(symbols& sym, const std::string& name) {
        backend->label(name);
    }

    void jump(symbols& sym, const std::string& name) {
        backend->jump(name);
    }

    void branch(symbols& sym, size_t p, size_t size, bool nonzero, const std::string& name) {
        backend->branch(p, size, nonzero, name);
    }

    std::optional<types::type> operand_type(symbols& sym, const std::string& value) {
        if( is_integer_literal(value) ) return std::nullopt;

        auto symbol = sym.lookup(value);
        if(! symbol.has_value() ) CompilerOutputs::fatal("Value " + value + " not found");

        if( std::holds_alternative<allocation>(symbol.value()) )
            return std::get<allocation>(symbol.value()).allocated;

        if( std::holds_alternative<borrowed>(symbol.value()) )
            return std::get<borrowed>(symbol.value()).allocated;

        CompilerOutputs::fatal("'" + value + "' can't be used as an operand");
        return std::nullopt;
    }

    void binary(symbols& sym, const std::string& op, size_t size,
                const std::string& a, const std::string& b,
                size_t dst, size_t dst_size) {
        auto operand = [&](const std::string& v) -> std::pair<bool, std::string> {
            if( is_integer_literal(v) ) return { true, "$" + v };

            auto symbol = sym.lookup(v);
            if(! symbol.has_value() ) CompilerOutputs::fatal("Value " + v + " not found");

            size_t p = 0;
            if( std::holds_alternative<allocation>(symbol.value()) )
                p = std::get<allocation>(symbol.value()).pos;
            else if( std::holds_alternative<borrowed>(symbol.value()) )
                p = std::get<borrowed>(symbol.value()).pos;
            else
                CompilerOutputs::fatal("'" + v + "' can't be used as an operand");

            return { false, "-" + std::to_string(p) + "(%rbp)" };
        };

        auto [a_imm, a_src] = operand(a);
        auto [b_imm, b_src] = operand(b);

        backend->binary(sym, op, size, a_imm, a_src, b_imm, b_src, dst, dst_size);
    }

    void store_ptr(symbols& sym, size_t ptr_pos, bool ptr_lea, types::type type, std::string value) {
        auto [lea, src] = _load_data(sym, value, type);
        backend->store_ptr(sym, type.size, ptr_pos, ptr_lea, lea, src);
    }

    void copy_slot(symbols& sym, size_t src_pos, size_t size, size_t dst_pos) {
        if( size == 1 || size == 2 || size == 4 || size == 8 ) {
            std::string src = "-" + std::to_string(src_pos) + "(%rbp)";
            backend->store(sym, size, false, src, dst_pos);
            return;
        }
        backend->copy_bytes(sym, src_pos, size, dst_pos);
    }
};
