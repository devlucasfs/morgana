#pragma once

#include "../parser/types.hpp"
#include "../symbols.hpp"
#include "./backends.hpp"
#include "backends.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
namespace fs = std::filesystem;

inline subroutine lookup_subroutine(symbols& sym, const std::string& name);

/// `123` or `-123`
inline bool is_integer_literal(const std::string& s) {
    size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
    return i < s.size() && std::all_of(s.begin() + i, s.end(), [](unsigned char c) { return std::isdigit(c); });
}

struct codegen {
    using backend_factory = std::function<std::unique_ptr<assembly_backend>(const std::string& output)>;

    struct unit {
        std::string path;
        std::string ram;
        bool in_ram   = false;
        bool defined  = false;
        bool existing = false;
        std::vector<std::string> callees;
    };

    size_t FUNC = 0;
    std::string current_function, last_call;

    size_t pos = 0;
    size_t reserved = 0;

    std::unique_ptr<assembly_backend> backend;
    std::unique_ptr<assembly_backend> header;

    backend_factory factory;
    std::string final_output;
    bool use_ram = false;
    bool in_function = false;
    bool finalized = false;

    std::string entry_id = "_start";

    fs::path workdir;
    size_t next_unit = 0;
    std::unordered_map<std::string, unit> units;
    std::unordered_set<std::string> externs;

    codegen(backend_factory f, std::string output, bool ram = false)
        : factory(std::move(f)), final_output(std::move(output)), use_ram(ram) {
        backend = factory(final_output);
        if(! use_ram ) create_workdir();
    }

    codegen(const codegen&) = delete;
    codegen& operator=(const codegen&) = delete;

    ~codegen() {
        backend.reset();
        header.reset();

        std::error_code ec;
        if(! workdir.empty() ) fs::remove_all(workdir, ec);
    }

    const std::string& get_entry_id() const { return entry_id; }
    void set_entry_id(std::string id) { entry_id = std::move(id); }

    bool exists(const std::string& name) const {
        auto it = units.find(name);
        return it != units.end() && it->second.existing;
    }

    void create_workdir() {
        std::error_code ec;
        auto base = fs::temp_directory_path(ec);
        if( ec ) base = fs::current_path(ec);

        std::random_device rd;
        auto stamp = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
        std::ostringstream name;
        name << "morgana-" << std::hex << stamp << '-' << rd() << rd();

        workdir = base / name.str();
        fs::create_directories(workdir, ec);
        if( ec ) CompilerOutputs::fatal("Could not create the temporary directory '" + workdir.string() + "': " + ec.message());
    }

    static void remove_file(const std::string& path) {
        std::error_code ec;
        fs::remove(path, ec);
    }

    void append_unit(std::ostream& out, unit& u) {
        if( u.in_ram ) {
            out << u.ram;
            u.ram.clear();
            u.ram.shrink_to_fit();
            return;
        }

        {
            std::ifstream in(u.path, std::ios::binary);
            if(! in ) CompilerOutputs::fatal("Could not read the temporary file '" + u.path + "'");

            // `out << rdbuf` sets failbit when nothing is copied
            if( in.peek() != std::ifstream::traits_type::eof() ) out << in.rdbuf();
            out.clear();
        }
        remove_file(u.path);
    }

    void reference(const std::string& name) {
        if(! in_function ) return;

        auto& callees = units[current_function].callees;
        if( std::find(callees.begin(), callees.end(), name) == callees.end() )
            callees.push_back(name);
    }

    void finalize() {
        if( finalized ) return;
        finalized = true;

        if( in_function )
            CompilerOutputs::fatal("The function '" + current_function + "' was not closed with '}'");

        auto entry = units.find(entry_id);
        if( entry == units.end() || !entry->second.defined )
            CompilerOutputs::fatal("The entry point '" + entry_id + "' is not defined");

        backend.reset();
        header.reset();

        std::ofstream out(final_output, std::ios::binary | std::ios::app);
        if(! out ) CompilerOutputs::fatal("Could not open the output '" + final_output + "'");

        std::vector<std::string> stack { entry_id };

        while(! stack.empty() ) {
            auto name = std::move(stack.back());
            stack.pop_back();

            auto it = units.find(name);

            if( it == units.end() || !it->second.defined ) {
                if( externs.count(name) ) continue;
                CompilerOutputs::fatal("The function '" + name + "' is used, but it was never defined");
            }

            auto& u = it->second;
            if( u.existing ) continue;

            u.existing = true;
            append_unit(out, u);

            for( size_t i = u.callees.size(); i-- > 0; )
                stack.push_back(u.callees[i]);
        }

        out.flush();
        if(! out ) CompilerOutputs::fatal("Could not write the output '" + final_output + "'");

        for( auto& [name, u] : units ) {
            if( u.existing ) continue;
            u.ram.clear();
            u.ram.shrink_to_fit();
            if(! u.in_ram && !u.path.empty() ) remove_file(u.path);
        }
    }

    void extern_fn(symbols& sym, std::string data) {
        externs.insert(data);
        backend->extern_fn(sym, data);
    }

    backend_void(save_sret, (symbols& sym, std::size_t pos) , sym, pos);
    backend_void(param, (symbols& sym, std::size_t index, std::size_t pos, std::size_t size, bool sret, bool tuple), sym, index, pos, size, sret, tuple);
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

        auto is_int = [](const types::type& x) {
            auto s = x.kto_string();
            return s.rfind("mi", 0) == 0 || s.rfind("mu", 0) == 0;
        };

        if( t.kind != type.kind && !(is_int(t) && is_int(type) && t.size == type.size) )
            CompilerOutputs::fatal("Was expected receive a " + type.kto_string() + " but was received " + t.kto_string());

        src = "-" + std::to_string(pos) + "(%rbp)";

        return { lea, src };
    }

    void argument(symbols& sym, subroutine& sub, std::string arg, size_t i, bool sret) {
        const bool tuple_param = sub.args[i].kind == types::mtuple;

        if( tuple_param && is_integer_literal(arg) )
            CompilerOutputs::fatal("A tuple argument can't be a literal: '" + arg + "'");

        auto [lea, src] = _load_data(sym, arg, sub.args[i]);
        if( tuple_param ) lea = true;

        backend->argument(sym, lea, src, i, sret);
    }

    void prologue(symbols& sym, std::string fn) {
        if( in_function )
            CompilerOutputs::fatal("The function '" + fn + "' can't be defined inside '" + current_function + "'");

        current_function = fn;
        pos = reserved = 0;

        auto& u = units[fn];
        u = unit{};
        u.defined = true;
        u.path    = use_ram
                  ? std::string(assembly_backend::memory_target)
                  : (workdir / (std::to_string(next_unit++) + ".s")).string();

        header  = std::move(backend);
        backend = factory(u.path);
        in_function = true;

        backend->prologue(sym, fn, FUNC);

        if( lookup_subroutine(sym, fn).ret.kind == types::mtuple ) {
            size_t p = alloca(8);
            save_sret(sym, p);
        }
    }

    void epilogue(symbols& sym) {
        backend->epilogue(sym, current_function, FUNC++);

        auto& u = units[current_function];
        if( use_ram ) {
            u.ram    = backend->take_text();
            u.in_ram = true;
            u.path.clear();
        }

        backend.reset();
        backend = std::move(header);
        in_function = false;

        pos = reserved = 0;
    }

    void call(symbols& sym, std::string data) {
        last_call = data;
        reference(data);
        backend->call(sym, data);
    }

    void collect_ret(symbols& sym, size_t p) {
        auto sub = lookup_subroutine(sym, last_call);
        backend->collect_ret(sym, p, sub.ret.size, sub.ret.kind == types::mtuple);
    }

    void collect_ret(symbols& sym, size_t p, const types::type& ret) {
        backend->collect_ret(sym, p, ret.size, ret.kind == types::mtuple);
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

    void function_address(symbols& sym, const std::string& name, std::size_t pos) {
        reference(name);
        backend->function_address(sym, name, pos);
    }

    void argument_untyped(symbols& sym, const std::string& arg, size_t i, bool sret = false) {
        bool lea = false;
        std::string src;

        if( is_integer_literal(arg) ) {
            src = "$" + arg;
        } else {
            auto symbol = sym.lookup(arg);
            if(! symbol.has_value() ) CompilerOutputs::fatal("Invalid data informed: '" + arg + "'");

            size_t pos = 0;
            types::type t;

            if( std::holds_alternative<allocation>(symbol.value()) ) {
                auto a = std::get<allocation>(symbol.value());
                lea = true, pos = a.pos, t = a.allocated;
            }
            else if( std::holds_alternative<borrowed>(symbol.value()) ) {
                auto b = std::get<borrowed>(symbol.value());
                pos = b.pos, t = b.allocated;
            }
            else {
                CompilerOutputs::fatal("'" + arg + "' can't be used as an argument");
            }

            if( t.kind == types::mtuple ) lea = true;
            src = "-" + std::to_string(pos) + "(%rbp)";
        }

        backend->argument(sym, lea, src, i, sret);
    }

    syscall_arg value_operand(symbols& sym, const std::string& v) {
        syscall_arg a;

        if( is_integer_literal(v) ) {
            a.imm = true;
            a.src = "$" + v;
            return a;
        }

        auto symbol = sym.lookup(v);
        if(! symbol.has_value() ) CompilerOutputs::fatal("Value " + v + " not found");

        types::type t;
        size_t p = 0;

        if( std::holds_alternative<allocation>(symbol.value()) ) {
            auto al = std::get<allocation>(symbol.value());
            t = al.allocated, p = al.pos;
            a.lea = t.kind == types::mptr;
        }
        else if( std::holds_alternative<borrowed>(symbol.value()) ) {
            auto b = std::get<borrowed>(symbol.value());
            t = b.allocated, p = b.pos;
        }
        else {
            CompilerOutputs::fatal("'" + v + "' can't be used as a register / io value");
        }

        if( t.kind == types::mtuple )
            CompilerOutputs::fatal("A tuple can't be written to a register or io address: '" + v + "'");

        a.size = t.size;
        a.src  = "-" + std::to_string(p) + "(%rbp)";
        return a;
    }

    void loc_load(symbols& sym, const reg_loc& r, size_t p) {
        backend->loc_load(sym, r, p);
    }

    void loc_store(symbols& sym, const reg_loc& r, const std::string& value) {
        backend->loc_store(sym, r, value_operand(sym, value));
    }

    void loc_get(symbols& sym, const reg_loc& r, size_t lo, size_t width, size_t dst, size_t dst_size) {
        backend->loc_get(sym, r, lo, width, dst, dst_size);
    }

    void loc_set(symbols& sym, const reg_loc& r, size_t lo, size_t width, const std::string& value) {
        backend->loc_set(sym, r, lo, width, value_operand(sym, value));
    }

    void call_indirect(symbols& sym, const std::string& name) {
        auto symbol = sym.lookup(name);
        if(! symbol.has_value() ) CompilerOutputs::fatal("Value " + name + " not found");

        bool lea = false;
        size_t pos = 0;
        types::type t;

        if( std::holds_alternative<allocation>(symbol.value()) ) {
            auto a = std::get<allocation>(symbol.value());
            lea = true, pos = a.pos, t = a.allocated;
        }
        else if( std::holds_alternative<borrowed>(symbol.value()) ) {
            auto b = std::get<borrowed>(symbol.value());
            pos = b.pos, t = b.allocated;
        }
        else {
            CompilerOutputs::fatal("'" + name + "' can't be used as a call target");
        }

        if( t.kind != types::mptr )
            CompilerOutputs::fatal("call_indirect needs a pointer, but '" + name + "' is a " + t.kto_string());

        backend->call_indirect(sym, lea, "-" + std::to_string(pos) + "(%rbp)");
    }
};
