/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/session.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/retryable_writes_stats.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/server_transactions_metrics.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

// Server parameter that dictates the max number of milliseconds that any transaction lock request
// will wait for lock acquisition. If an operation provides a greater timeout in a lock request,
// maxTransactionLockRequestTimeoutMillis will override it. If this is set to a negative value, it
// is inactive and nothing will be overridden.
//
// 5 milliseconds will help avoid deadlocks, but will still allow fast-running metadata operations
// to run without aborting transactions.
MONGO_EXPORT_SERVER_PARAMETER(maxTransactionLockRequestTimeoutMillis, int, 5);

// Server parameter that dictates the lifetime given to each transaction.
// Transactions must eventually expire to preempt storage cache pressure immobilizing the system.
MONGO_EXPORT_SERVER_PARAMETER(transactionLifetimeLimitSeconds, std::int32_t, 60)
    ->withValidator([](const auto& potentialNewValue) {
        if (potentialNewValue < 1) {
            return Status(ErrorCodes::BadValue,
                          "transactionLifetimeLimitSeconds must be greater than or equal to 1s");
        }

        return Status::OK();
    });


namespace {

// The command names that are allowed in a multi-document transaction.
const StringMap<int> txnCmdWhitelist = {{"abortTransaction", 1},
                                        {"aggregate", 1},
                                        {"commitTransaction", 1},
                                        {"coordinateCommitTransaction", 1},
                                        {"delete", 1},
                                        {"distinct", 1},
                                        {"doTxn", 1},
                                        {"find", 1},
                                        {"findandmodify", 1},
                                        {"findAndModify", 1},
                                        {"geoSearch", 1},
                                        {"getMore", 1},
                                        {"insert", 1},
                                        {"killCursors", 1},
                                        {"prepareTransaction", 1},
                                        {"update", 1}};

// The command names that are allowed in a multi-document transaction only when test commands are
// enabled.
const StringMap<int> txnCmdForTestingWhitelist = {{"dbHash", 1}};

// The commands that can be run on the 'admin' database in multi-document transactions.
const StringMap<int> txnAdminCommands = {{"abortTransaction", 1},
                                         {"commitTransaction", 1},
                                         {"coordinateCommitTransaction", 1},
                                         {"doTxn", 1},
                                         {"prepareTransaction", 1}};

void fassertOnRepeatedExecution(const LogicalSessionId& lsid,
                                TxnNumber txnNumber,
                                StmtId stmtId,
                                const repl::OpTime& firstOpTime,
                                const repl::OpTime& secondOpTime) {
    severe() << "Statement id " << stmtId << " from transaction [ " << lsid.toBSON() << ":"
             << txnNumber << " ] was committed once with opTime " << firstOpTime
             << " and a second time with opTime " << secondOpTime
             << ". This indicates possible data corruption or server bug and the process will be "
                "terminated.";
    fassertFailed(40526);
}

struct ActiveTransactionHistory {
    boost::optional<SessionTxnRecord> lastTxnRecord;
    Session::CommittedStatementTimestampMap committedStatements;
    bool transactionCommitted{false};
    bool hasIncompleteHistory{false};
};

ActiveTransactionHistory fetchActiveTransactionHistory(OperationContext* opCtx,
                                                       const LogicalSessionId& lsid) {
    ActiveTransactionHistory result;

    result.lastTxnRecord = [&]() -> boost::optional<SessionTxnRecord> {
        DBDirectClient client(opCtx);
        auto result =
            client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                           {BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON())});
        if (result.isEmpty()) {
            return boost::none;
        }

        return SessionTxnRecord::parse(IDLParserErrorContext("parse latest txn record for session"),
                                       result);
    }();

    if (!result.lastTxnRecord) {
        return result;
    }

    auto it = TransactionHistoryIterator(result.lastTxnRecord->getLastWriteOpTime());
    while (it.hasNext()) {
        try {
            const auto entry = it.next(opCtx);
            invariant(entry.getStatementId());

            if (*entry.getStatementId() == kIncompleteHistoryStmtId) {
                // Only the dead end sentinel can have this id for oplog write history
                invariant(entry.getObject2());
                invariant(entry.getObject2()->woCompare(Session::kDeadEndSentinel) == 0);
                result.hasIncompleteHistory = true;
                continue;
            }

            const auto insertRes =
                result.committedStatements.emplace(*entry.getStatementId(), entry.getOpTime());
            if (!insertRes.second) {
                const auto& existingOpTime = insertRes.first->second;
                fassertOnRepeatedExecution(lsid,
                                           result.lastTxnRecord->getTxnNum(),
                                           *entry.getStatementId(),
                                           existingOpTime,
                                           entry.getOpTime());
            }

            // applyOps oplog entry marks the commit of a transaction.
            if (entry.isCommand() &&
                entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps) {
                result.transactionCommitted = true;
            }
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
                result.hasIncompleteHistory = true;
                break;
            }

            throw;
        }
    }

    return result;
}

void updateSessionEntry(OperationContext* opCtx, const UpdateRequest& updateRequest) {
    // Current code only supports replacement update.
    dassert(UpdateDriver::isDocReplacement(updateRequest.getUpdates()));

    AutoGetCollection autoColl(opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IX);

    uassert(40527,
            str::stream() << "Unable to persist transaction state because the session transaction "
                             "collection is missing. This indicates that the "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns()
                          << " collection has been manually deleted.",
            autoColl.getCollection());

    WriteUnitOfWork wuow(opCtx);

    auto collection = autoColl.getCollection();
    auto idIndex = collection->getIndexCatalog()->findIdIndex(opCtx);

    uassert(40672,
            str::stream() << "Failed to fetch _id index for "
                          << NamespaceString::kSessionTransactionsTableNamespace.ns(),
            idIndex);

    auto indexAccess = collection->getIndexCatalog()->getIndex(idIndex);
    // Since we are looking up a key inside the _id index, create a key object consisting of only
    // the _id field.
    auto idToFetch = updateRequest.getQuery().firstElement();
    auto toUpdateIdDoc = idToFetch.wrap();
    dassert(idToFetch.fieldNameStringData() == "_id"_sd);
    auto recordId = indexAccess->findSingle(opCtx, toUpdateIdDoc);
    auto startingSnapshotId = opCtx->recoveryUnit()->getSnapshotId();

    if (recordId.isNull()) {
        // Upsert case.
        auto status = collection->insertDocument(
            opCtx, InsertStatement(updateRequest.getUpdates()), nullptr, false);

        if (status == ErrorCodes::DuplicateKey) {
            throw WriteConflictException();
        }

        uassertStatusOK(status);
        wuow.commit();
        return;
    }

    auto originalRecordData = collection->getRecordStore()->dataFor(opCtx, recordId);
    auto originalDoc = originalRecordData.toBson();

    invariant(collection->getDefaultCollator() == nullptr);
    boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, nullptr));

    auto matcher =
        fassert(40673, MatchExpressionParser::parse(updateRequest.getQuery(), std::move(expCtx)));
    if (!matcher->matchesBSON(originalDoc)) {
        // Document no longer match what we expect so throw WCE to make the caller re-examine.
        throw WriteConflictException();
    }

    OplogUpdateEntryArgs args;
    args.nss = NamespaceString::kSessionTransactionsTableNamespace;
    args.uuid = collection->uuid();
    args.update = updateRequest.getUpdates();
    args.criteria = toUpdateIdDoc;
    args.fromMigrate = false;

    collection->updateDocument(opCtx,
                               recordId,
                               Snapshotted<BSONObj>(startingSnapshotId, originalDoc),
                               updateRequest.getUpdates(),
                               false,  // indexesAffected = false because _id is the only index
                               nullptr,
                               &args);

    wuow.commit();
}

// Failpoint which allows different failure actions to happen after each write. Supports the
// parameters below, which can be combined with each other (unless explicitly disallowed):
//
// closeConnection (bool, default = true): Closes the connection on which the write was executed.
// failBeforeCommitExceptionCode (int, default = not specified): If set, the specified exception
//      code will be thrown, which will cause the write to not commit; if not specified, the write
//      will be allowed to commit.
MONGO_FAIL_POINT_DEFINE(onPrimaryTransactionalWrite);

// Failpoint which will pause an operation just after allocating a point-in-time storage engine
// transaction.
MONGO_FAIL_POINT_DEFINE(hangAfterPreallocateSnapshot);
}  // namespace

const BSONObj Session::kDeadEndSentinel(BSON("$incompleteOplogHistory" << 1));

Session::Session(LogicalSessionId sessionId) : _sessionId(std::move(sessionId)) {}

void Session::refreshFromStorageIfNeeded(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(!opCtx->lockState()->isLocked());
    invariant(repl::ReadConcernArgs::get(opCtx).getLevel() ==
              repl::ReadConcernLevel::kLocalReadConcern);

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    while (!_isValid) {
        const int numInvalidations = _numInvalidations;

        ul.unlock();

        auto activeTxnHistory = fetchActiveTransactionHistory(opCtx, _sessionId);

        ul.lock();

        // Protect against concurrent refreshes or invalidations
        if (!_isValid && _numInvalidations == numInvalidations) {
            _isValid = true;
            _lastWrittenSessionRecord = std::move(activeTxnHistory.lastTxnRecord);

            if (_lastWrittenSessionRecord) {
                _activeTxnNumber = _lastWrittenSessionRecord->getTxnNum();
                _activeTxnCommittedStatements = std::move(activeTxnHistory.committedStatements);
                _hasIncompleteHistory = activeTxnHistory.hasIncompleteHistory;
                if (activeTxnHistory.transactionCommitted) {
                    // When refreshing the state from storage, we relax transition validation since
                    // all states are valid next states and we do not want to pollute the state
                    // transition table for other callers.
                    _txnState.transitionTo(
                        ul,
                        TransitionTable::State::kCommitted,
                        TransitionTable::TransitionValidation::kRelaxTransitionValidation);
                }
            }

            break;
        }
    }
}

void Session::beginOrContinueTxn(OperationContext* opCtx,
                                 TxnNumber txnNumber,
                                 boost::optional<bool> autocommit,
                                 boost::optional<bool> startTransaction,
                                 StringData dbName,
                                 StringData cmdName) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(!opCtx->lockState()->isLocked());

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            "Cannot run 'count' in a multi-document transaction. Please see "
            "http://dochub.mongodb.org/core/transaction-count for a recommended alternative.",
            !autocommit || cmdName != "count"_sd);

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot run '" << cmdName << "' in a multi-document transaction.",
            !autocommit || txnCmdWhitelist.find(cmdName) != txnCmdWhitelist.cend() ||
                (getTestCommandsEnabled() &&
                 txnCmdForTestingWhitelist.find(cmdName) != txnCmdForTestingWhitelist.cend()));

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot run command against the '" << dbName
                          << "' database in a transaction",
            !autocommit || (dbName != "config"_sd && dbName != "local"_sd &&
                            (dbName != "admin"_sd ||
                             txnAdminCommands.find(cmdName) != txnAdminCommands.cend())));

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginOrContinueTxn(lg, txnNumber, autocommit, startTransaction);
}

void Session::beginOrContinueTxnOnMigration(OperationContext* opCtx, TxnNumber txnNumber) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _beginOrContinueTxnOnMigration(lg, txnNumber);
}

void Session::setSpeculativeTransactionOpTimeToLastApplied(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    repl::ReplicationCoordinator* replCoord =
        repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kLastAppliedSnapshot);
    opCtx->recoveryUnit()->preallocateSnapshot();
    auto readTimestamp = opCtx->recoveryUnit()->getPointInTimeReadTimestamp();
    invariant(readTimestamp);
    // Transactions do not survive term changes, so combining "getTerm" here with the
    // recovery unit timestamp does not cause races.
    _speculativeTransactionReadOpTime = {*readTimestamp, replCoord->getTerm()};
}

void Session::onWriteOpCompletedOnPrimary(OperationContext* opCtx,
                                          TxnNumber txnNumber,
                                          std::vector<StmtId> stmtIdsWritten,
                                          const repl::OpTime& lastStmtIdWriteOpTime,
                                          Date_t lastStmtIdWriteDate) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    // Sanity check that we don't double-execute statements
    for (const auto stmtId : stmtIdsWritten) {
        const auto stmtOpTime = _checkStatementExecuted(ul, txnNumber, stmtId);
        if (stmtOpTime) {
            fassertOnRepeatedExecution(
                _sessionId, txnNumber, stmtId, *stmtOpTime, lastStmtIdWriteOpTime);
        }
    }

    const auto updateRequest =
        _makeUpdateRequest(ul, txnNumber, lastStmtIdWriteOpTime, lastStmtIdWriteDate);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(
        opCtx, txnNumber, std::move(stmtIdsWritten), lastStmtIdWriteOpTime);
}

bool Session::onMigrateBeginOnPrimary(OperationContext* opCtx, TxnNumber txnNumber, StmtId stmtId) {
    beginOrContinueTxnOnMigration(opCtx, txnNumber);

    try {
        if (checkStatementExecuted(opCtx, txnNumber, stmtId)) {
            return false;
        }
    } catch (const DBException& ex) {
        // If the transaction chain was truncated on the recipient shard, then we
        // are most likely copying from a session that hasn't been touched on the
        // recipient shard for a very long time but could be recent on the donor.
        // We continue copying regardless to get the entire transaction from the donor.
        if (ex.code() != ErrorCodes::IncompleteTransactionHistory) {
            throw;
        }
        if (stmtId == kIncompleteHistoryStmtId) {
            return false;
        }
    }

    return true;
}

void Session::onMigrateCompletedOnPrimary(OperationContext* opCtx,
                                          TxnNumber txnNumber,
                                          std::vector<StmtId> stmtIdsWritten,
                                          const repl::OpTime& lastStmtIdWriteOpTime,
                                          Date_t oplogLastStmtIdWriteDate) {
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    _checkValid(ul);
    _checkIsActiveTransaction(ul, txnNumber, false);

    // If the transaction has a populated lastWriteDate, we will use that as the most up-to-date
    // value. Using the lastWriteDate from the oplog being migrated may move the lastWriteDate
    // back. However, in the case that the transaction doesn't have the lastWriteDate populated,
    // the oplog's value serves as a best-case fallback.
    const auto txnLastStmtIdWriteDate = _getLastWriteDate(ul, txnNumber);
    const auto updatedLastStmtIdWriteDate =
        txnLastStmtIdWriteDate == Date_t::min() ? oplogLastStmtIdWriteDate : txnLastStmtIdWriteDate;

    const auto updateRequest =
        _makeUpdateRequest(ul, txnNumber, lastStmtIdWriteOpTime, updatedLastStmtIdWriteDate);

    ul.unlock();

    repl::UnreplicatedWritesBlock doNotReplicateWrites(opCtx);

    updateSessionEntry(opCtx, updateRequest);
    _registerUpdateCacheOnCommit(
        opCtx, txnNumber, std::move(stmtIdsWritten), lastStmtIdWriteOpTime);
}

void Session::invalidate() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _isValid = false;
    _numInvalidations++;

    _lastWrittenSessionRecord.reset();

    _activeTxnNumber = kUninitializedTxnNumber;
    _activeTxnCommittedStatements.clear();
    _speculativeTransactionReadOpTime = repl::OpTime();
    _hasIncompleteHistory = false;
}

repl::OpTime Session::getLastWriteOpTime(TxnNumber txnNumber) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _checkValid(lg);
    _checkIsActiveTransaction(lg, txnNumber, false);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return {};

    return _lastWrittenSessionRecord->getLastWriteOpTime();
}

boost::optional<repl::OplogEntry> Session::checkStatementExecuted(OperationContext* opCtx,
                                                                  TxnNumber txnNumber,
                                                                  StmtId stmtId) const {
    const auto stmtTimestamp = [&] {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        return _checkStatementExecuted(lg, txnNumber, stmtId);
    }();

    if (!stmtTimestamp)
        return boost::none;

    TransactionHistoryIterator txnIter(*stmtTimestamp);
    while (txnIter.hasNext()) {
        const auto entry = txnIter.next(opCtx);
        invariant(entry.getStatementId());
        if (*entry.getStatementId() == stmtId)
            return entry;
    }

    MONGO_UNREACHABLE;
}

bool Session::checkStatementExecutedNoOplogEntryFetch(TxnNumber txnNumber, StmtId stmtId) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return bool(_checkStatementExecuted(lg, txnNumber, stmtId));
}

void Session::_beginOrContinueTxn(WithLock wl,
                                  TxnNumber txnNumber,
                                  boost::optional<bool> autocommit,
                                  boost::optional<bool> startTransaction) {

    // Check whether the session information needs to be refreshed from disk.
    _checkValid(wl);

    // Check if the given transaction number is valid for this session. The transaction number must
    // be >= the active transaction number.
    _checkTxnValid(wl, txnNumber);

    //
    // Continue an active transaction.
    //
    if (txnNumber == _activeTxnNumber) {

        // It is never valid to specify 'startTransaction' on an active transaction.
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Cannot specify 'startTransaction' on transaction " << txnNumber
                              << " since it is already in progress.",
                startTransaction == boost::none);

        // Continue a retryable write.
        if (_txnState.isNone(wl)) {
            uassert(ErrorCodes::InvalidOptions,
                    "Cannot specify 'autocommit' on an operation not inside a multi-statement "
                    "transaction.",
                    autocommit == boost::none);
            return;
        }

        // Continue a multi-statement transaction. In this case, it is required that
        // autocommit=false be given as an argument on the request. Retryable writes will have
        // _autocommit=true, so that is why we verify that _autocommit=false here.
        if (!_autocommit) {
            uassert(
                ErrorCodes::InvalidOptions,
                "Must specify autocommit=false on all operations of a multi-statement transaction.",
                autocommit == boost::optional<bool>(false));
            if (_txnState.isInProgress(wl) && !_txnResourceStash) {
                // This indicates that the first command in the transaction failed but did not
                // implicitly abort the transaction. It is not safe to continue the transaction, in
                // particular because we have not saved the readConcern from the first statement of
                // the transaction.
                _abortTransaction(wl);
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream() << "Transaction " << txnNumber << " has been aborted.");
            }
        }
        return;
    }

    //
    // Start a new transaction.
    //
    // At this point, the given transaction number must be > _activeTxnNumber. Existence of an
    // 'autocommit' field means we interpret this operation as part of a multi-document transaction.
    invariant(txnNumber > _activeTxnNumber);
    if (autocommit) {
        // Start a multi-document transaction.
        invariant(*autocommit == false);
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Given transaction number " << txnNumber
                              << " does not match any in-progress transactions.",
                startTransaction != boost::none);

        _setActiveTxn(wl, txnNumber);
        _autocommit = false;
        _txnState.transitionTo(wl, TransitionTable::State::kInProgress);
        // Tracks various transactions metrics.
        _singleTransactionStats = SingleTransactionStats();
        _singleTransactionStats->setStartTime(curTimeMicros64());
        _transactionExpireDate =
            Date_t::fromMillisSinceEpoch(_singleTransactionStats->getStartTime() / 1000) +
            stdx::chrono::seconds{transactionLifetimeLimitSeconds.load()};
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementTotalStarted();
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentOpen();
    } else {
        // Execute a retryable write.
        invariant(startTransaction == boost::none);
        _setActiveTxn(wl, txnNumber);
        _autocommit = true;
        _txnState.transitionTo(wl, TransitionTable::State::kNone);
        // SingleTransactionStats are only for multi-document transactions.
        _singleTransactionStats = boost::none;
    }

    invariant(_transactionOperations.empty());
}

void Session::_checkTxnValid(WithLock, TxnNumber txnNumber) const {
    uassert(ErrorCodes::TransactionTooOld,
            str::stream() << "Cannot start transaction " << txnNumber << " on session "
                          << getSessionId()
                          << " because a newer transaction "
                          << _activeTxnNumber
                          << " has already started.",
            txnNumber >= _activeTxnNumber);
}

Session::TxnResources::TxnResources(OperationContext* opCtx) {
    _ruState = opCtx->getWriteUnitOfWork()->release();
    opCtx->setWriteUnitOfWork(nullptr);

    _locker = opCtx->swapLockState(stdx::make_unique<LockerImpl>());
    _locker->releaseTicket();
    _locker->unsetThreadId();

    // This thread must still respect the transaction lock timeout, since it can prevent the
    // transaction from making progress.
    auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
    if (maxTransactionLockMillis >= 0) {
        opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
    }

    _recoveryUnit = std::unique_ptr<RecoveryUnit>(opCtx->releaseRecoveryUnit());
    opCtx->setRecoveryUnit(opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    _readConcernArgs = repl::ReadConcernArgs::get(opCtx);
}

Session::TxnResources::~TxnResources() {
    if (!_released && _recoveryUnit) {
        // This should only be reached when aborting a transaction that isn't active, i.e.
        // when starting a new transaction before completing an old one.  So we should
        // be at WUOW nesting level 1 (only the top level WriteUnitOfWork).
        _locker->endWriteUnitOfWork();
        invariant(!_locker->inAWriteUnitOfWork());
        _recoveryUnit->abortUnitOfWork();
    }
}

void Session::TxnResources::release(OperationContext* opCtx) {
    // Perform operations that can fail the release before marking the TxnResources as released.
    _locker->reacquireTicket(opCtx);

    invariant(!_released);
    _released = true;

    // We intentionally do not capture the return value of swapLockState(), which is just an empty
    // locker. At the end of the operation, if the transaction is not complete, we will stash the
    // operation context's locker and replace it with a new empty locker.
    invariant(opCtx->lockState()->getClientState() == Locker::ClientState::kInactive);
    opCtx->swapLockState(std::move(_locker));
    opCtx->lockState()->updateThreadIdToCurrentThread();

    opCtx->setRecoveryUnit(_recoveryUnit.release(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    opCtx->setWriteUnitOfWork(WriteUnitOfWork::createForSnapshotResume(opCtx, _ruState));

    auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    readConcernArgs = _readConcernArgs;
}

Session::SideTransactionBlock::SideTransactionBlock(OperationContext* opCtx) : _opCtx(opCtx) {
    if (_opCtx->getWriteUnitOfWork()) {
        // This must be done under the client lock, since we are modifying '_opCtx'.
        stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
        _txnResources = Session::TxnResources(_opCtx);
    }
}

Session::SideTransactionBlock::~SideTransactionBlock() {
    if (_txnResources) {
        // Restore the transaction state onto '_opCtx'. This must be done under the
        // client lock, since we are modifying '_opCtx'.
        stdx::lock_guard<Client> clientLock(*_opCtx->getClient());
        _txnResources->release(_opCtx);
    }
}

void Session::stashTransactionResources(OperationContext* opCtx) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());

    // We must lock the Client to change the Locker on the OperationContext and the Session mutex to
    // access Session state. We must lock the Client before the Session mutex, since the Client
    // effectively owns the Session. That is, a user might lock the Client to ensure it doesn't go
    // away, and then lock the Session owned by that client. We rely on the fact that we are not
    // using the DefaultLockerImpl to avoid deadlock.
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    stdx::unique_lock<stdx::mutex> lg(_mutex);

    // Always check '_activeTxnNumber', since it can be modified by migration, which does not
    // check out the session. We intentionally do not error if _txnState=kAborted, since we
    // expect this function to be called at the end of the 'abortTransaction' command.
    _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

    if (!_txnState.inMultiDocumentTransaction(lg)) {
        // Not in a multi-document transaction: nothing to do.
        return;
    }

    if (_singleTransactionStats->isActive()) {
        _singleTransactionStats->setInactive(curTimeMicros64());
    }

    // Add the latest operation stats to the aggregate OpDebug object stored in the
    // SingleTransactionStats instance on the Session.
    _singleTransactionStats->getOpDebug()->additiveMetrics.add(
        CurOp::get(opCtx)->debug().additiveMetrics);

    invariant(!_txnResourceStash);
    _txnResourceStash = TxnResources(opCtx);

    // We accept possible slight inaccuracies in these counters from non-atomicity.
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentActive();
    ServerTransactionsMetrics::get(opCtx)->incrementCurrentInactive();

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the Session
    // with this Client's information. This is the last client that ran a transaction operation on
    // the Session.
    _singleTransactionStats->updateLastClientInfo(opCtx->getClient());
}

void Session::unstashTransactionResources(OperationContext* opCtx, const std::string& cmdName) {
    if (opCtx->getClient()->isInDirectClient()) {
        return;
    }

    invariant(opCtx->getTxnNumber());

    {
        // We must lock the Client to change the Locker on the OperationContext and the Session
        // mutex to access Session state. We must lock the Client before the Session mutex, since
        // the Client effectively owns the Session. That is, a user might lock the Client to ensure
        // it doesn't go away, and then lock the Session owned by that client.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session
        // kill and migration, which do not check out the session.
        _checkIsActiveTransaction(lg, *opCtx->getTxnNumber(), false);

        // If this is not a multi-document transaction, there is nothing to unstash.
        if (_txnState.isNone(lg)) {
            invariant(!_txnResourceStash);
            return;
        }

        // Throw NoSuchTransaction error instead of TransactionAborted error since this is the entry
        // point of transaction execution.
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been aborted.",
                !_txnState.isAborted(lg));

        // Cannot change committed transaction but allow retrying commitTransaction command.
        uassert(ErrorCodes::TransactionCommitted,
                str::stream() << "Transaction " << *opCtx->getTxnNumber() << " has been committed.",
                cmdName == "commitTransaction" || !_txnState.isCommitted(lg));

        if (_txnResourceStash) {
            // Transaction resources already exist for this transaction.  Transfer them from the
            // stash to the operation context.
            auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            uassert(ErrorCodes::InvalidOptions,
                    "Only the first command in a transaction may specify a readConcern",
                    readConcernArgs.isEmpty());
            _txnResourceStash->release(opCtx);
            _txnResourceStash = boost::none;
            // Set the starting active time for this transaction.
            if (_txnState.isInProgress(lk)) {
                _singleTransactionStats->setActive(curTimeMicros64());
            }
            // We accept possible slight inaccuracies in these counters from non-atomicity.
            ServerTransactionsMetrics::get(opCtx)->incrementCurrentActive();
            ServerTransactionsMetrics::get(opCtx)->decrementCurrentInactive();
            return;
        }

        // If we have no transaction resources then we cannot be prepared. If we're not in progress,
        // we don't do anything else.
        invariant(!_txnState.isPrepared(lk));
        if (!_txnState.isInProgress(lg)) {
            // At this point we're either committed and this is a 'commitTransaction' command, or we
            // are in the process of committing.
            return;
        }

        // Stashed transaction resources do not exist for this in-progress multi-document
        // transaction. Set up the transaction resources on the opCtx.
        opCtx->setWriteUnitOfWork(std::make_unique<WriteUnitOfWork>(opCtx));
        ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementCurrentActive();

        // Set the starting active time for this transaction.
        _singleTransactionStats->setActive(curTimeMicros64());

        // If maxTransactionLockRequestTimeoutMillis is set, then we will ensure no
        // future lock request waits longer than maxTransactionLockRequestTimeoutMillis
        // to acquire a lock. This is to avoid deadlocks and minimize non-transaction
        // operation performance degradations.
        auto maxTransactionLockMillis = maxTransactionLockRequestTimeoutMillis.load();
        if (maxTransactionLockMillis >= 0) {
            opCtx->lockState()->setMaxLockTimeout(Milliseconds(maxTransactionLockMillis));
        }
    }

    // Storage engine transactions may be started in a lazy manner. By explicitly
    // starting here we ensure that a point-in-time snapshot is established during the
    // first operation of a transaction.
    //
    // Active transactions are protected by the locking subsystem, so we must always hold at least a
    // Global intent lock before starting a transaction.  We pessimistically acquire an intent
    // exclusive lock here because we might be doing writes in this transaction, and it is currently
    // not deadlock-safe to upgrade IS to IX.
    Lock::GlobalLock(opCtx, MODE_IX);
    opCtx->recoveryUnit()->preallocateSnapshot();

    // The Client lock must not be held when executing this failpoint as it will block currentOp
    // execution.
    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterPreallocateSnapshot);
}

Timestamp Session::prepareTransaction(OperationContext* opCtx) {
    // This ScopeGuard is created outside of the lock so that the lock is always released before
    // this is called.
    ScopeGuard abortGuard = MakeGuard([&] { abortActiveTransaction(opCtx); });

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by
    // session kill and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    _txnState.transitionTo(lk, TransitionTable::State::kPrepared);

    // We need to unlock the session to run the opObserver onTransactionPrepare, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionPrepare(opCtx);
    lk.lock();
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that the transaction is still prepared.
    invariant(_txnState.isPrepared(lk), str::stream() << "Current state: " << _txnState);

    opCtx->getWriteUnitOfWork()->prepare();

    abortGuard.Dismiss();

    // Return the prepareTimestamp from the recovery unit.
    return opCtx->recoveryUnit()->getPrepareTimestamp();
}

void Session::abortArbitraryTransaction() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _abortArbitraryTransaction(lock);
}

void Session::abortArbitraryTransactionIfExpired() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (!_transactionExpireDate || _transactionExpireDate >= Date_t::now()) {
        return;
    }
    _abortArbitraryTransaction(lock);
}

void Session::_abortArbitraryTransaction(WithLock lock) {
    if (!_txnState.isInProgress(lock)) {
        // We do not want to abort transactions that are prepared unless we get an
        // 'abortTransaction' command.
        return;
    }

    _abortTransaction(lock);
}

void Session::abortActiveTransaction(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    if (!_txnState.inMultiDocumentTransaction(lock)) {
        return;
    }

    _abortTransaction(lock);

    // Abort the WUOW. We should be able to abort empty transactions that don't have WUOW.
    if (opCtx->getWriteUnitOfWork()) {
        opCtx->setWriteUnitOfWork(nullptr);
    }
    // We must clear the recovery unit and locker so any post-transaction writes can run without
    // transactional settings such as a read timestamp.
    opCtx->setRecoveryUnit(opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                           WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    opCtx->lockState()->unsetMaxLockTimeout();

    // Add the latest operation stats to the aggregate OpDebug object stored in the
    // SingleTransactionStats instance on the Session.
    _singleTransactionStats->getOpDebug()->additiveMetrics.add(
        CurOp::get(opCtx)->debug().additiveMetrics);

    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the Session
    // with this Client's information.
    _singleTransactionStats->updateLastClientInfo(opCtx->getClient());
}

void Session::_abortTransaction(WithLock wl) {
    // If the transaction is stashed, then we have aborted an inactive transaction.
    if (_txnResourceStash) {
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentInactive();
    } else {
        ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
    }

    _txnResourceStash = boost::none;
    _transactionOperationBytes = 0;
    _transactionOperations.clear();
    _txnState.transitionTo(wl, TransitionTable::State::kAborted);
    _speculativeTransactionReadOpTime = repl::OpTime();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->incrementTotalAborted();
    if (!_txnState.isNone(wl)) {
        _singleTransactionStats->setEndTime(curTimeMicros64());
        // The transaction has aborted, so we mark it as inactive.
        if (_singleTransactionStats->isActive()) {
            _singleTransactionStats->setInactive(curTimeMicros64());
        }
    }
    ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentOpen();
}

void Session::_beginOrContinueTxnOnMigration(WithLock wl, TxnNumber txnNumber) {
    _checkValid(wl);
    _checkTxnValid(wl, txnNumber);

    // Check for continuing an existing transaction
    if (txnNumber == _activeTxnNumber)
        return;

    _setActiveTxn(wl, txnNumber);
}

void Session::_setActiveTxn(WithLock wl, TxnNumber txnNumber) {
    // Abort the existing transaction if it's not prepared, committed, or aborted.
    if (_txnState.isInProgress(wl)) {
        _abortTransaction(wl);
    }
    _activeTxnNumber = txnNumber;
    _activeTxnCommittedStatements.clear();
    _hasIncompleteHistory = false;
    _txnState.transitionTo(wl, TransitionTable::State::kNone);
    _singleTransactionStats = boost::none;
    _speculativeTransactionReadOpTime = repl::OpTime();
    _multikeyPathInfo.clear();
}

void Session::addTransactionOperation(OperationContext* opCtx,
                                      const repl::ReplOperation& operation) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever add operations to an in progress transaction.
    invariant(_txnState.isInProgress(lk), str::stream() << "Current state: " << _txnState);

    invariant(!_autocommit && _activeTxnNumber != kUninitializedTxnNumber);
    invariant(opCtx->lockState()->inAWriteUnitOfWork());
    _transactionOperations.push_back(operation);
    _transactionOperationBytes += repl::OplogEntry::getReplOperationSize(operation);
    // _transactionOperationBytes is based on the in-memory size of the operation.  With overhead,
    // we expect the BSON size of the operation to be larger, so it's possible to make a transaction
    // just a bit too large and have it fail only in the commit.  It's still useful to fail early
    // when possible (e.g. to avoid exhausting server memory).
    uassert(ErrorCodes::TransactionTooLarge,
            str::stream() << "Total size of all transaction operations must be less than "
                          << BSONObjMaxInternalSize
                          << ". Actual size is "
                          << _transactionOperationBytes,
            _transactionOperationBytes <= BSONObjMaxInternalSize);
}

std::vector<repl::ReplOperation> Session::endTransactionAndRetrieveOperations(
    OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    // Ensure that we only ever end a transaction when prepared or committing.
    invariant(_txnState.isPrepared(lk) || _txnState.isCommittingWithoutPrepare(lk),
              str::stream() << "Current state: " << _txnState);

    invariant(!_autocommit);
    _transactionOperationBytes = 0;
    return std::move(_transactionOperations);
}

void Session::commitUnpreparedTransaction(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction must provide commitTimestamp to prepared transaction.",
            !_txnState.isPrepared(lk));

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    _txnState.transitionTo(lk, TransitionTable::State::kCommittingWithoutPrepare);

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(opCtx, false /* wasPrepared */);
    lk.lock();

    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);
    _commitTransaction(std::move(lk), opCtx);
}

void Session::commitPreparedTransaction(OperationContext* opCtx, Timestamp commitTimestamp) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    uassert(ErrorCodes::InvalidOptions,
            "commitTransaction cannot provide commitTimestamp to unprepared transaction.",
            _txnState.isPrepared(lk));
    uassert(
        ErrorCodes::InvalidOptions, "'commitTimestamp' cannot be null", !commitTimestamp.isNull());

    // Always check '_activeTxnNumber' and '_txnState', since they can be modified by session kill
    // and migration, which do not check out the session.
    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);

    _txnState.transitionTo(lk, TransitionTable::State::kCommittingWithPrepare);
    opCtx->recoveryUnit()->setCommitTimestamp(commitTimestamp);

    // We need to unlock the session to run the opObserver onTransactionCommit, which calls back
    // into the session.
    lk.unlock();
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    invariant(opObserver);
    opObserver->onTransactionCommit(opCtx, true /* wasPrepared */);
    lk.lock();

    _checkIsActiveTransaction(lk, *opCtx->getTxnNumber(), true);
    _commitTransaction(std::move(lk), opCtx);
}

void Session::_commitTransaction(stdx::unique_lock<stdx::mutex> lk, OperationContext* opCtx) {

    bool committed = false;
    ON_BLOCK_EXIT([this, &committed, opCtx]() {
        // If we're still "committing", the recovery unit failed to commit, and the lock is not
        // held.  We can't safely use _txnState here, as it is protected by the lock.
        if (!committed) {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            opCtx->setWriteUnitOfWork(nullptr);

            // Make sure the transaction didn't change because of chunk migration.
            if (opCtx->getTxnNumber() == _activeTxnNumber) {
                _txnState.transitionTo(lk, TransitionTable::State::kAborted);
                ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
                // After the transaction has been aborted, we must update the end time and mark it
                // as inactive.
                auto curTime = curTimeMicros64();
                _singleTransactionStats->setEndTime(curTime);
                if (_singleTransactionStats->isActive()) {
                    _singleTransactionStats->setInactive(curTime);
                }
                ServerTransactionsMetrics::get(opCtx)->incrementTotalAborted();
                ServerTransactionsMetrics::get(opCtx)->decrementCurrentOpen();
                // Add the latest operation stats to the aggregate OpDebug object stored in the
                // SingleTransactionStats instance on the Session.
                _singleTransactionStats->getOpDebug()->additiveMetrics.add(
                    CurOp::get(opCtx)->debug().additiveMetrics);
                // Update the LastClientInfo object stored in the SingleTransactionStats instance on
                // the Session with this Client's information.
                _singleTransactionStats->updateLastClientInfo(opCtx->getClient());
            }
        }
        // We must clear the recovery unit and locker so any post-transaction writes can run without
        // transactional settings such as a read timestamp.
        opCtx->setRecoveryUnit(opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        opCtx->lockState()->unsetMaxLockTimeout();
    });
    lk.unlock();
    opCtx->getWriteUnitOfWork()->commit();
    opCtx->setWriteUnitOfWork(nullptr);
    committed = true;
    lk.lock();
    auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
    // If no writes have been done, set the client optime forward to the read timestamp so waiting
    // for write concern will ensure all read data was committed.
    //
    // TODO(SERVER-34881): Once the default read concern is speculative majority, only set the
    // client optime forward if the original read concern level is "majority" or "snapshot".
    if (_speculativeTransactionReadOpTime > clientInfo.getLastOp()) {
        clientInfo.setLastOp(_speculativeTransactionReadOpTime);
    }
    _txnState.transitionTo(lk, TransitionTable::State::kCommitted);
    ServerTransactionsMetrics::get(opCtx)->incrementTotalCommitted();
    // After the transaction has been committed, we must update the end time and mark it as
    // inactive.
    _singleTransactionStats->setEndTime(curTimeMicros64());
    if (_singleTransactionStats->isActive()) {
        _singleTransactionStats->setInactive(curTimeMicros64());
    }
    ServerTransactionsMetrics::get(opCtx)->decrementCurrentOpen();
    ServerTransactionsMetrics::get(getGlobalServiceContext())->decrementCurrentActive();
    // Add the latest operation stats to the aggregate OpDebug object stored in the
    // SingleTransactionStats instance on the Session.
    _singleTransactionStats->getOpDebug()->additiveMetrics.add(
        CurOp::get(opCtx)->debug().additiveMetrics);
    // Update the LastClientInfo object stored in the SingleTransactionStats instance on the Session
    // with this Client's information.
    _singleTransactionStats->updateLastClientInfo(opCtx->getClient());
}

BSONObj Session::reportStashedState() const {
    BSONObjBuilder builder;
    reportStashedState(&builder);
    return builder.obj();
}

void Session::reportStashedState(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> ls(_mutex);

    if (_txnResourceStash && _txnResourceStash->locker()) {
        if (auto lockerInfo = _txnResourceStash->locker()->getLockerInfo()) {
            invariant(_activeTxnNumber != kUninitializedTxnNumber);
            builder->append("host", getHostNameCachedAndPort());
            builder->append("desc", "inactive transaction");
            auto lastClientInfo = _singleTransactionStats->getLastClientInfo();
            builder->append("client", lastClientInfo.clientHostAndPort);
            builder->append("connectionId", lastClientInfo.connectionId);
            builder->append("appName", lastClientInfo.appName);
            builder->append("clientMetadata", lastClientInfo.clientMetadata);
            {
                BSONObjBuilder lsid(builder->subobjStart("lsid"));
                getSessionId().serialize(&lsid);
            }
            BSONObjBuilder transactionBuilder;
            _reportTransactionStats(
                ls, &transactionBuilder, _txnResourceStash->getReadConcernArgs());
            builder->append("transaction", transactionBuilder.obj());
            builder->append("waitingForLock", false);
            builder->append("active", false);
            fillLockerInfo(*lockerInfo, *builder);
        }
    }
}

void Session::reportUnstashedState(repl::ReadConcernArgs readConcernArgs,
                                   BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> ls(_mutex);

    if (!_txnResourceStash) {
        BSONObjBuilder transactionBuilder;
        _reportTransactionStats(ls, &transactionBuilder, readConcernArgs);
        builder->append("transaction", transactionBuilder.obj());
    }
}

void Session::_reportTransactionStats(WithLock wl,
                                      BSONObjBuilder* builder,
                                      repl::ReadConcernArgs readConcernArgs) const {
    BSONObjBuilder parametersBuilder(builder->subobjStart("parameters"));
    parametersBuilder.append("txnNumber", _activeTxnNumber);

    if (!_txnState.inMultiDocumentTransaction(wl)) {
        // For retryable writes, we only include the txnNumber.
        parametersBuilder.done();
        return;
    }
    parametersBuilder.append("autocommit", _autocommit);
    readConcernArgs.appendInfo(&parametersBuilder);
    parametersBuilder.done();

    builder->append("readTimestamp", _speculativeTransactionReadOpTime.getTimestamp());
    builder->append("startWallClockTime",
                    dateToISOStringLocal(Date_t::fromMillisSinceEpoch(
                        _singleTransactionStats->getStartTime() / 1000)));
    // We use the same "now" time so that the following time metrics are consistent with each other.
    auto curTime = curTimeMicros64();
    builder->append("timeOpenMicros",
                    static_cast<long long>(_singleTransactionStats->getDuration(curTime)));
    auto timeActive =
        durationCount<Microseconds>(_singleTransactionStats->getTimeActiveMicros(curTime));
    auto timeInactive =
        durationCount<Microseconds>(_singleTransactionStats->getTimeInactiveMicros(curTime));
    builder->append("timeActiveMicros", timeActive);
    builder->append("timeInactiveMicros", timeInactive);
}

std::string Session::transactionInfoForLog(const SingleThreadedLockStats* lockStats) {
    // Need to lock because this function checks the state of _txnState.
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    invariant(lockStats);
    invariant(_txnState.isCommitted(lg) || _txnState.isAborted(lg));

    StringBuilder s;

    // User specified transaction parameters.
    BSONObjBuilder parametersBuilder;
    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId.serialize(&lsidBuilder);
    lsidBuilder.doneFast();
    parametersBuilder.append("txnNumber", _activeTxnNumber);
    // TODO: SERVER-35174 Add readConcern to parameters here once pushed.
    parametersBuilder.append("autocommit", _autocommit);
    s << "parameters:" << parametersBuilder.obj().toString() << ",";

    s << " readTimestamp:" << _speculativeTransactionReadOpTime.getTimestamp().toString() << ",";

    s << _singleTransactionStats->getOpDebug()->additiveMetrics.report();

    std::string terminationCause = _txnState.isCommitted(lg) ? "committed" : "aborted";
    s << " terminationCause:" << terminationCause;

    auto curTime = curTimeMicros64();
    s << " timeActiveMicros:"
      << durationCount<Microseconds>(_singleTransactionStats->getTimeActiveMicros(curTime));
    s << " timeInactiveMicros:"
      << durationCount<Microseconds>(_singleTransactionStats->getTimeInactiveMicros(curTime));

    // Number of yields is always 0 in multi-document transactions, but it is included mainly to
    // match the format with other slow operation logging messages.
    s << " numYields:" << 0;

    // Aggregate lock statistics.
    BSONObjBuilder locks;
    lockStats->report(&locks);
    s << " locks:" << locks.obj().toString();

    // Total duration of the transaction.
    s << " "
      << Milliseconds{static_cast<long long>(_singleTransactionStats->getDuration(curTime)) / 1000};

    return s.str();
}

void Session::_checkValid(WithLock) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Session " << getSessionId()
                          << " was concurrently modified and the operation must be retried.",
            _isValid);
}

void Session::_checkIsActiveTransaction(WithLock wl, TxnNumber txnNumber, bool checkAbort) const {
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Cannot perform operations on transaction " << txnNumber
                          << " on session "
                          << getSessionId()
                          << " because a different transaction "
                          << _activeTxnNumber
                          << " is now active.",
            txnNumber == _activeTxnNumber);

    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << txnNumber << " has been aborted.",
            !checkAbort || !_txnState.isAborted(wl));
}

boost::optional<repl::OpTime> Session::_checkStatementExecuted(WithLock wl,
                                                               TxnNumber txnNumber,
                                                               StmtId stmtId) const {
    _checkValid(wl);
    _checkIsActiveTransaction(wl, txnNumber, false);
    // Retries are not detected for multi-document transactions.
    if (!_txnState.isNone(wl))
        return boost::none;

    const auto it = _activeTxnCommittedStatements.find(stmtId);
    if (it == _activeTxnCommittedStatements.end()) {
        uassert(ErrorCodes::IncompleteTransactionHistory,
                str::stream() << "Incomplete history detected for transaction " << txnNumber
                              << " on session "
                              << _sessionId.toBSON(),
                !_hasIncompleteHistory);

        return boost::none;
    }

    invariant(_lastWrittenSessionRecord);
    invariant(_lastWrittenSessionRecord->getTxnNum() == txnNumber);

    return it->second;
}

Date_t Session::_getLastWriteDate(WithLock wl, TxnNumber txnNumber) const {
    _checkValid(wl);
    _checkIsActiveTransaction(wl, txnNumber, false);

    if (!_lastWrittenSessionRecord || _lastWrittenSessionRecord->getTxnNum() != txnNumber)
        return {};

    return _lastWrittenSessionRecord->getLastWriteDate();
}

UpdateRequest Session::_makeUpdateRequest(WithLock,
                                          TxnNumber newTxnNumber,
                                          const repl::OpTime& newLastWriteOpTime,
                                          Date_t newLastWriteDate) const {
    UpdateRequest updateRequest(NamespaceString::kSessionTransactionsTableNamespace);

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        newTxnRecord.setSessionId(_sessionId);
        newTxnRecord.setTxnNum(newTxnNumber);
        newTxnRecord.setLastWriteOpTime(newLastWriteOpTime);
        newTxnRecord.setLastWriteDate(newLastWriteDate);
        return newTxnRecord.toBSON();
    }();
    updateRequest.setUpdates(updateBSON);
    updateRequest.setQuery(BSON(SessionTxnRecord::kSessionIdFieldName << _sessionId.toBSON()));
    updateRequest.setUpsert(true);

    return updateRequest;
}

void Session::_registerUpdateCacheOnCommit(OperationContext* opCtx,
                                           TxnNumber newTxnNumber,
                                           std::vector<StmtId> stmtIdsWritten,
                                           const repl::OpTime& lastStmtIdWriteOpTime) {
    opCtx->recoveryUnit()->onCommit(
        [ this, newTxnNumber, stmtIdsWritten = std::move(stmtIdsWritten), lastStmtIdWriteOpTime ](
            boost::optional<Timestamp>) {
            RetryableWritesStats::get(getGlobalServiceContext())
                ->incrementTransactionsCollectionWriteCount();

            stdx::lock_guard<stdx::mutex> lg(_mutex);

            if (!_isValid)
                return;

            // The cache of the last written record must always be advanced after a write so that
            // subsequent writes have the correct point to start from.
            if (!_lastWrittenSessionRecord) {
                _lastWrittenSessionRecord.emplace();

                _lastWrittenSessionRecord->setSessionId(_sessionId);
                _lastWrittenSessionRecord->setTxnNum(newTxnNumber);
                _lastWrittenSessionRecord->setLastWriteOpTime(lastStmtIdWriteOpTime);
            } else {
                if (newTxnNumber > _lastWrittenSessionRecord->getTxnNum())
                    _lastWrittenSessionRecord->setTxnNum(newTxnNumber);

                if (lastStmtIdWriteOpTime > _lastWrittenSessionRecord->getLastWriteOpTime())
                    _lastWrittenSessionRecord->setLastWriteOpTime(lastStmtIdWriteOpTime);
            }

            if (newTxnNumber > _activeTxnNumber) {
                // This call is necessary in order to advance the txn number and reset the cached
                // state in the case where just before the storage transaction commits, the cache
                // entry gets invalidated and immediately refreshed while there were no writes for
                // newTxnNumber yet. In this case _activeTxnNumber will be less than newTxnNumber
                // and we will fail to update the cache even though the write was successful.
                _beginOrContinueTxn(lg, newTxnNumber, boost::none, boost::none);
            }

            if (newTxnNumber == _activeTxnNumber) {
                for (const auto stmtId : stmtIdsWritten) {
                    if (stmtId == kIncompleteHistoryStmtId) {
                        _hasIncompleteHistory = true;
                        continue;
                    }

                    const auto insertRes =
                        _activeTxnCommittedStatements.emplace(stmtId, lastStmtIdWriteOpTime);
                    if (!insertRes.second) {
                        const auto& existingOpTime = insertRes.first->second;
                        fassertOnRepeatedExecution(_sessionId,
                                                   newTxnNumber,
                                                   stmtId,
                                                   existingOpTime,
                                                   lastStmtIdWriteOpTime);
                    }
                }
            }
        });

    MONGO_FAIL_POINT_BLOCK(onPrimaryTransactionalWrite, customArgs) {
        const auto& data = customArgs.getData();

        const auto closeConnectionElem = data["closeConnection"];
        if (closeConnectionElem.eoo() || closeConnectionElem.Bool()) {
            opCtx->getClient()->session()->end();
        }

        const auto failBeforeCommitExceptionElem = data["failBeforeCommitExceptionCode"];
        if (!failBeforeCommitExceptionElem.eoo()) {
            const auto failureCode = ErrorCodes::Error(int(failBeforeCommitExceptionElem.Number()));
            uasserted(failureCode,
                      str::stream() << "Failing write for " << _sessionId << ":" << newTxnNumber
                                    << " due to failpoint. The write must not be reflected.");
        }
    }
}

boost::optional<repl::OplogEntry> Session::createMatchingTransactionTableUpdate(
    const repl::OplogEntry& entry) {
    auto sessionInfo = entry.getOperationSessionInfo();
    if (!sessionInfo.getTxnNumber()) {
        return boost::none;
    }

    invariant(sessionInfo.getSessionId());
    invariant(entry.getWallClockTime());

    const auto updateBSON = [&] {
        SessionTxnRecord newTxnRecord;
        newTxnRecord.setSessionId(*sessionInfo.getSessionId());
        newTxnRecord.setTxnNum(*sessionInfo.getTxnNumber());
        newTxnRecord.setLastWriteOpTime(entry.getOpTime());
        newTxnRecord.setLastWriteDate(*entry.getWallClockTime());
        return newTxnRecord.toBSON();
    }();

    return repl::OplogEntry(
        entry.getOpTime(),
        0,  // hash
        repl::OpTypeEnum::kUpdate,
        NamespaceString::kSessionTransactionsTableNamespace,
        boost::none,  // uuid
        false,        // fromMigrate
        repl::OplogEntry::kOplogVersion,
        updateBSON,
        BSON(SessionTxnRecord::kSessionIdFieldName << sessionInfo.getSessionId()->toBSON()),
        {},    // sessionInfo
        true,  // upsert
        *entry.getWallClockTime(),
        boost::none,  // statementId
        boost::none,  // prevWriteOpTime
        boost::none,  // preImangeOpTime
        boost::none   // postImageOpTime
        );
}

std::string Session::TransitionTable::toString(State state) {
    switch (state) {
        case Session::TransitionTable::State::kNone:
            return "TxnState::None";
        case Session::TransitionTable::State::kInProgress:
            return "TxnState::InProgress";
        case Session::TransitionTable::State::kPrepared:
            return "TxnState::Prepared";
        case Session::TransitionTable::State::kCommittingWithoutPrepare:
            return "TxnState::CommittingWithoutPrepare";
        case Session::TransitionTable::State::kCommittingWithPrepare:
            return "TxnState::CommittingWithPrepare";
        case Session::TransitionTable::State::kCommitted:
            return "TxnState::Committed";
        case Session::TransitionTable::State::kAborted:
            return "TxnState::Aborted";
    }
    MONGO_UNREACHABLE;
}

bool Session::TransitionTable::_isLegalTransition(State oldState, State newState) {
    switch (oldState) {
        case State::kNone:
            switch (newState) {
                case State::kNone:
                case State::kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kInProgress:
            switch (newState) {
                case State::kNone:
                case State::kPrepared:
                case State::kCommittingWithoutPrepare:
                case State::kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kPrepared:
            switch (newState) {
                case State::kCommittingWithPrepare:
                case State::kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kCommittingWithPrepare:
        case State::kCommittingWithoutPrepare:
            switch (newState) {
                case State::kNone:
                case State::kCommitted:
                case State::kAborted:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kCommitted:
            switch (newState) {
                case State::kNone:
                case State::kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case State::kAborted:
            switch (newState) {
                case State::kNone:
                case State::kInProgress:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

void Session::TransitionTable::transitionTo(WithLock,
                                            State newState,
                                            TransitionValidation shouldValidate) {

    if (shouldValidate == TransitionValidation::kValidateTransition) {
        invariant(TransitionTable::_isLegalTransition(_state, newState),
                  str::stream() << "Current state: " << toString(_state)
                                << ", Illegal attempted next state: "
                                << toString(newState));
    }

    _state = newState;
}

}  // namespace mongo
