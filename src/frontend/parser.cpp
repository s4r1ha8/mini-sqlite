// ============================================================================
// parser.cpp — Recursive-descent parser implementation.
//
// Each grammar rule from ast_spec.md has its own private method.
// Error messages always include the offending line number (taken from the
// token's `line` field) so the user knows exactly where parsing failed.
// ============================================================================

#include "frontend/parser.h"
#include <stdexcept>
#include <string>

// ── Constructor ───────────────────────────────────────────────────────────────

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

// ── Token stream helpers ──────────────────────────────────────────────────────

const Token& Parser::peek() const {
    return tokens_[pos_];
}

const Token& Parser::peekNext() const {
    if (pos_ + 1 < tokens_.size()) return tokens_[pos_ + 1];
    return tokens_.back(); // EOF sentinel
}

const Token& Parser::previous() const {
    return tokens_[pos_ > 0 ? pos_ - 1 : 0];
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::END_OF_FILE;
}

const Token& Parser::advance() {
    if (!isAtEnd()) pos_++;
    return tokens_[pos_ - 1];
}

bool Parser::match(TokenType t) {
    if (peek().type == t) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenType t, const std::string& context) {
    if (peek().type != t) {
        throw ParseError(
            "Expected " + tokenTypeToString(t) +
            " " + context +
            " but got '" + peek().lexeme +
            "' at line " + std::to_string(peek().line));
    }
    return advance();
}

// ── Public entry point ────────────────────────────────────────────────────────

Statement Parser::parse() {
    Statement stmt = parseStatement();

    match(TokenType::SEMICOLON); // optional trailing semicolon

    if (!isAtEnd()) {
        throw ParseError(
            "Unexpected token '" + peek().lexeme +
            "' after statement at line " + std::to_string(peek().line));
    }

    return stmt;
}

// ── Top-level dispatch ────────────────────────────────────────────────────────

Statement Parser::parseStatement() {
    const Token& t = peek();

    switch (t.type) {
        case TokenType::CREATE:
            advance(); // consume CREATE

            if (match(TokenType::TABLE))
                return parseCreateTable();

            if (match(TokenType::INDEX))
                return parseCreateIndex();

            throw ParseError(
                "Expected TABLE or INDEX after CREATE at line " +
                std::to_string(previous().line));

        case TokenType::INSERT:
            advance();
            return parseInsert();

        case TokenType::SELECT:
            advance();
            return parseSelect();

        case TokenType::UPDATE:
            advance();
            return parseUpdate();

        case TokenType::DELETE:
            advance();
            return parseDelete();

        case TokenType::BEGIN:
            advance();
            return parseTxn(TxnStmt::Kind::BEGIN);

        case TokenType::COMMIT:
            advance();
            return parseTxn(TxnStmt::Kind::COMMIT);

        case TokenType::ROLLBACK:
            advance();
            return parseTxn(TxnStmt::Kind::ROLLBACK);

        default:
            throw ParseError(
                "Expected a statement keyword (SELECT, INSERT, UPDATE, DELETE, CREATE, BEGIN, "
                "COMMIT, ROLLBACK) but got '" + t.lexeme +
                "' at line " + std::to_string(t.line));
    }
}

// ── CREATE TABLE ─────────────────────────────────────────────────────────────

// create_table := "CREATE" "TABLE" identifier
//                 "(" column_def ("," column_def)* ")"

CreateTableStmt Parser::parseCreateTable() {
    CreateTableStmt stmt;

    stmt.table_name =
        expect(TokenType::IDENTIFIER,
               "as table name after CREATE TABLE").lexeme;

    expect(TokenType::LPAREN,
           "after table name in CREATE TABLE");

    stmt.columns.push_back(parseColumnDef());

    while (match(TokenType::COMMA)) {
        stmt.columns.push_back(parseColumnDef());
    }

    expect(TokenType::RPAREN,
           "after column list in CREATE TABLE");

    return stmt;
}

// column_def := identifier data_type
//               ("PRIMARY" "KEY")?
//               ("NOT" "NULL")?

ColumnDef Parser::parseColumnDef() {
    ColumnDef col;

    col.name =
        expect(TokenType::IDENTIFIER,
               "as column name").lexeme;

    // data_type
    const Token& dt = peek();

    if (dt.type == TokenType::INTEGER) {
        col.type = DataType::INTEGER;
        advance();
    }
    else if (dt.type == TokenType::TEXT) {
        col.type = DataType::TEXT;
        advance();
    }
    else if (dt.type == TokenType::REAL) {
        col.type = DataType::REAL;
        advance();
    }
    else {
        throw ParseError(
            "Expected data type (INTEGER, TEXT, REAL) after column name '" +
            col.name + "' at line " + std::to_string(dt.line));
    }

    // Optional PRIMARY KEY
    if (peek().type == TokenType::PRIMARY &&
        peekNext().type == TokenType::KEY) {

        advance();
        advance();

        col.is_primary_key = true;
        col.is_not_null    = true; // primary key implies NOT NULL
    }

    // Optional NOT NULL
    if (peek().type == TokenType::NOT &&
        peekNext().type == TokenType::NULL_KW) {

        advance();
        advance();

        col.is_not_null = true;
    }

    return col;
}

// ── CREATE INDEX ──────────────────────────────────────────────────────────────

// create_index := "CREATE" "INDEX" identifier
//                 "ON" identifier "(" identifier ")"

CreateIndexStmt Parser::parseCreateIndex() {
    CreateIndexStmt stmt;

    stmt.index_name =
        expect(TokenType::IDENTIFIER,
               "as index name").lexeme;

    expect(TokenType::ON,
           "after index name in CREATE INDEX");

    stmt.table_name =
        expect(TokenType::IDENTIFIER,
               "as table name in CREATE INDEX").lexeme;

    expect(TokenType::LPAREN,
           "before column name in CREATE INDEX");

    stmt.column_name =
        expect(TokenType::IDENTIFIER,
               "as column name in CREATE INDEX").lexeme;

    expect(TokenType::RPAREN,
           "after column name in CREATE INDEX");

    return stmt;
}

// ── INSERT ────────────────────────────────────────────────────────────────────

// insert := "INSERT" "INTO" identifier "VALUES"
//           "(" literal ("," literal)* ")"

InsertStmt Parser::parseInsert() {
    InsertStmt stmt;

    expect(TokenType::INTO,
           "after INSERT");

    stmt.table_name =
        expect(TokenType::IDENTIFIER,
               "as table name after INSERT INTO").lexeme;

    expect(TokenType::VALUES,
           "after table name in INSERT");

    expect(TokenType::LPAREN,
           "after VALUES");

    stmt.values.push_back(parseLiteral());

    while (match(TokenType::COMMA)) {
        stmt.values.push_back(parseLiteral());
    }

    expect(TokenType::RPAREN,
           "after value list in INSERT");

    return stmt;
}

// ── UPDATE ────────────────────────────────────────────────────────────────────

// update := "UPDATE" identifier "SET" identifier "=" expr ("," identifier "=" expr)* ("WHERE" expr)?

UpdateStmt Parser::parseUpdate() {
    UpdateStmt stmt;

    stmt.table_name =
        expect(TokenType::IDENTIFIER,
               "as table name after UPDATE").lexeme;

    expect(TokenType::SET,
           "after table name in UPDATE");

    do {
        Assignment assign;
        assign.column =
            expect(TokenType::IDENTIFIER,
                   "as column name in SET clause").lexeme;

        expect(TokenType::EQUALS,
               "after column name in SET clause");

        assign.value = parseExpr();

        stmt.updates.push_back(std::move(assign));
    } while (match(TokenType::COMMA));

    if (match(TokenType::WHERE)) {
        stmt.where = parseExpr();
    }

    return stmt;
}

// ── DELETE ────────────────────────────────────────────────────────────────────

// delete := "DELETE" "FROM" identifier ("WHERE" expr)?

DeleteStmt Parser::parseDelete() {
    DeleteStmt stmt;

    expect(TokenType::FROM,
           "after DELETE");

    stmt.table_name =
        expect(TokenType::IDENTIFIER,
               "as table name after DELETE FROM").lexeme;

    if (match(TokenType::WHERE)) {
        stmt.where = parseExpr();
    }

    return stmt;
}

// ── SELECT ────────────────────────────────────────────────────────────────────

// select := "SELECT" select_list "FROM" table_ref (join_clause)*
//           ("WHERE" expr)?
//           ("GROUP" "BY" identifier_list)?
//           ("HAVING" expr)?
//           ("ORDER" "BY" order_item_list)?
//           ("LIMIT" integer_literal)?

SelectStmt Parser::parseSelect() {
    SelectStmt stmt;

    // ── select_list ──────────────────────────────────────────────────────────
    // Supports:
    //   *
    //   column
    //   table.column
    //   COUNT(*)
    //   COUNT(column)
    //   SUM(column)
    //   AVG(column)
    //   MIN(column)
    //   MAX(column)

    if (match(TokenType::STAR)) {
        stmt.columns.push_back("*");
    } else {

        auto parseSelectItem = [&]() {

            Expr item = parseColumnRefOrAggregate();

            if (const auto* aggregate =
                    std::get_if<AggregateCall>(&item.node)) {

                // Keep the aggregate AST separately so the planner can
                // populate AggregateOp::aggregates.
                stmt.aggregate_columns.push_back(
                    AggregateCall{
                        aggregate->func,
                        aggregate->is_star,
                        aggregate->is_star
                            ? nullptr
                            : std::make_unique<Expr>(
                                  cloneExpr(*aggregate->arg))
                    }
                );

                // Preserve the exact textual SELECT item for ProjectOp.
                //
                // COUNT(*)      -> COUNT(*)
                // COUNT(id)     -> COUNT(id)
                // SUM(score)    -> SUM(score)
                // COUNT(u.id)   -> COUNT(u.id)
                //
                // ExecutorAdapter uses the same naming convention when it
                // constructs the output schema of Aggregate.
                if (aggregate->is_star) {
                    stmt.columns.push_back(
                        aggFuncToString(aggregate->func) + "(*)"
                    );
                } else {
                    if (!aggregate->arg) {
                        throw ParseError(
                            "Aggregate function is missing its argument at line " +
                            std::to_string(peek().line)
                        );
                    }

                    const auto* arg =
                        std::get_if<ColumnRef>(
                            &aggregate->arg->node
                        );

                    // v1 executor support currently handles aggregate
                    // arguments as column references.
                    if (!arg) {
                        throw ParseError(
                            "Aggregate arguments must be column references "
                            "in v1 at line " +
                            std::to_string(peek().line)
                        );
                    }

                    const std::string argName =
                        arg->table.has_value()
                            ? *arg->table + "." + arg->column
                            : arg->column;

                    stmt.columns.push_back(
                        aggFuncToString(aggregate->func) +
                        "(" + argName + ")"
                    );
                }

            } else if (const auto* column =
                           std::get_if<ColumnRef>(&item.node)) {

                stmt.columns.push_back(
                    column->table.has_value()
                        ? *column->table + "." + column->column
                        : column->column
                );

            } else {
                throw ParseError(
                    "Invalid expression in SELECT list at line " +
                    std::to_string(peek().line)
                );
            }
        };

        parseSelectItem();

        while (match(TokenType::COMMA)) {
            parseSelectItem();
        }
    }

    // ── FROM ──────────────────────────────────────────────────────────────────
    expect(TokenType::FROM,
           "after SELECT column list");

    stmt.from.push_back(parseTableRef());

    // Implicit comma-join is not supported in v1 — FROM only has one table ref.
    // Multiple tables come via explicit JOIN clauses below.

    // ── JOIN clauses ──────────────────────────────────────────────────────────
    while (peek().type == TokenType::JOIN) {
        advance(); // consume JOIN
        stmt.joins.push_back(parseJoinClause());
    }

    // ── WHERE ─────────────────────────────────────────────────────────────────
    if (match(TokenType::WHERE)) {
        stmt.where = parseExpr();
    }

    // ── GROUP BY ──────────────────────────────────────────────────────────────
    if (peek().type == TokenType::GROUP &&
        peekNext().type == TokenType::BY) {

        advance();
        advance(); // consume GROUP BY

        std::vector<std::string> groupCols;

        groupCols.push_back(
            expect(TokenType::IDENTIFIER,
                   "in GROUP BY column list").lexeme
        );

        while (match(TokenType::COMMA)) {
            groupCols.push_back(
                expect(TokenType::IDENTIFIER,
                       "in GROUP BY column list").lexeme
            );
        }

        stmt.group_by = std::move(groupCols);
    }

    // ── HAVING ────────────────────────────────────────────────────────────────
    // Parsed here (after GROUP BY) even if GROUP BY is absent — sema will
    // reject that case with a clear error message rather than a parse error.
    if (match(TokenType::HAVING)) {
        stmt.having = parseExpr();
    }

    // ── ORDER BY ──────────────────────────────────────────────────────────────
    if (peek().type == TokenType::ORDER &&
        peekNext().type == TokenType::BY) {

        advance();
        advance(); // consume ORDER BY

        std::vector<OrderByItem> items;

        items.push_back(parseOrderByItem());

        while (match(TokenType::COMMA)) {
            items.push_back(parseOrderByItem());
        }

        stmt.order_by = std::move(items);
    }

    // ── LIMIT ─────────────────────────────────────────────────────────────────
    if (match(TokenType::LIMIT)) {
        const Token& lit =
            expect(TokenType::INTEGER_LITERAL,
                   "after LIMIT");

        stmt.limit =
            std::stoll(lit.lexeme);
    }

    return stmt;
}

// ── TxnStmt ───────────────────────────────────────────────────────────────────

TxnStmt Parser::parseTxn(TxnStmt::Kind kind) {
    return TxnStmt{kind};
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// table_ref := identifier (identifier)? -- optional alias

TableRef Parser::parseTableRef() {
    TableRef ref;

    ref.table_name =
        expect(TokenType::IDENTIFIER,
               "as table name").lexeme;

    // Alias: bare identifier that is not a keyword.
    // We peek: if the next token is an IDENTIFIER, it's the alias.
    if (peek().type == TokenType::IDENTIFIER) {
        ref.alias = advance().lexeme;
    }

    return ref;
}

// join_clause := identifier "ON" expr
//                (JOIN already consumed)

JoinClause Parser::parseJoinClause() {
    JoinClause jc;

    jc.table = parseTableRef();

    expect(TokenType::ON,
           "after JOIN table reference");

    jc.on = parseExpr();

    return jc;
}

// order_by_item := identifier ("ASC" | "DESC")?

OrderByItem Parser::parseOrderByItem() {
    OrderByItem item;

    // Support qualified names (table.column) in ORDER BY
    item.column =
        expect(TokenType::IDENTIFIER,
               "in ORDER BY").lexeme;

    if (peek().type == TokenType::DOT) {
        advance();

        item.column +=
            "." +
            expect(TokenType::IDENTIFIER,
                   "after '.' in ORDER BY column").lexeme;
    }

    item.ascending = true;

    if (match(TokenType::DESC)) {
        item.ascending = false;
    } else {
        match(TokenType::ASC); // consume optional ASC
    }

    return item;
}

// Literal: integer, real, string, or NULL

Literal Parser::parseLiteral() {
    const Token& t = peek();

    if (t.type == TokenType::INTEGER_LITERAL) {
        advance();

        return Literal{
            static_cast<int64_t>(
                std::stoll(t.lexeme)
            )
        };
    }

    if (t.type == TokenType::REAL_LITERAL) {
        advance();

        return Literal{
            std::stod(t.lexeme)
        };
    }

    if (t.type == TokenType::STRING_LITERAL) {
        advance();

        return Literal{
            t.lexeme
        };
    }

    if (t.type == TokenType::NULL_KW) {
        advance();

        return Literal{
            std::monostate{}
        };
    }

    throw ParseError(
        "Expected a literal value (number, string, or NULL) but got '" +
        t.lexeme + "' at line " + std::to_string(t.line)
    );
}

// ── Expression parsing (precedence climbing) ──────────────────────────────────

// expr → or_expr

Expr Parser::parseExpr() {
    return parseOrExpr();
}

// or_expr → and_expr ("OR" and_expr)*

Expr Parser::parseOrExpr() {
    Expr left = parseAndExpr();

    while (peek().type == TokenType::OR) {
        advance();

        Expr right = parseAndExpr();

        left =
            makeBinaryOp(
                std::move(left),
                "OR",
                std::move(right)
            );
    }

    return left;
}

// and_expr → not_expr ("AND" not_expr)*

Expr Parser::parseAndExpr() {
    Expr left = parseNotExpr();

    while (peek().type == TokenType::AND) {
        advance();

        Expr right = parseNotExpr();

        left =
            makeBinaryOp(
                std::move(left),
                "AND",
                std::move(right)
            );
    }

    return left;
}

// not_expr → "NOT" not_expr | comparison

Expr Parser::parseNotExpr() {
    if (peek().type == TokenType::NOT) {
        advance();

        Expr operand =
            parseNotExpr();

        return makeUnaryOp(
            "NOT",
            std::move(operand)
        );
    }

    return parseComparison();
}

// comparison → operand comp_op operand
//            | operand  (if no comp_op follows — bare bool column, future)

Expr Parser::parseComparison() {
    Expr left =
        parseOperand();

    if (isCompOp()) {
        std::string op =
            advance().lexeme; // consume operator

        Expr right =
            parseOperand();

        left =
            makeBinaryOp(
                std::move(left),
                op,
                std::move(right)
            );
    }

    return left;
}

bool Parser::isCompOp() const {
    switch (peek().type) {
        case TokenType::EQUALS:
        case TokenType::NOT_EQUALS:
        case TokenType::LESS_THAN:
        case TokenType::GREATER_THAN:
        case TokenType::LESS_EQUALS:
        case TokenType::GREATER_EQUALS:
            return true;

        default:
            return false;
    }
}

// operand → column_ref | aggregate_call | literal | "(" expr ")"

Expr Parser::parseOperand() {
    const Token& t =
        peek();

    // Parenthesised sub-expression
    if (t.type == TokenType::LPAREN) {
        advance();

        Expr inner =
            parseExpr();

        expect(TokenType::RPAREN,
               "to close parenthesised expression");

        return inner;
    }

    // Literals
    if (t.type == TokenType::INTEGER_LITERAL ||
        t.type == TokenType::REAL_LITERAL    ||
        t.type == TokenType::STRING_LITERAL  ||
        t.type == TokenType::NULL_KW) {

        return Expr{
            parseLiteral()
        };
    }

    // IDENTIFIER — could be a plain column, a qualified table.column, or
    // an aggregate function call COUNT(...) / SUM(...) / ...
    if (t.type == TokenType::IDENTIFIER) {
        return parseColumnRefOrAggregate();
    }

    throw ParseError(
        "Expected an expression operand (column, literal, or '(') "
        "but got '" + t.lexeme +
        "' at line " + std::to_string(t.line)
    );
}

// Parses:
//   identifier
//   identifier.identifier
//   identifier "(" expr | "*" ")"

Expr Parser::parseColumnRefOrAggregate() {
    std::string name =
        advance().lexeme; // consume identifier

    // Check for aggregate function call: name "(" ...
    if (peek().type == TokenType::LPAREN) {

        // Determine if `name` is an aggregate function
        std::string upper = name;

        for (auto& c : upper) {
            c = static_cast<char>(
                std::toupper(
                    static_cast<unsigned char>(c)
                )
            );
        }

        AggFunc func;
        bool isAgg = true;

        if      (upper == "COUNT") func = AggFunc::COUNT;
        else if (upper == "SUM")   func = AggFunc::SUM;
        else if (upper == "AVG")   func = AggFunc::AVG;
        else if (upper == "MIN")   func = AggFunc::MIN;
        else if (upper == "MAX")   func = AggFunc::MAX;
        else isAgg = false;

        if (isAgg) {
            advance(); // consume '('

            if (peek().type == TokenType::STAR) {
                advance(); // consume '*'

                expect(
                    TokenType::RPAREN,
                    "after '*' in aggregate function"
                );

                return makeAggregate(
                    func,
                    true
                );
            }

            Expr arg =
                parseExpr();

            expect(
                TokenType::RPAREN,
                "after aggregate function argument"
            );

            return makeAggregate(
                func,
                false,
                std::move(arg)
            );
        }

        // If not an aggregate name, fall through — it will be a parse error
        // since we don't support arbitrary function calls in v1.
        throw ParseError(
            "Unknown function '" + name + "' at line " +
            std::to_string(previous().line) +
            ". Supported aggregate functions: COUNT, SUM, AVG, MIN, MAX."
        );
    }

    // Qualified column: identifier.identifier
    if (peek().type == TokenType::DOT) {
        advance(); // consume '.'

        std::string col =
            expect(
                TokenType::IDENTIFIER,
                "after '.' in qualified column name"
            ).lexeme;

        return makeColumnRef(
            name,
            col
        );
    }

    // Plain unqualified column
    return makeColumnRef(
        std::nullopt,
        name
    );
}