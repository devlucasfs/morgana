#pragma once

#include "../symbols.hpp"
#include "./factory.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define backend_void(name, args, ...) \
    void name args { backend->name(__VA_ARGS__); }

#define backend_ret(type, name, ...) \
    type name args { return backend->name(__VA_ARGS__); }

enum arch { NARCH, X86, X86_64, ARM, AARCH64, RISCV32, RISCV64, AVRATMEGA328P };
enum os { NOS, WINDOWS, MACOS, GNU, BSD };

struct target_t {
    arch architecture;
    os convention;
    std::string output;

    target_t() = default;
};

struct reg_loc {
    bool          io   = false;
    std::string   name;
    std::uint64_t addr = 0;
    std::size_t   bits = 0;
};

class assembly_backend {
public:
    static constexpr const char* memory_target = ":memory:";

    static bool is_memory_target(const std::string& output) {
        return output == memory_target;
    }

protected:
    std::ofstream      file;
    std::ostringstream memory;
    std::ostream&      stream;
    std::size_t tabs = 0;

public:
    explicit assembly_backend(std::string output)
        : file(is_memory_target(output) ? std::ofstream() : std::ofstream(output)),
          memory(),
          stream(is_memory_target(output) ? static_cast<std::ostream&>(memory)
                                          : static_cast<std::ostream&>(file)) {}

    static std::size_t next_unique_id() {
        static std::size_t counter = 0;
        return counter++;
    }

    virtual ~assembly_backend() = default;

    assembly_backend(assembly_backend&) = delete;
    assembly_backend& operator=(assembly_backend&) = delete;

    std::string take_text() { return memory.str(); }

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

    virtual void function_address(symbols& sym, const std::string& name, std::size_t pos) {
        throw std::runtime_error("function pointers not supported by this backend");
    }

    virtual void call_indirect(symbols& sym, bool lea, const std::string& src) {
        throw std::runtime_error("call_indirect not supported by this backend");
    }

    virtual void loc_load(symbols& sym, const reg_loc& r, std::size_t pos) {
        throw std::runtime_error("register / io access not supported by this backend");
    }

    virtual void loc_store(symbols& sym, const reg_loc& r, const syscall_arg& value) {
        throw std::runtime_error("register / io access not supported by this backend");
    }

    virtual void loc_get(symbols& sym, const reg_loc& r, std::size_t lo, std::size_t width,
                         std::size_t dst_pos, std::size_t dst_size) {
        throw std::runtime_error("register / io access not supported by this backend");
    }

    virtual void loc_set(symbols& sym, const reg_loc& r, std::size_t lo, std::size_t width,
                         const syscall_arg& value) {
        throw std::runtime_error("register / io access not supported by this backend");
    }
};
