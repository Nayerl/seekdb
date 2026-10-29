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
#include <limits.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <string>

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

} /* namespace sql */
} /* namespace oceanbase */

