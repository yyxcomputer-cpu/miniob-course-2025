#pragma once

#include "sql/operator/physical_operator.h"
#include "sql/parser/parse_defs.h"

// A blocking, owning sort. Both iterator interfaces share the comparison logic.
class OrderByPhysicalOperator : public PhysicalOperator
{
public:
  OrderByPhysicalOperator(vector<OrderBySqlNode> &&items, bool vectorized)
      : items_(std::move(items)), vectorized_(vectorized) {}
  PhysicalOperatorType type() const override
  { return vectorized_ ? PhysicalOperatorType::ORDER_BY_VEC : PhysicalOperatorType::ORDER_BY; }
  RC open(Trx *trx) override;
  RC next() override;
  RC next(Chunk &chunk) override;
  RC close() override;
  Tuple *current_tuple() override;

private:
  struct Row {
    ValueListTuple tuple;
    vector<Value> values;
    vector<Value> keys;
  };
  vector<OrderBySqlNode> items_;
  bool vectorized_;
  vector<Row> rows_;
  vector<size_t> order_;
  size_t cursor_ = 0;
  bool current_valid_ = false;
  bool child_open_ = false;
  Chunk output_;
};

class LimitPhysicalOperator : public PhysicalOperator
{
public:
  LimitPhysicalOperator(int limit, int offset, bool vectorized)
      : limit_(limit), offset_(offset), vectorized_(vectorized) {}
  PhysicalOperatorType type() const override
  { return vectorized_ ? PhysicalOperatorType::LIMIT_VEC : PhysicalOperatorType::LIMIT; }
  RC open(Trx *trx) override;
  RC next() override;
  RC next(Chunk &chunk) override;
  RC close() override;
  Tuple *current_tuple() override { return current_valid_ ? children_[0]->current_tuple() : nullptr; }
  RC tuple_schema(TupleSchema &schema) const override { return children_[0]->tuple_schema(schema); }

private:
  int limit_, offset_;
  bool vectorized_;
  size_t skipped_ = 0, emitted_ = 0;
  bool current_valid_ = false;
  Chunk input_, output_;
  int input_row_ = 0;
};
