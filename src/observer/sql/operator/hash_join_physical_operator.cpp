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

RC HashJoinPhysicalOperator::key(const Tuple &tuple,
    const vector<unique_ptr<Expression>> &expressions, string &result)
{
  result.clear();
  for (auto &expression : expressions) {
    Value value;
    RC rc = expression->get_value(tuple, value);
    if (rc != RC::SUCCESS) return rc;
    if (value.attr_type() == AttrType::INTS) {
      const int data = value.get_int();
      result.append(reinterpret_cast<const char *>(&data), sizeof(data));
    } else if (value.attr_type() == AttrType::CHARS) {
      const string data = value.get_string();
      const uint64_t length = data.size();
      result.append(reinterpret_cast<const char *>(&length), sizeof(length));
      result.append(data);
    } else {
      return RC::UNSUPPORTED;
    }
  }
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
  buckets_.clear(); matches_ = nullptr; match_ = 0; valid_ = false;
  if (children_.size() != 2 || left_keys_.empty()) return RC::INVALID_ARGUMENT;
  RC rc = children_[0]->open(trx);
  if (rc != RC::SUCCESS) return rc;
  left_open_ = true;
  auto fail = [&](RC error) { close(); return error; };
  while ((rc = children_[0]->next()) == RC::SUCCESS) {
    Tuple *tuple = children_[0]->current_tuple();
    if (!tuple) return fail(RC::INTERNAL);
    string hash_key;
    rc = key(*tuple, left_keys_, hash_key);
    if (rc != RC::SUCCESS) return fail(rc);
    ValueListTuple copy;
    rc = ValueListTuple::make(*tuple, copy);
    if (rc != RC::SUCCESS) return fail(rc);
    buckets_[hash_key].push_back(std::move(copy));
  }
  if (rc != RC::RECORD_EOF) return fail(rc);
  rc = children_[1]->open(trx);
  if (rc != RC::SUCCESS) return fail(rc);
  right_open_ = true;
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::next()
{
  valid_ = false;
  while (true) {
    if (matches_ && match_ < matches_->size()) {
      joined_.set_left(&(*matches_)[match_++]);
      valid_ = true;
      return RC::SUCCESS;
    }
    RC rc = children_[1]->next();
    if (rc != RC::SUCCESS) return rc;
    Tuple *tuple = children_[1]->current_tuple();
    if (!tuple) return RC::INTERNAL;
    string hash_key;
    rc = key(*tuple, right_keys_, hash_key);
    if (rc != RC::SUCCESS) return rc;
    auto found = buckets_.find(hash_key);
    if (found == buckets_.end()) continue;
    matches_ = &found->second; match_ = 0;
    joined_.set_right(tuple);
  }
}

RC HashJoinPhysicalOperator::close()
{
  RC left_rc = left_open_ ? children_[0]->close() : RC::SUCCESS;
  RC right_rc = right_open_ ? children_[1]->close() : RC::SUCCESS;
  left_open_ = right_open_ = valid_ = false;
  matches_ = nullptr; buckets_.clear();
  return left_rc != RC::SUCCESS ? left_rc : right_rc;
}
