#pragma once

#include <cassert>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>
#include <filesystem>

#include "lexer.hpp"
#include "../errors.hpp"

#define idek 68
#define TYPE_SET(E)                                      \
E(mi8,     "i8",  true,  1) E(mu8,   "u8",    false,  1) \
E(mi16,    "i16", true,  2) E(mu16,  "u16",   false,  2) \
E(mi32,    "i32", true,  4) E(mu32,  "u32",   false,  4) \
E(mi64,    "i64", true,  8) E(mu64,  "u64",   false,  8) \
E(mint,    "int", true, -1) E(uint,  "uint",  false, -1) \
E(mptr,    "ptr", idek, -1) E(mvoid, "void",  idek,   0) \
E(unknown, "",    idek,  0)

#define X(m, ...) m,
enum class morgana_type { TYPE_SET(X) mtuple };
#undef X

struct morgana_tuple {
    std::vector<std::pair<morgana_type, morgana_tuple>> elements;
};

struct Ty {
    morgana_type kind = morgana_type::unknown;
    morgana_tuple tuple;
};


#define default_struct(id) id { id() = default; ~id() = default; }
#define deconstructor(id) ~id() = default;

struct default_struct(epilogue);
struct prologue {
    deconstructor(prologue)
    std::string identifier;
    std::vector<Ty> params;
    prologue(std::string identifier, std::vector<Ty> params)
        : identifier(identifier), params(params) {};
};

#define INSTRUCTION_SET(E) \
E(prologue) E(epilogue)

#define X(id) , id
using Instruction = std::variant<std::monostate INSTRUCTION_SET(X)>;
#undef X

#define X(id) using id##_t = id;
INSTRUCTION_SET(X)
#undef X

struct Ins {
    std::vector<Instruction> set;
};

#define HELPERS_ARGS std::size_t& i, std::vector<Tk> tks, Ins& ins
#define helper(name) bool name(HELPERS_ARGS)
#define helperT(T, name) std::pair<bool, T> name(HELPERS_ARGS)
#define helperS(name) bool name(std::filesystem::path& path, HELPERS_ARGS)

helperT(Ty, types) {
    auto _default = std::make_pair(false, (Ty) {});

    if( tks.at(i).lex == "{" ) {
        Ty result;
        result.kind = morgana_type::mtuple;

        ++i;

        while(i < tks.size()) {
            auto [valid, ty] = types(i, tks, ins);
            if(! valid ) return _default;

            result.tuple.elements.push_back({
                ty.kind,
                std::move(ty.tuple)
            });

            if( tks.at(i + 1).lex == "}" ) {
                ++i;
                return _default;
            }

            if( tks.at(i + 1).lex != "," ) return _default;

            i += 2;
        }

        return _default;
    }

    i += 1;

    #define X(m, str, ...) \
    if( tks.at(i-1).lex == str ) return std::make_pair(true, (Ty) { morgana_type::m, {} });
    TYPE_SET(X)
    #undef X

    i -= 1;
    return _default;
}

helper(instructions) {
    return false;
}

helper(definition) {
    return false;
}

helperS(function_def) {
    auto [ valid_type, ty ] = types(i, tks, ins);
    if(! valid_type ) return false;

    auto tk = tks.at(i++);
    if( tk.kind != TkKind::Identifier )
        syntax_error(path, tk, "Expected the function name as the second token in the function declaration.");

    auto identifier = tk.lex;
    tk = tks.at(i++);

    auto params_error = [&](std::string found) {
        syntax_error(path, tk, "Expected '(' followed by explicitly typed parameters in the function declaration, "
                     "or ')' if no parameters are provided; Was found `" + found + "`");
    };

    std::vector<Ty> params;
    if( tk.lex != "(" ) params_error(tk.lex);
    while( (tk = tks.at(i++)).lex != ")" ) {
        if( params.size() > 0 && tk.lex != "," ) params_error(tk.lex);
        else if( params.size() > 0 ) tk = tks.at(i++);

        i--; auto [ valid_type, ty ] = types(i, tks, ins);
        if(! valid_type ) params_error(tk.lex);
        params.push_back(ty);
    }

    tk = tks.at(i++);
    if( tk.lex != "{" )
        syntax_error(path, tk, "Expected '{' to the function block; Was found `" + tk.lex + "`");

    ins.set.push_back(prologue(identifier, std::move(params)));
    return true;
}

Ins parser(std::filesystem::path path, std::vector<Tk> tks) {
    Ins ins;
    for( std::size_t i = 0; i < tks.size(); i++ ) {
        auto tk = tks.at(i);
        switch(tk.kind) {
        case TkKind::Identifier: {
            if( function_def(path, i, tks, ins) ) continue;
        } break;

        case TkKind::Unknown: {
            if( tk.lex == "}" ) ins.set.push_back(epilogue());
        } break;

        default: if( tk.lex != "<eof>" )
            syntax_error(path, tk, "Unknown token found here, this should be unreachable: ");
        }
    }
    return ins;
}
