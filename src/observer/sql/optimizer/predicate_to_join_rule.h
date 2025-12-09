/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/lang/vector.h"
#include "sql/optimizer/rewrite_rule.h"

class TableGetLogicalOperator;
class JoinLogicalOperator;

/**
 * @brief 将一些谓词表达式下推到join中
 * @ingroup Rewriter
 */
class PredicateToJoinRewriter : public RewriteRule
{
public:
  PredicateToJoinRewriter() = default;
  virtual ~PredicateToJoinRewriter() = default;

  RC rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made) override;

private:
  /**
   * @brief 收集子树中的所有 TableGetLogicalOperator
   */
  void collect_table_get_operators(
      LogicalOperator *oper,
      vector<TableGetLogicalOperator *> &table_get_opers);

  /**
   * @brief 收集子树中的所有 JoinLogicalOperator
   */
  void collect_join_operators(
      LogicalOperator *oper,
      vector<JoinLogicalOperator *> &join_opers);

  /**
   * @brief 将表达式分类为：字段vs字段、字段vs值、其他
   */
  RC classify_expression(
      unique_ptr<Expression> &expr,
      vector<unique_ptr<Expression>> &field_field_exprs,
      vector<unique_ptr<Expression>> &field_value_exprs,
      vector<unique_ptr<Expression>> &other_exprs);

  /**
   * @brief 检查表达式是否可以下推到指定的表
   */
  bool can_pushdown_to_table(
      Expression *expr,
      TableGetLogicalOperator *table_get_oper);

  /**
   * @brief 将条件下推到 TableGetLogicalOperator
   */
  RC pushdown_to_table_get(
      vector<unique_ptr<Expression>> &exprs,
      vector<TableGetLogicalOperator *> &table_get_opers,
      vector<unique_ptr<Expression>> &remaining_exprs);

  /**
   * @brief 将字段vs字段的条件下推到 JoinLogicalOperator
   */
  RC pushdown_to_joins(
      vector<unique_ptr<Expression>> &exprs,
      vector<JoinLogicalOperator *> &join_opers,
      vector<unique_ptr<Expression>> &remaining_exprs);
};
