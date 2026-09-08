#include "token.h"

std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::SELECT:          return "SELECT";
        case TokenType::FROM:            return "FROM";
        case TokenType::WHERE:           return "WHERE";
        case TokenType::INSERT:          return "INSERT";
        case TokenType::INTO:            return "INTO";
        case TokenType::VALUES:          return "VALUES";
        case TokenType::UPDATE:          return "UPDATE";
        case TokenType::SET:             return "SET";
        case TokenType::DELETE:          return "DELETE";
        case TokenType::CREATE:          return "CREATE";
        case TokenType::TABLE:           return "TABLE";
        case TokenType::INDEX:           return "INDEX";
        case TokenType::ON:              return "ON";
        case TokenType::JOIN:            return "JOIN";
        case TokenType::AND:             return "AND";
        case TokenType::OR:              return "OR";
        case TokenType::NOT:             return "NOT";
        case TokenType::ORDER:           return "ORDER";
        case TokenType::BY:              return "BY";
        case TokenType::GROUP:           return "GROUP";
        case TokenType::HAVING:          return "HAVING";
        case TokenType::LIMIT:           return "LIMIT";
        case TokenType::BEGIN:           return "BEGIN";
        case TokenType::COMMIT:          return "COMMIT";
        case TokenType::ROLLBACK:        return "ROLLBACK";
        case TokenType::PRIMARY:         return "PRIMARY";
        case TokenType::KEY:             return "KEY";
        case TokenType::NULL_KW:         return "NULL";
        case TokenType::INTEGER:         return "INTEGER";
        case TokenType::TEXT:            return "TEXT";
        case TokenType::REAL:            return "REAL";
        case TokenType::ASC:             return "ASC";
        case TokenType::DESC:            return "DESC";
        case TokenType::IDENTIFIER:      return "IDENTIFIER";
        case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
        case TokenType::STRING_LITERAL:  return "STRING_LITERAL";
        case TokenType::REAL_LITERAL:    return "REAL_LITERAL";
        case TokenType::EQUALS:          return "EQUALS";
        case TokenType::NOT_EQUALS:      return "NOT_EQUALS";
        case TokenType::LESS_THAN:       return "LESS_THAN";
        case TokenType::GREATER_THAN:    return "GREATER_THAN";
        case TokenType::LESS_EQUALS:     return "LESS_EQUALS";
        case TokenType::GREATER_EQUALS:  return "GREATER_EQUALS";
        case TokenType::COMMA:           return "COMMA";
        case TokenType::SEMICOLON:       return "SEMICOLON";
        case TokenType::LPAREN:          return "LPAREN";
        case TokenType::RPAREN:          return "RPAREN";
        case TokenType::STAR:            return "STAR";
        case TokenType::DOT:             return "DOT";
        case TokenType::END_OF_FILE:     return "EOF";
        case TokenType::UNKNOWN:         return "UNKNOWN";
    }
    return "UNKNOWN";
}
