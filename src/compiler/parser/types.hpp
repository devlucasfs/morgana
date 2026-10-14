#pragma once
#include "../lexer.hpp"
#include "../hash.hpp"
#include "../../compiler_outputs.hpp"
#include <optional>
#include <string>
#include <vector>
#include <algorithm>

#define MTYPES                         \
    X(mi8) X(mi16) X(mi32) X(mi64) \
    X(mu8) X(mu16) X(mu32) X(mu64) \
    X(mvoid) X(mptr) X(mtuple)

struct types {
    using type = types;

    inline static size_t BASE_ALIGNMENT = 8;

    #define X(id) id,
    enum kinds { MTYPES } kind;
    #undef X

    std::string kto_string() {
        #define X(id) case id: return #id;
        switch(kind) { MTYPES }
        #undef X
    }

    size_t alignment = 1;
    size_t size = 0;

    std::optional<std::vector<types>> tuple;

    types(std::vector<types> vec)
        : tuple(std::move(vec)), kind(mtuple) {
        recalculate();
    }

    types() = default;

    types(kinds k) : kind(k) {
        recalculate();
    }

    void recalculate() {
        if( kind == mtuple ) {
            size_t offset = 0;
            alignment = 1;

            for( auto& field : *tuple ) {
                field.recalculate();
                offset = (offset + field.alignment - 1) / field.alignment * field.alignment;
                offset += field.size;
                alignment = std::max(
                    alignment,
                    field.alignment
                );
            }

            size = (offset + alignment - 1) / alignment * alignment;
            return;
        }

        switch(kind) {
            case mi8:
            case mu8: size = 1;
            break;

            case mi16:
            case mu16: size = 2;
            break;

            case mi32:
            case mu32: size = 4;
            break;

            case mi64:
            case mu64: size = 8;
            break;

            case mptr: size = BASE_ALIGNMENT;
            break;

            case mvoid: size = 0;
            break;

            case mtuple: break;
        }

        alignment = size == 0
            ? 1
            : std::min(size, BASE_ALIGNMENT);
    }

    static std::vector<types> get_sequence(
        Lexer& lex,
        std::string end
    ) {
        std::vector<types> t;

        while( auto data = lex.next() ) {
            if( *data == end ) break;
            if( *data == "," ) continue;

            lex.back();
            t.push_back(types::get_from(lex));
        }

        return t;
    }

    static types get_from(Lexer& lex) {
        auto tk = lex.next();

        if( !tk ) return types(kinds::mvoid);

        if( *tk == "{" ) {
            return types(get_sequence(lex, "}"));
        }

        switch(hash(*tk)) {
            case hash("i8"):   return types(kinds::mi8);
            case hash("u8"):   return types(kinds::mu8);
            case hash("ptr"):  return types(kinds::mptr);
            case hash("i16"):  return types(kinds::mi16);
            case hash("u16"):  return types(kinds::mu16);
            case hash("i32"):  return types(kinds::mi32);
            case hash("u32"):  return types(kinds::mu32);
            case hash("i64"):  return types(kinds::mi64);
            case hash("u64"):  return types(kinds::mu64);
            case hash("void"): return types(kinds::mvoid);
            default: CompilerOutputs::fatal("Unknown type " + *tk);
        }

        return types(kinds::mvoid);
    }
};
