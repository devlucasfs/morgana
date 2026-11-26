#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "lexer.hpp"
#include "../errors.hpp"

// ═════════════════════════════════════════════════════════════════════════════
//  Types
// ═════════════════════════════════════════════════════════════════════════════

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
// mvec: a SIMD register value (v64 / v128 / v256 / v512). It only comes out of
// `vreg_load` / `vreg_get` and only goes into `vreg_store` / `vreg_set`.
enum class morgana_type { TYPE_SET(X) mtuple, mvec };
#undef X

struct morgana_tuple {
    std::vector<std::pair<morgana_type, morgana_tuple>> elements;
};

struct Ty {
    morgana_type  kind = morgana_type::unknown;
    morgana_tuple tuple;
    std::size_t   bits = 0;   // mvec only
};

inline Ty make_ty(morgana_type k, morgana_tuple t = {}) {
    Ty r;
    r.kind  = k;
    r.tuple = std::move(t);
    return r;
}

inline Ty make_vec(std::size_t bits) {
    Ty r;
    r.kind = morgana_type::mvec;
    r.bits = bits;
    return r;
}

inline bool is_tuple(const Ty& t) { return t.kind == morgana_type::mtuple; }
inline bool is_void(const Ty& t)  { return t.kind == morgana_type::mvoid; }
inline bool is_ptr(const Ty& t)   { return t.kind == morgana_type::mptr; }
inline bool is_vec(const Ty& t)   { return t.kind == morgana_type::mvec; }

// `int` / `uint` / `ptr` have a size that depends on the target: only the codegen knows it.
inline std::optional<std::size_t> fixed_size(morgana_type k) {
    #define X(m, str, sgn, size) \
        if( k == morgana_type::m && (size) > 0 ) return static_cast<std::size_t>(size);
    TYPE_SET(X)
    #undef X
    return std::nullopt;
}

inline bool is_integer(morgana_type k) {
    #define X(m, str, sgn, size) if( k == morgana_type::m ) return int(sgn) == 0 || int(sgn) == 1;
    TYPE_SET(X)
    #undef X
    return false;
}

inline bool is_64bit_integer(morgana_type k) {
    return k == morgana_type::mi64 || k == morgana_type::mu64
        || k == morgana_type::mint || k == morgana_type::uint;
}

inline bool same_type(const Ty& a, const Ty& b);

inline bool same_tuple(const morgana_tuple& a, const morgana_tuple& b) {
    if( a.elements.size() != b.elements.size() ) return false;

    for( std::size_t i = 0; i < a.elements.size(); i++ ) {
        if( a.elements[i].first != b.elements[i].first ) return false;
        if( a.elements[i].first == morgana_type::mtuple
            && !same_tuple(a.elements[i].second, b.elements[i].second) ) return false;
    }
    return true;
}

inline bool same_type(const Ty& a, const Ty& b) {
    return a.kind == b.kind
        && (a.kind != morgana_type::mtuple || same_tuple(a.tuple, b.tuple))
        && (a.kind != morgana_type::mvec   || a.bits == b.bits);
}

inline std::string type_name(const Ty& t);

inline std::string type_name(morgana_type k) {
    #define X(m, str, ...) \
        if( k == morgana_type::m ) return std::string(str).empty() ? "<unknown>" : str;
    TYPE_SET(X)
    #undef X
    return "{...}";
}

inline std::string tuple_name(const morgana_tuple& t) {
    std::string s = "{";
    for( std::size_t i = 0; i < t.elements.size(); i++ ) {
        if( i ) s += ", ";
        s += type_name(make_ty(t.elements[i].first, t.elements[i].second));
    }
    return s + "}";
}

inline std::string type_name(const Ty& t) {
    if( t.kind == morgana_type::mvec ) return "v" + std::to_string(t.bits);
    return t.kind == morgana_type::mtuple ? tuple_name(t.tuple) : type_name(t.kind);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Instructions (what the parser produces and the codegen consumes)
//
//  Names are carried as strings: the parser only knows TYPES, never sizes, offsets,
//  alignment or stack slots. All of that belongs to the codegen.
// ═════════════════════════════════════════════════════════════════════════════

#define default_struct(id) id { id() = default; ~id() = default; }
#define deconstructor(id) ~id() = default;

struct default_struct(epilogue);
struct prologue {
    deconstructor(prologue)
    std::string identifier;
    std::vector<Ty> params;
    Ty ret;
    bool exported = false;
    prologue(Ty ret, std::string identifier, std::vector<Ty> params, bool exported = false)
        : ret(ret), identifier(identifier), params(params), exported(exported) {};
};

struct Operand {
    std::string text;
    bool literal = false;
};

// A register, special register, vector register, mapped io port or memory address.
//   vreg  : SIMD register                     (64, 128, 256, 512 bits)
//   io    : address in the I/O space          (0x00..0x3F on AVR)
//   mem   : address in the DATA space
struct Loc {
    enum class Space { reg, sreg, vreg, io, mem } space = Space::reg;
    std::string   name;
    std::uint64_t addr = 0;
    std::size_t   bits = 0;
};

struct Param {
    std::string name;
    Ty          type;
};

struct extern_op        { std::string name; Ty ret; std::vector<Ty> params; };
struct unfold_op        { std::string name; Ty ret; std::vector<Ty> params; };  // declaration only
struct alloc_op         { std::string dst; Ty type; };
struct ptr_to_op        { std::string dst, target; bool function; };            // function: address of a function
struct move_op          { std::string dst, src; };
struct load_op          { std::string dst, src; };
struct pack_op          { std::string dst; Ty type; std::vector<std::string> elements; };
struct call_op          { std::string dst; Ty ret; std::string callee; std::vector<Operand> args; bool sret; };
struct call_indirect_op { std::string dst; Ty ret; std::string target; bool via_lea; std::vector<Operand> args; bool sret; };
struct deref_op         { std::string dst; Ty type; std::string src; bool via_lea; };
struct store_op         { std::string target; Operand value; };
struct storep_op        { Ty type; std::string ptr; bool via_lea; Operand value; };
struct ret_op           { Operand value; };
struct label_op         { std::string name; };
struct br_op            { std::string target; };
struct branch_op        { std::string cond, target; bool nonzero; };
struct binary_op        { std::string dst, op; Ty operand, result; Operand a, b; };
struct cast_op          { std::string dst; Ty to, from; std::string src; };
struct syscall_op       { std::string dst, number; std::vector<Operand> args; };
struct str_op           { std::string dst, bytes; };
struct loc_load_op      { std::string dst; Loc loc; };
struct loc_store_op     { Loc loc; Operand value; };
struct loc_get_op       { std::string dst; Loc loc; std::size_t lo, width; Ty type; };
struct loc_set_op       { Loc loc; std::size_t lo, width; Operand value; };
struct destroyer_op     { std::vector<Param> args; };
struct extract_op       { std::string dst; Ty type; std::string src; std::size_t index; };

#define INSTRUCTION_SET(E) \
E(prologue) E(epilogue) \
E(extern_op) E(unfold_op) E(alloc_op) E(ptr_to_op) E(move_op) E(load_op) E(pack_op) \
E(call_op) E(call_indirect_op) E(deref_op) E(store_op) E(storep_op) E(ret_op) \
E(label_op) E(br_op) E(branch_op) E(binary_op) E(cast_op) E(syscall_op) E(str_op) \
E(loc_load_op) E(loc_store_op) E(loc_get_op) E(loc_set_op) E(destroyer_op) E(extract_op)

#define X(id) , id
using Instruction = std::variant<std::monostate INSTRUCTION_SET(X)>;
#undef X

#define X(id) using id##_t = id;
INSTRUCTION_SET(X)
#undef X

struct Ins {
    std::vector<Instruction> set;
};

// ═════════════════════════════════════════════════════════════════════════════
//  Symbols: only a TYPE and a category. No positions, no sizes.
// ═════════════════════════════════════════════════════════════════════════════

enum class sym_kind {
    value,       // a value already held somewhere (result of a call, load, cast, ...)
    allocation,  // an alloc'd slot: used as a value it is the address of the slot
    function,    // a defined or extern function   (type = return type)
    unfold,      // a function only declared with `unfold`
};

struct Symbol {
    sym_kind        kind = sym_kind::value;
    Ty              type;     // value / allocation: its type; function / unfold: return type
    std::vector<Ty> params;   // function / unfold only
};

inline Symbol value_symbol(Ty t)      { return Symbol{ sym_kind::value,      std::move(t), {} }; }
inline Symbol allocation_symbol(Ty t) { return Symbol{ sym_kind::allocation, std::move(t), {} }; }

inline bool has_storage(const Symbol& s) {
    return s.kind == sym_kind::value || s.kind == sym_kind::allocation;
}

// does the backend need an `lea` to get the pointer out of it?
inline bool via_lea(const Symbol& s) { return s.kind == sym_kind::allocation; }

// where a symbol is about to be used
enum class Use {
    callee,        // call / call_indirect target of a direct call
    address_of,    // ptr_to
    store_target,  // store
    value,         // anything with storage (ret, move, load, call arguments, vector loc writes, ...)
    scalar,        // value that is not a tuple, vector nor void (operands, conditions, casts, ...)
    pointer,       // value of type ptr (deref, storep, call_indirect)
    element,       // tuple element for `pack`
};

// "" if `s` is valid for `u`; otherwise the end of the sentence "'<name>' ..."
inline std::string invalid_for(const Symbol& s, Use u) {
    switch( u ) {
        case Use::callee:
            if( s.kind == sym_kind::function || s.kind == sym_kind::unfold ) return "";
            return "is not a function";

        case Use::address_of:
            if( s.kind == sym_kind::unfold )
                return "is only declared with `unfold`; define it before taking its address";
            return "";

        case Use::store_target:
            if( s.kind != sym_kind::allocation ) return "is not an allocation";
            return "";

        default: break;
    }

    if(! has_storage(s) ) return "is not a value";

    switch( u ) {
        case Use::scalar:
            if( is_tuple(s.type) ) return "can't be a tuple here";
            if( is_vec(s.type) )   return "can't be a vector here";
            if( is_void(s.type) || s.type.kind == morgana_type::unknown ) return "has no value";
            return "";

        case Use::pointer:
            if(! is_ptr(s.type) ) return "needs to be a pointer, but it is a " + type_name(s.type);
            return "";

        case Use::element:
            if( is_vec(s.type) ) return "can't be a tuple element (vector)";
            if( is_void(s.type) || s.type.kind == morgana_type::unknown )
                return "can't be a tuple element (zero-sized)";
            return "";

        default: return "";
    }
}

class Symbols {
    std::vector<std::unordered_map<std::string, Symbol>> scopes;

public:
    Symbols() { scopes.emplace_back(); }   // global scope

    void push_scope() { scopes.emplace_back(); }

    // false if only the global scope is left (unmatched '}')
    bool pop_scope() {
        if( scopes.size() <= 1 ) return false;
        scopes.pop_back();
        return true;
    }

    std::optional<Symbol> lookup(const std::string& name) const {
        for( auto it = scopes.rbegin(); it != scopes.rend(); ++it ) {
            auto f = it->find(name);
            if( f != it->end() ) return f->second;
        }
        return std::nullopt;
    }

    void push(const std::string& name, Symbol s) { scopes.back()[name] = std::move(s); }

    void erase(const std::string& name) {
        for( auto it = scopes.rbegin(); it != scopes.rend(); ++it )
            if( it->erase(name) ) return;
    }
};

struct Fn {
    Ty              ret;
    std::vector<Ty> params;
};

struct Ctx {
    std::filesystem::path   path;
    const std::vector<Tk>&  tks;
    std::size_t             i = 0;      // next unread token
    Ins&                    ins;
    Symbols                 sym;
    std::optional<Fn>       current;    // function being parsed
    std::string             current_name;

    Ctx(std::filesystem::path p, const std::vector<Tk>& t, Ins& out)
        : path(std::move(p)), tks(t), ins(out) {}

    // never reads past the last token (the lexer ends the stream with `<eof>`)
    const Tk& peek(std::size_t ahead = 0) const { return tks[std::min(i + ahead, tks.size() - 1)]; }
    const Tk& next() { const Tk& tk = peek(); ++i; return tk; }
    bool at_end() const { return i >= tks.size() || peek().lex == "<eof>"; }

    void emit(Instruction op) { ins.set.push_back(std::move(op)); }
};

[[noreturn]] inline void fail(Ctx& c, Tk tk, const std::string& message) {
    syntax_error(c.path, tk, message);
    std::abort();   // syntax_error should not return
}

inline Tk need(Ctx& c, const char* expected) {
    if( c.at_end() )
        fail(c, c.peek(), std::string("Unexpected end of file, expected ") + expected);
    return c.next();
}

// next token must be one of `expected`
inline std::string expect(Ctx& c, std::initializer_list<std::string> expected, std::string extra = "") {
    Tk tk = c.peek();
    if(! c.at_end() ) {
        for( const auto& str : expected ) {
            if( tk.lex == str ) { c.next(); return str; }
        }
    }

    std::string message = "Expected ";
    bool first = true;
    for( const auto& str : expected ) {
        if(! first ) message += " or ";
        message += "'" + str + "'";
        first = false;
    }

    message += " and was received '" + (c.at_end() ? std::string("<end of file>") : tk.lex) + "' instead.";
    if(! extra.empty() ) message += "\n~ " + extra;

    fail(c, tk, message);
}

// `123` or `-123`
inline bool is_number(const std::string& s) {
    std::size_t k = (! s.empty() && s[0] == '-') ? 1 : 0;
    if( s.size() <= k ) return false;
    return s.find_first_not_of("0123456789", k) == std::string::npos;
}

inline bool is_zero_literal(const std::string& s) {
    return s.find_first_not_of("-0") == std::string::npos;
}

// ::name::   (the lexer reads it as a single token)
inline bool is_label_token(const std::string& t) {
    return t.size() > 4 && t.compare(0, 2, "::") == 0 && t.compare(t.size() - 2, 2, "::") == 0;
}

inline Symbol lookup_or_fail(Ctx& c, const Tk& tk, const char* kind) {
    auto s = c.sym.lookup(tk.lex);
    if(! s ) fail(c, tk, std::string(kind) + " " + tk.lex + " not found");
    return *s;
}

// finds the symbol and checks that it is valid for the expression it is used in
inline Symbol use_as(Ctx& c, const Tk& tk, Use u, const char* kind = "Value") {
    Symbol s = lookup_or_fail(c, tk, kind);
    auto why = invalid_for(s, u);
    if(! why.empty() ) fail(c, tk, "'" + tk.lex + "' " + why);
    return s;
}

inline Operand read_operand(Ctx& c, const char* what, Use u = Use::scalar) {
    Tk tk = need(c, what);
    Operand op{ tk.lex, is_number(tk.lex) };
    if(! op.literal ) use_as(c, tk, u);
    return op;
}

// nullopt for literals
inline std::optional<Ty> operand_type(Ctx& c, const Operand& op) {
    if( op.literal ) return std::nullopt;
    return c.sym.lookup(op.text)->type;   // already validated by read_operand
}

inline Ty need_type(Ctx& c, const char* what);

// consumes a type if there is one at the cursor; consumes nothing otherwise
inline std::optional<Ty> try_type(Ctx& c) {
    if( c.at_end() ) return std::nullopt;

    // `{ T, T, ... }`
    if( c.peek().lex == "{" ) {
        c.next();

        Ty result = make_ty(morgana_type::mtuple);

        while( true ) {
            Tk start = c.peek();
            Ty el = need_type(c, "a tuple element type");
            if( is_void(el) ) fail(c, start, "void can't be a tuple element");

            result.tuple.elements.push_back({ el.kind, std::move(el.tuple) });

            Tk sep = need(c, "',' or '}'");
            if( sep.lex == "}" ) break;
            if( sep.lex != "," )
                fail(c, sep, "Expected ',' or '}' in the tuple type, but received '" + sep.lex + "'");
        }

        return result;
    }

    #define X(m, str, ...) \
        if(! std::string(str).empty() && c.peek().lex == str ) { c.next(); return make_ty(morgana_type::m); }
    TYPE_SET(X)
    #undef X

    return std::nullopt;
}

inline Ty need_type(Ctx& c, const char* what) {
    if( auto t = try_type(c) ) return *t;

    Tk tk = c.peek();
    fail(c, tk, std::string("Expected ") + what + ", but received '" + (c.at_end() ? "<end of file>" : tk.lex) + "'");
}

// T, T, ... `close`
inline std::vector<Ty> type_list(Ctx& c, const char* close) {
    std::vector<Ty> out;

    if( c.peek().lex == close ) { c.next(); return out; }

    while( true ) {
        Tk start = c.peek();
        Ty t = need_type(c, "a type");
        if( is_void(t) ) fail(c, start, "void can't be a parameter type");
        out.push_back(std::move(t));

        Tk sep = need(c, "',' or ')'");
        if( sep.lex == close ) break;
        if( sep.lex != "," )
            fail(c, sep, std::string("Expected ',' or '") + close + "', but received '" + sep.lex + "'");
    }

    return out;
}

// a, b, ... `close`    (names or literals)
inline std::vector<Operand> operand_list(Ctx& c, const char* close, Use u = Use::value) {
    std::vector<Operand> out;

    if( c.peek().lex == close ) { c.next(); return out; }

    while( true ) {
        out.push_back(read_operand(c, "an argument", u));

        Tk sep = need(c, "',' or ')'");
        if( sep.lex == close ) break;
        if( sep.lex != "," )
            fail(c, sep, std::string("Expected ',' or '") + close + "', but received '" + sep.lex + "'");
    }

    return out;
}

inline std::string decode_literal(Ctx& c, const Tk& tk) {
    const std::string& token = tk.lex;

    if( token.empty() || token[0] != 'h' || (token.size() - 1) % 2 != 0 )
        fail(c, tk, "Invalid string literal '" + token + "'");

    auto hex = [&](char ch) -> int {
        if( ch >= '0' && ch <= '9' ) return ch - '0';
        if( ch >= 'a' && ch <= 'f' ) return ch - 'a' + 10;
        if( ch >= 'A' && ch <= 'F' ) return ch - 'A' + 10;
        fail(c, tk, "Invalid string literal '" + token + "'");
    };

    std::string bytes;
    for( std::size_t k = 1; k < token.size(); k += 2 )
        bytes.push_back(static_cast<char>(hex(token[k]) * 16 + hex(token[k + 1])));

    return bytes;
}

// <name> = <op> <a>, <b>
inline bool is_binary_op(const std::string& op) {
    static const std::unordered_set<std::string> ops = {
        "add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor",
        "shl", "shr", "sar",
        "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
    };
    return ops.count(op) > 0;
}

// comparisons produce a 1-byte boolean (u8)
inline bool is_comparison_op(const std::string& op) {
    static const std::unordered_set<std::string> ops = {
        "eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
    };
    return ops.count(op) > 0;
}

// (a and b: names or integer literals, at least one must be a name)
inline void parse_binary(Ctx& c, const std::string& dst, const Tk& op_tk) {
    const std::string& op = op_tk.lex;

    Operand a = read_operand(c, "left operand");
    expect(c, {","}, "A binary operation needs two operands: <name> = " + op + " a, b");
    Operand b = read_operand(c, "right operand");

    auto ta = operand_type(c, a);
    auto tb = operand_type(c, b);

    if(! ta && !tb )
        fail(c, op_tk, "'" + op + "' needs at least one named operand");

    Ty operand = ta ? *ta : *tb;

    // only the fixed-size types can be compared here; int/uint/ptr are the codegen's business
    if( ta && tb ) {
        auto sa = fixed_size(ta->kind), sb = fixed_size(tb->kind);
        if( sa && sb && *sa != *sb )
            fail(c, op_tk, "The operands of '" + op + "' must have the same size");
    }

    Ty result = is_comparison_op(op) ? make_ty(morgana_type::mu8) : operand;

    c.emit(binary_op{ dst, op, operand, result, a, b });
    c.sym.push(dst, value_symbol(result));
}

inline void parse_label(Ctx& c, const std::string& token) {
    c.emit(label_op{ token.substr(2, token.size() - 4) });
}

// br <label>
inline void parse_br(Ctx& c) {
    c.emit(br_op{ need(c, "label name").lex });
}

// brez <cond>, <label>   |   brnz <cond>, <label>
inline void parse_branch(Ctx& c, bool nonzero) {
    Tk cond = need(c, "condition value");
    expect(c, {","}, "A branch needs a label: brez cond, label");
    Tk target = need(c, "label name");

    if( is_number(cond.lex) ) {
        bool taken = nonzero ? !is_zero_literal(cond.lex) : is_zero_literal(cond.lex);
        if( taken ) c.emit(br_op{ target.lex });
        return;
    }

    Symbol s = lookup_or_fail(c, cond, "Value");
    auto why = invalid_for(s, Use::scalar);
    if(! why.empty() )
        fail(c, cond, "'" + cond.lex + "' can't be used as a branch condition: it " + why);

    c.emit(branch_op{ cond.lex, target.lex, nonzero });
}

// "(" arg, arg, ... ")"
inline std::vector<Operand> emit_arguments(Ctx& c, const Symbol& fn) {
    Tk open = c.peek();
    expect(c, {"("});
    auto args = operand_list(c, ")");

    if( fn.params.size() != args.size() )
        fail(c, open,
            "Was expected " + std::to_string(fn.params.size())
            + " arguments, but was received " + std::to_string(args.size()));

    for( std::size_t k = 0; k < args.size(); k++ ) {
        const Ty& param = fn.params[k];
        auto at = operand_type(c, args[k]);
        const std::string n = std::to_string(k + 1);

        if( at && is_vec(*at) )
            fail(c, open, "Argument " + n + " ('" + args[k].text
                          + "') is a vector: vectors can't be passed to functions");

        if( ! at && is_tuple(param) )
            fail(c, open, "Argument " + n + " is a tuple, but a literal was received");

        if( at && is_tuple(param) != is_tuple(*at) )
            fail(c, open, "Argument " + n + " is a " + type_name(param) + ", but '"
                          + args[k].text + "' is a " + type_name(*at));

        if( at && is_tuple(param) && !same_type(param, *at) )
            fail(c, open, "Argument " + n + " expects " + type_name(param)
                          + ", but '" + args[k].text + "' is " + type_name(*at));
    }

    return args;
}

// call <fn>(<args>)   # void functions
inline void parse_call_void(Ctx& c) {
    Tk name = need(c, "function name");
    Symbol fn = use_as(c, name, Use::callee, "Function");

    if(! is_void(fn.type) )
        fail(c, name, "The function " + name.lex + " has a return type. You need to collect it.");

    auto args = emit_arguments(c, fn);
    c.emit(call_op{ "", fn.type, name.lex, std::move(args), false });
}

// call_indirect <ptr>(<args>)    # void only: the signature isn't known
inline void parse_call_indirect(Ctx& c) {
    Tk target = need(c, "function pointer name");
    Symbol p = use_as(c, target, Use::pointer);

    expect(c, {"("});
    auto args = operand_list(c, ")");

    c.emit(call_indirect_op{ "", make_ty(morgana_type::mvoid), target.lex, via_lea(p), std::move(args), false });
}

// <name> = call <fn>(<args>)
inline void assign_call(Ctx& c, const std::string& dst) {
    Tk fn_tk = need(c, "function name");
    Symbol fn = use_as(c, fn_tk, Use::callee, "Function");

    if( is_void(fn.type) )
        fail(c, fn_tk, "The function " + fn_tk.lex + " has no return type. You do not need to collect it.");

    bool sret = is_tuple(fn.type);
    auto args = emit_arguments(c, fn);

    c.emit(call_op{ dst, fn.type, fn_tk.lex, std::move(args), sret });
    c.sym.push(dst, value_symbol(fn.type));
}

// <name> = call_indirect <ret type> <ptr>(<args>)
// The signature isn't known, so the return type is written in the instruction.
inline void assign_call_indirect(Ctx& c, const std::string& dst) {
    Ty ret = need_type(c, "a return type");
    Tk target = need(c, "function pointer name");

    if( is_void(ret) )
        fail(c, target, "call_indirect returns void: don't collect it, use `call_indirect " + target.lex + "(...)`");

    Symbol p = use_as(c, target, Use::pointer);

    expect(c, {"("});
    auto args = operand_list(c, ")");

    c.emit(call_indirect_op{ dst, ret, target.lex, via_lea(p), std::move(args), is_tuple(ret) });
    c.sym.push(dst, value_symbol(ret));
}

inline Tk need_function_name(Ctx& c) {
    Tk tk = need(c, "function name");
    if( tk.kind != TkKind::Identifier )
        fail(c, tk, "Expected the function name as the second token in the function declaration.");
    return tk;
}

// extern <type> <name>(<types>)
inline void parse_extern(Ctx& c) {
    Ty ret = need_type(c, "a return type");
    Tk name = need_function_name(c);

    expect(c, {"("});
    auto params = type_list(c, ")");

    c.emit(extern_op{ name.lex, ret, params });
    c.sym.push(name.lex, Symbol{ sym_kind::function, ret, params });
}

// unfold <type> <name>(<types>)
// (a declaration only: if the function is never cited, nothing is emitted and it is not an error)
inline void parse_unfold(Ctx& c) {
    Ty ret = need_type(c, "a return type");
    Tk name = need_function_name(c);

    expect(c, {"("});
    auto params = type_list(c, ")");

    c.emit(unfold_op{ name.lex, ret, params });
    c.sym.push(name.lex, Symbol{ sym_kind::unfold, ret, params });
}

// [export] <type> <name>(<types>) {
inline void parse_function(Ctx& c, const Tk& first, Ty ret, bool exported = false) {
    if( c.current )
        fail(c, first, "A function can't be declared inside another function ('" + c.current_name + "' is still open).");

    Tk name = need_function_name(c);

    expect(c, {"("},
        "Expected '(' followed by explicitly typed parameters in the function declaration, "
        "or ')' if no parameters are provided");
    auto params = type_list(c, ")");

    if( auto data = c.sym.lookup(name.lex) ) {
        if( data->kind != sym_kind::unfold )
            fail(c, name, name.lex + " is already defined.");

        bool same = same_type(data->type, ret) && data->params.size() == params.size();
        for( std::size_t k = 0; same && k < params.size(); k++ )
            same = same_type(data->params[k], params[k]);

        if(! same )
            fail(c, name, "Definition of " + name.lex + " does not match its unfold declaration.");

        c.sym.erase(name.lex);
    }

    c.sym.push(name.lex, Symbol{ sym_kind::function, ret, params });

    expect(c, {"{"}, "Expected '{' to the function block");
    c.sym.push_scope();

    c.current      = Fn{ ret, params };
    c.current_name = name.lex;
    c.emit(prologue(ret, name.lex, std::move(params), exported));
}

// }
inline void parse_close(Ctx& c, const Tk& tk) {
    if(! c.sym.pop_scope() )
        fail(c, tk, "Unmatched '}'");

    c.current.reset();
    c.emit(epilogue());
}

// @_ : <arg>, <arg>, ...
inline void parse_destroyer(Ctx& c, const Tk& at) {
    if(! c.current )
        fail(c, at, "A destroyer '@_' must be declared inside a function.");

    expect(c, {":"}, "A destroyer needs its arguments: @_ : self, ...");

    std::vector<Param> args;

    while( true ) {
        Tk name = need(c, "argument name");

        if( name.lex == "," || name.lex == ":" || name.lex == "{" || name.lex == "}" || name.lex == "="
            || is_number(name.lex) )
            fail(c, name, "Expected an argument name in the destroyer, but received '" + name.lex + "'");

        for( const auto& a : args )
            if( a.name == name.lex )
                fail(c, name, "Duplicated argument '" + name.lex + "' in the destroyer");

        if( args.size() >= c.current->params.size() )
            fail(c, name,
                "The destroyer has more arguments than the function, which takes "
                + std::to_string(c.current->params.size()));

        if( c.sym.lookup(name.lex).has_value() )
            fail(c, name, "'" + name.lex + "' is already defined.");

        args.push_back({ name.lex, c.current->params[args.size()] });

        if( c.peek().lex != "," ) break;
        c.next();
    }

    if( args.size() != c.current->params.size() )
        fail(c, at,
            "The destroyer needs " + std::to_string(c.current->params.size())
            + " arguments (one per function parameter), but received " + std::to_string(args.size()));

    for( const auto& a : args )
        c.sym.push(a.name, value_symbol(a.type));

    c.emit(destroyer_op{ std::move(args) });
}

// store <alloc> <value>
inline void parse_store(Ctx& c) {
    Tk name = need(c, "allocation name");
    use_as(c, name, Use::store_target, "Allocation");

    Operand value = read_operand(c, "value", Use::value);
    c.emit(store_op{ name.lex, value });
}

// ret <integer> | ret <name>
inline void parse_ret(Ctx& c, const Tk& at) {
    if(! c.current )
        fail(c, at, "'ret' must be inside a function.");

    Tk tk = need(c, "return value");
    const bool tuple_fn = is_tuple(c.current->ret);

    if( is_number(tk.lex) ) {
        if( tuple_fn )
            fail(c, tk, "A function that returns " + type_name(c.current->ret)
                        + " can't return the literal " + tk.lex);
        c.emit(ret_op{ Operand{ tk.lex, true } });
        return;
    }

    Symbol s = use_as(c, tk, Use::value);

    if( is_vec(s.type) )
        fail(c, tk, "'" + tk.lex + "' is a vector: vectors can't be returned");

    if( tuple_fn && !same_type(s.type, c.current->ret) )
        fail(c, tk, "'" + tk.lex + "' is " + type_name(s.type)
                    + ", but the function returns " + type_name(c.current->ret));

    if(! tuple_fn && is_tuple(s.type) )
        fail(c, tk, "'" + tk.lex + "' is a tuple, but the function returns " + type_name(c.current->ret));

    c.emit(ret_op{ Operand{ tk.lex, false } });
}

// storep <type> <ptr>, <value>     (*ptr = value, `type` is how wide the write is)
inline void parse_storep(Ctx& c) {
    Tk type_tk = c.peek();
    Ty type = need_type(c, "a type");
    Tk target = need(c, "pointer name");
    expect(c, {","}, "storep needs a value: storep type ptr, value");
    Operand value = read_operand(c, "value");

    if( is_tuple(type) || is_void(type) )
        fail(c, type_tk, "storep only writes scalar values");

    Symbol p = use_as(c, target, Use::pointer);
    c.emit(storep_op{ type, target.lex, via_lea(p), value });
}

// <name> = alloc <type>
inline void assign_alloc(Ctx& c, const std::string& dst) {
    Tk type_tk = c.peek();
    Ty type = need_type(c, "a type");
    if( is_void(type) ) fail(c, type_tk, "alloc can't allocate void");

    c.emit(alloc_op{ dst, type });
    c.sym.push(dst, allocation_symbol(type));
}

// <name> = ptr_to <value>
inline void assign_ptr_to(Ctx& c, const std::string& dst) {
    Tk target = need(c, "value name");
    Symbol s = use_as(c, target, Use::address_of);

    if( s.kind == sym_kind::function ) {
        c.emit(ptr_to_op{ dst, target.lex, true });
        c.sym.push(dst, value_symbol(make_ty(morgana_type::mptr)));
        return;
    }

    // the same slot, now seen as a pointer (the codegen takes its address with lea)
    c.emit(ptr_to_op{ dst, target.lex, false });
    c.sym.push(dst, allocation_symbol(make_ty(morgana_type::mptr)));
}

// `move` preserves the symbol category; it only introduces another name.
inline void assign_move(Ctx& c, const std::string& dst) {
    Tk source = need(c, "value name");
    Symbol s = lookup_or_fail(c, source, "Value");

    c.emit(move_op{ dst, source.lex });
    c.sym.push(dst, s);
}

// `load` treats an allocation as the value stored in that slot, rather than as its address.
inline void assign_load(Ctx& c, const std::string& dst) {
    Tk source = need(c, "value name");
    Symbol s = lookup_or_fail(c, source, "Value");

    if(! has_storage(s) )
        fail(c, source, "'" + source.lex + "' can't be loaded as a value");

    c.emit(load_op{ dst, source.lex });
    c.sym.push(dst, value_symbol(s.type));
}

// <name> = pack <tuple type> <a>, <b>, ...
// Elements must be names (a constant goes through alloc + store first). The layout
// (alignment, padding, offsets) is the codegen's job.
inline void assign_pack(Ctx& c, const std::string& dst) {
    Tk type_tk = c.peek();
    Ty type = need_type(c, "a tuple type");

    if(! is_tuple(type) )
        fail(c, type_tk, "pack needs a tuple type, but received a " + type_name(type));

    std::vector<std::string> names;
    std::vector<Ty> types;

    while( true ) {
        Tk tk = need(c, "tuple element");

        if( is_number(tk.lex) )
            fail(c, tk, "pack can't take the literal " + tk.lex + ": use alloc + store first");

        Symbol s = use_as(c, tk, Use::element);
        names.push_back(tk.lex);
        types.push_back(s.type);

        if( c.peek().lex != "," ) break;
        c.next();
    }

    const auto& decl = type.tuple.elements;

    if( decl.size() != types.size() )
        fail(c, type_tk,
            "pack received " + std::to_string(types.size()) + " elements, but the tuple type "
            + type_name(type) + " has " + std::to_string(decl.size()));

    for( std::size_t k = 0; k < types.size(); k++ ) {
        Ty expected = make_ty(decl[k].first, decl[k].second);
        if(! same_type(types[k], expected) )
            fail(c, type_tk,
                "Element " + std::to_string(k + 1) + " of pack ('" + names[k] + "') is "
                + type_name(types[k]) + ", but the tuple type expects " + type_name(expected));
    }

    c.emit(pack_op{ dst, type, std::move(names) });
    c.sym.push(dst, value_symbol(type));
}

// <name> = deref <type> <ptr>
inline void assign_deref(Ctx& c, const std::string& dst) {
    Tk type_tk = c.peek();
    Ty type = need_type(c, "a type");
    Tk source = need(c, "pointer name");

    if( is_void(type) )
        fail(c, type_tk, "deref can't read a zero-sized type");

    Symbol p = use_as(c, source, Use::pointer);

    c.emit(deref_op{ dst, type, source.lex, via_lea(p) });
    c.sym.push(dst, value_symbol(type));
}

// <name> = syscall <number>, <arg>, ...
inline void assign_syscall(Ctx& c, const std::string& dst) {
    Tk number = need(c, "syscall number");
    if(! is_number(number.lex) )
        fail(c, number, "Expected a syscall number, but received '" + number.lex + "'");

    std::vector<Operand> values;
    while( c.peek().lex == "," ) {
        c.next();
        values.push_back(read_operand(c, "syscall argument"));
    }

    if( values.size() > 6 )
        fail(c, number, "A syscall takes at most 6 arguments");

    c.emit(syscall_op{ dst, number.lex, std::move(values) });
    c.sym.push(dst, value_symbol(make_ty(morgana_type::mi64)));
}

// <name> = str h<hex bytes>
inline void assign_str(Ctx& c, const std::string& dst) {
    Tk lit = need(c, "string literal");
    auto bytes = decode_literal(c, lit);

    c.emit(str_op{ dst, std::move(bytes) });
    c.sym.push(dst, value_symbol(make_ty(morgana_type::mptr)));
}

// <name> = cast <type> <value>   (integer <-> integer, ptr <-> ptr, ptr <-> 64-bit)
inline void assign_cast(Ctx& c, const std::string& dst) {
    Tk type_tk = c.peek();
    Ty type = need_type(c, "a type");
    Tk source = need(c, "value name");
    Symbol s = use_as(c, source, Use::scalar);

    if( is_tuple(type) || is_void(type) )
        fail(c, type_tk, "cast only converts scalar values");

    if( is_ptr(s.type) != is_ptr(type) ) {
        const Ty& other = is_ptr(type) ? s.type : type;
        if(! is_64bit_integer(other.kind) )
            fail(c, type_tk,
                "A pointer can only be cast to or from a 64-bit integer, not " + type_name(other));
    }

    c.emit(cast_op{ dst, type, s.type, source.lex });
    c.sym.push(dst, value_symbol(type));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Registers, special registers, vector registers, mapped io and memory
//
//   <n> = reg_load <name>, <bits>              reg_store <name>, <bits>, <value>
//   <n> = reg_get  <name>, <bits>, <lo>, <w>   reg_set   <name>, <bits>, <lo>, <w>, <value>
//
//   sreg_* : the same as reg_*, for special / status registers (8..64 bits)
//   vreg_* : the same, for SIMD registers (64, 128, 256 or 512 bits)
//              vreg_load / vreg_store      work on the whole register: type v<bits>
//              vreg_get  / vreg_set        work on one LANE (aligned to its own width):
//                                          width <= 64 -> uN scalar, width 128/256 -> v<width>
//              a vector is only written from another vector of the same width,
//              or from the literal 0 (zeroing)
//   io_*   : the same as reg_*, with an address in the I/O SPACE (0x00..0x3F on AVR)
//   mem_*  : the same as reg_*, with an address in the DATA SPACE
//
//   <n> = extract <type> <tuple>, <index>      one element of a tuple
// ═════════════════════════════════════════════════════════════════════════════

enum class loc_kind { reg, sreg, vreg, io, mem };

inline std::size_t parse_count(Ctx& c, const char* what) {
    Tk tk = need(c, what);
    if(! is_number(tk.lex) || tk.lex[0] == '-' || tk.lex.size() > 9 )
        fail(c, tk, std::string("Expected ") + what + " (a non-negative number), but received '" + tk.lex + "'");
    return std::stoul(tk.lex);
}

inline bool is_vector_loc(const Loc& l) { return l.space == Loc::Space::vreg; }

inline Loc parse_location(Ctx& c, loc_kind kind) {
    Loc r;
    switch( kind ) {
        case loc_kind::reg:  r.space = Loc::Space::reg;  break;
        case loc_kind::sreg: r.space = Loc::Space::sreg; break;
        case loc_kind::vreg: r.space = Loc::Space::vreg; break;
        case loc_kind::io:   r.space = Loc::Space::io;   break;
        case loc_kind::mem:  r.space = Loc::Space::mem;  break;
    }

    const char* what = kind == loc_kind::io   ? "io address"
                     : kind == loc_kind::mem  ? "memory address"
                     : kind == loc_kind::sreg ? "special register name"
                     : kind == loc_kind::vreg ? "vector register name"
                                              : "register name";

    Tk tk = need(c, what);
    std::string head = tk.lex;

    if( kind == loc_kind::io || kind == loc_kind::mem ) {
        // the lexer may split `0x25` into `0` + `x25`: glue the pieces back until the comma
        if( head[0] == '0' ) {
            while(! c.at_end() && c.peek().lex != "," && head.size() <= 18 )
                head += c.next().lex;
        }

        if( head.size() < 3 || head.size() > 18 || head[0] != '0' || (head[1] != 'x' && head[1] != 'X')
            || head.find_first_not_of("0123456789abcdefABCDEF", 2) != std::string::npos )
            fail(c, tk, "Expected a hexadecimal address (0x...), but received '" + head + "'");

        r.addr = std::stoull(head.substr(2), nullptr, 16);
    } else {
        r.name = head;
    }

    expect(c, {","}, "A register / io access needs its size in bits: name, bits");
    Tk bits_tk = c.peek();
    r.bits = parse_count(c, "the size in bits");

    if( is_vector_loc(r) ) {
        if( r.bits != 64 && r.bits != 128 && r.bits != 256 && r.bits != 512 )
            fail(c, bits_tk, "A vector register is 64, 128, 256 or 512 bits, but received " + std::to_string(r.bits));
    } else if( r.bits != 8 && r.bits != 16 && r.bits != 32 && r.bits != 64 ) {
        fail(c, bits_tk, "The size must be 8, 16, 32 or 64 bits, but received " + std::to_string(r.bits));
    }

    return r;
}

// `, lo, width`
inline std::pair<std::size_t, std::size_t> parse_slice(Ctx& c, const Loc& r) {
    expect(c, {","}, "A slice needs lo and width: name, bits, lo, width");
    std::size_t lo = parse_count(c, "the first bit of the slice");
    expect(c, {","}, "A slice needs lo and width: name, bits, lo, width");
    Tk w_tk = c.peek();
    std::size_t width = parse_count(c, "the width of the slice");

    if( width == 0 || lo + width > r.bits )
        fail(c, w_tk,
            "The slice " + std::to_string(lo) + "+" + std::to_string(width)
            + " doesn't fit in " + std::to_string(r.bits) + " bits");

    // vector registers are accessed by whole lanes
    if( is_vector_loc(r) ) {
        const bool lane = width == 8 || width == 16 || width == 32 || width == 64
                       || width == 128 || width == 256;
        if(! lane )
            fail(c, w_tk, "A vector lane is 8, 16, 32, 64, 128 or 256 bits wide, but received "
                          + std::to_string(width));
        if( lo % width != 0 )
            fail(c, w_tk, "A lane of " + std::to_string(width) + " bits must start at a multiple of "
                          + std::to_string(width) + ", but starts at bit " + std::to_string(lo));
    }

    return { lo, width };
}

// the value read from a scalar location is always an unsigned integer of its width
inline Ty unsigned_type(std::size_t bits) {
    switch( bits ) {
        case 8:  return make_ty(morgana_type::mu8);
        case 16: return make_ty(morgana_type::mu16);
        case 32: return make_ty(morgana_type::mu32);
        default: return make_ty(morgana_type::mu64);
    }
}

inline std::size_t slice_bits(std::size_t width) {
    return width <= 8 ? 8 : width <= 16 ? 16 : width <= 32 ? 32 : 64;
}

// type of a slice of `loc`: a vector lane wider than 64 bits is itself a vector
inline Ty slice_type(const Loc& loc, std::size_t width) {
    if( is_vector_loc(loc) && width > 64 ) return make_vec(width);
    return unsigned_type(slice_bits(width));
}

inline loc_kind kind_of(const std::string& statement) {
    if( statement.rfind("sreg_", 0) == 0 ) return loc_kind::sreg;
    if( statement.rfind("vreg_", 0) == 0 ) return loc_kind::vreg;
    if( statement.rfind("io_",   0) == 0 ) return loc_kind::io;
    if( statement.rfind("mem_",  0) == 0 ) return loc_kind::mem;
    return loc_kind::reg;
}

// `reg_<op>`, `sreg_<op>`, `vreg_<op>`, `io_<op>` or `mem_<op>`
inline bool is_loc_statement(const std::string& s, const char* op) {
    static const char* families[] = { "reg_", "sreg_", "vreg_", "io_", "mem_" };
    for( const char* f : families )
        if( s == std::string(f) + op ) return true;
    return false;
}

// the value written to a location.
//   want_vec: the destination is a vector of `vbits` bits -> a vector of that width or the literal 0
//   otherwise: a scalar (name or integer literal)
inline Operand read_loc_value(Ctx& c, bool want_vec, std::size_t vbits) {
    Tk tk = need(c, "value");
    Operand op{ tk.lex, is_number(tk.lex) };

    if( op.literal ) {
        if( want_vec && !is_zero_literal(op.text) )
            fail(c, tk, "A vector can only be written from another vector or from the literal 0 (zeroing), "
                        "but received " + op.text);
        return op;
    }

    Symbol s = use_as(c, tk, want_vec ? Use::value : Use::scalar);

    if( want_vec && !(is_vec(s.type) && s.type.bits == vbits) )
        fail(c, tk, "'" + tk.lex + "' is " + type_name(s.type)
                    + ", but a v" + std::to_string(vbits) + " is needed here");

    return op;
}

// reg_store <name>, <bits>, <value>   |   sreg_store / vreg_store / io_store / mem_store ...
inline void parse_loc_store(Ctx& c, loc_kind kind) {
    Loc loc = parse_location(c, kind);
    expect(c, {","}, "A store needs a value: name, bits, value");
    Operand value = read_loc_value(c, is_vector_loc(loc), loc.bits);
    c.emit(loc_store_op{ loc, value });
}

// reg_set <name>, <bits>, <lo>, <width>, <value>   |   sreg_set / vreg_set / io_set / mem_set ...
inline void parse_loc_set(Ctx& c, loc_kind kind) {
    Loc loc = parse_location(c, kind);
    auto [lo, width] = parse_slice(c, loc);
    expect(c, {","}, "A set needs a value: name, bits, lo, width, value");
    Operand value = read_loc_value(c, is_vector_loc(loc) && width > 64, width);
    c.emit(loc_set_op{ loc, lo, width, value });
}

// <n> = reg_load <name>, <bits>   |   sreg_load / vreg_load / io_load <addr> / mem_load <addr> ...
inline void assign_loc_load(Ctx& c, const std::string& dst, loc_kind kind) {
    Loc loc = parse_location(c, kind);

    Ty t = is_vector_loc(loc) ? make_vec(loc.bits) : unsigned_type(loc.bits);
    c.emit(loc_load_op{ dst, loc });
    c.sym.push(dst, value_symbol(t));
}

// <n> = reg_get <name>, <bits>, <lo>, <width>   |   sreg_get / vreg_get / io_get / mem_get ...
inline void assign_loc_get(Ctx& c, const std::string& dst, loc_kind kind) {
    Loc loc = parse_location(c, kind);
    auto [lo, width] = parse_slice(c, loc);

    Ty t = slice_type(loc, width);
    c.emit(loc_get_op{ dst, loc, lo, width, t });
    c.sym.push(dst, value_symbol(t));
}

// <name> = extract <type> <tuple>, <index>
inline void assign_extract(Ctx& c, const std::string& dst) {
    Tk type_tk = c.peek();
    Ty type = need_type(c, "an element type");
    Tk src = need(c, "tuple name");
    expect(c, {","}, "extract needs an index: extract type tuple, index");
    Tk idx_tk = c.peek();
    std::size_t index = parse_count(c, "the element index");

    if( is_void(type) )
        fail(c, type_tk, "extract can't read a zero-sized type");

    Symbol s = use_as(c, src, Use::value);
    if(! is_tuple(s.type) )
        fail(c, src, "'" + src.lex + "' is a " + type_name(s.type) + ", extract needs a tuple");

    const auto& els = s.type.tuple.elements;
    if( index >= els.size() )
        fail(c, idx_tk, "Index " + std::to_string(index) + " is out of range: " + type_name(s.type)
                        + " has " + std::to_string(els.size()) + " elements");

    Ty expected = make_ty(els[index].first, els[index].second);
    if(! same_type(expected, type) )
        fail(c, type_tk, "Element " + std::to_string(index) + " of '" + src.lex + "' is "
                         + type_name(expected) + ", not " + type_name(type));

    c.emit(extract_op{ dst, type, src.lex, index });
    c.sym.push(dst, value_symbol(type));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Assignment dispatch:  <name> = <statement>
// ═════════════════════════════════════════════════════════════════════════════

inline void parse_assignment(Ctx& c, const Tk& name_tk) {
    const std::string& name = name_tk.lex;

    if( is_number(name) )
        fail(c, name_tk, "'" + name + "' is a number, it can't be assigned.");

    Tk st = need(c, "statement");
    const std::string& s = st.lex;

    if( is_binary_op(s ) )      return parse_binary(c, name, st);

    if( s == "alloc" )          return assign_alloc(c, name);
    if( s == "ptr_to" )         return assign_ptr_to(c, name);
    if( s == "move" )           return assign_move(c, name);
    if( s == "pack" )           return assign_pack(c, name);
    if( s == "extract" )        return assign_extract(c, name);
    if( s == "load" )           return assign_load(c, name);
    if( s == "call" )           return assign_call(c, name);
    if( s == "call_indirect" )  return assign_call_indirect(c, name);
    if( s == "deref" )          return assign_deref(c, name);
    if( s == "syscall" )        return assign_syscall(c, name);
    if( s == "str" )            return assign_str(c, name);
    if( s == "cast" )           return assign_cast(c, name);

    if( is_loc_statement(s, "load") ) return assign_loc_load(c, name, kind_of(s));
    if( is_loc_statement(s, "get")  ) return assign_loc_get(c, name, kind_of(s));

    fail(c, st, "Unknown statement '" + s + "' after '='");
}

inline Ins parser(std::filesystem::path path, const std::vector<Tk>& tks) {
    Ins ins;
    if( tks.empty() ) return ins;

    Ctx c(std::move(path), tks, ins);

    using stmt_fn = void(*)(Ctx&);
    static const std::unordered_map<std::string, stmt_fn> statements = {
        { "extern",        parse_extern        },
        { "unfold",        parse_unfold        },
        { "call",          parse_call_void     },
        { "store",         parse_store         },
        { "storep",        parse_storep        },
        { "br",            parse_br            },
        { "call_indirect", parse_call_indirect },
        { "brez",      +[](Ctx& c) { parse_branch(c, false); } },
        { "brnz",      +[](Ctx& c) { parse_branch(c, true);  } },
        { "reg_store", +[](Ctx& c) { parse_loc_store(c, loc_kind::reg);  } },
        { "sreg_store",+[](Ctx& c) { parse_loc_store(c, loc_kind::sreg); } },
        { "vreg_store",+[](Ctx& c) { parse_loc_store(c, loc_kind::vreg); } },
        { "io_store",  +[](Ctx& c) { parse_loc_store(c, loc_kind::io);   } },
        { "mem_store", +[](Ctx& c) { parse_loc_store(c, loc_kind::mem);  } },
        { "reg_set",   +[](Ctx& c) { parse_loc_set(c, loc_kind::reg);    } },
        { "sreg_set",  +[](Ctx& c) { parse_loc_set(c, loc_kind::sreg);   } },
        { "vreg_set",  +[](Ctx& c) { parse_loc_set(c, loc_kind::vreg);   } },
        { "io_set",    +[](Ctx& c) { parse_loc_set(c, loc_kind::io);     } },
        { "mem_set",   +[](Ctx& c) { parse_loc_set(c, loc_kind::mem);    } },
    };

    while(! c.at_end() ) {
        Tk tk = c.peek();

        if( tk.lex == "ret" )  { c.next(); parse_ret(c, tk);        continue; }
        if( tk.lex == "}" )    { c.next(); parse_close(c, tk);      continue; }
        if( tk.lex == "@_" )   { c.next(); parse_destroyer(c, tk);  continue; }

        if( tk.lex == "export" ) {
            c.next();

            auto ret = try_type(c);
            if(! ret )
                fail(c, c.peek(), "Expected a function declaration after 'export', but received '"
                                  + (c.at_end() ? std::string("<end of file>") : c.peek().lex) + "'");

            parse_function(c, tk, *ret, true);
            continue;
        }

        if( auto f = statements.find(tk.lex); f != statements.end() ) {
            c.next();
            f->second(c);
            continue;
        }

        if( is_label_token(tk.lex) ) {
            c.next();
            parse_label(c, tk.lex);
            continue;
        }

        if( c.peek(1).lex == "=" ) {
            c.next();
            c.next();
            parse_assignment(c, tk);
            continue;
        }

        if( auto ret = try_type(c) ) {
            parse_function(c, tk, *ret);
            continue;
        }

        fail(c, tk, "Expected a statement or a function declaration, but received '" + tk.lex + "'");
    }

    if( c.current )
        fail(c, c.tks.back(), "Missing '}' to close the function '" + c.current_name + "'");

    return ins;
}
