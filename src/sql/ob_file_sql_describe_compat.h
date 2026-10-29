/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#ifndef OCEANBASE_SQL_OB_FILE_SQL_DESCRIBE_COMPAT_H_
#define OCEANBASE_SQL_OB_FILE_SQL_DESCRIBE_COMPAT_H_

#include "sql/ob_file_sql_copy_compat.h"

#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql_describe {

struct Column
{
  std::string name;
  std::string type;
  bool nullable = false;
};

// Ordinary DESCRIBE continues down the existing EXPLAIN path. File SQL's
// DuckDB-style schema form is recognized only for a single CSV table source.
inline bool parse_path(const std::string &sql, std::string &path)
{
  size_t pos = 0;
  if (!file_sql_copy::consume(sql, pos, "DESCRIBE")) {
    if (!file_sql_copy::consume(sql, pos, "DESC")) { return false; }
  }
  if (!file_sql_copy::consume(sql, pos, "SELECT")) { return false; }
  file_sql_copy::space(sql, pos);
  if (pos >= sql.size() || '*' != sql[pos++]) { return false; }
  if (!file_sql_copy::consume(sql, pos, "FROM")
      || !file_sql_copy::consume(sql, pos, "read_csv")) { return false; }
  file_sql_copy::space(sql, pos);
  if (pos >= sql.size() || '(' != sql[pos++]) { return false; }
  if (!file_sql_copy::literal(sql, pos, path)) { return false; }
  file_sql_copy::space(sql, pos);
  if (pos >= sql.size() || ')' != sql[pos++]) { return false; }
  file_sql_copy::space(sql, pos);
  if (pos < sql.size() && ';' == sql[pos]) { ++pos; file_sql_copy::space(sql, pos); }
  return pos == sql.size();
}

inline bool render(const std::vector<Column> &columns, std::string &out)
{
  if (columns.empty() || columns.size() > 256) { return false; }
  const char *null_label = "\x60" "null" "\x60";
  const char *key_label = "\x60" "key" "\x60";
  const char *default_label = "\x60" "default" "\x60";
  out = "/* read_csv file_sql_describe */ SELECT column_name, column_type, "
      + std::string(null_label) + ", " + key_label + ", " + default_label
      + ", extra FROM (";
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i) { out += " UNION ALL "; }
    out += "SELECT " + std::to_string(i + 1) + " AS ordinal, "
        + file_sql_copy::quote(columns[i].name) + " AS column_name, "
        + file_sql_copy::quote(columns[i].type) + " AS column_type, "
        + file_sql_copy::quote(columns[i].nullable ? "YES" : "NO") + " AS " + null_label
        + ", NULL AS " + key_label + ", NULL AS " + default_label
        + ", NULL AS extra";
  }
  out += ") AS file_sql_schema ORDER BY ordinal";
  return true;
}

} } } // namespace oceanbase::sql::file_sql_describe

#endif // OCEANBASE_SQL_OB_FILE_SQL_DESCRIBE_COMPAT_H_
