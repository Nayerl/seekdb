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

#ifndef OCEANBASE_SQL_OB_EXPR_FUNC_GENERATOR_FUNC_H_
#define OCEANBASE_SQL_OB_EXPR_FUNC_GENERATOR_FUNC_H_

#include "sql/engine/expr/ob_expr_operator.h"
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
class ObExprGeneratorFunc : public ObFuncExprOperator
{
	class ObExprGeneratorFuncCtx: public ObExprOperatorCtx
	{
	public:
		ObExprGeneratorFuncCtx() : curr_value_(0) {}
    ~ObExprGeneratorFuncCtx() = default;
    // increment 1 after every call to expr evaluation
    int64_t curr_value_;
  };
public:
	explicit ObExprGeneratorFunc(common::ObIAllocator &alloc);
	virtual ~ObExprGeneratorFunc();
  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &limit,
                                common::ObExprTypeCtx &type_ctx) const;
  virtual bool need_rt_ctx() const override { return true; }
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int eval_next_value(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum);
private:
	DISALLOW_COPY_AND_ASSIGN(ObExprGeneratorFunc);
};

// The file discovery function is a table-valued stream, like generator().
class FileSqlGlobExpr : public ObFuncExprOperator
{
public:
  explicit FileSqlGlobExpr(common::ObIAllocator &alloc);
  virtual ~FileSqlGlobExpr() = default;
  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &pattern,
                                common::ObExprTypeCtx &type_ctx) const override;
  virtual bool need_rt_ctx() const override { return true; }
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int eval_next_file(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum);
  static void reset_runtime(ObExecContext &exec_ctx, const ObExpr &expr);
private:
  DISALLOW_COPY_AND_ASSIGN(FileSqlGlobExpr);
};

class ObSQLSessionInfo;

struct FileSqlCsvSchemaColumn
{
  std::string name_;
  common::ObObjType type_ = common::ObNullType;
  bool nullable_ = false;
};

// A row-at-a-time CSV source. The resolver exposes its header as SQL columns.
class FileSqlCsvExpr : public ObFuncExprOperator
{
public:
  explicit FileSqlCsvExpr(common::ObIAllocator &alloc);
  virtual ~FileSqlCsvExpr() = default;
  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &path,
                                common::ObExprTypeCtx &type_ctx) const override;
  virtual bool need_rt_ctx() const override { return true; }
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int inspect_columns(const ObRawExpr &raw_expr,
                             ObSQLSessionInfo *session,
                             std::vector<std::string> &column_names,
                             std::vector<common::ObObjMeta> *column_types = nullptr);
  static int describe_path(const std::string &path, ObSQLSessionInfo *session,
                           std::vector<FileSqlCsvSchemaColumn> &columns);
  static int eval_next_row(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum);
  static int write_current_row(ObExecContext &exec_ctx,
                               ObEvalCtx &eval_ctx,
                               const ObExpr &expr,
                               const common::ObIArray<ObExpr *> &columns,
                               const common::ObIArray<int64_t> &column_indexes);
  static void reset_runtime(ObExecContext &exec_ctx, const ObExpr &expr);
private:
  DISALLOW_COPY_AND_ASSIGN(FileSqlCsvExpr);
};

// Newline-delimited JSON table source with unioned object keys.
class FileSqlJsonExpr : public ObFuncExprOperator
{
public:
  explicit FileSqlJsonExpr(common::ObIAllocator &alloc);
  virtual ~FileSqlJsonExpr() = default;
  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &path,
                                common::ObExprTypeCtx &type_ctx) const override;
  virtual bool need_rt_ctx() const override { return true; }
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int inspect_columns(const ObRawExpr &raw_expr,
                             ObSQLSessionInfo *session,
                             std::vector<std::string> &column_names,
                             std::vector<common::ObObjMeta> *column_types = nullptr);
  static int eval_next_row(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum);
  static int write_current_row(ObExecContext &exec_ctx,
                               ObEvalCtx &eval_ctx,
                               const ObExpr &expr,
                               const common::ObIArray<ObExpr *> &columns,
                               const common::ObIArray<int64_t> &column_indexes);
  static void reset_runtime(ObExecContext &exec_ctx, const ObExpr &expr);
private:
  DISALLOW_COPY_AND_ASSIGN(FileSqlJsonExpr);
};

class FileSqlParquetExpr : public ObFuncExprOperator
{
public:
  explicit FileSqlParquetExpr(common::ObIAllocator &alloc);
  virtual ~FileSqlParquetExpr() = default;
  virtual int calc_result_type1(ObExprResType &type, ObExprResType &path,
                                common::ObExprTypeCtx &type_ctx) const override;
  virtual bool need_rt_ctx() const override { return true; }
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int inspect_columns(const ObRawExpr &raw_expr, ObSQLSessionInfo *session,
                             std::vector<std::string> &column_names,
                             std::vector<common::ObObjMeta> *column_types = nullptr);
  static int eval_next_row(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res_datum);
  static int write_current_row(ObExecContext &exec_ctx, ObEvalCtx &eval_ctx,
                               const ObExpr &expr,
                               const common::ObIArray<ObExpr *> &columns,
                               const common::ObIArray<int64_t> &column_indexes);
  static void reset_runtime(ObExecContext &exec_ctx, const ObExpr &expr);
private:
  DISALLOW_COPY_AND_ASSIGN(FileSqlParquetExpr);
};

} /* namespace sql */
} /* namespace oceanbase */

#endif /* OCEANBASE_SQL_OB_EXPR_FUNC_GENERATOR_FUNC_H_ */
