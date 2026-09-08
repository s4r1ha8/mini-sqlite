#pragma once

#include <string>

// All the token kinds our tokenizer can produce.
// Keeping keywords as their own explicit types (rather than a generic
// IDENTIFIER + string check downstream) makes the parser's job much easier.
enum class TokenType {
    // ── Keywords ────────────────────────────────────────────────────────────
    SELECT, FROM, WHERE, INSERT, INTO, VALUES,
    UPDATE, SET, DELETE,
    CREATE, TABLE, INDEX, ON, JOIN,
    AND, OR, NOT,
    ORDER, BY, GROUP, HAVING,          // HAVING reserved for M2 GROUP BY
    LIMIT,                              // LIMIT clause (M2+)
    BEGIN, COMMIT, ROLLBACK,
    PRIMARY, KEY,
    NULL_KW,                            // NULL keyword (named NULL_KW to avoid
                                        // collision with the C NULL macro)
    INTEGER, TEXT, REAL,                // data type keywords
    ASC, DESC,

    // ── Identifiers & literals ──────────────────────────────────────────────
    IDENTIFIER,
    INTEGER_LITERAL,
    STRING_LITERAL,
    REAL_LITERAL,

    // ── Comparison operators ────────────────────────────────────────────────
    EQUALS,         // =
    NOT_EQUALS,     // !=
    LESS_THAN,      // <
    GREATER_THAN,   // >
    LESS_EQUALS,    // <=
    GREATER_EQUALS, // >=

    // ── Punctuation ─────────────────────────────────────────────────────────
    COMMA,     // ,
    SEMICOLON, // ;
    LPAREN,    // (
    RPAREN,    // )
    STAR,      // *
    DOT,       // .

    END_OF_FILE,
    UNKNOWN
};

struct Token {
    TokenType   type;
    std::string lexeme; // raw text this token was scanned from
    int         line;   // 1-based line number — handy for error messages

    Token(TokenType t, std::string lex, int ln)
        : type(t), lexeme(std::move(lex)), line(ln) {}
};

// Human-readable name for a TokenType — used in debug output and error msgs.
std::string tokenTypeToString(TokenType type);
