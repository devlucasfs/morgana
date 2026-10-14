#pragma once

#include <fstream>
#include <string>
#include <deque>
#include <optional>
#include <cctype>
#include <utility>

class Lexer {
private:
    static constexpr size_t BUFFER_SIZE = 20;

    std::ifstream file;
    std::deque<std::string> tokens;
    std::deque<std::string> history;

    std::optional<std::string> read() {
        char c;

        while( file.get(c) ) {
            if( std::isspace(static_cast<unsigned char>(c)) ) {
                continue;
            }

            if( c == '#' ) {
                while( file.get(c) && c != '\n' ) {}
                continue;
            }

            if( c == '{' || c == '}' ||
                c == '(' || c == ')' ||
                c == '[' || c == ']' ||
                c == ',' ) {
                return std::string(1, c);
            }

            if( c == '"' || c == '\'' ) {
                std::string token(1, c);
                char quote = c;

                while( file.get(c) ) {
                    token += c;

                    if( c == '\\' ) {
                        if( file.get(c) ) {
                            token += c;
                        }

                        continue;
                    }

                    if( c == quote ) {
                        break;
                    }
                }

                return token;
            }

            bool number = std::isdigit(
                static_cast<unsigned char>(c)
            );

            bool dot = c == '.';
            std::string token(1, c);

            while( file.peek() != EOF ) {
                char next = static_cast<char>(file.peek());

                if( std::isspace(static_cast<unsigned char>(next)) ||
                    next == '#' ||
                    next == '{' || next == '}' ||
                    next == '(' || next == ')' ||
                    next == '[' || next == ']' ||
                    next == ',' ||
                    next == '"' || next == '\'' ) {
                    break;
                }

                if( number ) {
                    if( std::isdigit(static_cast<unsigned char>(next)) ) {
                        token += static_cast<char>(file.get());
                        continue;
                    }

                    if( next == '.' && !dot ) {
                        dot = true;
                        token += static_cast<char>(file.get());
                        continue;
                    }

                    break;
                }

                token += static_cast<char>(file.get());
            }

            return token;
        }

        return std::nullopt;
    }

    void fill() {
        while( tokens.size() < BUFFER_SIZE ) {
            auto token = read();

            if( !token ) {
                break;
            }

            tokens.push_back(std::move(*token));
        }
    }

public:
    explicit Lexer(const std::string& path)
        : file(path, std::ios::binary) {
        fill();
    }

    bool is_open() const {
        return file.is_open();
    }

    std::optional<std::string> next() {
        fill();

        if( tokens.empty() ) {
            return std::nullopt;
        }

        std::string token = std::move(tokens.front());
        tokens.pop_front();

        history.push_back(token);

        if( history.size() > BUFFER_SIZE ) {
            history.pop_front();
        }

        fill();

        return token;
    }

    std::optional<std::string> peek(size_t offset = 0) {
        if( offset >= BUFFER_SIZE ) {
            return std::nullopt;
        }

        while( tokens.size() <= offset ) {
            auto token = read();

            if( !token ) {
                return std::nullopt;
            }

            tokens.push_back(std::move(*token));
        }

        return tokens[offset];
    }

    void back() {
        if( history.empty() ) {
            return;
        }

        tokens.push_front(std::move(history.back()));
        history.pop_back();
    }
};
