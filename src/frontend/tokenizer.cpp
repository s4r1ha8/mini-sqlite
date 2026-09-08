#include "frontend/tokenizer.h"
#include <cctype>
#include <stdexcept>

// Keyword table — all uppercase. scanIdentifierOrKeyword() uppercases the
// scanned text before doing the lookup so matching is case-insensitive.
const std::unordered_map<std::string, TokenType> Tokenizer::keywords_ = {
    {"SELECT",   TokenType::SELECT},
    {"FROM",     TokenType::FROM},
    {"WHERE",    TokenType::WHERE},
    {"INSERT",   TokenType::INSERT},
    {"INTO",     TokenType::INTO},
    {"VALUES",   TokenType::VALUES},
    {"UPDATE",   TokenType::UPDATE},
    {"SET",      TokenType::SET},
    {"DELETE",   TokenType::DELETE},
    {"CREATE",   TokenType::CREATE},
    {"TABLE",    TokenType::TABLE},
    {"INDEX",    TokenType::INDEX},
    {"ON",       TokenType::ON},
    {"JOIN",     TokenType::JOIN},
    {"AND",      TokenType::AND},
    {"OR",       TokenType::OR},
    {"NOT",      TokenType::NOT},
    {"ORDER",    TokenType::ORDER},
    {"BY",       TokenType::BY},
    {"GROUP",    TokenType::GROUP},
    {"HAVING",   TokenType::HAVING},
    {"LIMIT",    TokenType::LIMIT},
    {"BEGIN",    TokenType::BEGIN},
    {"COMMIT",   TokenType::COMMIT},
    {"ROLLBACK", TokenType::ROLLBACK},
    {"PRIMARY",  TokenType::PRIMARY},
    {"KEY",      TokenType::KEY},
    {"NULL",     TokenType::NULL_KW},
    {"INTEGER",  TokenType::INTEGER},
    {"TEXT",     TokenType::TEXT},
    {"REAL",     TokenType::REAL},
    {"ASC",      TokenType::ASC},
    {"DESC",     TokenType::DESC},
};

Tokenizer::Tokenizer(std::string source) : source_(std::move(source)) {}

// ── Private helpers ──────────────────────────────────────────────────────────

bool Tokenizer::isAtEnd() const {
    return pos_ >= source_.size();
}

char Tokenizer::peek() const {
    return isAtEnd() ? '\0' : source_[pos_];
}

char Tokenizer::peekNext() const {
    return (pos_ + 1 >= source_.size()) ? '\0' : source_[pos_ + 1];
}

char Tokenizer::advance() {
    char c = source_[pos_++];
    if (c == '\n') line_++;
    return c;
}

// Consume next char only if it equals `expected`. Returns true if consumed.
bool Tokenizer::match(char expected) {
    if (isAtEnd() || source_[pos_] != expected) return false;
    pos_++;
    return true;
}

// Skip spaces, tabs, carriage returns, newlines, and -- line comments.
void Tokenizer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '-' && peekNext() == '-') {
            // SQL line comment: everything up to (but not including) the newline
            while (!isAtEnd() && peek() != '\n') advance();
        } else {
            break;
        }
    }
}

// ── Public entry point ───────────────────────────────────────────────────────

std::vector<Token> Tokenizer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skipWhitespaceAndComments();
        if (isAtEnd()) {
            tokens.emplace_back(TokenType::END_OF_FILE, "", line_);
            break;
        }
        tokens.push_back(scanToken());
    }
    return tokens;
}

// ── Token scanners ───────────────────────────────────────────────────────────

Token Tokenizer::scanToken() {
    char c = peek();

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        return scanIdentifierOrKeyword();
    if (std::isdigit(static_cast<unsigned char>(c)))
        return scanNumber();
    if (c == '\'')
        return scanString();

    int startLine = line_;
    advance(); // consume the character we just peeked

    switch (c) {
        case '=': return Token(TokenType::EQUALS,       "=",  startLine);
        case '!':
            if (match('=')) return Token(TokenType::NOT_EQUALS, "!=", startLine);
            throw std::runtime_error(
                "Unexpected character '!' at line " + std::to_string(startLine) +
                " — did you mean '!='?");
        case '<':
            if (match('=')) return Token(TokenType::LESS_EQUALS,    "<=", startLine);
            return Token(TokenType::LESS_THAN,    "<",  startLine);
        case '>':
            if (match('=')) return Token(TokenType::GREATER_EQUALS, ">=", startLine);
            return Token(TokenType::GREATER_THAN, ">",  startLine);
        case ',': return Token(TokenType::COMMA,     ",", startLine);
        case ';': return Token(TokenType::SEMICOLON, ";", startLine);
        case '(': return Token(TokenType::LPAREN,    "(", startLine);
        case ')': return Token(TokenType::RPAREN,    ")", startLine);
        case '*': return Token(TokenType::STAR,      "*", startLine);
        case '.': return Token(TokenType::DOT,       ".", startLine);
        default:
            throw std::runtime_error(
                "Unexpected character '" + std::string(1, c) +
                "' at line " + std::to_string(startLine));
    }
}

Token Tokenizer::scanIdentifierOrKeyword() {
    int    startLine = line_;
    size_t start     = pos_;
    while (!isAtEnd() &&
           (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_'))
        advance();

    std::string text  = source_.substr(start, pos_ - start);
    std::string upper = text;
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

    auto it = keywords_.find(upper);
    if (it != keywords_.end())
        return Token(it->second, text, startLine);   // preserve original casing in lexeme
    return Token(TokenType::IDENTIFIER, text, startLine);
}

Token Tokenizer::scanNumber() {
    int    startLine = line_;
    size_t start     = pos_;
    bool   isReal    = false;

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        advance();

    // Optional fractional part — only a REAL if there are digits after the dot
    if (!isAtEnd() && peek() == '.' &&
        std::isdigit(static_cast<unsigned char>(peekNext()))) {
        isReal = true;
        advance(); // consume '.'
        while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek())))
            advance();
    }

    std::string text = source_.substr(start, pos_ - start);
    return Token(isReal ? TokenType::REAL_LITERAL : TokenType::INTEGER_LITERAL,
                 text, startLine);
}

Token Tokenizer::scanString() {
    int startLine = line_;
    advance(); // consume opening '
    size_t start = pos_;

    while (!isAtEnd()) {
        // Handle escaped single-quote '' → '  (check BEFORE terminator check)
        if (peek() == '\'' && peekNext() == '\'') {
            advance(); advance(); // skip both quote chars
            continue;
        }
        if (peek() == '\'') break; // closing quote — stop
        advance();
    }
    if (isAtEnd())
        throw std::runtime_error(
            "Unterminated string literal starting at line " + std::to_string(startLine));

    std::string text = source_.substr(start, pos_ - start);
    advance(); // consume closing '
    return Token(TokenType::STRING_LITERAL, text, startLine);
}
