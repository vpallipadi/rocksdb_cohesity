//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "utilities/transactions/write_prepared_txn.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <map>
#include <set>

#include "db/column_family.h"
#include "db/db_impl.h"
#include "rocksdb/db.h"
#include "rocksdb/status.h"
#include "rocksdb/utilities/transaction_db.h"
#include "utilities/transactions/pessimistic_transaction.h"
#include "utilities/transactions/write_prepared_txn_db.h"

namespace rocksdb {

struct WriteOptions;

WritePreparedTxn::WritePreparedTxn(WritePreparedTxnDB* txn_db,
                                   const WriteOptions& write_options,
                                   const TransactionOptions& txn_options)
    : PessimisticTransaction(txn_db, write_options, txn_options),
      wpt_db_(txn_db) {}

Status WritePreparedTxn::Get(const ReadOptions& read_options,
                             ColumnFamilyHandle* column_family,
                             const Slice& key, PinnableSlice* pinnable_val) {
  auto snapshot = read_options.snapshot;
  auto snap_seq =
      snapshot != nullptr ? snapshot->GetSequenceNumber() : kMaxSequenceNumber;

  WritePreparedTxnReadCallback callback(wpt_db_, snap_seq);
  return write_batch_.GetFromBatchAndDB(db_, read_options, column_family, key,
                                        pinnable_val, &callback);
}

Iterator* WritePreparedTxn::GetIterator(const ReadOptions& options) {
  // Make sure to get iterator from WritePrepareTxnDB, not the root db.
  Iterator* db_iter = wpt_db_->NewIterator(options);
  assert(db_iter);

  return write_batch_.NewIteratorWithBase(db_iter);
}

Iterator* WritePreparedTxn::GetIterator(const ReadOptions& options,
                                        ColumnFamilyHandle* column_family) {
  // Make sure to get iterator from WritePrepareTxnDB, not the root db.
  Iterator* db_iter = wpt_db_->NewIterator(options, column_family);
  assert(db_iter);

  return write_batch_.NewIteratorWithBase(column_family, db_iter);
}

Status WritePreparedTxn::PrepareInternal() {
  WriteOptions write_options = write_options_;
  write_options.disableWAL = false;
  const bool WRITE_AFTER_COMMIT = true;
  WriteBatchInternal::MarkEndPrepare(GetWriteBatch()->GetWriteBatch(), name_,
                                     !WRITE_AFTER_COMMIT);
  const bool DISABLE_MEMTABLE = true;
  uint64_t seq_used = kMaxSequenceNumber;
  // For each duplicate key we account for a new sub-batch
  prepare_batch_cnt_ = 1;
  if (UNLIKELY(GetWriteBatch()->HasDuplicateKeys())) {
    ROCKS_LOG_WARN(db_impl_->immutable_db_options().info_log,
                   "Duplicate key overhead");
    SubBatchCounter counter(*wpt_db_->GetCFComparatorMap());
    auto s = GetWriteBatch()->GetWriteBatch()->Iterate(&counter);
    assert(s.ok());
    prepare_batch_cnt_ = counter.BatchCount();
  }
  Status s =
      db_impl_->WriteImpl(write_options, GetWriteBatch()->GetWriteBatch(),
                          /*callback*/ nullptr, &log_number_, /*log ref*/ 0,
                          !DISABLE_MEMTABLE, &seq_used, prepare_batch_cnt_);
  assert(!s.ok() || seq_used != kMaxSequenceNumber);
  auto prepare_seq = seq_used;
  SetId(prepare_seq);
  // TODO(myabandeh): AddPrepared better to be called in the pre-release
  // callback otherwise there is a non-zero chance of max dvancing prepare_seq
  // and readers assume the data as committed.
  if (s.ok()) {
    wpt_db_->AddPrepared(prepare_seq);
  }
  return s;
}

Status WritePreparedTxn::CommitWithoutPrepareInternal() {
  // For each duplicate key we account for a new sub-batch
  size_t batch_cnt = 1;
  if (UNLIKELY(GetWriteBatch()->HasDuplicateKeys())) {
    batch_cnt = 0;  // this will trigger a batch cnt compute
  }
  return CommitBatchInternal(GetWriteBatch()->GetWriteBatch(), batch_cnt);
}

Status WritePreparedTxn::CommitBatchInternal(WriteBatch* batch,
                                             size_t batch_cnt) {
  return wpt_db_->WriteInternal(write_options_, batch, batch_cnt, this);
}

Status WritePreparedTxn::CommitInternal() {
  ROCKS_LOG_DETAILS(db_impl_->immutable_db_options().info_log,
                    "CommitInternal prepare_seq: %" PRIu64, GetID());
  // We take the commit-time batch and append the Commit marker.
  // The Memtable will ignore the Commit marker in non-recovery mode
  WriteBatch* working_batch = GetCommitTimeWriteBatch();
  const bool empty = working_batch->Count() == 0;
  WriteBatchInternal::MarkCommit(working_batch, name_);

  const bool for_recovery = use_only_the_last_commit_time_batch_for_recovery_;
  if (!empty && for_recovery) {
    // When not writing to memtable, we can still cache the latest write batch.
    // The cached batch will be written to memtable in WriteRecoverableState
    // during FlushMemTable
    WriteBatchInternal::SetAsLastestPersistentState(working_batch);
  }

  auto prepare_seq = GetId();
  const bool includes_data = !empty && !for_recovery;
  assert(prepare_batch_cnt_);
  size_t commit_batch_cnt = 0;
  if (UNLIKELY(includes_data)) {
    ROCKS_LOG_WARN(db_impl_->immutable_db_options().info_log,
                   "Duplicate key overhead");
    SubBatchCounter counter(*wpt_db_->GetCFComparatorMap());
    auto s = working_batch->Iterate(&counter);
    assert(s.ok());
    commit_batch_cnt = counter.BatchCount();
  }
  WritePreparedCommitEntryPreReleaseCallback update_commit_map(
      wpt_db_, db_impl_, prepare_seq, prepare_batch_cnt_, commit_batch_cnt);
  const bool disable_memtable = !includes_data;
  uint64_t seq_used = kMaxSequenceNumber;
  // Since the prepared batch is directly written to memtable, there is already
  // a connection between the memtable and its WAL, so there is no need to
  // redundantly reference the log that contains the prepared data.
  const uint64_t zero_log_number = 0ull;
  size_t batch_cnt = UNLIKELY(commit_batch_cnt) ? commit_batch_cnt : 1;
  auto s = db_impl_->WriteImpl(write_options_, working_batch, nullptr, nullptr,
                               zero_log_number, disable_memtable, &seq_used,
                               batch_cnt, &update_commit_map);
  assert(!s.ok() || seq_used != kMaxSequenceNumber);
  return s;
}

Status WritePreparedTxn::RollbackInternal() {
  ROCKS_LOG_WARN(db_impl_->immutable_db_options().info_log,
                 "RollbackInternal prepare_seq: %" PRIu64, GetId());
  WriteBatch rollback_batch;
  assert(GetId() != kMaxSequenceNumber);
  assert(GetId() > 0);
  // In WritePrepared, the txn is is the same as prepare seq
  auto last_visible_txn = GetId() - 1;
  struct RollbackWriteBatchBuilder : public WriteBatch::Handler {
    DBImpl* db_;
    ReadOptions roptions;
    WritePreparedTxnReadCallback callback;
    WriteBatch* rollback_batch_;
    std::map<uint32_t, const Comparator*>& comparators_;
    using CFKeys = std::set<Slice, SetComparator>;
    std::map<uint32_t, CFKeys> keys_;
    RollbackWriteBatchBuilder(
        DBImpl* db, WritePreparedTxnDB* wpt_db, SequenceNumber snap_seq,
        WriteBatch* dst_batch,
        std::map<uint32_t, const Comparator*>& comparators)
        : db_(db),
          callback(wpt_db, snap_seq),
          rollback_batch_(dst_batch),
          comparators_(comparators) {}

    Status Rollback(uint32_t cf, const Slice& key) {
      Status s;
      CFKeys& cf_keys = keys_[cf];
      if (cf_keys.size() == 0) {  // just inserted
        auto cmp = comparators_[cf];
        keys_[cf] = CFKeys(SetComparator(cmp));
      }
      auto it = cf_keys.insert(key);
      if (it.second ==
          false) {  // second is false if a element already existed.
        return s;
      }

      PinnableSlice pinnable_val;
      bool not_used;
      auto cf_handle = db_->GetColumnFamilyHandle(cf);
      s = db_->GetImpl(roptions, cf_handle, key, &pinnable_val, &not_used,
                       &callback);
      assert(s.ok() || s.IsNotFound());
      if (s.ok()) {
        s = rollback_batch_->Put(cf_handle, key, pinnable_val);
        assert(s.ok());
      } else if (s.IsNotFound()) {
        // There has been no readable value before txn. By adding a delete we
        // make sure that there will be none afterwards either.
        s = rollback_batch_->Delete(cf_handle, key);
        assert(s.ok());
      } else {
        // Unexpected status. Return it to the user.
      }
      return s;
    }

    Status PutCF(uint32_t cf, const Slice& key, const Slice& /*val*/) override {
      return Rollback(cf, key);
    }

    Status DeleteCF(uint32_t cf, const Slice& key) override {
      return Rollback(cf, key);
    }

    Status SingleDeleteCF(uint32_t cf, const Slice& key) override {
      return Rollback(cf, key);
    }

    Status MergeCF(uint32_t cf, const Slice& key,
                   const Slice& /*val*/) override {
      return Rollback(cf, key);
    }

    Status MarkNoop(bool) override { return Status::OK(); }
    Status MarkBeginPrepare() override { return Status::OK(); }
    Status MarkEndPrepare(const Slice&) override { return Status::OK(); }
    Status MarkCommit(const Slice&) override { return Status::OK(); }
    Status MarkRollback(const Slice&) override {
      return Status::InvalidArgument();
    }

   protected:
    virtual bool WriteAfterCommit() const override { return false; }
  } rollback_handler(db_impl_, wpt_db_, last_visible_txn, &rollback_batch,
                     *wpt_db_->GetCFComparatorMap());
  auto s = GetWriteBatch()->GetWriteBatch()->Iterate(&rollback_handler);
  assert(s.ok());
  if (!s.ok()) {
    return s;
  }
  // The Rollback marker will be used as a batch separator
  WriteBatchInternal::MarkRollback(&rollback_batch, name_);
  bool do_one_write = !db_impl_->immutable_db_options().two_write_queues;
  const bool DISABLE_MEMTABLE = true;
  const uint64_t no_log_ref = 0;
  uint64_t seq_used = kMaxSequenceNumber;
  const size_t ZERO_PREPARES = 0;
  const size_t ONE_BATCH = 1;
  WritePreparedCommitEntryPreReleaseCallback update_commit_map(
      wpt_db_, db_impl_, kMaxSequenceNumber, ZERO_PREPARES, ONE_BATCH);
  s = db_impl_->WriteImpl(write_options_, &rollback_batch, nullptr, nullptr,
                          no_log_ref, !DISABLE_MEMTABLE, &seq_used, ONE_BATCH,
                          do_one_write ? &update_commit_map : nullptr);
  assert(!s.ok() || seq_used != kMaxSequenceNumber);
  if (!s.ok()) {
    return s;
  }
  if (do_one_write) {
    // Mark the txn as rolled back
    uint64_t& rollback_seq = seq_used;
    wpt_db_->RollbackPrepared(GetId(), rollback_seq);
    return s;
  }  // else do the 2nd write for commit
  uint64_t& prepare_seq = seq_used;
  ROCKS_LOG_DETAILS(db_impl_->immutable_db_options().info_log,
                    "RollbackInternal 2nd write prepare_seq: %" PRIu64,
                    prepare_seq);
  // Commit the batch by writing an empty batch to the queue that will release
  // the commit sequence number to readers.
  const size_t ZERO_COMMITS = 0;
  const bool PREP_HEAP_SKIPPED = true;
  WritePreparedCommitEntryPreReleaseCallback update_commit_map_with_prepare(
      wpt_db_, db_impl_, prepare_seq, ONE_BATCH, ZERO_COMMITS,
      PREP_HEAP_SKIPPED);
  WriteBatch empty_batch;
  empty_batch.PutLogData(Slice());
  // In the absence of Prepare markers, use Noop as a batch separator
  WriteBatchInternal::InsertNoop(&empty_batch);
  s = db_impl_->WriteImpl(write_options_, &empty_batch, nullptr, nullptr,
                          no_log_ref, DISABLE_MEMTABLE, &seq_used, ONE_BATCH,
                          &update_commit_map_with_prepare);
  assert(!s.ok() || seq_used != kMaxSequenceNumber);
  // Mark the txn as rolled back
  uint64_t& rollback_seq = seq_used;
  if (s.ok()) {
    wpt_db_->RollbackPrepared(GetId(), rollback_seq);
  }

  return s;
}

Status WritePreparedTxn::ValidateSnapshot(ColumnFamilyHandle* column_family,
                                          const Slice& key,
                                          SequenceNumber* tracked_at_seq) {
  assert(snapshot_);

  SequenceNumber snap_seq = snapshot_->GetSequenceNumber();
  // tracked_at_seq is either max or the last snapshot with which this key was
  // trackeed so there is no need to apply the IsInSnapshot to this comparison
  // here as tracked_at_seq is not a prepare seq.
  if (*tracked_at_seq <= snap_seq) {
    // If the key has been previous validated at a sequence number earlier
    // than the curent snapshot's sequence number, we already know it has not
    // been modified.
    return Status::OK();
  }

  *tracked_at_seq = snap_seq;

  ColumnFamilyHandle* cfh =
      column_family ? column_family : db_impl_->DefaultColumnFamily();

  WritePreparedTxnReadCallback snap_checker(wpt_db_, snap_seq);
  return TransactionUtil::CheckKeyForConflicts(db_impl_, cfh, key.ToString(),
                                               snap_seq, false /* cache_only */,
                                               &snap_checker);
}

Status WritePreparedTxn::RebuildFromWriteBatch(WriteBatch* src_batch) {
  auto ret = PessimisticTransaction::RebuildFromWriteBatch(src_batch);
  prepare_batch_cnt_ = 1;
  if (UNLIKELY(GetWriteBatch()->HasDuplicateKeys())) {
    ROCKS_LOG_WARN(db_impl_->immutable_db_options().info_log,
                   "Duplicate key overhead");
    SubBatchCounter counter(*wpt_db_->GetCFComparatorMap());
    auto s = GetWriteBatch()->GetWriteBatch()->Iterate(&counter);
    assert(s.ok());
    prepare_batch_cnt_ = counter.BatchCount();
  }
  return ret;
}

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
