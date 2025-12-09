/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include <unordered_set>

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  RC rc = RC::SUCCESS;
  
  // 只处理 PredicateLogicalOperator
  if (oper->type() != LogicalOperatorType::PREDICATE) {
    return rc;
  }

  // 必须有恰好一个子节点
  if (oper->children().size() != 1) {
    return rc;
  }

  unique_ptr<LogicalOperator> &child_oper = oper->children().front();
  
  // 子节点必须是 JOIN
  LogicalOperator *current = child_oper.get();
  if (current->type() != LogicalOperatorType::JOIN) {
    return rc;
  }
  JoinLogicalOperator *join_oper = static_cast<JoinLogicalOperator *>(current);

  vector<unique_ptr<Expression>> &predicate_exprs = oper->expressions();
  if (predicate_exprs.empty()) {
    return rc;
  }

  // 收集所有可以下推的目标
  vector<TableGetLogicalOperator *> table_get_opers;
  collect_table_get_operators(join_oper, table_get_opers);
  
  // 收集所有的 JOIN 节点（包括嵌套的）
  vector<JoinLogicalOperator *> all_join_opers;
  collect_join_operators(join_oper, all_join_opers);
  
  // 将表达式移动到临时容器以便处理
  vector<unique_ptr<Expression>> temp_exprs;
  for (auto &pred_expr : predicate_exprs) {
    temp_exprs.push_back(std::move(pred_expr));
  }
  predicate_exprs.clear();
  
  // 分类表达式
  vector<unique_ptr<Expression>> field_field_exprs;  // 字段 vs 字段（join 条件）
  vector<unique_ptr<Expression>> field_value_exprs;  // 字段 vs 值（table 过滤条件）
  vector<unique_ptr<Expression>> other_exprs;        // 其他
  
  for (auto &pred_expr : temp_exprs) {
    rc = classify_expression(pred_expr, field_field_exprs, field_value_exprs, other_exprs);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to classify expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  // 下推字段 vs 字段的条件到 JOIN
  vector<unique_ptr<Expression>> remaining_field_field_exprs;
  if (!field_field_exprs.empty() && !all_join_opers.empty()) {
    rc = pushdown_to_joins(field_field_exprs, all_join_opers, remaining_field_field_exprs);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to pushdown to joins. rc=%s", strrc(rc));
      return rc;
    }
    
    if (remaining_field_field_exprs.size() != field_field_exprs.size()) {
      change_made = true;
    }
  } else {
    remaining_field_field_exprs = std::move(field_field_exprs);
  }

  // 下推字段 vs 值的条件到 TABLE_GET
  vector<unique_ptr<Expression>> final_remaining_exprs;
  if (!field_value_exprs.empty() && !table_get_opers.empty()) {
    rc = pushdown_to_table_get(field_value_exprs, table_get_opers, final_remaining_exprs);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to pushdown to table get. rc=%s", strrc(rc));
      return rc;
    }
    
    if (final_remaining_exprs.size() != field_value_exprs.size()) {
      change_made = true;
    }
  } else {
    final_remaining_exprs = std::move(field_value_exprs);
  }
  
  // 合并其他无法下推的表达式
  for (auto &expr : other_exprs) {
    final_remaining_exprs.push_back(std::move(expr));
  }
  
  // 合并无法下推的字段 vs 字段表达式
  for (auto &expr : remaining_field_field_exprs) {
    final_remaining_exprs.push_back(std::move(expr));
  }

  // 更新 PredicateLogicalOperator 中的表达式
  if (!final_remaining_exprs.empty()) {
    // PredicateLogicalOperator 期望恰好有 1 个表达式
    // 如果有多个，需要用 ConjunctionExpr 合并
    if (final_remaining_exprs.size() == 1) {
      predicate_exprs.clear();
      predicate_exprs.push_back(std::move(final_remaining_exprs[0]));
    } else {
      // 多个表达式，用 AND 合并
      unique_ptr<ConjunctionExpr> conjunction_expr(
          new ConjunctionExpr(ConjunctionExpr::Type::AND, final_remaining_exprs));
      predicate_exprs.clear();
      predicate_exprs.push_back(std::move(conjunction_expr));
    }
  } else {
    // 所有条件都下推了，设置一个恒真条件
    Value value((bool)true);
    predicate_exprs.clear();
    predicate_exprs.emplace_back(new ValueExpr(value));
  }

  return rc;
}


void PredicateToJoinRewriter::collect_table_get_operators(
    LogicalOperator *oper,
    vector<TableGetLogicalOperator *> &table_get_opers)
{
  if (oper == nullptr) {
    return;
  }

  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    table_get_opers.push_back(static_cast<TableGetLogicalOperator *>(oper));
    return;
  }

  // 递归收集子节点中的 TableGetLogicalOperator
  for (auto &child : oper->children()) {
    collect_table_get_operators(child.get(), table_get_opers);
  }
}

void PredicateToJoinRewriter::collect_join_operators(
    LogicalOperator *oper,
    vector<JoinLogicalOperator *> &join_opers)
{
  if (oper == nullptr) {
    return;
  }

  if (oper->type() == LogicalOperatorType::JOIN) {
    join_opers.push_back(static_cast<JoinLogicalOperator *>(oper));
  }

  // 递归收集子节点中的 JoinLogicalOperator
  for (auto &child : oper->children()) {
    collect_join_operators(child.get(), join_opers);
  }
}

RC PredicateToJoinRewriter::classify_expression(
    unique_ptr<Expression> &expr,
    vector<unique_ptr<Expression>> &field_field_exprs,
    vector<unique_ptr<Expression>> &field_value_exprs,
    vector<unique_ptr<Expression>> &other_exprs)
{
  RC rc = RC::SUCCESS;

  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());
    
    // 只处理 AND 连接
    if (conjunction_expr->conjunction_type() != ConjunctionExpr::Type::AND) {
      other_exprs.push_back(std::move(expr));
      return rc;
    }

    // 提取子表达式并递归处理
    vector<unique_ptr<Expression>> &children = conjunction_expr->children();
    vector<unique_ptr<Expression>> temp_children;
    
    // 将子表达式移动到临时容器
    for (auto &child : children) {
      temp_children.push_back(std::move(child));
    }
    children.clear();
    
    // 递归处理每个子表达式
    for (auto &child : temp_children) {
      rc = classify_expression(child, field_field_exprs, field_value_exprs, other_exprs);
      if (rc != RC::SUCCESS) {
        return rc;
      }
    }
  } else if (expr->type() == ExprType::COMPARISON) {
    ComparisonExpr *comp_expr = static_cast<ComparisonExpr *>(expr.get());
    
    bool left_is_field = (comp_expr->left()->type() == ExprType::FIELD);
    bool right_is_field = (comp_expr->right()->type() == ExprType::FIELD);
    bool left_is_value = (comp_expr->left()->type() == ExprType::VALUE);
    bool right_is_value = (comp_expr->right()->type() == ExprType::VALUE);
    
    // 仅当等值且字段-字段时，作为 join 条件候选；否则作为过滤条件
    if (comp_expr->comp() == CompOp::EQUAL_TO && left_is_field && right_is_field) {
      field_field_exprs.push_back(std::move(expr));
    } else if ((left_is_field && right_is_value) || (left_is_value && right_is_field)) {
      field_value_exprs.push_back(std::move(expr));
    } else {
      other_exprs.push_back(std::move(expr));
    }
  } else {
    // 其他类型的表达式
    other_exprs.push_back(std::move(expr));
  }

  return rc;
}

RC PredicateToJoinRewriter::pushdown_to_joins(
    vector<unique_ptr<Expression>> &exprs,
    vector<JoinLogicalOperator *> &join_opers,
    vector<unique_ptr<Expression>> &remaining_exprs)
{
  RC rc = RC::SUCCESS;

  // 预计算每个 join 左右子树包含的表名集合
  auto collect_tables = [](LogicalOperator *oper, std::unordered_set<std::string> &tables, auto &&self_ref) -> void {
    if (oper == nullptr) {
      return;
    }
    if (oper->type() == LogicalOperatorType::TABLE_GET) {
      auto table_get = static_cast<TableGetLogicalOperator *>(oper);
      tables.insert(table_get->table()->name());
      return;
    }
    for (auto &child : oper->children()) {
      self_ref(child.get(), tables, self_ref);
    }
  };

  struct JoinSides {
    std::unordered_set<std::string> left_tables;
    std::unordered_set<std::string> right_tables;
  };
  std::vector<JoinSides> join_sides(join_opers.size());
  for (size_t i = 0; i < join_opers.size(); i++) {
    auto &join = join_opers[i];
    if (join->children().size() == 2) {
      collect_tables(join->children()[0].get(), join_sides[i].left_tables, collect_tables);
      collect_tables(join->children()[1].get(), join_sides[i].right_tables, collect_tables);
    }
  }

  // 将字段 vs 字段的条件下推到匹配的 JOIN
  for (auto &expr : exprs) {
    bool pushed_down = false;

    if (expr->type() != ExprType::COMPARISON) {
      remaining_exprs.push_back(std::move(expr));
      continue;
    }

    auto *comp_expr = static_cast<ComparisonExpr *>(expr.get());
    if (comp_expr->left()->type() != ExprType::FIELD || comp_expr->right()->type() != ExprType::FIELD) {
      remaining_exprs.push_back(std::move(expr));
      continue;
    }

    FieldExpr *lfield = static_cast<FieldExpr *>(comp_expr->left().get());
    FieldExpr *rfield = static_cast<FieldExpr *>(comp_expr->right().get());
    std::string ltable = lfield->field().table_name();
    std::string rtable = rfield->field().table_name();

    for (size_t i = 0; i < join_opers.size(); i++) {
      auto &sides = join_sides[i];
      bool l_in_left = sides.left_tables.count(ltable) > 0;
      bool l_in_right = sides.right_tables.count(ltable) > 0;
      bool r_in_left = sides.left_tables.count(rtable) > 0;
      bool r_in_right = sides.right_tables.count(rtable) > 0;

      if ((l_in_left && r_in_right) || (l_in_right && r_in_left)) {
        join_opers[i]->add_join_predicate(std::move(expr));
        pushed_down = true;
        break;
      }
    }

    if (!pushed_down) {
      // 如果没有匹配到特定 JOIN，兜底推到最顶层 JOIN，避免等值条件丢失导致无法选择哈希连接
      if (!join_opers.empty()) {
        join_opers.front()->add_join_predicate(std::move(expr));
        pushed_down = true;
      }
    }
  }

  return rc;
}

bool PredicateToJoinRewriter::can_pushdown_to_table(
    Expression *expr,
    TableGetLogicalOperator *table_get_oper)
{
  if (expr == nullptr || table_get_oper == nullptr) {
    return false;
  }

  // 只处理 ComparisonExpr
  if (expr->type() != ExprType::COMPARISON) {
    return false;
  }

  ComparisonExpr *comp_expr = static_cast<ComparisonExpr *>(expr);
  
  // 检查是否是"字段 vs 值"的形式
  // 可以是 field op value 或 value op field
  bool left_is_field = (comp_expr->left()->type() == ExprType::FIELD);
  bool right_is_field = (comp_expr->right()->type() == ExprType::FIELD);
  bool left_is_value = (comp_expr->left()->type() == ExprType::VALUE);
  bool right_is_value = (comp_expr->right()->type() == ExprType::VALUE);

  if (!((left_is_field && right_is_value) || (left_is_value && right_is_field))) {
    return false;
  }

  // 获取字段表达式
  FieldExpr *field_expr = nullptr;
  if (left_is_field) {
    field_expr = static_cast<FieldExpr *>(comp_expr->left().get());
  } else {
    field_expr = static_cast<FieldExpr *>(comp_expr->right().get());
  }

  // 检查字段是否属于这个表
  const char *table_name = field_expr->field().table_name();
  const char *target_table_name = table_get_oper->table()->name();
  
  return (strcmp(table_name, target_table_name) == 0);
}

RC PredicateToJoinRewriter::pushdown_to_table_get(
    vector<unique_ptr<Expression>> &exprs,
    vector<TableGetLogicalOperator *> &table_get_opers,
    vector<unique_ptr<Expression>> &remaining_exprs)
{
  RC rc = RC::SUCCESS;

  for (auto &expr : exprs) {
    bool pushed_down = false;

    // 尝试将表达式下推到某个 TableGetLogicalOperator
    for (auto *table_get_oper : table_get_opers) {
      if (can_pushdown_to_table(expr.get(), table_get_oper)) {
        // 可以下推到这个表
        // 直接添加到 predicates 中
        table_get_oper->predicates().push_back(expr->copy());
        pushed_down = true;
        break;
      }
    }

    if (!pushed_down) {
      // 无法下推，保留在原位置
      remaining_exprs.push_back(std::move(expr));
    }
  }

  return rc;
}
