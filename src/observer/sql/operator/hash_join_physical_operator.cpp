/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/hash_join_physical_operator.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple_cell.h"

HashJoinPhysicalOperator::HashJoinPhysicalOperator() {}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("hash join operator should have 2 children");
    return RC::INTERNAL;
  }

  RC rc = RC::SUCCESS;
  left_  = children_[0].get();
  right_ = children_[1].get();

  trx_ = trx;

  // 打开左算子
  rc = left_->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open left operator. rc=%s", strrc(rc));
    return rc;
  }

  // 构建哈希表（build stage）
  rc = build_hash_table();
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to build hash table. rc=%s", strrc(rc));
    left_->close();
    return rc;
  }

  // 注意：不关闭左算子，因为哈希表中存储的是 tuple 指针
  // 这些指针在 left_ 算子关闭后可能失效
  // 我们将在 close() 方法中关闭 left_ 算子

  // 打开右算子
  rc = right_->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open right operator. rc=%s", strrc(rc));
    return rc;
  }

  build_done_ = true;
  probe_done_ = false;
  current_matches_ = nullptr;
  current_match_index_ = 0;

  return rc;
}

RC HashJoinPhysicalOperator::next()
{
  if (!build_done_) {
    return RC::INTERNAL;
  }

  // 如果当前有未处理的匹配，继续处理
  if (current_matches_ != nullptr && current_match_index_ < current_matches_->size()) {
    Tuple *left_tuple = (*current_matches_)[current_match_index_].get();
    joined_tuple_.set_left(left_tuple);
    joined_tuple_.set_right(right_tuple_);
    bool passed = true;
    RC rc = evaluate_join_predicates(joined_tuple_, passed);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    current_match_index_++;
    if (passed) {
      return RC::SUCCESS;
    }
    // 没通过则尝试下一个匹配
    return next();
  }

  // 继续从右表读取下一行
  while (true) {
    RC rc = right_->next();
    if (rc != RC::SUCCESS) {
      if (rc == RC::RECORD_EOF) {
        probe_done_ = true;
      }
      return rc;
    }

    right_tuple_ = right_->current_tuple();
    if (right_tuple_ == nullptr) {
      continue;
    }

    // 从右表元组提取哈希键
    HashKeyNode probe_key;
    rc = extract_hash_key(right_tuple_, false, probe_key);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to extract hash key from right tuple. rc=%s", strrc(rc));
      continue;
    }

    // 在哈希表中查找匹配
    auto it = hash_table_.find(probe_key);
    if (it != hash_table_.end()) {
      // 找到匹配，设置当前匹配列表
      current_matches_ = &(it->second);
      current_match_index_ = 0;

      if (!current_matches_->empty()) {
        // 返回第一个匹配
        Tuple *left_tuple = (*current_matches_)[0].get();
        joined_tuple_.set_left(left_tuple);
        joined_tuple_.set_right(right_tuple_);
        current_match_index_ = 1;

        bool passed = true;
        RC rc2 = evaluate_join_predicates(joined_tuple_, passed);
        if (rc2 != RC::SUCCESS) {
          return rc2;
        }
        if (passed) {
          return RC::SUCCESS;
        }
        // 如果未通过，继续寻找下一个匹配
        continue;
      }
    }
    // 如果没有匹配，继续读取下一行
  }
}

RC HashJoinPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;

  // 清理哈希表
  hash_table_.clear();

  // 关闭左算子（如果还没有关闭）
  if (left_ != nullptr) {
    rc = left_->close();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close left operator. rc=%s", strrc(rc));
    }
  }

  // 关闭右算子
  if (right_ != nullptr) {
    RC rc2 = right_->close();
    if (rc2 != RC::SUCCESS) {
      LOG_WARN("failed to close right operator. rc=%s", strrc(rc2));
      if (rc == RC::SUCCESS) {
        rc = rc2;
      }
    }
  }

  return rc;
}

Tuple *HashJoinPhysicalOperator::current_tuple()
{
  return &joined_tuple_;
}

RC HashJoinPhysicalOperator::extract_hash_key(Tuple *tuple, bool is_left, HashKeyNode &key)
{
  key.values.clear();

  if (tuple == nullptr) {
    LOG_WARN("tuple is nullptr");
    return RC::INVALID_ARGUMENT;
  }

  // 遍历所有 join 条件
  for (auto &predicate : join_predicates_) {
    if (predicate->type() != ExprType::COMPARISON) {
      continue;
    }

    ComparisonExpr *comp_expr = dynamic_cast<ComparisonExpr *>(predicate.get());
    if (comp_expr == nullptr) {
      continue;
    }

    // 只处理等值连接
    if (comp_expr->comp() != CompOp::EQUAL_TO) {
      continue;
    }

    // 仅使用字段=字段的等值条件参与哈希键；常量过滤留给后续谓词判断
    if (comp_expr->left()->type() != ExprType::FIELD || comp_expr->right()->type() != ExprType::FIELD) {
      continue;
    }

    // 为保证左右子使用各自对应的一侧字段作为哈希键：
    // - build 阶段（is_left=true）优先取比较左侧字段，失败再取右侧
    // - probe 阶段（is_left=false）优先取比较右侧字段，失败再取左侧
    Value value;
    RC rc = is_left ? comp_expr->left()->get_value(*tuple, value) : comp_expr->right()->get_value(*tuple, value);
    if (rc != RC::SUCCESS) {
      rc = is_left ? comp_expr->right()->get_value(*tuple, value) : comp_expr->left()->get_value(*tuple, value);
    }
    if (rc == RC::SUCCESS) {
      key.values.push_back(value);
    }
  }

  // 注意：在多表 join 中，某个 join 条件可能不涉及当前层的表
  // 例如 t1 JOIN t2 ON t1.id=t2.id JOIN t3 ON t1.id=t3.id
  // 第二个 JOIN 的右侧 t3 只能提取 t3.id，无法提取 t1.id
  // 因此 key.values 可能为空，这是正常的
  // 但如果为空，说明当前 tuple 不参与这个 join 条件的哈希
  
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::build_hash_table()
{
  hash_table_.clear();

  RC rc = RC::SUCCESS;
  while ((rc = left_->next()) == RC::SUCCESS) {
    Tuple *left_tuple = left_->current_tuple();
    if (left_tuple == nullptr) {
      continue;
    }

    // 从左表元组提取哈希键
    HashKeyNode key;
    rc = extract_hash_key(left_tuple, true, key);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to extract hash key from left tuple. rc=%s", strrc(rc));
      continue;
    }

    // 将元组内容拷贝后插入哈希表，避免底层算子复用 tuple 对象导致指针失效
    auto stored_tuple = std::make_unique<ValueListTuple>();
    rc = ValueListTuple::make(*left_tuple, *stored_tuple);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to copy left tuple for hash table. rc=%s", strrc(rc));
      continue;
    }
    hash_table_[key].push_back(std::move(stored_tuple));
  }

  if (rc == RC::RECORD_EOF) {
    return RC::SUCCESS;
  }

  return rc;
}

RC HashJoinPhysicalOperator::probe_hash_table()
{
  // probe 逻辑已经在 next() 中实现
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::evaluate_join_predicates(JoinedTuple &tuple, bool &passed)
{
  passed = true;
  Value value;
  for (auto &expr : join_predicates_) {
    RC rc = expr->get_value(tuple, value);
    if (rc == RC::NOTFOUND) {
      // 当前 join 输出中不存在该字段，跳过此谓词
      continue;
    } else if (rc == RC::INVALID_ARGUMENT) {
      // 子树不包含该字段，继续尝试其他谓词
      continue;
    }
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (!value.get_boolean()) {
      passed = false;
      return RC::SUCCESS;
    }
  }
  return RC::SUCCESS;
}
