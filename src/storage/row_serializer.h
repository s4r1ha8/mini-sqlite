#pragma once
#include "../common/row.h"
#include <vector>
#include <cstring>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────
//  Row Serializer
//
//  A Row is a vector of Values — nice for C++ code to work with,
//  but we can't just dump it to disk as-is (pointers, std::string
//  internal buffers, etc. don't survive a process restart).
//
//  We need to convert a Row into a flat sequence of bytes, and
//  be able to rebuild the Row from those bytes later. That's
//  serialization (writing) and deserialization (reading).
//
//  Format for each Value in a Row:
//    [type_tag: 1 byte]
//    then depending on type:
//      INT:  [int64_t: 8 bytes]
//      TEXT: [length: 4 bytes][chars: length bytes]
//      REAL: [double:  8 bytes]
//      NULL: nothing extra
// ─────────────────────────────────────────────────────────────────

enum class ValueTag : uint8_t {
    Int  = 0,
    Text = 1,
    Real = 2,
    Null = 3,
};

class RowSerializer {
public:

    // Convert a Row into a flat byte vector ready to store in a page.
    static std::vector<std::byte> serialize(const Row& row) {
        std::vector<std::byte> buf;

        for (const Value& val : row) {
            std::visit([&buf](const auto& v) {
                using T = std::decay_t<decltype(v)>;

                if constexpr (std::is_same_v<T, int64_t>) {
                    // Write tag
                    buf.push_back(static_cast<std::byte>(ValueTag::Int));
                    // Write 8 bytes of the integer
                    std::byte tmp[8];
                    std::memcpy(tmp, &v, 8);
                    buf.insert(buf.end(), tmp, tmp + 8);

                } else if constexpr (std::is_same_v<T, std::string>) {
                    buf.push_back(static_cast<std::byte>(ValueTag::Text));
                    // Write string length as 4 bytes
                    uint32_t len = static_cast<uint32_t>(v.size());
                    std::byte tmp[4];
                    std::memcpy(tmp, &len, 4);
                    buf.insert(buf.end(), tmp, tmp + 4);
                    // Write the actual characters
                    for (char c : v) buf.push_back(static_cast<std::byte>(c));

                } else if constexpr (std::is_same_v<T, double>) {
                    buf.push_back(static_cast<std::byte>(ValueTag::Real));
                    std::byte tmp[8];
                    std::memcpy(tmp, &v, 8);
                    buf.insert(buf.end(), tmp, tmp + 8);

                } else if constexpr (std::is_same_v<T, std::monostate>) {
                    // NULL: just the tag, no data
                    buf.push_back(static_cast<std::byte>(ValueTag::Null));
                }
            }, val);
        }

        return buf;
    }

    // Rebuild a Row from the flat bytes we stored earlier.
    // `num_columns` tells us how many Values to read back.
    static Row deserialize(const std::vector<std::byte>& buf, size_t num_columns) {
        Row row;
        size_t pos = 0;

        for (size_t i = 0; i < num_columns; i++) {
            if (pos >= buf.size()) {
                throw std::runtime_error("Deserialize: ran out of bytes");
            }

            auto tag = static_cast<ValueTag>(static_cast<uint8_t>(buf[pos]));
            pos++;

            switch (tag) {
                case ValueTag::Int: {
                    int64_t v;
                    std::memcpy(&v, buf.data() + pos, 8);
                    pos += 8;
                    row.push_back(Value(v));
                    break;
                }
                case ValueTag::Text: {
                    uint32_t len;
                    std::memcpy(&len, buf.data() + pos, 4);
                    pos += 4;
                    std::string s(reinterpret_cast<const char*>(buf.data() + pos), len);
                    pos += len;
                    row.push_back(Value(s));
                    break;
                }
                case ValueTag::Real: {
                    double v;
                    std::memcpy(&v, buf.data() + pos, 8);
                    pos += 8;
                    row.push_back(Value(v));
                    break;
                }
                case ValueTag::Null: {
                    row.push_back(Value(std::monostate{}));
                    break;
                }
                default:
                    throw std::runtime_error("Deserialize: unknown value tag");
            }
        }

        return row;
    }
};
