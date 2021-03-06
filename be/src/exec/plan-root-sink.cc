// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/plan-root-sink.h"

#include "exprs/scalar-expr.h"
#include "exprs/scalar-expr-evaluator.h"
#include "runtime/row-batch.h"
#include "runtime/tuple-row.h"
#include "service/query-result-set.h"

#include <memory>
#include <boost/thread/mutex.hpp>

using namespace std;
using boost::unique_lock;
using boost::mutex;

namespace impala {

PlanRootSink::PlanRootSink(const RowDescriptor* row_desc, RuntimeState* state)
  : DataSink(row_desc, "PLAN_ROOT_SINK", state) {}

namespace {

/// Validates that all collection-typed slots in the given batch are set to NULL.
/// See SubplanNode for details on when collection-typed slots are set to NULL.
/// TODO: This validation will become obsolete when we can return collection values.
/// We will then need a different mechanism to assert the correct behavior of the
/// SubplanNode with respect to setting collection-slots to NULL.
void ValidateCollectionSlots(const RowDescriptor& row_desc, RowBatch* batch) {
#ifndef NDEBUG
  if (!row_desc.HasVarlenSlots()) return;
  for (int i = 0; i < batch->num_rows(); ++i) {
    TupleRow* row = batch->GetRow(i);
    for (int j = 0; j < row_desc.tuple_descriptors().size(); ++j) {
      const TupleDescriptor* tuple_desc = row_desc.tuple_descriptors()[j];
      if (tuple_desc->collection_slots().empty()) continue;
      for (int k = 0; k < tuple_desc->collection_slots().size(); ++k) {
        const SlotDescriptor* slot_desc = tuple_desc->collection_slots()[k];
        int tuple_idx = row_desc.GetTupleIdx(slot_desc->parent()->id());
        const Tuple* tuple = row->GetTuple(tuple_idx);
        if (tuple == NULL) continue;
        DCHECK(tuple->IsNull(slot_desc->null_indicator_offset()));
      }
    }
  }
#endif
}
}

Status PlanRootSink::Send(RuntimeState* state, RowBatch* batch) {
  ValidateCollectionSlots(*row_desc_, batch);
  int current_batch_row = 0;

  // Don't enter the loop if batch->num_rows() == 0; no point triggering the consumer with
  // 0 rows to return. Be wary of ever returning 0-row batches to the client; some poorly
  // written clients may not cope correctly with them. See IMPALA-4335.
  while (current_batch_row < batch->num_rows()) {
    unique_lock<mutex> l(lock_);
    // Wait until the consumer gives us a result set to fill in, or the fragment
    // instance has been cancelled.
    while (results_ == nullptr && !state->is_cancelled()) sender_cv_.Wait(l);
    RETURN_IF_CANCELLED(state);

    // Otherwise the consumer is ready. Fill out the rows.
    DCHECK(results_ != nullptr);
    // List of expr values to hold evaluated rows from the query
    vector<void*> result_row;
    result_row.resize(output_exprs_.size());

    // List of scales for floating point values in result_row
    vector<int> scales;
    scales.resize(result_row.size());

    int num_to_fetch = batch->num_rows() - current_batch_row;
    if (num_rows_requested_ > 0) num_to_fetch = min(num_to_fetch, num_rows_requested_);
    for (int i = 0; i < num_to_fetch; ++i) {
      TupleRow* row = batch->GetRow(current_batch_row);
      GetRowValue(row, &result_row, &scales);
      RETURN_IF_ERROR(results_->AddOneRow(result_row, scales));
      ++current_batch_row;
    }
    // Prevent expr result allocations from accumulating.
    expr_results_pool_->Clear();
    // Signal the consumer.
    results_ = nullptr;
    consumer_cv_.NotifyAll();
  }
  return Status::OK();
}

Status PlanRootSink::FlushFinal(RuntimeState* state) {
  unique_lock<mutex> l(lock_);
  sender_state_ = SenderState::EOS;
  consumer_cv_.NotifyAll();
  return Status::OK();
}

void PlanRootSink::Close(RuntimeState* state) {
  unique_lock<mutex> l(lock_);
  // FlushFinal() won't have been called when the fragment instance encounters an error
  // before sending all rows.
  if (sender_state_ == SenderState::ROWS_PENDING) {
    sender_state_ = SenderState::CLOSED_NOT_EOS;
  }
  consumer_cv_.NotifyAll();
  DataSink::Close(state);
}

void PlanRootSink::Cancel(RuntimeState* state) {
  DCHECK(state->is_cancelled());
  sender_cv_.NotifyAll();
  consumer_cv_.NotifyAll();
}

Status PlanRootSink::GetNext(
    RuntimeState* state, QueryResultSet* results, int num_results, bool* eos) {
  unique_lock<mutex> l(lock_);

  results_ = results;
  num_rows_requested_ = num_results;
  sender_cv_.NotifyAll();

  // Wait while the sender is still producing rows and hasn't filled in the current
  // result set.
  while (sender_state_ == SenderState::ROWS_PENDING && results_ != nullptr &&
      !state->is_cancelled()) {
    consumer_cv_.Wait(l);
  }

  *eos = sender_state_ == SenderState::EOS;
  return state->GetQueryStatus();
}

void PlanRootSink::GetRowValue(
    TupleRow* row, vector<void*>* result, vector<int>* scales) {
  DCHECK_GE(result->size(), output_expr_evals_.size());
  for (int i = 0; i < output_expr_evals_.size(); ++i) {
    (*result)[i] = output_expr_evals_[i]->GetValue(row);
    (*scales)[i] = output_expr_evals_[i]->output_scale();
  }
}
}
