#pragma once

#include "./runa.hpp"
#include "../parser/types.hpp"
#include "../symbols.hpp"
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <regex>
#include <string>
#include <utility>
#include <variant>

struct codegen {
    inline static std::ofstream stream;
    inline static std::size_t tabs = 0;

    static void writeln(Runa *runa) {
        RunaValueFFI arg = runa_peek_arg(runa, 0);
        char *str = runa_value_to_string(runa, arg);
        stream << (tabs == 0 ? "" : "\t") << std::string(str) << "\n";
        runa_optional(RUNA_FREE_STRING_BY_VALUE, runa_str_free, str, arg);
        runa_value_free(arg);
    }

    static void _tabs(Runa *runa) {
        RunaValueFFI arg = runa_peek_arg(runa, 0);
        tabs = arg.data.integer;
    }

    static void emptylines(Runa *runa) {
        RunaValueFFI arg = runa_peek_arg(runa, 0);
        for(int i = 0; i < arg.data.integer; i++) stream << "\n";
    }

    Runa *runa;
    size_t FUNC = 0;
    std::string current_function;
    std::streampos stack_position;

    void execute(void (*data)(Runa*), Runa* runa) {
        data(runa);
    }

    codegen(std::string output, std::string extensor)
    :   runa(runa_start())
    {   runa_loadfile(runa, (char*) extensor.c_str());
        runa_push_function(runa, (char*) "writeln", writeln, 1);
        runa_push_function(runa, (char*) "emptylines", emptylines, 1);
        runa_push_function(runa, (char*) "tabs", _tabs, 1);

        codegen::stream = std::ofstream(output);
        types::BASE_ALIGNMENT = runa_spawn_function(runa, (char*) "align", [](Runa*){}).data.integer;
    }

    std::pair<bool, std::string> _load_data(symbols sym, std::string value, types::type type) {
        bool lea = false;
        std::string src = "unknown";

        std::regex number("\\d*");
        if( std::regex_match(value, number) ) {
            return { lea, "$" + value };
        }

        auto symbol = sym.lookup(value);
        if(! symbol.has_value() ) CompilerOutputs::fatal("Invalid data informed");

        size_t pos;
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

    void extern_fn(std::string name) {
        pcap(runa, { { "identifier", name } });
        runa_spawn_function(runa, (char*) "extern", generic_cap);
    }

    void prologue(std::string name) {
        pcap(runa, { { "identifier", name }, { "id", FUNC } });
        current_function = name;
        runa_spawn_function(runa, (char*) "prologue", generic_cap);
    }

    void epilogue() {
        pcap(runa, { { "identifier", current_function }, { "id", FUNC++ } });
        runa_spawn_function(runa, (char*) "epilogue", generic_cap);
    }

    void argument(symbols& sym, subroutine& sub, std::string arg, size_t i) {
        auto [lea, src] = _load_data(sym, arg, sub.args[i]);
        pcap(runa, { { "lea", lea }, { "src", src }, { "index", i } });
        runa_spawn_function(runa, (char*) "arguments", generic_cap);
    }

    void call(std::string name) {
        pcap(runa, { { "label", name } });
        runa_spawn_function(runa, (char*) "call", generic_cap);
    }

    void collect_ret(size_t pos) {
        pcap(runa, { { "pos", pos } });
        runa_spawn_function(runa, (char*) "collect_ret", generic_cap);
    }

    void load(size_t pos) {
        pcap(runa, { { "pos", pos } });
        runa_spawn_function(runa, (char*) "load", generic_cap);
    }

    void store(symbols& sym, allocation alloc, std::string store) {
        auto [lea, src] = _load_data(sym, store, alloc.allocated);
        pcap(runa, { { "size", alloc.allocated.size } });
        std::string ax(runa_spawn_function(runa, (char*) "rax", generic_cap).data.string);

        pcap(runa, { { "size", alloc.allocated.size } });
        std::string mov(runa_spawn_function(runa, (char*) "mov", generic_cap).data.string);

        pcap(runa, { { "lea", lea }, { "src", src }, { "pos", alloc.pos }, { "register", ax }, { "mov", mov } });
        runa_spawn_function(runa, (char*) "store", generic_cap);
    }

    ~codegen() {
        stream.close();
    }
};
