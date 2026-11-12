/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/group_by_vec_physical_operator.h"

GroupByVecPhysicalOperator::GroupByVecPhysicalOperator(
    vector<unique_ptr<Expression>> &&groups, vector<Expression *> &&aggregates)
    : groups_(std::move(groups)), aggregates_(std::move(aggregates)) {}

RC GroupByVecPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 1 || groups_.empty()) return RC::INVALID_ARGUMENT;
  table_ = make_unique<StandardAggregateHashTable>(aggregates_);
  scanner_.reset(); output_.reset();
  int id = 0;
  for (auto &expr : groups_)
    output_.add_column(make_unique<Column>(expr->value_type(), expr->value_length()), id++);
  for (auto *expr : aggregates_)
    output_.add_column(make_unique<Column>(expr->value_type(), expr->value_length()), id++);
  RC rc = children_[0]->open(trx);
  if (rc != RC::SUCCESS) return rc;
  child_open_ = true;
  auto fail = [&](RC error) { close(); return error; };
  Chunk input;
  while ((rc = children_[0]->next(input)) == RC::SUCCESS) {
    Chunk groups, aggregates;
    for (size_t i = 0; i < groups_.size(); ++i) {
      auto column = make_unique<Column>();
      rc = groups_[i]->get_column(input, *column);
      if (rc != RC::SUCCESS) return fail(rc);
      groups.add_column(std::move(column), i);
    }
    for (size_t i = 0; i < aggregates_.size(); ++i) {
      auto *expr = static_cast<AggregateExpr *>(aggregates_[i]);
      auto column = make_unique<Column>();
      rc = expr->child()->get_column(input, *column);
      if (rc != RC::SUCCESS) return fail(rc);
      aggregates.add_column(std::move(column), i);
    }
    rc = table_->add_chunk(groups, aggregates);
    if (rc != RC::SUCCESS) return fail(rc);
    input.reset();
  }
  if (rc != RC::RECORD_EOF) return fail(rc);
  scanner_ = make_unique<StandardAggregateHashTable::Scanner>(table_.get());
  scanner_->open_scan();
  return RC::SUCCESS;
}

RC GroupByVecPhysicalOperator::next(Chunk &chunk)
{
  if (!scanner_) return RC::INTERNAL;
  output_.reset_data();
  RC rc = scanner_->next(output_);
  return rc == RC::SUCCESS ? chunk.reference(output_) : rc;
}

RC GroupByVecPhysicalOperator::close()
{
  RC rc = child_open_ ? children_[0]->close() : RC::SUCCESS;
  child_open_ = false;
  scanner_.reset(); table_.reset(); output_.reset();
  return rc;
}
