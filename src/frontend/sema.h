#pragma once

// ============================================================================
// sema.h  —  Semantic analysis pass.
//
// The parser guarantees syntactic correctness (grammar rules).
// The semantic analyser guarantees *meaning* correctness:
//   "Does this table exist?  Does this column exist?  Do the types match?"
//
// This is the "colorless green ideas" checker — catching things like
//   SELECT bananas FROM users;
// where "bananas" is syntactically a valid identifier but semantically wrong
// because the users table has no bananas column.
//
// SemanticAnalyzer::analyze() either:
//   • returns cleanly (query is valid), and as a side-effect:
//       – executes CREATE TABLE / CREATE INDEX against the Catalog
//   • throws SemanticError with a descriptive message
// ============================================================================

#include <stdexcept>
#include <string>
#include "common/ast.h"
#include "frontend/catalog.h"

// ── SemanticError — thrown on any semantic rule violation ─────────────────────
class SemanticError : public std::runtime_error {
public:
    explicit SemanticError(const std::string& msg)
        : std::runtime_error("Semantic error: " + msg) {}
};

// ── SemanticAnalyzer ──────────────────────────────────────────────────────────
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(Catalog& catalog);

    // Validate `stmt` against the catalog.
    // For DDL statements (CREATE TABLE / INDEX), also updates the catalog.
    void analyze(const Statement& stmt);

private:
    Catalog& catalog_;

    // Per-statement visitors
    void analyzeCreateTable(const CreateTableStmt& stmt);
    void analyzeCreateIndex(const CreateIndexStmt& stmt);
    void analyzeInsert(const InsertStmt& stmt);
    void analyzeSelect(const SelectStmt& stmt);
    void analyzeUpdate(const UpdateStmt& stmt);
    void analyzeDelete(const DeleteStmt& stmt);

    // Check that an Expr references only columns that exist in the given table
    // set.  `tables` maps alias-or-name → real table name.
    void analyzeExpr(const Expr& expr,
                     const std::unordered_map<std::string, std::string>& tables) const;

    // Resolve a column reference: return the real table name if found.
    // Throws SemanticError if the column can't be resolved unambiguously.
    std::string resolveColumn(const ColumnRef& ref,
                              const std::unordered_map<std::string, std::string>& tables) const;
};
