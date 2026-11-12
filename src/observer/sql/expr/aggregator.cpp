/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2024/05/29.
//

#include "sql/expr/aggregator.h"
#include "common/log/log.h"

RC SumAggregator::accumulate(const Value &value)
{
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  Value::add(value, value_, value_);
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  result = value_;
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  if (value.attr_type() == AttrType::INTS) sum_ += value.get_int();
  else if (value.attr_type() == AttrType::FLOATS) sum_ += value.get_float();
  else return RC::INVALID_ARGUMENT;
  ++count_;
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  result.set_float(count_ ? float(sum_ / count_) : 0.0f);
  return RC::SUCCESS;
}

RC ExtremumAggregator::accumulate(const Value &value)
{
  int comparison = 0;
  if (value_.attr_type() != AttrType::UNDEFINED) {
    if (value.attr_type() == AttrType::INTS) {
      const int a = value.get_int(), b = value_.get_int();
      comparison = (a > b) - (a < b);
    } else if (value.attr_type() == AttrType::FLOATS) {
      const float a = value.get_float(), b = value_.get_float();
      comparison = (a > b) - (a < b);
    } else comparison = value.compare(value_);
  }
  if (value_.attr_type() == AttrType::UNDEFINED || (minimum_ ? comparison < 0 : comparison > 0)) value_ = value;
  return RC::SUCCESS;
}
