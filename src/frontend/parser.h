#pragma once

// ============================================================================
// parser.h  —  Recursive-descent parser.
//
// Takes a flat vector<Token> produced by the Tokenizer and builds an AST
// node (Statement variant) matching the grammar in ast_spec.md.
//
// Usage:
//   Parser p(tokens);
//   Statement stmt = p.parse();
//
// Throws std::runtime_error (with a line-number message) on any grammar
// violation.  The error messages are written to be human-readable:
//   "Expected ')' after column list at line 3"
// ============================================================================

#include <stdexcept>
#include <string>
#include <vector>
#include "common/ast.h"
#include "common/token.h"

// ── ParseError — typed exception so callers can distinguish parse failures ───
class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

// ── Parser ────────────────────────────────────────────────────────────────────
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Parses exactly one statement from the token stream.
    // The stream must end with END_OF_FILE (or an optional SEMICOLON then EOF).
    Statement parse();

private:
    std::vector<Token> tokens_;
    size_t             pos_ = 0;

    // ── Token stream helpers ─────────────────────────────────────────────────
    const Token& peek()     const;
    const Token& peekNext() const;
    const Token& previous() const;
    bool         isAtEnd()  const;

    // Advance and return the consumed token.
    const Token& advance();

    // Return true (and advance) if the current token matches `t`.
    bool match(TokenType t);

    // Like match() but throws ParseError if the type doesn't match.
    const Token& expect(TokenType t, const std::string& context);

    // ── Top-level statement dispatching ─────────────────────────────────────
    Statement parseStatement();

    CreateTableStmt parseCreateTable();
    CreateIndexStmt parseCreateIndex();
    InsertStmt      parseInsert();
    SelectStmt      parseSelect();
    UpdateStmt      parseUpdate();
    DeleteStmt      parseDelete();
    TxnStmt         parseTxn(TxnStmt::Kind kind);

    // ── Clause parsers ───────────────────────────────────────────────────────
    ColumnDef      parseColumnDef();
    TableRef       parseTableRef();
    JoinClause     parseJoinClause();
    OrderByItem    parseOrderByItem();
    Literal        parseLiteral();

    // ── Expression parsers (precedence climbing) ─────────────────────────────
    //
    // Grammar (from ast_spec.md):
    //   expr       → or_expr
    //   or_expr    → and_expr  ("OR"  and_expr)*
    //   and_expr   → not_expr  ("AND" not_expr)*
    //   not_expr   → "NOT" not_expr | comparison
    //   comparison → operand comp_op operand
    //   operand    → column_ref | literal | aggregate_call | "(" expr ")"
    Expr parseExpr();
    Expr parseOrExpr();
    Expr parseAndExpr();
    Expr parseNotExpr();
    Expr parseComparison();
    Expr parseOperand();

    // Helper: is the current token a comparison operator?
    bool isCompOp() const;

    // Helper: build a ColumnRef or AggregateCall from the current position.
    Expr parseColumnRefOrAggregate();
};
