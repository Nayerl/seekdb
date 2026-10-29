/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef OCEANBASE_SQL_OB_FILE_SQL_COPY_COMPAT_H_
#define OCEANBASE_SQL_OB_FILE_SQL_COPY_COMPAT_H_

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql_copy {

inline void space(const std::string &sql, size_t &pos)
{
  while (pos < sql.size() && std::isspace(static_cast<unsigned char>(sql[pos]))) { ++pos; }
}

inline std::string trim(const std::string &value)
{
  size_t first = 0, last = value.size();
  while (first < last && std::isspace(static_cast<unsigned char>(value[first]))) { ++first; }
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) { --last; }
  return value.substr(first, last - first);
}

inline bool ident_char(char ch)
{
  return std::isalnum(static_cast<unsigned char>(ch)) || '_' == ch;
}

inline bool word_at(const std::string &sql, size_t pos, const char *word)
{
  const size_t length = std::strlen(word);
  if (pos + length > sql.size() || (pos && ident_char(sql[pos - 1]))
      || (pos + length < sql.size() && ident_char(sql[pos + length]))) { return false; }
  for (size_t i = 0; i < length; ++i) {
    if (std::tolower(static_cast<unsigned char>(sql[pos + i]))
        != std::tolower(static_cast<unsigned char>(word[i]))) { return false; }
  }
  return true;
}

inline bool consume(const std::string &sql, size_t &pos, const char *word)
{
  space(sql, pos);
  if (!word_at(sql, pos, word)) { return false; }
  pos += std::strlen(word);
  return true;
}

inline bool literal(const std::string &sql, size_t &pos, std::string &value)
{
  space(sql, pos);
  if (pos >= sql.size() || '\'' != sql[pos++]) { return false; }
  value.clear();
  while (pos < sql.size()) {
    const char ch = sql[pos++];
    if ('\'' == ch) {
      if (pos < sql.size() && '\'' == sql[pos]) { value.push_back('\''); ++pos; }
      else { return true; }
    } else if ('\\' == ch || 0 == ch || static_cast<unsigned char>(ch) < 32) {
      return false; // Avoid SQL-mode-dependent escapes and path control bytes.
    } else {
      value.push_back(ch);
    }
  }
  return false;
}

inline std::string quote(const std::string &value)
{
  std::string sql = "'";
  for (char ch : value) {
    sql.push_back(ch);
    if ('\'' == ch) { sql.push_back(ch); }
  }
  return sql + "'";
}

inline bool close_paren(const std::string &sql, size_t opening, size_t &closing)
{
  int depth = 0;
  for (size_t pos = opening; pos < sql.size(); ++pos) {
    if ('\'' == sql[pos]) {
      std::string ignored;
      if (!literal(sql, pos, ignored)) { return false; }
      --pos;
    } else if ('(' == sql[pos]) {
      ++depth;
    } else if (')' == sql[pos] && 0 == --depth) {
      closing = pos;
      return true;
    }
  }
  return false;
}

inline size_t top_word(const std::string &sql, const char *word)
{
  int depth = 0;
  for (size_t pos = 0; pos < sql.size(); ++pos) {
    if ('\'' == sql[pos]) {
      std::string ignored;
      if (!literal(sql, pos, ignored)) { return std::string::npos; }
      --pos;
    } else if ('(' == sql[pos]) {
      ++depth;
    } else if (')' == sql[pos]) {
      --depth;
    } else if (0 == depth && word_at(sql, pos, word)) {
      return pos;
    }
  }
  return std::string::npos;
}

inline bool column_name(const std::string &source, std::string &name)
{
  const std::string text = trim(source);
  if (text.empty()) { return false; }
  size_t start = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || '.' == text[i]) {
      if (i == start || !(std::isalpha(static_cast<unsigned char>(text[start]))
                          || '_' == text[start])) { return false; }
      for (size_t j = start + 1; j < i; ++j) {
        if (!ident_char(text[j])) { return false; }
      }
      name = text.substr(start, i - start);
      start = i + 1;
    }
  }
  return true;
}

// Support the bounded DuckDB COPY form used by File SQL. Unknown forms remain
// with the normal SQL parser, rather than being guessed or executed as a file write.
inline bool rewrite(const std::string &sql, std::string &out)
{
  size_t pos = 0, body_end = 0, opts_end = 0;
  if (!consume(sql, pos, "COPY")) { return false; }
  space(sql, pos);
  if (pos >= sql.size() || '(' != sql[pos] || !close_paren(sql, pos, body_end)) { return false; }
  const std::string body = trim(sql.substr(pos + 1, body_end - pos - 1));
  pos = body_end + 1;
  if (!consume(sql, pos, "TO")) { return false; }
  std::string path;
  if (!literal(sql, pos, path) || path.empty() || '/' != path[0]) { return false; }
  space(sql, pos);
  if (pos >= sql.size() || '(' != sql[pos] || !close_paren(sql, pos, opts_end)) { return false; }
  const std::string options = sql.substr(pos + 1, opts_end - pos - 1);
  pos = opts_end + 1;
  space(sql, pos);
  if (pos < sql.size() && ';' == sql[pos]) { ++pos; space(sql, pos); }
  if (pos != sql.size()) { return false; }
  pos = 0;
  if (!consume(options, pos, "FORMAT") || !consume(options, pos, "CSV")) { return false; }
  space(options, pos);
  bool header = false;
  if (pos < options.size() && ',' == options[pos]) {
    ++pos;
    header = consume(options, pos, "HEADER");
    if (!header) { return false; }
  }
  std::string nullstr;
  space(options, pos);
  if (pos < options.size() && ',' == options[pos]) {
    ++pos;
    if (!consume(options, pos, "NULLSTR") || !literal(options, pos, nullstr)) { return false; }
  }
  space(options, pos);
  if (pos != options.size()) { return false; }
  pos = 0;
  if (!consume(body, pos, "SELECT")) { return false; }
  const size_t from = top_word(body, "FROM");
  if (std::string::npos == from || from <= pos) { return false; }
  std::vector<std::string> names;
  const std::string projection = body.substr(pos, from - pos);
  size_t begin = 0;
  while (begin < projection.size()) {
    const size_t comma = projection.find(',', begin);
    std::string name;
    if (!column_name(projection.substr(begin, comma - begin), name)) { return false; }
    names.push_back(name);
    if (names.size() > 256 || std::string::npos == comma) { break; }
    begin = comma + 1;
  }
  if (names.empty()) { return false; }
  std::string sort_name;
  bool descending = false;
  const size_t order = top_word(body, "ORDER");
  if (std::string::npos != order) {
    size_t cursor = order + 5;
    if (!consume(body, cursor, "BY")) { return false; }
    space(body, cursor);
    const size_t start = cursor;
    while (cursor < body.size() && (ident_char(body[cursor]) || '.' == body[cursor])) { ++cursor; }
    if (!column_name(body.substr(start, cursor - start), sort_name)) { return false; }
    bool found = false;
    for (const std::string &name : names) { found |= name == sort_name; }
    if (!found) { return false; }
    size_t direction = cursor;
    descending = consume(body, direction, "DESC");
  }
  const std::string outfile = " INTO OUTFILE " + quote(path)
      + " FORMAT=(TYPE='CSV', FIELD_DELIMITER=',') SINGLE=TRUE";
  if (!header) {
    out = body + outfile;
  } else {
    std::string header_sql = "SELECT 0 AS __fs_ord, 0 AS __fs_sort";
    std::string data_sql = "SELECT 1, " + (sort_name.empty() ? "0" : "src." + sort_name);
    std::string selected;
    for (const std::string &name : names) {
      header_sql += ", " + quote(name) + " AS " + name;
      data_sql += ", IFNULL(CAST(src." + name + " AS CHAR), " + quote(nullstr) + ") AS " + name;
      if (!selected.empty()) { selected += ", "; }
      selected += name;
    }
    out = "SELECT " + selected + " FROM (" + header_sql + " UNION ALL " + data_sql
        + " FROM (" + body + ") AS src) AS __fs_rows ORDER BY __fs_ord, __fs_sort";
    if (descending) { out += " DESC"; }
    out += outfile;
  }
  return true;
}

} } } // namespace oceanbase::sql::file_sql_copy

#endif // OCEANBASE_SQL_OB_FILE_SQL_COPY_COMPAT_H_
