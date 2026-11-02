#ifndef MINISQL_RECOVERY_MANAGER_H
#define MINISQL_RECOVERY_MANAGER_H

#include <map>
#include <unordered_map>
#include <vector>

#include "recovery/log_rec.h"

using KvDatabase = std::unordered_map<KeyType, ValType>;
using ATT = std::unordered_map<txn_id_t, lsn_t>;

struct CheckPoint {
    lsn_t checkpoint_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase persist_data_{};

    inline void AddActiveTxn(txn_id_t txn_id, lsn_t last_lsn) { active_txns_[txn_id] = last_lsn; }

    inline void AddData(KeyType key, ValType val) { persist_data_.emplace(std::move(key), val); }
};

class RecoveryManager {
public:
    void Init(CheckPoint &last_checkpoint) {
        persist_lsn_ = last_checkpoint.checkpoint_lsn_;
        active_txns_ = last_checkpoint.active_txns_;
        data_ = last_checkpoint.persist_data_;
    }

    void RedoPhase() {
        for (const auto &entry : log_recs_) {
            auto log = entry.second;
            if (log->lsn_ <= persist_lsn_) {
                continue;
            }
            switch (log->type_) {
                case LogRecType::kBegin:
                    active_txns_[log->txn_id_] = log->lsn_;
                    break;
                case LogRecType::kInsert:
                    data_[log->new_key_] = log->new_val_;
                    active_txns_[log->txn_id_] = log->lsn_;
                    break;
                case LogRecType::kDelete:
                    data_.erase(log->old_key_);
                    active_txns_[log->txn_id_] = log->lsn_;
                    break;
                case LogRecType::kUpdate:
                    data_.erase(log->old_key_);
                    data_[log->new_key_] = log->new_val_;
                    active_txns_[log->txn_id_] = log->lsn_;
                    break;
                case LogRecType::kCommit:
                    active_txns_.erase(log->txn_id_);
                    break;
                case LogRecType::kAbort:
                    RollbackTxn(log->prev_lsn_);
                    active_txns_.erase(log->txn_id_);
                    break;
                default:
                    break;
            }
        }
    }

    void UndoPhase() {
        std::vector<lsn_t> undo_lsns;
        undo_lsns.reserve(active_txns_.size());
        for (const auto &entry : active_txns_) {
            undo_lsns.emplace_back(entry.second);
        }
        for (auto lsn : undo_lsns) {
            RollbackTxn(lsn);
        }
        active_txns_.clear();
    }

    // used for test only
    void AppendLogRec(LogRecPtr log_rec) { log_recs_.emplace(log_rec->lsn_, log_rec); }

    // used for test only
    inline KvDatabase &GetDatabase() { return data_; }

private:
    void UndoLog(const LogRecPtr &log_rec) {
        switch (log_rec->type_) {
            case LogRecType::kInsert:
                data_.erase(log_rec->new_key_);
                break;
            case LogRecType::kDelete:
                data_[log_rec->old_key_] = log_rec->old_val_;
                break;
            case LogRecType::kUpdate:
                data_.erase(log_rec->new_key_);
                data_[log_rec->old_key_] = log_rec->old_val_;
                break;
            default:
                break;
        }
    }

    void RollbackTxn(lsn_t last_lsn) {
        auto lsn = last_lsn;
        while (lsn != INVALID_LSN) {
            auto iter = log_recs_.find(lsn);
            if (iter == log_recs_.end()) {
                break;
            }
            UndoLog(iter->second);
            lsn = iter->second->prev_lsn_;
        }
    }

    std::map<lsn_t, LogRecPtr> log_recs_{};
    lsn_t persist_lsn_{INVALID_LSN};
    ATT active_txns_{};
    KvDatabase data_{};  // all data in database
};

#endif  // MINISQL_RECOVERY_MANAGER_H
