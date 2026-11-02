#ifndef MINISQL_LOG_REC_H
#define MINISQL_LOG_REC_H

#include <unordered_map>
#include <utility>
#include <memory>

#include "common/config.h"
#include "common/rowid.h"
#include "record/row.h"

enum class LogRecType {
    kInvalid,
    kInsert,
    kDelete,
    kUpdate,
    kBegin,
    kCommit,
    kAbort,
};

// used for testing only
using KeyType = std::string;
using ValType = int32_t;

/**
 * TODO: Student Implement
 */
struct LogRec {
    LogRec() = default;

    LogRecType type_{LogRecType::kInvalid};
    txn_id_t txn_id_{INVALID_TXN_ID};
    lsn_t lsn_{INVALID_LSN};
    lsn_t prev_lsn_{INVALID_LSN};
    KeyType old_key_{};
    ValType old_val_{};
    KeyType new_key_{};
    ValType new_val_{};

    /* used for testing only */
    static std::unordered_map<txn_id_t, lsn_t> prev_lsn_map_;
    static lsn_t next_lsn_;
};

typedef std::shared_ptr<LogRec> LogRecPtr;

static LogRecPtr CreateLogRec(LogRecType type, txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = type;
    log->txn_id_ = txn_id;
    log->lsn_ = LogRec::next_lsn_++;
    auto prev_iter = LogRec::prev_lsn_map_.find(txn_id);
    log->prev_lsn_ = prev_iter == LogRec::prev_lsn_map_.end() ? INVALID_LSN : prev_iter->second;
    if (type == LogRecType::kCommit || type == LogRecType::kAbort) {
        LogRec::prev_lsn_map_.erase(txn_id);
    } else {
        LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    }
    return log;
}

static LogRecPtr CreateInsertLog(txn_id_t txn_id, KeyType ins_key, ValType ins_val) {
    auto log = CreateLogRec(LogRecType::kInsert, txn_id);
    log->new_key_ = std::move(ins_key);
    log->new_val_ = ins_val;
    return log;
}

static LogRecPtr CreateDeleteLog(txn_id_t txn_id, KeyType del_key, ValType del_val) {
    auto log = CreateLogRec(LogRecType::kDelete, txn_id);
    log->old_key_ = std::move(del_key);
    log->old_val_ = del_val;
    return log;
}

static LogRecPtr CreateUpdateLog(txn_id_t txn_id, KeyType old_key, ValType old_val, KeyType new_key, ValType new_val) {
    auto log = CreateLogRec(LogRecType::kUpdate, txn_id);
    log->old_key_ = std::move(old_key);
    log->old_val_ = old_val;
    log->new_key_ = std::move(new_key);
    log->new_val_ = new_val;
    return log;
}

static LogRecPtr CreateBeginLog(txn_id_t txn_id) {
    auto log = std::make_shared<LogRec>();
    log->type_ = LogRecType::kBegin;
    log->txn_id_ = txn_id;
    log->lsn_ = LogRec::next_lsn_++;
    log->prev_lsn_ = INVALID_LSN;
    LogRec::prev_lsn_map_[txn_id] = log->lsn_;
    return log;
}

static LogRecPtr CreateCommitLog(txn_id_t txn_id) {
    return CreateLogRec(LogRecType::kCommit, txn_id);
}

static LogRecPtr CreateAbortLog(txn_id_t txn_id) {
    return CreateLogRec(LogRecType::kAbort, txn_id);
}

#endif  // MINISQL_LOG_REC_H
