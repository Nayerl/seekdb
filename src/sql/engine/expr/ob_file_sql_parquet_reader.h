/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef OCEANBASE_SQL_OB_FILE_SQL_PARQUET_READER_H_
#define OCEANBASE_SQL_OB_FILE_SQL_PARQUET_READER_H_

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql_parquet {

struct Cell {
  std::string text;
  bool is_null = true;
};

struct Column {
  std::string name;
  int64_t physical = -1;
  int64_t converted = -1;
  int64_t repetition = -1;
};

struct Result {
  std::vector<Column> columns;
  std::vector<std::vector<Cell>> rows;
};

// Parquet metadata and page headers use the Thrift compact protocol. Keep
// unknown fields parseable so newer writers can add metadata safely.
struct Value {
  int64_t integer = 0;
  bool boolean = false;
  std::string text;
  std::vector<Value> list;
  std::map<int64_t, Value> fields;
};

class CompactReader {
public:
  CompactReader(const uint8_t *data, size_t size, size_t pos = 0)
    : data_(data), size_(size), pos_(pos) {}
  size_t position() const { return pos_; }

  bool read_struct(Value &out, int depth = 0)
  {
    if (depth > 24) { return false; }
    int64_t last_id = 0;
    for (int count = 0; count < 100000; ++count) {
      uint8_t header = 0;
      if (!byte(header)) { return false; }
      const uint8_t type = header & 15;
      if (0 == type) { return true; }
      const uint8_t delta = header >> 4;
      int64_t field_id = last_id + delta;
      if (0 == delta) {
        uint64_t raw = 0;
        if (!varint(raw)) { return false; }
        field_id = zigzag(raw);
      }
      if (field_id <= 0) { return false; }
      last_id = field_id;
      Value value;
      if (!read_value(type, value, depth + 1)) { return false; }
      out.fields[field_id] = std::move(value);
    }
    return false;
  }

private:
  static int64_t zigzag(uint64_t v)
  {
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }

  bool byte(uint8_t &out)
  {
    if (pos_ >= size_) { return false; }
    out = data_[pos_++];
    return true;
  }

  bool varint(uint64_t &out)
  {
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      uint8_t b = 0;
      if (!byte(b) || (63 == shift && b > 1)) { return false; }
      out |= static_cast<uint64_t>(b & 127) << shift;
      if (b < 128) { return true; }
    }
    return false;
  }

  bool read_value(uint8_t type, Value &out, int depth)
  {
    if (depth > 24) { return false; }
    if (1 == type || 2 == type) {
      out.boolean = 1 == type;
      return true;
    } else if (3 == type) {
      uint8_t b = 0;
      if (!byte(b)) { return false; }
      out.integer = static_cast<int8_t>(b);
      return true;
    } else if (4 == type || 5 == type || 6 == type) {
      uint64_t v = 0;
      if (!varint(v)) { return false; }
      out.integer = zigzag(v);
      return true;
    } else if (7 == type) {
      if (size_ - pos_ < 8) { return false; }
      pos_ += 8; // No floating-point footer fields are currently consumed.
      return true;
    } else if (8 == type) {
      uint64_t length = 0;
      if (!varint(length) || length > size_ - pos_) { return false; }
      out.text.assign(reinterpret_cast<const char *>(data_ + pos_), length);
      pos_ += static_cast<size_t>(length);
      return true;
    } else if (9 == type || 10 == type) {
      uint8_t header = 0;
      if (!byte(header)) { return false; }
      uint64_t count = header >> 4;
      if (15 == count && !varint(count)) { return false; }
      if (count > 1000000) { return false; }
      const uint8_t item_type = header & 15;
      out.list.reserve(static_cast<size_t>(count));
      for (uint64_t i = 0; i < count; ++i) {
        Value item;
        if (!read_value(item_type, item, depth + 1)) { return false; }
        out.list.push_back(std::move(item));
      }
      return true;
    } else if (11 == type) {
      uint64_t count = 0;
      if (!varint(count) || count > 1000000) { return false; }
      if (count) {
        uint8_t types = 0;
        if (!byte(types)) { return false; }
        for (uint64_t i = 0; i < count; ++i) {
          Value key, value;
          if (!read_value(types >> 4, key, depth + 1)
              || !read_value(types & 15, value, depth + 1)) { return false; }
        }
      }
      return true;
    } else if (12 == type) {
      return read_struct(out, depth + 1);
    }
    return false;
  }

  const uint8_t *data_;
  size_t size_;
  size_t pos_;
};

inline const Value *field(const Value &value, int64_t id)
{
  const auto it = value.fields.find(id);
  return it == value.fields.end() ? nullptr : &it->second;
}

inline bool integer_field(const Value &value, int64_t id, int64_t &out)
{
  const Value *found = field(value, id);
  if (nullptr == found) { return false; }
  out = found->integer;
  return true;
}

inline uint32_t little32(const uint8_t *bytes)
{
  return static_cast<uint32_t>(bytes[0])
      | (static_cast<uint32_t>(bytes[1]) << 8)
      | (static_cast<uint32_t>(bytes[2]) << 16)
      | (static_cast<uint32_t>(bytes[3]) << 24);
}

inline uint64_t little64(const uint8_t *bytes)
{
  return static_cast<uint64_t>(little32(bytes))
      | (static_cast<uint64_t>(little32(bytes + 4)) << 32);
}

inline bool unsigned_varint(const uint8_t *data, size_t size, size_t &pos, uint64_t &out)
{
  out = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (pos >= size) { return false; }
    const uint8_t b = data[pos++];
    if (63 == shift && b > 1) { return false; }
    out |= static_cast<uint64_t>(b & 127) << shift;
    if (b < 128) { return true; }
  }
  return false;
}

// Snappy's block format is small enough to decode directly. The Parquet page
// supplies the expected uncompressed byte count, which we verify exactly.
inline bool snappy_decode(const uint8_t *data, size_t size, size_t expected,
                          std::vector<uint8_t> &out)
{
  out.clear();
  size_t pos = 0;
  uint64_t declared = 0;
  if (!unsigned_varint(data, size, pos, declared) || declared != expected
      || expected > 64 * 1024 * 1024) { return false; }
  out.reserve(expected);
  while (pos < size && out.size() < expected) {
    const uint8_t tag = data[pos++];
    const uint8_t kind = tag & 3;
    size_t count = 0;
    if (0 == kind) {
      const uint8_t prefix = tag >> 2;
      if (prefix < 60) {
        count = static_cast<size_t>(prefix) + 1;
      } else {
        const size_t extra = prefix - 59;
        if (extra > size - pos) { return false; }
        for (size_t i = 0; i < extra; ++i) {
          count |= static_cast<size_t>(data[pos++]) << (8 * i);
        }
        ++count;
      }
      if (count > expected - out.size() || count > size - pos) { return false; }
      out.insert(out.end(), data + pos, data + pos + count);
      pos += count;
    } else {
      size_t offset = 0;
      if (1 == kind) {
        if (pos >= size) { return false; }
        count = ((tag >> 2) & 7) + 4;
        offset = (static_cast<size_t>(tag & 0xe0) << 3) | data[pos++];
      } else if (2 == kind) {
        if (size - pos < 2) { return false; }
        count = (tag >> 2) + 1;
        offset = static_cast<size_t>(data[pos]) | (static_cast<size_t>(data[pos + 1]) << 8);
        pos += 2;
      } else {
        if (size - pos < 4) { return false; }
        count = (tag >> 2) + 1;
        offset = little32(data + pos);
        pos += 4;
      }
      if (0 == offset || offset > out.size() || count > expected - out.size()) {
        return false;
      }
      for (size_t i = 0; i < count; ++i) {
        out.push_back(out[out.size() - offset]);
      }
    }
  }
  return out.size() == expected && pos == size;
}

inline bool definition_levels(const uint8_t *data, size_t size, size_t count,
                              std::vector<uint8_t> &levels)
{
  levels.clear();
  size_t pos = 0;
  while (levels.size() < count && pos < size) {
    uint64_t header = 0;
    if (!unsigned_varint(data, size, pos, header) || 0 == header) { return false; }
    const uint64_t run = header >> 1;
    if (run > count + 7) { return false; }
    if (0 == (header & 1)) {
      if (pos >= size) { return false; }
      const uint8_t level = data[pos++];
      if (level > 1) { return false; }
      for (uint64_t i = 0; i < run && levels.size() < count; ++i) {
        levels.push_back(level);
      }
    } else {
      if (run > size - pos) { return false; }
      for (uint64_t i = 0; i < run && levels.size() < count; ++i) {
        const uint8_t bits = data[pos + i];
        for (int bit = 0; bit < 8 && levels.size() < count; ++bit) {
          levels.push_back((bits >> bit) & 1);
        }
      }
      pos += static_cast<size_t>(run);
    }
  }
  return levels.size() == count;
}

inline bool plain_cell(const uint8_t *data, size_t size, size_t &pos,
                       size_t non_null_index, const Column &column, Cell &cell)
{
  std::ostringstream formatted;
  if (0 == column.physical) { // BOOLEAN: packed one bit per present value.
    const size_t byte_index = non_null_index / 8;
    if (byte_index >= size) { return false; }
    cell.text = (data[byte_index] & (1U << (non_null_index & 7))) ? "true" : "false";
    pos = (non_null_index + 8) / 8;
  } else if (1 == column.physical) { // INT32
    if (size - pos < 4) { return false; }
    const int32_t value = static_cast<int32_t>(little32(data + pos));
    pos += 4;
    if (6 == column.converted) { // DATE, days from 1970-01-01.
      const time_t seconds = static_cast<time_t>(value) * 86400;
      struct tm utc = {};
      if (nullptr == ::gmtime_r(&seconds, &utc)) { return false; }
      formatted << std::setfill('0') << std::setw(4) << utc.tm_year + 1900 << '-'
                << std::setw(2) << utc.tm_mon + 1 << '-' << std::setw(2) << utc.tm_mday;
    } else {
      formatted << value;
    }
    cell.text = formatted.str();
  } else if (2 == column.physical) { // INT64
    if (size - pos < 8) { return false; }
    formatted << static_cast<int64_t>(little64(data + pos));
    pos += 8;
    cell.text = formatted.str();
  } else if (4 == column.physical) { // FLOAT
    if (size - pos < 4) { return false; }
    const uint32_t bits = little32(data + pos);
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) { return false; }
    formatted << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    pos += 4;
    cell.text = formatted.str();
  } else if (5 == column.physical) { // DOUBLE
    if (size - pos < 8) { return false; }
    const uint64_t bits = little64(data + pos);
    double value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) { return false; }
    formatted << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    pos += 8;
    cell.text = formatted.str();
  } else if (6 == column.physical) { // BYTE_ARRAY
    if (size - pos < 4) { return false; }
    const uint32_t length = little32(data + pos);
    pos += 4;
    if (length > size - pos) { return false; }
    cell.text.assign(reinterpret_cast<const char *>(data + pos), length);
    pos += length;
  } else {
    return false;
  }
  cell.is_null = false;
  return true;
}

inline bool decode_page(const uint8_t *data, size_t size, size_t expected,
                        int64_t codec, bool compressed, std::vector<uint8_t> &out)
{
  if (!compressed || 0 == codec) {
    if (size != expected) { return false; }
    out.assign(data, data + size);
    return true;
  }
  return 1 == codec && snappy_decode(data, size, expected, out);
}

inline bool read_column_pages(const std::vector<uint8_t> &bytes, size_t footer_offset,
                              const Value &chunk, const Column &column,
                              size_t row_offset, size_t row_count, size_t column_index,
                              Result &result)
{
  const Value *meta = field(chunk, 3);
  if (nullptr == meta || nullptr != field(chunk, 1)) { return false; }
  int64_t physical = -1, codec = -1, data_offset = -1, value_count = -1;
  if (!integer_field(*meta, 1, physical) || physical != column.physical
      || !integer_field(*meta, 4, codec)
      || !integer_field(*meta, 5, value_count) || value_count != static_cast<int64_t>(row_count)
      || !integer_field(*meta, 9, data_offset) || data_offset < 4) { return false; }
  const Value *path = field(*meta, 3);
  if (nullptr == path || path->list.size() != 1 || path->list[0].text != column.name) {
    return false;
  }
  int64_t dictionary_offset = -1;
  size_t position = static_cast<size_t>(data_offset);
  if (integer_field(*meta, 11, dictionary_offset)
      && dictionary_offset >= 4 && dictionary_offset < data_offset) {
    position = static_cast<size_t>(dictionary_offset);
  }
  size_t processed = 0;
  while (processed < row_count && position < footer_offset) {
    CompactReader header_reader(bytes.data(), footer_offset, position);
    Value header;
    if (!header_reader.read_struct(header)) { return false; }
    int64_t page_type = -1, uncompressed = -1, compressed = -1;
    if (!integer_field(header, 1, page_type)
        || !integer_field(header, 2, uncompressed)
        || !integer_field(header, 3, compressed)
        || uncompressed < 0 || compressed < 0
        || uncompressed > 64 * 1024 * 1024
        || compressed > static_cast<int64_t>(footer_offset - header_reader.position())) {
      return false;
    }
    const size_t body_offset = header_reader.position();
    position = body_offset + static_cast<size_t>(compressed);
    if (2 == page_type) { // Dictionary pages are rejected below if referenced.
      continue;
    }
    int64_t num_values = -1, encoding = -1;
    size_t definition_size = 0;
    std::vector<uint8_t> decoded;
    const uint8_t *body = bytes.data() + body_offset;
    const uint8_t *values = nullptr;
    size_t values_size = 0;
    if (3 == page_type) { // DATA_PAGE_V2
      const Value *detail = field(header, 8);
      int64_t num_nulls = -1, num_rows = -1, def_bytes = -1, rep_bytes = -1;
      if (nullptr == detail || !integer_field(*detail, 1, num_values)
          || !integer_field(*detail, 2, num_nulls)
          || !integer_field(*detail, 3, num_rows)
          || !integer_field(*detail, 4, encoding)
          || !integer_field(*detail, 5, def_bytes)
          || !integer_field(*detail, 6, rep_bytes)
          || num_values < 0 || num_rows != num_values
          || num_nulls < 0 || num_nulls > num_values
          || def_bytes < 0 || rep_bytes != 0
          || def_bytes > compressed || def_bytes > uncompressed) { return false; }
      definition_size = static_cast<size_t>(def_bytes);
      const Value *compression_flag = field(*detail, 7);
      const bool values_compressed = nullptr == compression_flag || compression_flag->boolean;
      const size_t compressed_values = static_cast<size_t>(compressed) - definition_size;
      const size_t uncompressed_values = static_cast<size_t>(uncompressed) - definition_size;
      if (!decode_page(body + definition_size, compressed_values, uncompressed_values,
                       codec, values_compressed, decoded)) { return false; }
      values = decoded.data();
      values_size = decoded.size();
    } else if (0 == page_type) { // DATA_PAGE
      const Value *detail = field(header, 5);
      if (nullptr == detail || !integer_field(*detail, 1, num_values)
          || !integer_field(*detail, 2, encoding) || num_values < 0
          || !decode_page(body, static_cast<size_t>(compressed),
                          static_cast<size_t>(uncompressed), codec, true, decoded)) {
        return false;
      }
      values = decoded.data();
      values_size = decoded.size();
      if (1 == column.repetition) {
        if (values_size < 4) { return false; }
        definition_size = little32(values);
        if (definition_size > values_size - 4) { return false; }
        values += 4;
        values_size -= 4;
      }
    } else {
      return false;
    }
    if (0 != encoding || static_cast<uint64_t>(num_values) > row_count - processed) {
      return false; // Only PLAIN scalar data is supported; never silently misdecode.
    }
    std::vector<uint8_t> levels;
    if (1 == column.repetition) {
      if (!definition_levels(3 == page_type ? body : values, definition_size,
                             static_cast<size_t>(num_values), levels)) { return false; }
      if (0 == page_type) {
        values += definition_size;
        values_size -= definition_size;
      }
    } else if (0 == column.repetition) {
      levels.assign(static_cast<size_t>(num_values), 1);
    } else {
      return false; // Nested/repeated columns need a separate row assembler.
    }
    size_t value_position = 0;
    size_t non_null_index = 0;
    for (size_t i = 0; i < static_cast<size_t>(num_values); ++i) {
      if (levels[i]) {
        Cell &cell = result.rows[row_offset + processed + i][column_index];
        if (!plain_cell(values, values_size, value_position, non_null_index, column, cell)) {
          return false;
        }
        ++non_null_index;
      }
    }
    processed += static_cast<size_t>(num_values);
  }
  return processed == row_count;
}

inline bool load(FILE *file, Result &result, bool include_rows)
{
  result.columns.clear();
  result.rows.clear();
  if (nullptr == file || 0 != ::fseek(file, 0, SEEK_END)) { return false; }
  const long file_size = ::ftell(file);
  if (file_size < 12 || file_size > 64 * 1024 * 1024
      || 0 != ::fseek(file, 0, SEEK_SET)) { return false; }
  std::vector<uint8_t> bytes(static_cast<size_t>(file_size));
  if (bytes.size() != ::fread(bytes.data(), 1, bytes.size(), file)
      || 0 != std::memcmp(bytes.data(), "PAR1", 4)
      || 0 != std::memcmp(bytes.data() + bytes.size() - 4, "PAR1", 4)) {
    return false;
  }
  const size_t footer_size = little32(bytes.data() + bytes.size() - 8);
  if (footer_size > bytes.size() - 12) { return false; }
  const size_t footer_offset = bytes.size() - 8 - footer_size;
  CompactReader footer_reader(bytes.data(), bytes.size() - 8, footer_offset);
  Value footer;
  if (!footer_reader.read_struct(footer)) { return false; }
  const Value *schema = field(footer, 2);
  const Value *groups = field(footer, 4);
  int64_t total_rows = -1, root_children = -1;
  if (nullptr == schema || schema->list.size() < 2 || nullptr == groups
      || !integer_field(footer, 3, total_rows) || total_rows < 0
      || total_rows > 1000000
      || !integer_field(schema->list[0], 5, root_children)
      || root_children != static_cast<int64_t>(schema->list.size() - 1)
      || root_children > 256) { return false; }
  for (size_t i = 1; i < schema->list.size(); ++i) {
    const Value &element = schema->list[i];
    const Value *name = field(element, 4);
    Column column;
    if (nullptr == name || name->text.empty()
        || !integer_field(element, 1, column.physical)
        || !integer_field(element, 3, column.repetition)
        || (column.repetition != 0 && column.repetition != 1)
        || (column.physical != 0 && column.physical != 1 && column.physical != 2
            && column.physical != 4 && column.physical != 5 && column.physical != 6)) {
      return false;
    }
    column.name = name->text;
    integer_field(element, 6, column.converted);
    result.columns.push_back(std::move(column));
  }
  if (!include_rows) { return true; }
  result.rows.assign(static_cast<size_t>(total_rows), std::vector<Cell>(result.columns.size()));
  size_t row_offset = 0;
  for (const Value &group : groups->list) {
    int64_t group_rows = -1;
    const Value *chunks = field(group, 1);
    if (nullptr == chunks || chunks->list.size() != result.columns.size()
        || !integer_field(group, 3, group_rows) || group_rows < 0
        || static_cast<uint64_t>(group_rows) > result.rows.size() - row_offset) {
      return false;
    }
    for (size_t col = 0; col < result.columns.size(); ++col) {
      if (!read_column_pages(bytes, footer_offset, chunks->list[col], result.columns[col],
                             row_offset, static_cast<size_t>(group_rows), col, result)) {
        return false;
      }
    }
    row_offset += static_cast<size_t>(group_rows);
  }
  return row_offset == result.rows.size();
}

} } } // namespace oceanbase::sql::file_sql_parquet

#endif // OCEANBASE_SQL_OB_FILE_SQL_PARQUET_READER_H_
