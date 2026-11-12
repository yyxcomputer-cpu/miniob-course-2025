#include "sql/operator/order_by_physical_operator.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
// A sort predicate must define a strict weak order. The legacy epsilon-based
// float comparison is non-transitive, and integer subtraction can overflow.
int compare_sort_values(const Value &left, const Value &right)
{
  if (left.attr_type() == AttrType::INTS) {
    const int a = left.get_int(), b = right.get_int();
    return (a > b) - (a < b);
  }
  if (left.attr_type() == AttrType::FLOATS) {
    const float a = left.get_float(), b = right.get_float();
    if (std::isnan(a) || std::isnan(b)) return int(std::isnan(a)) - int(std::isnan(b));
    return (a > b) - (a < b);
  }
  const string a = left.get_string(), b = right.get_string();
  return (a > b) - (a < b);
}

void initialize_output(Chunk &output, Chunk &input)
{
  output.reset();
  for (int i = 0; i < input.column_num(); ++i) {
    const Column &column = input.column(i);
    output.add_column(make_unique<Column>(column.attr_type(), column.attr_len()), input.column_ids(i));
  }
}
}

RC OrderByPhysicalOperator::open(Trx *trx)
{
  rows_.clear(); order_.clear(); cursor_ = 0; current_valid_ = false; output_.reset();
  if (children_.size() != 1 || items_.empty()) return RC::INVALID_ARGUMENT;
  RC rc = children_[0]->open(trx);
  if (rc != RC::SUCCESS) return rc;
  child_open_ = true;
  auto fail = [&](RC error) { close(); return error; };

  if (!vectorized_) {
    while ((rc = children_[0]->next()) == RC::SUCCESS) {
      Tuple *tuple = children_[0]->current_tuple();
      if (!tuple) return fail(RC::INTERNAL);
      Row row;
      rc = ValueListTuple::make(*tuple, row.tuple);
      if (rc != RC::SUCCESS) return fail(rc);
      for (auto &item : items_) {
        Value key;
        rc = item.expression->get_value(*tuple, key);
        if (rc != RC::SUCCESS) return fail(rc);
        row.keys.push_back(std::move(key));
      }
      rows_.push_back(std::move(row));
    }
  } else {
    Chunk input;
    while ((rc = children_[0]->next(input)) == RC::SUCCESS) {
      if (output_.column_num() == 0) initialize_output(output_, input);
      vector<Column> keys(items_.size());
      for (size_t i = 0; i < items_.size(); ++i) {
        rc = items_[i].expression->get_column(input, keys[i]);
        if (rc != RC::SUCCESS) return fail(rc);
      }
      for (int i = 0; i < input.rows(); ++i) {
        Row row;
        for (int col = 0; col < input.column_num(); ++col) row.values.push_back(input.get_value(col, i));
        for (auto &key : keys) row.keys.push_back(key.get_value(i));
        rows_.push_back(std::move(row));
      }
      input.reset();
    }
  }
  if (rc != RC::RECORD_EOF) return fail(rc);
  order_.resize(rows_.size());
  std::iota(order_.begin(), order_.end(), size_t(0));
  std::stable_sort(order_.begin(), order_.end(), [&](size_t a, size_t b) {
    for (size_t i = 0; i < items_.size(); ++i) {
      const int result = compare_sort_values(rows_[a].keys[i], rows_[b].keys[i]);
      if (result) return items_[i].ascending ? result < 0 : result > 0;
    }
    return false;
  });
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::next()
{
  current_valid_ = false;
  if (vectorized_) return RC::INVALID_ARGUMENT;
  if (cursor_ >= order_.size()) return RC::RECORD_EOF;
  ++cursor_; current_valid_ = true;
  return RC::SUCCESS;
}

Tuple *OrderByPhysicalOperator::current_tuple()
{
  return current_valid_ ? &rows_[order_[cursor_ - 1]].tuple : nullptr;
}

RC OrderByPhysicalOperator::next(Chunk &chunk)
{
  if (!vectorized_) return RC::INVALID_ARGUMENT;
  if (cursor_ >= order_.size()) return RC::RECORD_EOF;
  output_.reset_data();
  const size_t end = std::min(order_.size(), cursor_ + size_t(Chunk::MAX_ROWS));
  for (; cursor_ < end; ++cursor_) {
    const Row &row = rows_[order_[cursor_]];
    for (size_t col = 0; col < row.values.size(); ++col) {
      RC rc = output_.column(col).append_value(row.values[col]);
      if (rc != RC::SUCCESS) return rc;
    }
  }
  return chunk.reference(output_);
}

RC OrderByPhysicalOperator::close()
{
  RC rc = child_open_ ? children_[0]->close() : RC::SUCCESS;
  child_open_ = false;
  rows_.clear(); order_.clear(); output_.reset(); cursor_ = 0; current_valid_ = false;
  return rc;
}

RC LimitPhysicalOperator::open(Trx *trx)
{
  skipped_ = emitted_ = 0; current_valid_ = false; input_row_ = 0;
  input_.reset(); output_.reset();
  if (children_.size() != 1 || limit_ < 0 || offset_ < 0) return RC::INVALID_ARGUMENT;
  return children_[0]->open(trx);
}

RC LimitPhysicalOperator::next()
{
  current_valid_ = false;
  if (vectorized_) return RC::INVALID_ARGUMENT;
  if (emitted_ >= size_t(limit_)) return RC::RECORD_EOF;
  while (skipped_ < size_t(offset_)) {
    RC rc = children_[0]->next();
    if (rc != RC::SUCCESS) return rc;
    ++skipped_;
  }
  RC rc = children_[0]->next();
  if (rc == RC::SUCCESS) { ++emitted_; current_valid_ = true; }
  return rc;
}

RC LimitPhysicalOperator::next(Chunk &chunk)
{
  if (!vectorized_) return RC::INVALID_ARGUMENT;
  if (emitted_ >= size_t(limit_)) return RC::RECORD_EOF;
  output_.reset_data();
  while (output_.rows() < Chunk::MAX_ROWS && emitted_ < size_t(limit_)) {
    if (input_row_ >= input_.rows()) {
      input_.reset();
      RC rc = children_[0]->next(input_);
      if (rc == RC::RECORD_EOF) break;
      if (rc != RC::SUCCESS) return rc;
      input_row_ = 0;
      if (output_.column_num() == 0) initialize_output(output_, input_);
      if (input_.rows() == 0) continue;
    }
    if (skipped_ < size_t(offset_)) {
      ++skipped_; ++input_row_; continue;
    }
    for (int col = 0; col < input_.column_num(); ++col) {
      RC rc = output_.column(col).append_value(input_.get_value(col, input_row_));
      if (rc != RC::SUCCESS) return rc;
    }
    ++input_row_; ++emitted_;
  }
  return output_.rows() ? chunk.reference(output_) : RC::RECORD_EOF;
}

RC LimitPhysicalOperator::close()
{
  current_valid_ = false; input_.reset(); output_.reset();
  return children_[0]->close();
}
