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

#include "sql/operator/physical_operator.h"
#include "sql/parser/parse.h"
#include "sql/expr/tuple.h"
#include "sql/expr/tuple_cell.h"
#include "common/value.h"
#include <memory>
#include <unordered_map>
#include <vector>

/**
 * @brief Hash Join 算子的哈希键结构
 */
struct HashKeyNode {
  std::vector<Value> values;

  bool operator==(const HashKeyNode &other) const {
    if (values.size() != other.values.size()) {
      return false;
    }
    for (size_t i = 0; i < values.size(); i++) {
      if (values[i].compare(other.values[i]) != 0) {
        return false;
      }
    }
    return true;
  }
};

/**
 * @brief Hash Join 算子的哈希函数
 */
struct HashAlgo {
  size_t operator()(const HashKeyNode &key) const {
    size_t hash = 0;
    for (const auto &value : key.values) {
      // 使用简单的哈希组合方式
      size_t value_hash = 0;
      const char *data = value.data();
      int len = value.length();
      
      // 根据类型计算哈希值
      switch (value.attr_type()) {
        case AttrType::INTS: {
          int32_t int_val = value.get_int();
          value_hash = std::hash<int32_t>{}(int_val);
          break;
        }
        case AttrType::FLOATS: {
          float float_val = value.get_float();
          value_hash = std::hash<float>{}(float_val);
          break;
        }
        case AttrType::CHARS: {
          // 对字符串进行哈希
          for (int i = 0; i < len; i++) {
            value_hash = value_hash * 31 + static_cast<unsigned char>(data[i]);
          }
          break;
        }
        case AttrType::DATES: {
          int date_val = value.get_int();
          value_hash = std::hash<int>{}(date_val);
          break;
        }
        case AttrType::BOOLEANS: {
          bool bool_val = value.get_boolean();
          value_hash = std::hash<bool>{}(bool_val);
          break;
        }
        default:
          // 对于其他类型，使用数据内容的哈希
          for (int i = 0; i < len; i++) {
            value_hash = value_hash * 31 + static_cast<unsigned char>(data[i]);
          }
          break;
      }
      
      // 组合多个值的哈希
      hash = hash * 31 + value_hash;
    }
    return hash;
  }
};

/**
 * @brief Hash Join 算子
 * @details 基于哈希表的连接操作，分为构建阶段（build）和探测阶段（probe）
 * @ingroup PhysicalOperator
 */
class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator();
  virtual ~HashJoinPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }

  OpType get_op_type() const override { return OpType::INNERHASHJOIN; }

  virtual double calculate_cost(
      LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override
  {
    return 0.0;
  }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;
  Tuple *current_tuple() override;

  void set_join_predicates(vector<unique_ptr<Expression>> &&predicates) {
    join_predicates_ = std::move(predicates);
  }

private:
  /**
   * @brief 从 tuple 中提取哈希键
   * @param tuple 输入的 tuple
   * @param is_left 是否为左表（用于确定从 join 条件的哪一边提取）
   * @param key 输出的哈希键
   * @return RC
   */
  RC extract_hash_key(Tuple *tuple, bool is_left, HashKeyNode &key);

  /**
   * @brief 构建阶段：从左表构建哈希表
   * @return RC
   */
  RC build_hash_table();

  /**
   * @brief 探测阶段：从右表探测哈希表
   * @return RC
   */
  RC probe_hash_table();

  /**
   * @brief 评估 join 谓词，返回是否通过
   */
  RC evaluate_join_predicates(JoinedTuple &tuple, bool &passed);

private:
  Trx *trx_ = nullptr;

  PhysicalOperator *left_  = nullptr;   //! 左算子
  PhysicalOperator *right_ = nullptr;   //! 右算子

  // 哈希表存储左表行的拷贝，避免底层算子复用 tuple 对象造成指针失效
  std::unordered_map<HashKeyNode, std::vector<std::unique_ptr<ValueListTuple>>, HashAlgo> hash_table_;

  Tuple *right_tuple_ = nullptr;        //! 当前右表的元组
  JoinedTuple joined_tuple_;            //! 当前连接的元组

  std::vector<unique_ptr<Expression>> join_predicates_;  //! Join 条件

  bool build_done_ = false;              //! 构建阶段是否完成
  bool probe_done_ = false;              //! 探测阶段是否完成
  size_t current_bucket_index_ = 0;      //! 当前处理的哈希桶索引（用于处理多个匹配）
  std::vector<std::unique_ptr<ValueListTuple>> *current_matches_ = nullptr;  //! 当前匹配的左表元组列表
  size_t current_match_index_ = 0;       //! 当前匹配元组的索引
};