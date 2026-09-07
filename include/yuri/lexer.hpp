#pragma once

#include <string>
#include <vector>

namespace Yuri {

enum class TokenType {
    KeywordNamespace, KeywordClass, KeywordPublic, KeywordPrivate,
    KeywordFn, KeywordInit, KeywordSelf, KeywordOperator,
    KeywordPrintln, KeywordString, StringLiteral,
    KeywordReturn, KeywordIf, KeywordElse, KeywordWhile,
    KeywordModule, KeywordLet, KeywordType,

    Identifier, Number,

    Plus, PlusEquals, Equals, Arrow, Colon, DoubleColon,
    OpenBrace, CloseBrace, OpenParen, CloseParen, Semicolon, 
    Newline, Dot, Comma, Hash, Slash, Percent, Star, Minus,
    MinusEquals, StarEquals, SlashEquals, PercentEquals,
    At, BitAnd, BitOr, BitXor, BitshiftL, BitshiftR,
    BitNot, Less, Greater, OpenBracket, CloseBracket,
    LessEqual, GreaterEqual, DotDot, NotEqual, Question, Match,

    EndOfFile, Unknown
};

struct Token {
    TokenType type;
    std::string lexeme;
    size_t line;
    size_t column;
};

class Lexer {
private:
    std::string source;
    size_t start = 0;
    size_t current = 0;
    size_t line = 1;
    size_t column = 1;

    bool is_at_end() const;
    char advance();
    char peek() const;
    char peek_next() const;
    void skip_whitespace(std::vector<Token>& tokens);
    Token number();
    Token identifier();
    bool can_insert_semicolon(const std::vector<Token>& tokens) const;

public:
    explicit Lexer(std::string source_code);
    std::vector<Token> scan_tokens();
};

std::string read_file(const std::string& filepath);

} // namespace Yuri