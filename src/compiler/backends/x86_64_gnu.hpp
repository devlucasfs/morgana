#pragma once

#include "../codegen/backends.hpp"
#include "factory.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unordered_map>

class x86_64_gnu : public assembly_backend {
protected:
    static constexpr std::size_t arg_register_count = 6;

    std::unordered_map<std::string, std::string> strings;
    std::size_t stack_args = 0;
    std::size_t sret_slot = 0;
    std::size_t pending_cleanup = 0;
    std::size_t frame = 0;

    static std::string generate_position(std::size_t pos) {
        return "-" + std::to_string(pos) + "(%rbp)";
    }

    static std::string block_label(const std::string& name) {
        return ".Lb_" + name;
    }

    static const char* accumulator(std::size_t size) {
        switch( size ) {
            case 1: return "%al";
            case 2: return "%ax";
            case 4: return "%eax";
            case 8: return "%rax";
            default:
                throw std::runtime_error(
                    "Unsupported operand size: " + std::to_string(size)
                );
        }
    }

    static const char* move_instruction(std::size_t size) {
        switch( size ) {
            case 1: return "movb";
            case 2: return "movw";
            case 4: return "movl";
            case 8: return "movq";
            default:
                throw std::runtime_error(
                    "Unsupported operand size: " + std::to_string(size)
                );
        }
    }

    static bool is_signed_op(const std::string& op) {
        return op == "sdiv" || op == "srem"
            || op == "slt" || op == "sle" || op == "sgt" || op == "sge";
    }

    static long long immediate_value(const std::string& src, std::size_t size, bool sign) {
        const std::string digits = src.substr(1);   // drop '$'
        long long v = (!digits.empty() && digits[0] == '-')
            ? std::strtoll(digits.c_str(), nullptr, 10)
            : static_cast<long long>(std::strtoull(digits.c_str(), nullptr, 10));

        switch( size ) {
            case 1: return sign ? static_cast<long long>(static_cast<std::int8_t>(v))
                                : static_cast<long long>(static_cast<std::uint8_t>(v));
            case 2: return sign ? static_cast<long long>(static_cast<std::int16_t>(v))
                                : static_cast<long long>(static_cast<std::uint16_t>(v));
            case 4: return sign ? static_cast<long long>(static_cast<std::int32_t>(v))
                                : static_cast<long long>(static_cast<std::uint32_t>(v));
            default: return v;
        }
    }

    void load_operand(bool imm, const std::string& src, std::size_t size, bool sign,
                      const char* r64, const char* r32) {
        if( imm ) {
            const long long v = immediate_value(src, size, sign);

            if( v >= INT32_MIN && v <= INT32_MAX )
                writeln("movq $" + std::to_string(v) + ", " + r64);
            else
                writeln("movabsq $" + std::to_string(v) + ", " + r64);
            return;
        }

        switch( size ) {
            case 1: writeln(std::string(sign ? "movsbq " : "movzbq ") + src + ", " + r64); break;
            case 2: writeln(std::string(sign ? "movswq " : "movzwq ") + src + ", " + r64); break;
            case 4:
                if( sign ) writeln("movslq " + src + ", " + r64);
                else       writeln("movl "   + src + ", " + r32);
                break;
            case 8: writeln("movq " + src + ", " + r64); break;
            default:
                throw std::runtime_error("Unsupported operand size: " + std::to_string(size));
        }
    }

    void copy_from_r11(std::size_t dst_pos, std::size_t size) {
        std::size_t off = 0;

        while( off < size ) {
            const std::size_t left  = size - off;
            const std::size_t chunk = left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1;

            writeln(
                std::string(move_instruction(chunk)) + " " + std::to_string(off)
                + "(%r11), " + accumulator(chunk)
            );
            writeln(
                std::string(move_instruction(chunk)) + " " + accumulator(chunk)
                + ", " + generate_position(dst_pos - off)
            );

            off += chunk;
        }
    }

public:
    explicit x86_64_gnu(const std::string& output) : assembly_backend(output) {}

    inline static constexpr const char* registers[arg_register_count] = {
        "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"
    };

    static const char* param_register(std::size_t index, std::size_t size) {
        static constexpr const char* table[arg_register_count][4] = {
            { "%dil", "%di",  "%edi",  "%rdi" },
            { "%sil", "%si",  "%esi",  "%rsi" },
            { "%dl",  "%dx",  "%edx",  "%rdx" },
            { "%cl",  "%cx",  "%ecx",  "%rcx" },
            { "%r8b", "%r8w", "%r8d",  "%r8"  },
            { "%r9b", "%r9w", "%r9d",  "%r9"  },
        };

        switch( size ) {
            case 1: return table[index][0];
            case 2: return table[index][1];
            case 4: return table[index][2];
            case 8: return table[index][3];
            default:
                throw std::runtime_error(
                    "Unsupported operand size: " + std::to_string(size)
                );
        }
    }

    std::size_t alignment() override { return 16; }

    std::size_t align_stack(std::size_t size) {
        const std::size_t a = alignment();
        return (size + a - 1) & ~(a - 1);
    }

    void extern_fn(symbols& sym, std::string& name) override {
        writeln(".extern " + name);
    }

    void prologue(symbols& sym, std::string& name, std::size_t id) override {
        stack_args = pending_cleanup = 0;
        frame = 0;

        emptylines(1);
        writeln(".globl " + name);
        writeln(".type " + name + ", @function");
        writeln(name + ":");
        writeln(".LFP" + std::to_string(id) + ":");

        set_tabs(1);

        if( name == "_start" ) {
            writeln("xorl %ebp, %ebp");
            writeln("andq $-16, %rsp");
            writeln("movq %rsp, %rbp");
        } else {
            writeln("pushq %rbp");
            writeln("movq %rsp, %rbp");
        }

        writeln("subq $.LFRAME" + std::to_string(id) + ", %rsp");
    }

    void epilogue(symbols& sym, std::string& name, std::size_t id) override {
        set_tabs(1);
        writeln(".set .LFRAME" + std::to_string(id) + ", " + std::to_string(frame));

        set_tabs(0);
        writeln(".LFE" + std::to_string(id) + ":");
        set_tabs(1);

        if( name == "_start" ) {
            writeln("ud2");
        } else {
            writeln("leave");
            writeln("ret");
        }

        set_tabs(0);
        writeln(".size " + name + ", . - " + name);
    }

    // A tuple argument is passed by pointer: the caller must call this with `lea = true`
    // and `src` being the tuple's stack slot, so the callee receives its address.
    void argument(symbols& sym, bool lea, std::string& src, std::size_t index, bool sret) override {
        if( sret ) index++;

        if( index < arg_register_count ) {
            writeln(
                std::string(lea ? "leaq " : "movq ")
                + src + ", " + registers[index]
            );
            return;
        }

        if( stack_args == 0 ) {
            const std::size_t total = index - arg_register_count + 1;
            if( total % 2 != 0 ) {
                writeln("subq $8, %rsp");
                pending_cleanup += 8;
            }
        }

        if( lea ) {
            writeln("leaq " + src + ", %rax");
            writeln("pushq %rax");
        } else {
            writeln("pushq " + src);
        }

        stack_args++;
        pending_cleanup += 8;
    }

    void call(symbols& sym, std::string& label) override {
        writeln("call " + label);

        if( pending_cleanup != 0 ) {
            writeln("addq $" + std::to_string(pending_cleanup) + ", %rsp");
        }
        stack_args = pending_cleanup = 0;
    }

    void collect_ret(symbols& sym, std::size_t pos, std::size_t size, bool sret) override {
        if( sret ) return;

        writeln(
            std::string(move_instruction(size)) + " "
            + accumulator(size) + ", " + generate_position(pos)
        );
    }

    void load(symbols& sym, std::size_t pos) override {
        writeln("movq " + generate_position(pos) + ", %rax");
    }

    void store(symbols& sym, std::size_t size, bool lea, std::string& src, std::size_t pos) override {
        const std::string dst = generate_position(pos);

        if( lea ) {
            writeln("leaq " + src + ", %rax");
            writeln("movq %rax, " + dst);
            return;
        }

        const char* mov = move_instruction(size);
        const char* reg = accumulator(size);

        writeln(std::string(mov) + " " + src + ", " + reg);
        writeln(std::string(mov) + " " + reg + ", " + dst);
    }

    void store_ptr(symbols& sym, std::size_t size, std::size_t ptr_pos, bool ptr_lea,
                   bool lea, std::string& src) override {
        writeln(std::string(ptr_lea ? "leaq " : "movq ") + generate_position(ptr_pos) + ", %rcx");

        if( lea ) {
            writeln("leaq " + src + ", %rax");
            writeln("movq %rax, (%rcx)");
            return;
        }

        const char* mov = move_instruction(size);
        const char* reg = accumulator(size);

        writeln(std::string(mov) + " " + src + ", " + reg);
        writeln(std::string(mov) + " " + reg + ", (%rcx)");
    }

    void sret(symbols& sym, std::size_t pos) override {
        writeln("leaq " + generate_position(pos) + ", %rdi");
    }

    void save_sret(symbols& sym, std::size_t pos) override {
        sret_slot = pos;
        writeln("movq %rdi, " + generate_position(pos));
    }

    void ret_value(symbols& sym, std::size_t pos, std::size_t size, bool tuple, std::size_t id) override {
        if( tuple ) {
            if( sret_slot == 0 )
                throw std::runtime_error("ret of a tuple in a function without an sret slot");

            writeln("movq " + generate_position(sret_slot) + ", %rdi");
            writeln("leaq " + generate_position(pos) + ", %rsi");
            writeln("movq $" + std::to_string(size) + ", %rcx");
            writeln("rep movsb");

            writeln("movq " + generate_position(sret_slot) + ", %rax");
        } else {
            writeln(
                std::string(move_instruction(size)) + " "
                + generate_position(pos) + ", " + accumulator(size)
            );
        }

        writeln("jmp .LFE" + std::to_string(id));
    }

    void param(symbols& sym, std::size_t index, std::size_t pos,
               std::size_t size, bool sret, bool tuple) override {
        if( sret ) index++;

        if( tuple ) {
            if( index < arg_register_count )
                writeln(std::string("movq ") + registers[index] + ", %r11");
            else
                writeln("movq " + std::to_string(16 + 8 * (index - arg_register_count)) + "(%rbp), %r11");

            copy_from_r11(pos, size);
            return;
        }

        const std::string dst = generate_position(pos);

        if( index < arg_register_count ) {
            writeln(
                std::string(move_instruction(size)) + " "
                + param_register(index, size) + ", " + dst
            );
            return;
        }

        const std::size_t offset = 16 + 8 * (index - arg_register_count);

        writeln("movq " + std::to_string(offset) + "(%rbp), %rax");
        writeln(
            std::string(move_instruction(size)) + " "
            + accumulator(size) + ", " + dst
        );
    }

    void alloca(std::size_t size) override {
        frame += align_stack(size);
    }

    void copy_bytes(symbols& sym, std::size_t src_pos, std::size_t size, std::size_t dst_pos) override {
        writeln("leaq " + generate_position(src_pos) + ", %rsi");
        writeln("leaq " + generate_position(dst_pos) + ", %rdi");
        writeln("movq $" + std::to_string(size) + ", %rcx");
        writeln("rep movsb");
    }

    void ret_int(symbols& sym, std::string& data, std::size_t id) override {
        writeln("movq $" + data + ", %rax");
        writeln("jmp .LFE" + std::to_string(id));
    }

    void syscall(symbols& sym, const std::string& number, const std::vector<syscall_arg>& args, std::size_t pos) override {
        static constexpr const char* r64[6] = { "%rdi", "%rsi", "%rdx", "%r10", "%r8",  "%r9"  };
        static constexpr const char* r32[6] = { "%edi", "%esi", "%edx", "%r10d", "%r8d", "%r9d" };

        if( args.size() > 6 )
            throw std::runtime_error("syscall takes at most 6 arguments");

        for( std::size_t i = 0; i < args.size(); i++ ) {
            const auto& a = args[i];

            if( a.lea )      { writeln("leaq " + a.src + ", " + r64[i]); continue; }
            if( a.imm )      { writeln("movq " + a.src + ", " + r64[i]); continue; }

            switch( a.size ) {
                case 8: writeln("movq "   + a.src + ", " + r64[i]); break;
                case 4: writeln("movl "   + a.src + ", " + r32[i]); break;
                case 2: writeln("movzwl " + a.src + ", " + r32[i]); break;
                case 1: writeln("movzbl " + a.src + ", " + r32[i]); break;
                default:
                    throw std::runtime_error("Unsupported operand size: " + std::to_string(a.size));
            }
        }

        writeln("movq $" + number + ", %rax");
        writeln("syscall");
        writeln("movq %rax, " + generate_position(pos));
    }

    void deref(symbols& sym, std::size_t src_pos, bool via_lea,
               std::size_t size, std::size_t dst_pos) override {
        writeln(std::string(via_lea ? "leaq " : "movq ") + generate_position(src_pos) + ", %rax");

        if( size == 1 || size == 2 || size == 4 || size == 8 ) {
            writeln(
                std::string(move_instruction(size)) + " (%rax), " + accumulator(size)
            );
            writeln(
                std::string(move_instruction(size)) + " " + accumulator(size)
                + ", " + generate_position(dst_pos)
            );
            return;
        }

        writeln("movq %rax, %rsi");
        writeln("leaq " + generate_position(dst_pos) + ", %rdi");
        writeln("movq $" + std::to_string(size) + ", %rcx");
        writeln("rep movsb");
    }

    void string_ptr(symbols& sym, const std::string& bytes, std::size_t pos) override {
        auto it = strings.find(bytes);

        if( it == strings.end() ) {
            const std::string label = ".Lstr" + std::to_string(strings.size());
            it = strings.emplace(bytes, label).first;

            const bool mergeable = bytes.find('\0') == std::string::npos;
            writeln(mergeable
                ? ".pushsection .rodata.str1.1,\"aMS\",@progbits,1"
                : ".pushsection .rodata");
            writeln(label + ":");

            for( std::size_t i = 0; i <= bytes.size(); i += 16 ) {
                std::string line = ".byte ";
                for( std::size_t j = i; j < i + 16 && j <= bytes.size(); j++ ) {
                    unsigned v = j < bytes.size() ? static_cast<unsigned char>(bytes[j]) : 0;
                    if( j != i ) line += ", ";
                    line += std::to_string(v);
                }
                writeln(line);
            }

            writeln(".popsection");
        }

        writeln("leaq " + it->second + "(%rip), %rax");
        writeln("movq %rax, " + generate_position(pos));
    }

    void label(const std::string& name) override {
        set_tabs(0);
        writeln(block_label(name) + ":");
        set_tabs(1);
    }

    void jump(const std::string& name) override {
        writeln("jmp " + block_label(name));
    }

    void branch(std::size_t pos, std::size_t size, bool nonzero, const std::string& name) override {
        const char* acc = accumulator(size);

        writeln(std::string(move_instruction(size)) + " " + generate_position(pos) + ", " + acc);
        writeln(std::string("test ") + acc + ", " + acc);
        writeln(std::string(nonzero ? "jnz " : "jz ") + block_label(name));
    }

    void binary(symbols& sym, const std::string& op, std::size_t size,
                bool a_imm, const std::string& a,
                bool b_imm, const std::string& b,
                std::size_t dst_pos, std::size_t dst_size) override {
        const bool sign = is_signed_op(op);

        load_operand(a_imm, a, size, sign, "%rax", "%eax");
        load_operand(b_imm, b, size, sign, "%rcx", "%ecx");

        if( op == "add" )       writeln("addq %rcx, %rax");
        else if( op == "sub" )  writeln("subq %rcx, %rax");
        else if( op == "mul" )  writeln("imulq %rcx, %rax");
        else if( op == "and" )  writeln("andq %rcx, %rax");
        else if( op == "or" )   writeln("orq %rcx, %rax");
        else if( op == "sdiv" || op == "srem" ) {
            writeln("cqto");
            writeln("idivq %rcx");
            if( op == "srem" ) writeln("movq %rdx, %rax");
        }
        else if( op == "udiv" || op == "urem" ) {
            writeln("xorl %edx, %edx");
            writeln("divq %rcx");
            if( op == "urem" ) writeln("movq %rdx, %rax");
        }
        else {
            const char* set = nullptr;

            if( op == "eq" )       set = "sete";
            else if( op == "ne" )  set = "setne";
            else if( op == "slt" ) set = "setl";
            else if( op == "sle" ) set = "setle";
            else if( op == "sgt" ) set = "setg";
            else if( op == "sge" ) set = "setge";
            else if( op == "ult" ) set = "setb";
            else if( op == "ule" ) set = "setbe";
            else if( op == "ugt" ) set = "seta";
            else if( op == "uge" ) set = "setae";
            else throw std::runtime_error("Unknown binary operation '" + op + "'");

            writeln("cmpq %rcx, %rax");
            writeln(std::string(set) + " %al");
        }

        writeln(
            std::string(move_instruction(dst_size)) + " "
            + accumulator(dst_size) + ", " + generate_position(dst_pos)
        );
    }

    void cast(symbols& sym, std::size_t src_pos, std::size_t src_size, bool src_signed,
              std::size_t dst_pos, std::size_t dst_size) override {
        load_operand(false, generate_position(src_pos), src_size, src_signed, "%rax", "%eax");
        writeln(
            std::string(move_instruction(dst_size)) + " "
            + accumulator(dst_size) + ", " + generate_position(dst_pos)
        );
    }
};
