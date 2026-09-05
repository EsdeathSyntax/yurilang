#include "yuri/lexer.hpp"
#include "yuri/logger.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>

namespace Yuri {

std::string read_file(const std::string& filepath) {
    Logger::log(Subsystem::Lexer, LogLevel::Info, "Reading source file from disk: " + filepath);
    std::ifstream file(filepath, std::ios::in | std::ios::binary);
    if (!file) {
        Logger::log(Subsystem::Lexer, LogLevel::Error, "Failed to open source file: " + filepath);
        throw std::runtime_error("Failed to open source file: " + filepath);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    Logger::log(Subsystem::Lexer, LogLevel::Debug, "Successfully read source file contents (" + std::to_string(buffer.str().size()) + " bytes)");
    return buffer.str();
}

Lexer::Lexer(std::string source_code) : source(std::move(source_code)) {
    Logger::log(Subsystem::Lexer, LogLevel::Debug, "Lexer initialized with source code length: " + std::to_string(source.length()));
}

bool Lexer::is_at_end() const {
    return current >= source.length();
}

char Lexer::advance() {
    auto current_char = peek();
    current++;
    if (current_char == '\n') {
        line++;
        column = 1;
    } else if (current_char == '\r') {
        if (peek() == '\n') {
            current++;
        }
        line++;
        column = 1;
    } else {
        column++;
    }
    return current_char;
}

char Lexer::peek() const {
    if (is_at_end()) return '\0';
    return source[current];
}

char Lexer::peek_next() const {
    if (current + 1 >= source.length()) return '\0';
    return source[current + 1];
}

bool Lexer::can_insert_semicolon(const std::vector<Token>& tokens) const {
    if (tokens.empty()) return false;
    const auto& last = tokens.back().type;
    return last == TokenType::Identifier || 
           last == TokenType::Number || 
           last == TokenType::CloseParen ||
           last == TokenType::KeywordSelf;
}

void Lexer::skip_whitespace(std::vector<Token>& tokens) {
    while (true) {
        char c = peek();
        switch (c) {
            case ' ': case '\r': case '\t':
                advance();
                break;
            case '\n':
                if (can_insert_semicolon(tokens)) {
                    tokens.push_back(Token{TokenType::Semicolon, ";", line, column});
                }
                advance();
                break;
            case '/':
                if (peek_next() == '/') {
                    while (peek() != '\n' && !is_at_end()) advance();
                } else {
                    return;
                }
                break;
            default:
                if (c == '\xC2' && peek_next() == '\xA0') {
                    advance();
                    advance();
                    break;
                }
                return;
        }
    }
}

Token Lexer::number() {
    size_t start_line = line;
    size_t start_col = column;

    if (source[start] == '0' && (peek() == 'x' || peek() == 'X')) {
        advance();
        while (std::isxdigit(peek())) {
            advance();
        }
    } else {
        while (std::isdigit(peek())) {
            advance();
        }
        if (peek() == '.' && std::isdigit(peek_next())) {
            advance();
            while (std::isdigit(peek())) {
                advance();
            }
        }
    }

    std::string text = source.substr(start, current - start);
    return Token{TokenType::Number, text, start_line, start_col};
}

Token Lexer::identifier() {
    size_t start_line = line;
    size_t start_col = column;
    while (std::isalnum(peek()) || peek() == '_') advance();

    std::string text = source.substr(start, current - start);
    TokenType type = TokenType::Identifier;
    
    if (text == "namespace") type = TokenType::KeywordNamespace;
    else if (text == "class") type = TokenType::KeywordClass;
    else if (text == "public") type = TokenType::KeywordPublic;
    else if (text == "private") type = TokenType::KeywordPrivate;
    else if (text == "fn") type = TokenType::KeywordFn;
    else if (text == "init") type = TokenType::KeywordInit;
    else if (text == "self") type = TokenType::KeywordSelf;
    else if (text == "operator") type = TokenType::KeywordOperator;
    else if (text == "string") type = TokenType::KeywordString;
    else if (text == "return") type = TokenType::KeywordReturn;
    else if (text == "module") type = TokenType::KeywordModule;

    return Token{type, text, start_line, start_col};
}

std::vector<Token> Lexer::scan_tokens() {
    Logger::log(Subsystem::Lexer, LogLevel::Info, "Starting token scan stream");
    std::vector<Token> tokens;

    while (!is_at_end()) {
        start = current;
        size_t tok_line = line;
        size_t tok_col = column;
        char c = advance();

        switch (c) {
            case ' ': case '\r': case '\t': case '\n': 
                skip_whitespace(tokens); 
                break;
            case '+': 
                if (peek() == '=') {
                    advance();
                    tokens.push_back(Token{TokenType::PlusEquals, "+=", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Plus, "+", tok_line, tok_col});
                }
                break;
            case '-':
                if (peek() == '>') {
                    advance();
                    tokens.push_back(Token{TokenType::Arrow, "->", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Minus, "-", tok_line, tok_col});
                }
                break;
            case '*': 
                tokens.push_back(Token{TokenType::Star, "*", tok_line, tok_col}); 
                break;
            case '/':
                if (peek_next() == '/') {
                    while (peek() != '\n' && !is_at_end()) advance();
                } else {
                    tokens.push_back(Token{TokenType::Slash, "/", tok_line, tok_col});
                }
                break;
            case '=': 
                if (peek() == '>') {
                    advance();
                    tokens.push_back(Token{TokenType::Arrow, "=>", tok_line, tok_col});
                } else if (peek() == '=') {
                    advance();
                    tokens.push_back(Token{TokenType::Match, "==", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Equals, "=", tok_line, tok_col});
                }
                break;
            case ':':
                if (peek() == ':') {
                    advance();
                    tokens.push_back(Token{TokenType::DoubleColon, "::", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Colon, ":", tok_line, tok_col});
                }
                break;
            case '.':
                if (peek() == '.') {
                    advance();
                    tokens.push_back(Token{TokenType::DotDot, "..", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Dot, ".", tok_line, tok_col});
                }
                break;
            case ',': tokens.push_back(Token{TokenType::Comma, ",", tok_line, tok_col}); break;
            case '[': tokens.push_back(Token{TokenType::OpenBracket, "[", tok_line, tok_col}); break;
            case ']': tokens.push_back(Token{TokenType::CloseBracket, "]", tok_line, tok_col}); break;
            case '{': tokens.push_back(Token{TokenType::OpenBrace, "{", tok_line, tok_col}); break;
            case '}': tokens.push_back(Token{TokenType::CloseBrace, "}", tok_line, tok_col}); break;
            case '(': tokens.push_back(Token{TokenType::OpenParen, "(", tok_line, tok_col}); break;
            case ')': tokens.push_back(Token{TokenType::CloseParen, ")", tok_line, tok_col}); break;
            case ';': tokens.push_back(Token{TokenType::Semicolon, ";", tok_line, tok_col}); break;
            case '@': tokens.push_back(Token{TokenType::At, "@", tok_line, tok_col}); break;
            case '&': 
                tokens.push_back(Token{TokenType::BitAnd, "&", tok_line, tok_col}); 
                break;
            case '|': 
                tokens.push_back(Token{TokenType::BitOr, "|", tok_line, tok_col}); 
                break;
            case '^': 
                tokens.push_back(Token{TokenType::BitXor, "^", tok_line, tok_col}); 
                break;
            case '~': 
                tokens.push_back(Token{TokenType::BitNot, "~", tok_line, tok_col}); 
                break;
            case '!':
                if (peek() == '=') {
                    advance();
                    tokens.push_back(Token{TokenType::NotEqual, "!=", tok_line, tok_col});
                }
                break;
            case '?': tokens.push_back(Token{TokenType::Question, "?", tok_line, tok_col}); break;
            case '<': 
                if (peek() == '<') {
                    advance();
                    tokens.push_back(Token{TokenType::BitshiftL, "<<", tok_line, tok_col});
                } else if (peek() == '=') {
                    advance();
                    tokens.push_back(Token{TokenType::LessEqual, "<=", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Less, "<", tok_line, tok_col});
                }
                break;
            case '>': 
                if (peek() == '>') {
                    advance();
                    tokens.push_back(Token{TokenType::BitshiftR, ">>", tok_line, tok_col});
                } else if (peek() == '=') {
                    advance();
                    tokens.push_back(Token{TokenType::GreaterEqual, ">=", tok_line, tok_col});
                } else {
                    tokens.push_back(Token{TokenType::Greater, ">", tok_line, tok_col});
                }
                break;
            case '#': 
                tokens.push_back(Token{TokenType::Hash, "#", tok_line, tok_col}); 
                break;
            case '\'':
            case '"': {
                char quote = c;
                size_t string_start = current;
                while (peek() != quote && !is_at_end()) {
                    advance();
                }
                if (is_at_end()) {
                    Logger::log(Subsystem::Lexer, LogLevel::Error, "Unterminated string literal at line " + std::to_string(tok_line));
                    throw std::runtime_error("Unterminated string literal.");
                }
                std::string value = source.substr(string_start, current - string_start);
                advance(); 
                tokens.push_back(Token{TokenType::StringLiteral, value, tok_line, tok_col});
                break;
            }
            default:
                if (std::isdigit(c)) {
                    tokens.push_back(number());
                } else if (std::isalpha(c) || c == '_') {
                    tokens.push_back(identifier());
                } else {
                    Logger::log(Subsystem::Lexer, LogLevel::Warning, "Encountered unknown character token: '" + std::string(1, c) + "' at line " + std::to_string(tok_line));
                    tokens.push_back(Token{TokenType::Unknown, std::string(1, c), tok_line, tok_col});
                }
                break;
        }
    }

    if (can_insert_semicolon(tokens)) {
        tokens.push_back(Token{TokenType::Semicolon, ";", line, column});
    }

    tokens.push_back(Token{TokenType::EndOfFile, "", line, column});
    Logger::log(Subsystem::Lexer, LogLevel::Info, "Successfully completed token scanning. Total tokens: " + std::to_string(tokens.size()));
    return tokens;
}

} // namespace Yuri