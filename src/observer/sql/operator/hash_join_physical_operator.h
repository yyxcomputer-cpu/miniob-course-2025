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

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 */
class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator(vector<unique_ptr<Expression>> &&left_keys,
      vector<unique_ptr<Expression>> &&right_keys)
      : left_keys_(std::move(left_keys)), right_keys_(std::move(right_keys)) {}
  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }
  OpType get_op_type() const override { return OpType::INNERHASHJOIN; }
  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;
  Tuple *current_tuple() override { return valid_ ? &joined_ : nullptr; }
private:
  RC key(const Tuple &tuple, const vector<unique_ptr<Expression>> &expressions, string &result);
  vector<unique_ptr<Expression>> left_keys_, right_keys_;
  unordered_map<string, vector<ValueListTuple>> buckets_;
  vector<ValueListTuple> *matches_ = nullptr;
  size_t match_ = 0;
  JoinedTuple joined_;
  bool left_open_ = false, right_open_ = false, valid_ = false;
};
