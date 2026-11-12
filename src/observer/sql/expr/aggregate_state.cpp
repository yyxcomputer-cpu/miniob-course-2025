/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/aggregate_state.h"
#include <stdint.h>

#ifdef USE_SIMD
#include "common/math/simd_util.h"
#endif
template <typename T>
void SumState<T>::update(const T *values, int size)
{
#ifdef USE_SIMD
  if constexpr (is_same<T, float>::value) {
    value += mm256_sum_ps(values, size);
  } else if constexpr (is_same<T, int>::value) {
    value += mm256_sum_epi32(values, size);
  }
#else
  for (int i = 0; i < size; ++i) {
 	  value += values[i];
  }
#endif
}

template <typename T>
void AvgState<T>::update(const T *values, int size)
{
  for (int i = 0; i < size; ++i) {
 	  value += values[i];
  }
  count += size;
}

template <typename T>
void CountState<T>::update(const T *values, int size)
{
  value += size;
}

void* create_aggregate_state(AggregateExpr::Type aggr_type, AttrType attr_type)
{
  if (aggr_type == AggregateExpr::Type::MIN || aggr_type == AggregateExpr::Type::MAX)
    return new ExtremumAggregator(aggr_type == AggregateExpr::Type::MIN);
  void* state_ptr = nullptr;
  if (aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(SumState<int>));
      new (state_ptr) SumState<int>();
    } else if (attr_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(SumState<float>));
      new (state_ptr) SumState<float>();
    } else {
      LOG_WARN("unsupported aggregate value type");
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    state_ptr = malloc(sizeof(CountState<int>));
    new (state_ptr) CountState<int>();
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(AvgState<int>));
      new (state_ptr) AvgState<int>();
    } else if (attr_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(AvgState<float>));
      new (state_ptr) AvgState<float>();
    } else {
      LOG_WARN("unsupported aggregate value type");
    }
  } else {
    LOG_WARN("unsupported aggregator type");
  }
  return state_ptr;
}

RC aggregate_state_update_by_value(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, const Value& val)
{
  if (!state) return RC::INVALID_ARGUMENT;
  if (aggr_type == AggregateExpr::Type::MIN || aggr_type == AggregateExpr::Type::MAX)
    return static_cast<ExtremumAggregator *>(state)->accumulate(val);
  RC rc = RC::SUCCESS;
  if (aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      static_cast<SumState<int>*>(state)->update(val.get_int());
    } else if (attr_type == AttrType::FLOATS) {
      static_cast<SumState<float>*>(state)->update(val.get_float());
    } else {
      LOG_WARN("unsupported aggregate value type");
      return RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    static_cast<CountState<int>*>(state)->update(1);
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      static_cast<AvgState<int>*>(state)->update(val.get_int());
    } else if (attr_type == AttrType::FLOATS) {
      static_cast<AvgState<float>*>(state)->update(val.get_float());
    } else {
      LOG_WARN("unsupported aggregate value type");
      return RC::UNIMPLEMENTED;
    }
  } else {
    LOG_WARN("unsupported aggregator type");
    return RC::UNIMPLEMENTED;
  }
  return rc;
}

template <class STATE, typename T>
void append_to_column(void *state, Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T res = state_ptr->template finalize<T>();
  column.append_one((char *)&res);
}

RC finialize_aggregate_state(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, Column& col)
{
  if (!state) return RC::INVALID_ARGUMENT;
  if (aggr_type == AggregateExpr::Type::MIN || aggr_type == AggregateExpr::Type::MAX) {
    Value result;
    RC rc = static_cast<ExtremumAggregator *>(state)->evaluate(result);
    if (rc != RC::SUCCESS) return rc;
    if (result.attr_type() == AttrType::UNDEFINED) {
      if (attr_type == AttrType::INTS) result.set_int(0);
      else if (attr_type == AttrType::FLOATS) result.set_float(0);
      else result.set_string("");
    }
    return col.append_value(result);
  }
  RC rc = RC::SUCCESS;
  if ( aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      append_to_column<SumState<int>, int>(state, col);
    } else if (attr_type == AttrType::FLOATS) {
      append_to_column<SumState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
      LOG_WARN("unsupported aggregate value type");
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    append_to_column<CountState<int>, int>(state, col);
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      append_to_column<AvgState<int>, float>(state, col);
    } else if (attr_type == AttrType::FLOATS) {
      append_to_column<AvgState<float>, float>(state, col);
    } else {
      rc = RC::UNIMPLEMENTED;
      LOG_WARN("unsupported aggregate value type");
    }// 
  } else {
    rc = RC::UNIMPLEMENTED;
    LOG_WARN("unsupported aggregator type");
  }
  return rc;
}

template <class STATE, typename T>
void update_aggregate_state(void *state, const Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T *    data      = (T *)column.data();
  state_ptr->update(data, column.count());
}

RC aggregate_state_update_by_column(void *state, AggregateExpr::Type type, AttrType attr_type, Column &column)
{
  // Constant columns store one physical value but represent count() logical rows.
  for (int i = 0; i < column.count(); ++i) {
    RC rc = aggregate_state_update_by_value(state, type, attr_type, column.get_value(i));
    if (rc != RC::SUCCESS) return rc;
  }
  return RC::SUCCESS;
}

void destroy_aggregate_state(void *state, AggregateExpr::Type type)
{
  if (type == AggregateExpr::Type::MIN || type == AggregateExpr::Type::MAX)
    delete static_cast<ExtremumAggregator *>(state);
  else free(state);
}

template class SumState<int>;
template class SumState<float>;

template class CountState<int>;

template class AvgState<int>;
template class AvgState<float>;
