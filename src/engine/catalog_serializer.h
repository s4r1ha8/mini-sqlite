#pragma once

// ============================================================================
// catalog_serializer.h  -  Persist and restore the Catalog from page 0.
//
// Owner: Sarthak (engine layer)
//
// Format (all integers are little-endian, packed sequentially):
//
//   [magic:       4 bytes]  "MDB1"
//   [table_count: 4 bytes]
//   per table:
//     [name_len:     2 bytes]
//     [name:         name_len bytes]
//     [pk_len:       2 bytes]
//     [pk_name:      pk_len bytes]
//     [root_page_id: 4 bytes]
//     [col_count:    2 bytes]
//     per column:
//       [name_len:   2 bytes]
//       [name:       name_len bytes]
//       [type_tag:   1 byte]  0=INTEGER 1=TEXT 2=REAL
//       [flags:      1 byte]  bit0 = is_not_null
//     [index_count:  2 bytes]
//     per index:
//       [name_len:   2 bytes]
//       [name:       name_len bytes]
//       [col_len:    2 bytes]
//       [col_name:   col_len bytes]
//       [root_page_id: 4 bytes]
//
// If the catalog ever exceeds PAGE_SIZE bytes this needs chaining.
// At v1 data sizes that will never happen.
// ============================================================================

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "frontend/catalog.h"
#include "storage/page.h"
#include "common/ast.h"

// Magic bytes that mark a valid catalog page
static constexpr uint32_t CATALOG_MAGIC = 0x4D444231u; // "MDB1"

class CatalogSerializer {
public:

    // ---- Serialize catalog -> raw bytes ------------------------------------
    // Returns a byte vector of at most PAGE_SIZE bytes.
    // Throws if the catalog is too large (should not happen at v1 scale).
    static std::vector<std::byte> serialize(const Catalog& catalog) {
        std::vector<std::byte> buf;
        buf.reserve(PAGE_SIZE);

        write_u32(buf, CATALOG_MAGIC);

        const auto& tables = catalog.all_tables();
        write_u32(buf, static_cast<uint32_t>(tables.size()));

        for (const auto& [tname, schema] : tables) {
            write_str(buf, schema.name);
            write_str(buf, schema.primary_key);
            write_u32(buf, static_cast<uint32_t>(schema.root_page_id));

            write_u16(buf, static_cast<uint16_t>(schema.columns.size()));
            for (const auto& col : schema.columns) {
                write_str(buf, col.name);
                uint8_t type_tag = 0;
                switch (col.type) {
                    case DataType::INTEGER: type_tag = 0; break;
                    case DataType::TEXT:    type_tag = 1; break;
                    case DataType::REAL:    type_tag = 2; break;
                }
                buf.push_back(static_cast<std::byte>(type_tag));
                uint8_t flags = col.is_not_null ? 1u : 0u;
                buf.push_back(static_cast<std::byte>(flags));
            }

            write_u16(buf, static_cast<uint16_t>(schema.indexes.size()));
            for (const auto& idx : schema.indexes) {
                write_str(buf, idx.name);
                write_str(buf, idx.column);
                write_u32(buf, static_cast<uint32_t>(idx.root_page_id));
            }
        }

        if (buf.size() > PAGE_SIZE) {
            throw std::runtime_error(
                "CatalogSerializer: catalog exceeds PAGE_SIZE");
        }

        return buf;
    }

    // ---- Deserialize raw bytes -> Catalog ---------------------------------
    // Returns empty Catalog if page has no valid magic (fresh database).
    static Catalog deserialize(const std::array<std::byte, PAGE_SIZE>& data) {
        Catalog catalog;
        size_t pos = 0;

        uint32_t magic = read_u32(data, pos);
        if (magic != CATALOG_MAGIC) {
            return catalog; // fresh database, no catalog yet
        }

        uint32_t table_count = read_u32(data, pos);

        for (uint32_t t = 0; t < table_count; t++) {
            TableSchema schema;
            schema.name         = read_str(data, pos);
            schema.primary_key  = read_str(data, pos);
            schema.root_page_id = static_cast<int>(read_u32(data, pos));

            uint16_t col_count = read_u16(data, pos);
            for (uint16_t c = 0; c < col_count; c++) {
                ColumnInfo col;
                col.name    = read_str(data, pos);
                col.ordinal = static_cast<int>(c);

                uint8_t type_tag = static_cast<uint8_t>(data[pos++]);
                switch (type_tag) {
                    case 0: col.type = DataType::INTEGER; break;
                    case 1: col.type = DataType::TEXT;    break;
                    case 2: col.type = DataType::REAL;    break;
                    default:
                        throw std::runtime_error(
                            "CatalogSerializer: unknown type tag");
                }

                uint8_t flags = static_cast<uint8_t>(data[pos++]);
                col.is_not_null = (flags & 1u) != 0;

                schema.columns.push_back(std::move(col));
            }

            uint16_t index_count = read_u16(data, pos);
            for (uint16_t i = 0; i < index_count; i++) {
                IndexInfo idx;
                idx.name         = read_str(data, pos);
                idx.column       = read_str(data, pos);
                idx.root_page_id = static_cast<int>(read_u32(data, pos));
                schema.indexes.push_back(std::move(idx));
            }

            catalog.add_table(std::move(schema));
        }

        return catalog;
    }

private:
    static void write_u16(std::vector<std::byte>& buf, uint16_t v) {
        buf.push_back(static_cast<std::byte>(v & 0xFFu));
        buf.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
    }

    static void write_u32(std::vector<std::byte>& buf, uint32_t v) {
        buf.push_back(static_cast<std::byte>(v & 0xFFu));
        buf.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
        buf.push_back(static_cast<std::byte>((v >> 16) & 0xFFu));
        buf.push_back(static_cast<std::byte>((v >> 24) & 0xFFu));
    }

    static void write_str(std::vector<std::byte>& buf, const std::string& s) {
        write_u16(buf, static_cast<uint16_t>(s.size()));
        for (char c : s) buf.push_back(static_cast<std::byte>(c));
    }

    static uint16_t read_u16(
        const std::array<std::byte, PAGE_SIZE>& data, size_t& pos)
    {
        uint16_t v = static_cast<uint8_t>(data[pos])
                   | (static_cast<uint16_t>(static_cast<uint8_t>(data[pos+1])) << 8);
        pos += 2;
        return v;
    }

    static uint32_t read_u32(
        const std::array<std::byte, PAGE_SIZE>& data, size_t& pos)
    {
        uint32_t v = static_cast<uint8_t>(data[pos])
                   | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos+1])) << 8)
                   | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos+2])) << 16)
                   | (static_cast<uint32_t>(static_cast<uint8_t>(data[pos+3])) << 24);
        pos += 4;
        return v;
    }

    static std::string read_str(
        const std::array<std::byte, PAGE_SIZE>& data, size_t& pos)
    {
        uint16_t len = read_u16(data, pos);
        std::string s;
        s.reserve(len);
        for (uint16_t i = 0; i < len; i++) {
            s.push_back(static_cast<char>(data[pos++]));
        }
        return s;
    }
};
