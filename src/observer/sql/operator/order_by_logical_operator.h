#pragma once
#include "sql/operator/logical_operator.h"
#include "sql/parser/parse_defs.h"

class OrderByLogicalOperator : public LogicalOperator
{
public:
  explicit OrderByLogicalOperator(vector<OrderBySqlNode> &&items) : items_(std::move(items)) {}
  LogicalOperatorType type() const override { return LogicalOperatorType::ORDER_BY; }
  OpType get_op_type() const override { return OpType::ORDERBY; }
  vector<OrderBySqlNode> &items() { return items_; }
private:
  vector<OrderBySqlNode> items_;
};

class LimitLogicalOperator : public LogicalOperator
{
public:
  LimitLogicalOperator(int limit, int offset) : limit_(limit), offset_(offset) {}
  LogicalOperatorType type() const override { return LogicalOperatorType::LIMIT; }
  OpType get_op_type() const override { return OpType::LOGICALLIMIT; }
  int limit() const { return limit_; }
  int offset() const { return offset_; }
private:
  int limit_, offset_;
};
