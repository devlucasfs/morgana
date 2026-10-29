#pragma once

#include "../symbols.hpp"
#include "../backends/factory.hpp"

#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#define backend_void(name, args, ...) \
    void name args { backend->name(__VA_ARGS__); }

#define backend_ret(type, name, ...) \
    type name args { return backend->name(__VA_ARGS__); }

enum arch { X86, X86_64, ARM, AARCH64, RISCV32, RISCV64 };
enum abi { SYSTEM_V, WINDOWS, EABI, GNU };

struct target {
    arch architecture;
    abi convention;
    std::string output;
};

class assembly_backend {
protected:
    std::ofstream stream;
    std::size_t tabs = 0;

public:
    explicit assembly_backend(std::string output) : stream(output) {}

    virtual ~assembly_backend() = default;

    assembly_backend(assembly_backend&) = delete;
    assembly_backend& operator=(assembly_backend&) = delete;

    void writeln(const std::string& value)
    {   for( std::size_t i = 0; i < tabs; i++ ) stream << '\t';
        stream << value << '\n';
    }

    void emptylines(std::size_t count)
    { for( std::size_t i = 0; i < count; i++ ) stream << '\n'; }

    void set_tabs(std::size_t count)
    { tabs = count; }

    virtual std::size_t alignment() = 0;

    virtual void extern_fn(symbols& sym, std::string& name) = 0;

    // Reserves `size` more bytes in the current function frame. It does NOT emit code:
    // the whole frame is reserved once, in the prologue (so jumps can never skip it).
    virtual void alloca(std::size_t size) = 0;

    virtual void prologue(symbols& sym, std::string& name, std::size_t id) = 0;
    virtual void epilogue(symbols& sym, std::string& name, std::size_t id) = 0;
    virtual void argument(symbols& sym, bool lea, std::string& src, std::size_t index, bool sret) = 0;
    virtual void call(symbols& sym, std::string& label) = 0;
    virtual void collect_ret(symbols& sym, std::size_t pos, std::size_t size, bool sret) = 0;
    virtual void load(symbols& sym, std::size_t pos) = 0;
    virtual void store(symbols& sym, std::size_t size, bool lea, std::string& src, std::size_t pos) = 0;
    virtual void store_ptr(symbols& sym, std::size_t size, std::size_t ptr_pos, bool ptr_lea,
                           bool lea, std::string& src) = 0;
    virtual void sret(symbols& sym, std::size_t pos) = 0;
    virtual void ret_int(symbols& sym, std::string& data, std::size_t id) = 0;
    virtual void save_sret(symbols& sym, std::size_t pos) = 0;
    virtual void ret_value(symbols& sym, std::size_t pos, std::size_t size, bool tuple, std::size_t id) = 0;
    virtual void param(symbols& sym, std::size_t index, std::size_t pos, std::size_t size, bool sret, bool tuple) = 0;
    virtual void deref(symbols& sym, std::size_t src_pos, bool via_lea, std::size_t size, std::size_t dst_pos) = 0;
    virtual void string_ptr(symbols& sym, const std::string& bytes, std::size_t pos) = 0;
    virtual void cast(symbols& sym, std::size_t src_pos, std::size_t src_size, bool src_signed,
                      std::size_t dst_pos, std::size_t dst_size) = 0;
    virtual void copy_bytes(symbols& sym, std::size_t src_pos, std::size_t size, std::size_t dst_pos) = 0;

    virtual void label(const std::string& name) = 0;
    virtual void jump(const std::string& name) = 0;
    virtual void branch(std::size_t pos, std::size_t size, bool nonzero, const std::string& name) = 0;
    virtual void binary(symbols& sym, const std::string& op, std::size_t size,
                        bool a_imm, const std::string& a,
                        bool b_imm, const std::string& b,
                        std::size_t dst_pos, std::size_t dst_size) = 0;

    virtual void syscall(symbols& sym, const std::string& number,
                         const std::vector<syscall_arg>& args, std::size_t pos) {
        throw std::runtime_error("syscall not supported by this backend");
    }
};
