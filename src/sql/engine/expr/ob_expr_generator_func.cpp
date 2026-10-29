/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX  SQL_ENG
#include "sql/engine/expr/ob_expr_generator_func.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "share/system_variable/ob_sys_var_class_type.h"

#include <glob.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace oceanbase
{
using namespace common;
namespace sql
{

ObExprGeneratorFunc::ObExprGeneratorFunc(common::ObIAllocator &alloc)
	: ObFuncExprOperator(alloc, T_FUN_SYS_GENERATOR, "generator", 1, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

ObExprGeneratorFunc::~ObExprGeneratorFunc()
{
}

int ObExprGeneratorFunc::calc_result_type1(ObExprResType &type,
                                           ObExprResType &limit,
                                           common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;
  limit.set_calc_type(ObIntType);
  type.set_int();
  return ret;
}

int ObExprGeneratorFunc::eval_next_value(const ObExpr &expr,
                                     ObEvalCtx &ctx,
                                     ObDatum &res_datum)
{
  int ret = OB_SUCCESS;
  ObExprGeneratorFuncCtx *generator_ctx = NULL;
  uint64_t op_id = expr.expr_ctx_id_;
  ObExecContext &exec_ctx = ctx.exec_ctx_;
  if (OB_ISNULL(generator_ctx = static_cast<ObExprGeneratorFuncCtx *>(
              exec_ctx.get_expr_op_ctx(op_id)))) {
    if (OB_FAIL(exec_ctx.create_expr_op_ctx(op_id, generator_ctx))) {
    } else if (OB_ISNULL(generator_ctx)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
  }

  if (OB_SUCC(ret)) {
    ObDatum *limit_datum = NULL;
    if (OB_UNLIKELY(1 != expr.arg_cnt_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(expr.eval_param_value(ctx, limit_datum))) {
    } else if (OB_UNLIKELY(limit_datum->is_null())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "generator function. The argument should be an integer.");
    } else {
      int64_t	limit = limit_datum->get_int();
      int64_t next_value_res = (++generator_ctx->curr_value_);
      if (OB_UNLIKELY(next_value_res > limit)) {
        ret = OB_ITER_END;
      } else {
        res_datum.set_int(next_value_res);
      }
    }
  }
  return ret;
}

int ObExprGeneratorFunc::cg_expr(ObExprCGCtx &expr_cg_ctx,
                             const ObRawExpr &raw_expr,
                             ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  UNUSED(expr_cg_ctx);
  if (OB_UNLIKELY(1 != raw_expr.get_param_count())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(raw_expr.get_param_expr(0))) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!raw_expr.get_param_expr(0)->is_const_expr()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "generator function. The argument should be a constant integer");
  }
  rt_expr.eval_func_ = ObExprGeneratorFunc::eval_next_value;
  return ret;
}

namespace
{
struct FileSqlGlobCtx : public ObExprOperatorCtx
{
  FileSqlGlobCtx() : matches_(), next_(0), initialized_(false), secure_root_() {}
  ~FileSqlGlobCtx() override
  {
    if (initialized_) {
      ::globfree(&matches_);
    }
  }
  glob_t matches_;
  size_t next_;
  bool initialized_;
  std::string secure_root_;
};

bool is_within_secure_root(const char *path, const std::string &root)
{
  const size_t root_len = root.length();
  return root == "/"
         ? '/' == path[0]
         : (0 == std::strncmp(path, root.c_str(), root_len)
            && ('\0' == path[root_len] || '/' == path[root_len]));
}
} // namespace

FileSqlGlobExpr::FileSqlGlobExpr(ObIAllocator &alloc)
  : ObFuncExprOperator(alloc, T_FUN_SYS_GLOB, "glob", 1,
                       NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{}

int FileSqlGlobExpr::calc_result_type1(ObExprResType &type,
                                       ObExprResType &pattern,
                                       ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  type.set_varchar();
  type.set_length(PATH_MAX);
  type.set_collation_level(CS_LEVEL_IMPLICIT);
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  pattern.set_calc_type(ObVarcharType);
  return OB_SUCCESS;
}

int FileSqlGlobExpr::cg_expr(ObExprCGCtx &expr_cg_ctx,
                             const ObRawExpr &raw_expr,
                             ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  int ret = OB_SUCCESS;
  if (1 != raw_expr.get_param_count() || OB_ISNULL(raw_expr.get_param_expr(0))
      || !raw_expr.get_param_expr(0)->is_const_expr()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "glob requires a constant path pattern");
  } else {
    rt_expr.eval_func_ = FileSqlGlobExpr::eval_next_file;
  }
  return ret;
}

int FileSqlGlobExpr::eval_next_file(const ObExpr &expr, ObEvalCtx &ctx,
                                    ObDatum &res_datum)
{
  int ret = OB_SUCCESS;
  FileSqlGlobCtx *glob_ctx = static_cast<FileSqlGlobCtx *>(
      ctx.exec_ctx_.get_expr_op_ctx(expr.expr_ctx_id_));
  if (OB_ISNULL(glob_ctx)
      && OB_FAIL(ctx.exec_ctx_.create_expr_op_ctx(expr.expr_ctx_id_, glob_ctx))) {
  }
  if (OB_SUCC(ret) && OB_ISNULL(glob_ctx)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (OB_SUCC(ret) && !glob_ctx->initialized_) {
    ObSQLSessionInfo *session = ctx.exec_ctx_.get_my_session();
    bool enabled = false;
    ObString secure_file_priv;
    ObDatum *pattern_datum = nullptr;
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled");
    } else if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
    } else if (secure_file_priv.empty()) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if (OB_FAIL(expr.eval_param_value(ctx, pattern_datum))) {
    } else if (OB_ISNULL(pattern_datum) || pattern_datum->is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ObString pattern_value = pattern_datum->get_string();
      const std::string pattern(pattern_value.ptr(), pattern_value.length());
      const std::string secure(secure_file_priv.ptr(), secure_file_priv.length());
      char canonical_root[PATH_MAX] = {};
      char canonical_base[PATH_MAX] = {};
      const size_t wildcard = pattern.find_first_of("*?[");
      const size_t slash = pattern.rfind('/', wildcard);
      const std::string base = (std::string::npos == slash)
          ? "." : (0 == slash ? "/" : pattern.substr(0, slash));
      if (pattern.empty() || pattern.length() >= PATH_MAX || secure == "NULL"
          || secure == "null" || OB_ISNULL(::realpath(secure.c_str(), canonical_root))
          || OB_ISNULL(::realpath(base.c_str(), canonical_base))) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else {
        struct stat root_stat = {};
        if (0 != ::stat(canonical_root, &root_stat) || !S_ISDIR(root_stat.st_mode)
            || !is_within_secure_root(canonical_base, canonical_root)) {
          ret = OB_ERR_NO_PRIVILEGE;
        } else {
          glob_ctx->secure_root_ = canonical_root;
          const int glob_ret = ::glob(pattern.c_str(), 0, nullptr, &glob_ctx->matches_);
          glob_ctx->initialized_ = true;
          if (GLOB_NOMATCH == glob_ret) {
            // An empty discovery result is an empty table.
          } else if (0 != glob_ret) {
            ret = (GLOB_NOSPACE == glob_ret) ? OB_ALLOCATE_MEMORY_FAILED : OB_IO_ERROR;
          } else if (glob_ctx->matches_.gl_pathc > 10000) {
            ret = OB_SIZE_OVERFLOW;
          }
        }
      }
    }
  }
  bool emitted = false;
  while (OB_SUCC(ret) && glob_ctx->next_ < glob_ctx->matches_.gl_pathc) {
    const char *match = glob_ctx->matches_.gl_pathv[glob_ctx->next_++];
    char canonical_match[PATH_MAX] = {};
    struct stat match_stat = {};
    if (OB_ISNULL(::realpath(match, canonical_match))
        || 0 != ::stat(canonical_match, &match_stat)) {
      ret = OB_IO_ERROR;
    } else if (!is_within_secure_root(canonical_match, glob_ctx->secure_root_)) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else if (S_ISREG(match_stat.st_mode)) {
      res_datum.set_string(match, std::strlen(match));
      emitted = true;
      break;
    }
  }
  if (OB_SUCC(ret) && !emitted) {
    ret = OB_ITER_END;
  }
  return ret;
}

void FileSqlGlobExpr::reset_runtime(ObExecContext &exec_ctx, const ObExpr &expr)
{
  FileSqlGlobCtx *glob_ctx = static_cast<FileSqlGlobCtx *>(
      exec_ctx.get_expr_op_ctx(expr.expr_ctx_id_));
  if (nullptr != glob_ctx) {
    glob_ctx->next_ = 0;
  }
}

namespace
{
struct FileSqlCsvField
{
  std::string value_;
  bool quoted_ = false;
};

struct FileSqlCsvCtx : public ObExprOperatorCtx
{
  FileSqlCsvCtx() : file_(nullptr), initialized_(false), file_size_(0), modified_at_(0) {}
  ~FileSqlCsvCtx() override { if (nullptr != file_) { ::fclose(file_); } }
  FILE *file_;
  bool initialized_;
  off_t file_size_;
  time_t modified_at_;
  std::vector<FileSqlCsvField> fields_;
  int64_t column_count_ = 0;
};

bool parse_file_sql_csv_int(const std::string &text, int64_t &value)
{
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) { return false; }
  char *end = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(text.c_str(), &end, 10);
  if (ERANGE == errno || end != text.c_str() + text.size()) { return false; }
  value = static_cast<int64_t>(parsed);
  return true;
}

bool parse_file_sql_csv_double(const std::string &text, double &value)
{
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) { return false; }
  char *end = nullptr;
  errno = 0;
  const double parsed = std::strtod(text.c_str(), &end);
  if (ERANGE == errno || end != text.c_str() + text.size() || !std::isfinite(parsed)) {
    return false;
  }
  value = parsed;
  return true;
}

bool parse_file_sql_csv_bool(const std::string &text, bool &value)
{
  const ObString str = ObString::make_string(text.c_str());
  if (0 == str.case_compare("true")) { value = true; }
  else if (0 == str.case_compare("false")) { value = false; }
  else { return false; }
  return true;
}

ObObjType classify_file_sql_csv_field(const FileSqlCsvField &field)
{
  bool boolean = false;
  int64_t integer = 0;
  double floating = 0;
  if (parse_file_sql_csv_bool(field.value_, boolean)) { return ObTinyIntType; }
  if (parse_file_sql_csv_int(field.value_, integer)) { return ObIntType; }
  if (parse_file_sql_csv_double(field.value_, floating)) { return ObDoubleType; }
  return ObVarcharType;
}

int read_file_sql_csv_record(FILE *file, std::vector<FileSqlCsvField> &fields)
{
  int ret = OB_SUCCESS;
  fields.clear();
  FileSqlCsvField field;
  bool in_quotes = false;
  bool closed_quote = false;
  bool saw_input = false;
  bool ended_record = false;
  size_t record_bytes = 0;
  int ch = 0;
  auto finish_field = [&]() -> int {
    if (fields.size() >= 256) {
      return OB_SIZE_OVERFLOW;
    }
    fields.push_back(std::move(field));
    field = FileSqlCsvField();
    closed_quote = false;
    return OB_SUCCESS;
  };
  while (OB_SUCC(ret) && EOF != (ch = ::fgetc(file))) {
    saw_input = true;
    if (++record_bytes > 16 * 1024 * 1024 || '\0' == ch) {
      ret = OB_SIZE_OVERFLOW;
    } else if (in_quotes) {
      if ('"' == ch) {
        int next = ::fgetc(file);
        if ('"' == next) {
          field.value_.push_back('"');
          ++record_bytes;
        } else {
          in_quotes = false;
          closed_quote = true;
          if (EOF != next) { ::ungetc(next, file); }
        }
      } else {
        field.value_.push_back(static_cast<char>(ch));
      }
    } else if (',' == ch) {
      ret = finish_field();
    } else if ('\n' == ch || '\r' == ch) {
      if ('\r' == ch) {
        int next = ::fgetc(file);
        if ('\n' != next && EOF != next) { ::ungetc(next, file); }
      }
      ret = finish_field();
      ended_record = true;
      break;
    } else if ('"' == ch && field.value_.empty() && !closed_quote) {
      in_quotes = true;
      field.quoted_ = true;
    } else if ('"' == ch || closed_quote) {
      ret = OB_INVALID_DATA;
    } else {
      field.value_.push_back(static_cast<char>(ch));
    }
  }
  if (OB_SUCC(ret) && ::ferror(file)) {
    ret = OB_IO_ERROR;
  } else if (OB_SUCC(ret) && in_quotes) {
    ret = OB_INVALID_DATA;
  } else if (OB_SUCC(ret) && fields.empty() && !saw_input) {
    ret = OB_ITER_END;
  } else if (OB_SUCC(ret) && !ended_record) {
    ret = finish_field();
  }
  return ret;
}

int open_file_sql_csv(const std::string &path, ObSQLSessionInfo *session, FILE *&file)
{
  int ret = OB_SUCCESS;
  ObString secure_file_priv;
  char secure_root[PATH_MAX] = {};
  char canonical_path[PATH_MAX] = {};
  file = nullptr;
  if (OB_ISNULL(session) || path.empty() || path.size() >= PATH_MAX || '/' != path[0]) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("read_csv invalid file path arguments", "path", path.c_str(), KP(session));
  } else if (OB_FAIL(session->get_secure_file_priv(secure_file_priv))) {
  } else if (secure_file_priv.empty()
             || 0 == secure_file_priv.case_compare("NULL")) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else {
    const std::string root(secure_file_priv.ptr(), secure_file_priv.length());
    if (OB_ISNULL(::realpath(root.c_str(), secure_root))
        || OB_ISNULL(::realpath(path.c_str(), canonical_path))
        || !is_within_secure_root(canonical_path, secure_root)) {
      ret = OB_ERR_NO_PRIVILEGE;
    } else {
      const std::string relative = (0 == std::strcmp(secure_root, "/"))
          ? std::string(canonical_path + 1)
          : std::string(canonical_path + std::strlen(secure_root) + 1);
      int fd = ::open(secure_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (relative.empty() || fd < 0) {
        ret = OB_ERR_NO_PRIVILEGE;
      } else {
        size_t pos = 0;
        while (OB_SUCC(ret) && pos < relative.size()) {
          const size_t slash = relative.find('/', pos);
          const bool last = std::string::npos == slash;
          const std::string component = relative.substr(pos, last ? slash : slash - pos);
          const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (last ? 0 : O_DIRECTORY);
          const int next_fd = ::openat(fd, component.c_str(), flags);
          ::close(fd);
          fd = next_fd;
          if (fd < 0) { ret = OB_ERR_NO_PRIVILEGE; }
          pos = last ? relative.size() : slash + 1;
        }
      }
      if (OB_SUCC(ret)) {
        struct stat st = {};
        if (0 != ::fstat(fd, &st) || !S_ISREG(st.st_mode)) {
          ret = OB_ERR_NO_PRIVILEGE;
        } else if (OB_ISNULL(file = ::fdopen(fd, "rb"))) {
          ret = OB_IO_ERROR;
        } else {
          fd = -1;
        }
      }
      if (fd >= 0) { ::close(fd); }
    }
  }
  return ret;
}

int get_file_sql_csv_path(const ObRawExpr &raw_expr, std::string &path)
{
  int ret = OB_SUCCESS;
  const ObRawExpr *arg = raw_expr.get_param_count() == 1
      ? raw_expr.get_param_expr(0) : nullptr;
  if (OB_ISNULL(arg)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("read_csv path argument missing", K(raw_expr.get_param_count()));
  } else if (!arg->is_const_raw_expr()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("read_csv path is not a constant raw expression", K(arg->get_expr_type()));
  } else if (!arg->get_result_type().is_string_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("read_csv path expression is not a string", K(arg->get_result_type()));
  } else {
    const ObConstRawExpr *constant = static_cast<const ObConstRawExpr *>(arg);
    const ObObj &value = constant->get_value().is_unknown()
        ? constant->get_param() : constant->get_value();
    if (value.is_null() || value.is_unknown()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("read_csv path expression has no usable value");
    } else {
      const ObString str = value.get_string();
      path.assign(str.ptr(), str.length());
    }
  }
  return ret;
}
} // namespace

FileSqlCsvExpr::FileSqlCsvExpr(ObIAllocator &alloc)
  : ObFuncExprOperator(alloc, T_FUN_SYS_READ_CSV, "read_csv", 1,
                       NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{}

int FileSqlCsvExpr::calc_result_type1(ObExprResType &type,
                                      ObExprResType &path,
                                      ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  type.set_varchar();
  type.set_length(PATH_MAX);
  type.set_collation_level(CS_LEVEL_IMPLICIT);
  type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  path.set_calc_type(ObVarcharType);
  return OB_SUCCESS;
}

int FileSqlCsvExpr::cg_expr(ObExprCGCtx &expr_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  int ret = OB_SUCCESS;
  std::string path;
  if (OB_FAIL(get_file_sql_csv_path(raw_expr, path))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "read_csv requires a constant path");
  } else {
    rt_expr.eval_func_ = FileSqlCsvExpr::eval_next_row;
  }
  return ret;
}

int FileSqlCsvExpr::inspect_columns(const ObRawExpr &raw_expr,
                                    ObSQLSessionInfo *session,
                                    std::vector<std::string> &column_names,
                                    std::vector<ObObjMeta> *column_types)
{
  int ret = OB_SUCCESS;
  FILE *file = nullptr;
  std::string path;
  std::vector<FileSqlCsvField> header;
  column_names.clear();
  if (nullptr != column_types) { column_types->clear(); }
  if (OB_FAIL(get_file_sql_csv_path(raw_expr, path))) {
  } else if (OB_FAIL(open_file_sql_csv(path, session, file))) {
  } else if (OB_FAIL(read_file_sql_csv_record(file, header))) {
    if (OB_ITER_END == ret) { ret = OB_INVALID_DATA; }
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < header.size(); ++i) {
      std::string name = header[i].value_.empty()
          ? "column" + std::to_string(i + 1) : header[i].value_;
      const std::string base = name;
      for (size_t suffix = 2; std::find(column_names.begin(), column_names.end(), name)
                              != column_names.end(); ++suffix) {
        name = base + "__" + std::to_string(suffix);
      }
      if (name.length() > 128) { ret = OB_SIZE_OVERFLOW; }
      else { column_names.push_back(std::move(name)); }
    }
    if (OB_SUCC(ret) && nullptr != column_types) {
      std::vector<ObObjType> inferred(header.size(), ObNullType);
      std::vector<FileSqlCsvField> fields;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(read_file_sql_csv_record(file, fields))) {
          if (OB_ITER_END == ret) { ret = OB_SUCCESS; }
          break;
        } else if (fields.size() != header.size()) {
          ret = OB_INVALID_DATA;
        } else {
          for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].value_.empty() && !fields[i].quoted_) { continue; }
            const ObObjType next = classify_file_sql_csv_field(fields[i]);
            ObObjType &current = inferred[i];
            if (ObNullType == current) { current = next; }
            else if (current == next || ObVarcharType == current) { }
            else if ((ObIntType == current && ObDoubleType == next)
                     || (ObDoubleType == current && ObIntType == next)) {
              current = ObDoubleType;
            } else { current = ObVarcharType; }
          }
        }
      }
      for (size_t i = 0; OB_SUCC(ret) && i < inferred.size(); ++i) {
        ObObjMeta meta = raw_expr.get_result_meta();
        if (ObNullType != inferred[i]) { meta.set_type(inferred[i]); }
        column_types->push_back(meta);
      }
    }
  }
  if (nullptr != file) { ::fclose(file); }
  if (OB_FAIL(ret)) {
    LOG_WARN("read_csv column inspection failed", K(ret), "path", path.c_str());
  }
  return ret;
}

int FileSqlCsvExpr::eval_next_row(const ObExpr &expr, ObEvalCtx &ctx,
                                  ObDatum &res_datum)
{
  int ret = OB_SUCCESS;
  FileSqlCsvCtx *csv_ctx = static_cast<FileSqlCsvCtx *>(
      ctx.exec_ctx_.get_expr_op_ctx(expr.expr_ctx_id_));
  if (OB_ISNULL(csv_ctx)
      && OB_FAIL(ctx.exec_ctx_.create_expr_op_ctx(expr.expr_ctx_id_, csv_ctx))) {
  }
  if (OB_SUCC(ret) && OB_ISNULL(csv_ctx)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  if (OB_SUCC(ret) && !csv_ctx->initialized_) {
    bool enabled = false;
    ObSQLSessionInfo *session = ctx.exec_ctx_.get_my_session();
    ObDatum *path_datum = nullptr;
    if (OB_ISNULL(session)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(session->get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
    } else if (OB_FAIL(expr.eval_param_value(ctx, path_datum))) {
    } else if (OB_ISNULL(path_datum) || path_datum->is_null()) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ObString path_value = path_datum->get_string();
      const std::string path(path_value.ptr(), path_value.length());
      if (OB_FAIL(open_file_sql_csv(path, session, csv_ctx->file_))) {
      } else if (OB_FAIL(read_file_sql_csv_record(csv_ctx->file_, csv_ctx->fields_))) {
        if (OB_ITER_END == ret) { ret = OB_INVALID_DATA; }
      } else {
        csv_ctx->column_count_ = csv_ctx->fields_.size();
        struct stat st = {};
        if (0 != ::fstat(::fileno(csv_ctx->file_), &st)) {
          ret = OB_IO_ERROR;
        } else {
          csv_ctx->file_size_ = st.st_size;
          csv_ctx->modified_at_ = st.st_mtime;
          csv_ctx->initialized_ = true;
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    struct stat st = {};
    if (0 != ::fstat(::fileno(csv_ctx->file_), &st)
        || st.st_size != csv_ctx->file_size_
        || st.st_mtime != csv_ctx->modified_at_) {
      ret = OB_IO_ERROR;
    } else if (OB_FAIL(read_file_sql_csv_record(csv_ctx->file_, csv_ctx->fields_))) {
    } else if (csv_ctx->fields_.size() != static_cast<size_t>(csv_ctx->column_count_)) {
      ret = OB_INVALID_DATA;
    } else {
      res_datum.set_string("", 0);
    }
  }
  return ret;
}

int FileSqlCsvExpr::write_current_row(ObExecContext &exec_ctx,
                                      ObEvalCtx &eval_ctx,
                                      const ObExpr &expr,
                                      const ObIArray<ObExpr *> &columns,
                                      const ObIArray<int64_t> &column_indexes)
{
  int ret = OB_SUCCESS;
  FileSqlCsvCtx *csv_ctx = static_cast<FileSqlCsvCtx *>(
      exec_ctx.get_expr_op_ctx(expr.expr_ctx_id_));
  if (OB_ISNULL(csv_ctx) || columns.count() != column_indexes.count()
      || csv_ctx->fields_.size() != static_cast<size_t>(csv_ctx->column_count_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      ObExpr *column = columns.at(i);
      const int64_t index = column_indexes.at(i);
      if (OB_ISNULL(column) || index < 0 || index >= csv_ctx->column_count_) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        const FileSqlCsvField &field = csv_ctx->fields_[index];
        ObDatum &datum = column->locate_datum_for_write(eval_ctx);
        if (field.value_.empty() && !field.quoted_) {
          datum.set_null();
        } else if (ObTinyIntType == column->datum_meta_.get_type()) {
          bool value = false;
          if (!parse_file_sql_csv_bool(field.value_, value)) { ret = OB_INVALID_DATA; }
          else { datum.set_bool(value); }
        } else if (ObIntType == column->datum_meta_.get_type()) {
          int64_t value = 0;
          if (!parse_file_sql_csv_int(field.value_, value)) { ret = OB_INVALID_DATA; }
          else { datum.set_int(value); }
        } else if (ObDoubleType == column->datum_meta_.get_type()) {
          double value = 0;
          if (!parse_file_sql_csv_double(field.value_, value)) { ret = OB_INVALID_DATA; }
          else { datum.set_double(value); }
        } else if (field.value_.empty()) {
          datum.set_string("", 0);
        } else {
          char *dest = column->get_str_res_mem(eval_ctx, field.value_.length());
          if (OB_ISNULL(dest)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            std::memcpy(dest, field.value_.data(), field.value_.length());
            datum.set_string(dest, field.value_.length());
          }
        }
        if (OB_SUCC(ret)) { column->set_evaluated_projected(eval_ctx); }
      }
    }
  }
  return ret;
}

void FileSqlCsvExpr::reset_runtime(ObExecContext &exec_ctx, const ObExpr &expr)
{
  FileSqlCsvCtx *csv_ctx = static_cast<FileSqlCsvCtx *>(
      exec_ctx.get_expr_op_ctx(expr.expr_ctx_id_));
  if (nullptr != csv_ctx) {
    if (nullptr != csv_ctx->file_) { ::fclose(csv_ctx->file_); }
    csv_ctx->file_ = nullptr;
    csv_ctx->initialized_ = false;
    csv_ctx->fields_.clear();
  }
}

} /* namespace sql */
} /* namespace oceanbase */

