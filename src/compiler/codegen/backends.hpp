#pragma once

#include "../symbols.hpp"

#include <cstddef>
#include <fstream>
#include <string>

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
    virtual void alloca(std::size_t size) = 0;
    virtual void prologue(symbols& sym, std::string& name, std::size_t id) = 0;
    virtual void epilogue(symbols& sym, std::string& name, std::size_t id) = 0;
    virtual void argument(symbols& sym, bool lea, std::string& src, std::size_t index, bool sret) = 0;
    virtual void call(symbols& sym, std::string& label) = 0;
    virtual void collect_ret(symbols& sym, std::size_t pos, std::size_t size, bool sret) = 0;
    virtual void load(symbols& sym, std::size_t pos) = 0;
    virtual void store(symbols& sym, std::size_t size, bool lea, std::string& src, std::size_t pos) = 0;
    virtual void sret(symbols& sym, std::size_t pos) = 0;
    virtual void ret_int(symbols& sym, std::string& data, std::size_t id) = 0;
};
