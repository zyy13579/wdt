/**
 * Copyright (c) 2014-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include <wdt/Receiver.h>
#include <wdt/util/FileWriter.h>
#include <wdt/util/ServerSocket.h>
#include <wdt/util/EncryptionUtils.h>

#include <folly/Conv.h>
#include <folly/Memory.h>
#include <folly/String.h>
#include <folly/ScopeGuard.h>
#include <folly/Bits.h>
#include <folly/Checksum.h>

#include <fcntl.h>
#include <unistd.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
using std::vector;
namespace facebook {
namespace wdt {
void Receiver::addCheckpoint(Checkpoint checkpoint) {
  LOG(INFO) << "Adding global checkpoint " << checkpoint.port << " "
            << checkpoint.numBlocks << " " << checkpoint.lastBlockReceivedBytes;
  checkpoints_.emplace_back(checkpoint);
}

std::vector<Checkpoint> Receiver::getNewCheckpoints(int startIndex) {
  std::vector<Checkpoint> checkpoints;
  const int64_t numCheckpoints = checkpoints_.size();
  for (int64_t i = startIndex; i < numCheckpoints; i++) {
    checkpoints.emplace_back(checkpoints_[i]);
  }
  return checkpoints;
}

Receiver::Receiver(const WdtTransferRequest &transferRequest)
    : transferLogManager_(options_) {
  LOG(INFO) << "WDT Receiver " << Protocol::getFullVersion();
  transferRequest_ = transferRequest;
}

Receiver::Receiver(int port, int numSockets, const std::string &destDir)
    : Receiver(WdtTransferRequest(port, numSockets, destDir)) {
}

void Receiver::traverseDestinationDir(
    std::vector<FileChunksInfo> &fileChunksInfo) {
  DirectorySourceQueue dirQueue(options_, destDir_, &abortCheckerCallback_);
  dirQueue.buildQueueSynchronously();
  auto &discoveredFilesInfo = dirQueue.getDiscoveredFilesMetaData();
  for (auto &fileInfo : discoveredFilesInfo) {
    FileChunksInfo chunkInfo(fileInfo->seqId, fileInfo->relPath,
                             fileInfo->size);
    chunkInfo.addChunk(Interval(0, fileInfo->size));
    fileChunksInfo.emplace_back(std::move(chunkInfo));
  }
  return;
}

void Receiver::startNewGlobalSession(const std::string &peerIp) {
  if (throttler_) {
    // If throttler is configured/set then register this session
    // in the throttler. This is guranteed to work in either of the
    // modes long running or not. We will de register from the throttler
    // when the current session ends
    throttler_->registerTransfer();
  }
  startTime_ = Clock::now();
  if (options_.enable_download_resumption) {
    transferLogManager_.startThread();
    bool verifySuccessful = transferLogManager_.verifySenderIp(peerIp);
    if (!verifySuccessful) {
      fileChunksInfo_.clear();
    }
  }
  hasNewTransferStarted_.store(true);
  LOG(INFO) << "Starting new transfer,  peerIp " << peerIp << " , transfer id "
            << getTransferId();
}

bool Receiver::hasNewTransferStarted() const {
  return hasNewTransferStarted_.load();
}

void Receiver::endCurGlobalSession() {
  setTransferStatus(FINISHED);
  if (!hasNewTransferStarted_) {
    LOG(WARNING) << "WDT transfer did not start, no need to end session";
    return;
  }
  LOG(INFO) << "Ending the transfer " << getTransferId();
  if (throttler_) {
    throttler_->deRegisterTransfer();
  }
  checkpoints_.clear();
  fileCreator_->clearAllocationMap();
  // TODO might consider moving closing the transfer log here
  hasNewTransferStarted_.store(false);
}

const WdtTransferRequest &Receiver::init() {
  if (validateTransferRequest() != OK) {
    LOG(ERROR) << "Couldn't validate the transfer request "
               << transferRequest_.getLogSafeString();
    return transferRequest_;
  }
  checkAndUpdateBufferSize();
  backlog_ = options_.backlog;
  if (getTransferId().empty()) {
    setTransferId(WdtBase::generateTransferId());
  }
  setProtocolVersion(transferRequest_.protocolVersion);
  setDir(transferRequest_.directory);
  auto numThreads = transferRequest_.ports.size();
  // This creates the destination directory (which is needed for transferLogMgr)
  fileCreator_.reset(
      new FileCreator(destDir_, numThreads, transferLogManager_));
  // Make sure we can get the lock on the transfer log manager early
  // so if we can't we don't generate a valid but useless url and end up
  // starting a sender doomed to fail
  if (options_.enable_download_resumption) {
    WDT_CHECK(!options_.skip_writes)
        << "Can not skip transfers with download resumption turned on";
    if (options_.resume_using_dir_tree) {
      WDT_CHECK(!options_.shouldPreallocateFiles())
          << "Can not resume using directory tree if preallocation is enabled";
    }
    ErrorCode errCode = transferLogManager_.openLog();
    if (errCode != OK) {
      LOG(ERROR) << "Failed to open transfer log " << errorCodeToStr(errCode);
      transferRequest_.errorCode = errCode;
      return transferRequest_;
    }
    ErrorCode code = transferLogManager_.parseAndMatch(
        recoveryId_, getTransferConfig(), fileChunksInfo_);
    if (code == OK && options_.resume_using_dir_tree) {
      WDT_CHECK(fileChunksInfo_.empty());
      traverseDestinationDir(fileChunksInfo_);
    }
  }

  EncryptionType encryptionType = parseEncryptionType(options_.encryption_type);
  // is encryption enabled?
  bool encrypt = (encryptionType != ENC_NONE &&
                  protocolVersion_ >= Protocol::ENCRYPTION_V1_VERSION);
  if (encrypt) {
    LOG(INFO) << encryptionTypeToStr(encryptionType)
              << " encryption is enabled for this transfer ";
    if (!transferRequest_.encryptionData.isSet()) {
      LOG(INFO) << "Receiver generating encryption key for type "
                << encryptionTypeToStr(encryptionType);
      transferRequest_.encryptionData =
          EncryptionParams::generateEncryptionParams(encryptionType);
    }
    if (!transferRequest_.encryptionData.isSet()) {
      LOG(ERROR) << "Unable to generate encryption key for type "
                 << encryptionTypeToStr(encryptionType);
      transferRequest_.errorCode = ENCRYPTION_ERROR;
      return transferRequest_;
    }
  } else {
    if (encryptionType != ENC_NONE) {
      LOG(WARNING) << "Encryption is enabled, but protocol version is "
                   << protocolVersion_
                   << ", minimum version required for encryption is "
                   << Protocol::ENCRYPTION_V1_VERSION;
    }
    transferRequest_.encryptionData.erase();
  }
  threadsController_ = new ThreadsController(numThreads);
  threadsController_->setNumFunnels(ReceiverThread::NUM_FUNNELS);
  threadsController_->setNumBarriers(ReceiverThread::NUM_BARRIERS);
  threadsController_->setNumConditions(ReceiverThread::NUM_CONDITIONS);
  // TODO: take transferRequest directly !
  receiverThreads_ = threadsController_->makeThreads<Receiver, ReceiverThread>(
      this, transferRequest_.ports.size(), transferRequest_.ports);
  size_t numSuccessfulInitThreads = 0;
  for (auto &receiverThread : receiverThreads_) {
    ErrorCode code = receiverThread->init();
    if (code == OK) {
      ++numSuccessfulInitThreads;
    }
  }
  LOG(INFO) << "Registered " << numSuccessfulInitThreads
            << " successful sockets";
  ErrorCode code = OK;
  const size_t targetSize = transferRequest_.ports.size();
  // TODO: replace with getNumPorts/thread
  if (numSuccessfulInitThreads != targetSize) {
    code = FEWER_PORTS;
    if (numSuccessfulInitThreads == 0) {
      code = ERROR;
    }
  }
  transferRequest_.protocolVersion = protocolVersion_;

  transferRequest_.ports.clear();
  for (const auto &receiverThread : receiverThreads_) {
    transferRequest_.ports.push_back(receiverThread->getPort());
  }

  if (transferRequest_.hostName.empty()) {
    char hostName[1024];
    int ret = gethostname(hostName, sizeof(hostName));
    if (ret == 0) {
      transferRequest_.hostName.assign(hostName);
    } else {
      PLOG(ERROR) << "Couldn't find the host name";
      code = ERROR;
    }
  }
  transferRequest_.directory = getDir();
  transferRequest_.errorCode = code;
  return transferRequest_;
}

void Receiver::setDir(const std::string &destDir) {
  destDir_ = destDir;
  transferLogManager_.setRootDir(destDir_);
}

TransferLogManager &Receiver::getTransferLogManager() {
  return transferLogManager_;
}

std::unique_ptr<FileCreator> &Receiver::getFileCreator() {
  return fileCreator_;
}

const std::string &Receiver::getDir() {
  return destDir_;
}

void Receiver::setRecoveryId(const std::string &recoveryId) {
  recoveryId_ = recoveryId;
  LOG(INFO) << "recovery id " << recoveryId_;
}

Receiver::~Receiver() {
  TransferStatus status = getTransferStatus();
  if (status == ONGOING) {
    LOG(WARNING) << "There is an ongoing transfer and the destructor"
                 << " is being called. Trying to finish the transfer";
    abort(ABORTED_BY_APPLICATION);
  }
  finish();
}

const std::vector<FileChunksInfo> &Receiver::getFileChunksInfo() const {
  return fileChunksInfo_;
}

int64_t Receiver::getTransferConfig() const {
  int64_t config = 0;
  if (options_.shouldPreallocateFiles()) {
    config = 1;
  }
  if (options_.resume_using_dir_tree) {
    config |= (1 << 1);
  }
  return config;
}

std::unique_ptr<TransferReport> Receiver::finish() {
  std::unique_lock<std::mutex> instanceLock(instanceManagementMutex_);
  TransferStatus status = getTransferStatus();
  if (status == NOT_STARTED) {
    LOG(WARNING) << "Even though transfer has not started, finish is called";
    // getTransferReport will set the error code to ERROR
    return getTransferReport();
  }
  if (status == THREADS_JOINED) {
    LOG(WARNING) << "Threads have already been joined. Returning the "
                 << "transfer report";
    return getTransferReport();
  }
  if (!isJoinable_) {
    // TODO: don't complain about this when coming from runForever()
    LOG(WARNING) << "The receiver is not joinable. The threads will never"
                 << " finish and this method will never return";
  }
  for (auto &receiverThread : receiverThreads_) {
    receiverThread->finish();
  }

  setTransferStatus(THREADS_JOINED);

  if (isJoinable_) {
    // Make sure to join the progress thread.
    progressTrackerThread_.join();
  }
  std::unique_ptr<TransferReport> report = getTransferReport();
  auto &summary = report->getSummary();
  bool transferSuccess = (report->getSummary().getErrorCode() == OK);
  fixAndCloseTransferLog(transferSuccess);
  auto totalSenderBytes = summary.getTotalSenderBytes();
  if (progressReporter_ && totalSenderBytes >= 0) {
    report->setTotalFileSize(totalSenderBytes);
    report->setTotalTime(durationSeconds(Clock::now() - startTime_));
    progressReporter_->end(report);
  }
  if (options_.enable_perf_stat_collection) {
    PerfStatReport globalPerfReport(options_);
    for (auto &receiverThread : receiverThreads_) {
      globalPerfReport += receiverThread->getPerfReport();
    }
    LOG(INFO) << globalPerfReport;
  }

  LOG(WARNING) << "WDT receiver's transfer has been finished";
  LOG(INFO) << *report;
  return report;
}

std::unique_ptr<TransferReport> Receiver::getTransferReport() {
  TransferStats globalStats;
  for (const auto &receiverThread : receiverThreads_) {
    globalStats += receiverThread->getTransferStats();
  }
  std::unique_ptr<TransferReport> transferReport =
      folly::make_unique<TransferReport>(std::move(globalStats));
  TransferStatus status = getTransferStatus();
  ErrorCode errCode = transferReport->getSummary().getErrorCode();
  if (status == NOT_STARTED && errCode == OK) {
    LOG(INFO) << "Transfer not started, setting the error code to ERROR";
    transferReport->setErrorCode(ERROR);
  }
  VLOG(1) << "Summary code " << errCode;
  return transferReport;
}

ErrorCode Receiver::transferAsync() {
  isJoinable_ = true;
  int progressReportIntervalMillis = options_.progress_report_interval_millis;
  if (!progressReporter_ && progressReportIntervalMillis > 0) {
    // if progress reporter has not been set, use the default one
    progressReporter_ = folly::make_unique<ProgressReporter>(transferRequest_);
  }
  return start();
}

ErrorCode Receiver::runForever() {
  WDT_CHECK(!options_.enable_download_resumption)
      << "Transfer resumption not supported in long running mode";

  // Enforce the full reporting to be false in the daemon mode.
  // These statistics are expensive, and useless as they will never
  // be received/reviewed in a forever running process.
  ErrorCode errCode = start();
  if (errCode != OK) {
    return errCode;
  }
  finish();
  // This method should never finish
  return ERROR;
}

void Receiver::progressTracker() {
  // Progress tracker will check for progress after the time specified
  // in milliseconds.
  int progressReportIntervalMillis = options_.progress_report_interval_millis;
  int throughputUpdateIntervalMillis =
      options_.throughput_update_interval_millis;
  if (progressReportIntervalMillis <= 0 || throughputUpdateIntervalMillis < 0 ||
      !isJoinable_) {
    return;
  }
  int throughputUpdateInterval =
      throughputUpdateIntervalMillis / progressReportIntervalMillis;

  int64_t lastEffectiveBytes = 0;
  std::chrono::time_point<Clock> lastUpdateTime = Clock::now();
  int intervalsSinceLastUpdate = 0;
  double currentThroughput = 0;
  LOG(INFO) << "Progress reporter updating every "
            << progressReportIntervalMillis << " ms";
  auto waitingTime = std::chrono::milliseconds(progressReportIntervalMillis);
  int64_t totalSenderBytes = -1;
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      conditionFinished_.wait_for(lock, waitingTime);
      if (transferStatus_ == THREADS_JOINED) {
        break;
      }
    }
    double totalTime = durationSeconds(Clock::now() - startTime_);
    TransferStats globalStats;
    for (const auto &receiverThread : receiverThreads_) {
      globalStats += receiverThread->getTransferStats();
    }
    totalSenderBytes = globalStats.getTotalSenderBytes();
    if (totalSenderBytes == -1) {
      continue;
    }
    auto transferReport = folly::make_unique<TransferReport>(
        std::move(globalStats), totalTime, totalSenderBytes);
    intervalsSinceLastUpdate++;
    if (intervalsSinceLastUpdate >= throughputUpdateInterval) {
      auto curTime = Clock::now();
      int64_t curEffectiveBytes =
          transferReport->getSummary().getEffectiveDataBytes();
      double time = durationSeconds(curTime - lastUpdateTime);
      currentThroughput = (curEffectiveBytes - lastEffectiveBytes) / time;
      lastEffectiveBytes = curEffectiveBytes;
      lastUpdateTime = curTime;
      intervalsSinceLastUpdate = 0;
    }
    transferReport->setCurrentThroughput(currentThroughput);
    progressReporter_->progress(transferReport);
  }
}

ErrorCode Receiver::start() {
  WDT_CHECK_EQ(getTransferStatus(), NOT_STARTED)
      << "There is already a transfer running on this instance of receiver";
  startTime_ = Clock::now();
  LOG(INFO) << "Starting (receiving) server on ports [ "
            << transferRequest_.ports << "] Target dir : " << destDir_;
  // TODO do the init stuff here
  if (!throttler_) {
    configureThrottler();
  } else {
    LOG(INFO) << "Throttler set externally. Throttler : " << *throttler_;
  }
  setTransferStatus(ONGOING);
  while (true) {
    for (auto &receiverThread : receiverThreads_) {
      receiverThread->startThread();
    }
    if (isJoinable_) {
      break;
    }
    // If it is long running mode, finish the threads
    // processing the current transfer and re spawn them again
    // with the same sockets
    for (auto &receiverThread : receiverThreads_) {
      receiverThread->finish();
      receiverThread->reset();
    }
    threadsController_->reset();
    // reset transfer status
    setTransferStatus(NOT_STARTED);
    continue;
  }
  if (isJoinable_) {
    if (progressReporter_) {
      progressReporter_->start();
    }
    std::thread trackerThread(&Receiver::progressTracker, this);
    progressTrackerThread_ = std::move(trackerThread);
  }
  return OK;
}

void Receiver::addTransferLogHeader(bool isBlockMode, bool isSenderResuming) {
  if (!options_.enable_download_resumption) {
    return;
  }
  bool invalidationEntryNeeded = false;
  if (!isSenderResuming) {
    LOG(INFO) << "Sender is not in resumption mode. Invalidating directory.";
    invalidationEntryNeeded = true;
  } else if (options_.resume_using_dir_tree && isBlockMode) {
    LOG(INFO) << "Sender is running in block mode, but receiver is running in "
                 "size based resumption mode. Invalidating directory.";
    invalidationEntryNeeded = true;
  }
  if (invalidationEntryNeeded) {
    transferLogManager_.invalidateDirectory();
  }
  bool isInconsistentDirectory =
      (transferLogManager_.getResumptionStatus() == INCONSISTENT_DIRECTORY);
  bool shouldWriteHeader =
      (!options_.resume_using_dir_tree || !isInconsistentDirectory);
  if (shouldWriteHeader) {
    transferLogManager_.writeLogHeader();
  }
}

void Receiver::fixAndCloseTransferLog(bool transferSuccess) {
  if (!options_.enable_download_resumption) {
    return;
  }

  bool isInconsistentDirectory =
      (transferLogManager_.getResumptionStatus() == INCONSISTENT_DIRECTORY);
  bool isInvalidLog =
      (transferLogManager_.getResumptionStatus() == INVALID_LOG);
  if (transferSuccess && isInconsistentDirectory) {
    // write log header to validate directory in case of success
    WDT_CHECK(options_.resume_using_dir_tree);
    transferLogManager_.writeLogHeader();
  }
  transferLogManager_.closeLog();
  if (!transferSuccess) {
    return;
  }
  if (isInvalidLog) {
    transferLogManager_.renameBuggyLog();
  }
  if (!options_.keep_transfer_log) {
    transferLogManager_.unlink();
  }
}
}
}  // namespace facebook::wdt
