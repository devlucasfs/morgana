#pragma once

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cctype>
#include <algorithm>

enum class TkKind {
    Identifier,
    Integer,
    Decimal,
    String,
    Unknown,
    Eof
};

struct Tk {
    std::string lex;
    std::size_t line = 1;
    std::size_t col = 1;
    TkKind kind = TkKind::Unknown;

    void classify() {
        if( lex.empty() ) {
            kind = TkKind::Unknown;
            return;
        }

        if( lex.size() >= 2 && lex.front() == '"' && lex.back() == '"' ) {
            kind = TkKind::String;
            return;
        }

        std::size_t start = (lex[0] == '-') ? 1 : 0;

        if( start < lex.size() ) {
            bool valid = true;
            bool decimal = false;
            bool has_digit = false;

            for( std::size_t i = start; i < lex.size(); i++ ) {
                if( std::isdigit(static_cast<unsigned char>(lex[i])) ) has_digit = true;
                else if( lex[i] == '.' && !decimal ) decimal = true;
                else {
                    valid = false;
                    break;
                }
            }

            if( valid && has_digit ) {
                kind = decimal
                    ? TkKind::Decimal
                    : TkKind::Integer;
                return;
            }
        }

        if( std::isalpha(static_cast<unsigned char>(lex[0])) || lex[0] == '_' ) {
            bool valid = std::all_of(
                lex.begin() + 1,
                lex.end(),
                [](unsigned char c) {
                    return std::isalnum(c) || c == '_';
                }
            );

            if( valid ) {
                kind = TkKind::Identifier;
                return;
            }
        }

        kind = TkKind::Unknown;
    }
};

inline void lex_debug(const std::vector<Tk>& tokens) {
    auto kind_name = [](TkKind kind) -> const char* {
        switch( kind ) {
            case TkKind::Identifier: return "Identifier";
            case TkKind::Integer:    return "Integer";
            case TkKind::Decimal:    return "Decimal";
            case TkKind::String:     return "String";
            case TkKind::Eof:        return "Eof";
            default:                 return "Unknown";
        }
    };

    for( const auto& tk : tokens ) {
        std::cout
            << "["
            << kind_name(tk.kind)
            << "] "
            << "\"" << tk.lex << "\""
            << " (line: " << tk.line
            << ", col: " << tk.col << ")"
            << '\n';
    }
}

inline std::vector<uint8_t> read_file(
    std::filesystem::path path
) {
    std::ifstream file(path, std::ios::binary);

    if(! file ) {
        throw std::runtime_error(
            "Lexer: Failed to open file"
        );
    }

    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

inline std::vector<Tk> lexer(std::filesystem::path file) {
    auto bytes = read_file(file);

    std::vector<Tk> tokens;
    std::string lex;

    std::size_t line = 1;
    std::size_t col = 1;
    std::size_t start_line = 1;
    std::size_t start_col = 1;

    bool in_string = false;
    bool in_comment = false;

    auto flush = [&]() {
        if( lex.empty() ) return;

        Tk tk {
            .lex = lex,
            .line = start_line,
            .col = start_col
        };

        tk.classify();
        tokens.push_back(std::move(tk));

        lex.clear();
    };

    for( std::size_t i = 0; i < bytes.size(); i++ ) {
        char c = static_cast<char>(bytes[i]);
        if( in_comment ) {
            if( c != '\n' ) {
                ++col;
                continue;
            }

            in_comment = false;
        }

        if( lex.empty() ) {
            start_line = line;
            start_col = col;
        }

        if( c == '"' && (i == 0 || bytes[i - 1] != '\\') ) {
            in_string = !in_string;
            lex += c;
        }
        else if( c == '#' && !in_string ) {
            flush();
            in_comment = true;
        }

        else if( (c == ',' || c == '(' || c == ')' || c == '{' || c == '}') && !in_string ) {
            flush();

            Tk tk {
                .lex = std::string(1, c),
                .line = line,
                .col = col,
                .kind = TkKind::Unknown
            };

            tokens.push_back(std::move(tk));
        }

        else if( std::isspace(static_cast<unsigned char>(c)) && !in_string ) flush();
        else lex += c;

        if( c == '\n' ) {
            ++line;
            col = 1;
        } else ++col;
    }

    if( in_string ) {
        throw std::runtime_error(
            "Lexer: Unterminated string starting at line " +
            std::to_string(start_line) +
            ", col " + std::to_string(start_col)
        );
    }

    flush();

    Tk eof {
        .lex = "<eof>",
        .line = line,
        .col = col,
        .kind = TkKind::Eof
    };

    tokens.push_back(std::move(eof));

    return tokens;
}
