/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#include "eden/fs/inodes/Overlay.h"

#include <boost/filesystem.hpp>
#include <algorithm>

#include <folly/Exception.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/Range.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include "eden/fs/inodes/DirEntry.h"
#include "eden/fs/utils/Bug.h"
#include "eden/fs/utils/PathFuncs.h"

#ifdef _WIN32
#include "eden/fs/inodes/InodeBase.h"
#include "eden/fs/inodes/TreeInode.h"
#include "eden/fs/win/utils/StringConv.h" // @manual
#include "eden/fs/win/utils/Stub.h" // @manual

#else
#include "eden/fs/inodes/InodeTable.h"
#include "eden/fs/inodes/OverlayFile.h"

#endif // !_WIN32

namespace facebook {
namespace eden {

namespace {
constexpr uint64_t ioCountMask = 0x7FFFFFFFFFFFFFFFull;
constexpr uint64_t ioClosedMask = 1ull << 63;
} // namespace

using folly::Unit;
using std::optional;

std::shared_ptr<Overlay> Overlay::create(AbsolutePathPiece localDir) {
  struct MakeSharedEnabler : public Overlay {
    explicit MakeSharedEnabler(AbsolutePathPiece localDir)
        : Overlay(localDir) {}
  };
  return std::make_shared<MakeSharedEnabler>(localDir);
}

Overlay::Overlay(AbsolutePathPiece localDir) : backingOverlay_{localDir} {}

Overlay::~Overlay() {
  close();
}

void Overlay::close() {
  CHECK_NE(std::this_thread::get_id(), gcThread_.get_id());

  gcQueue_.lock()->stop = true;
  gcCondVar_.notify_one();
  if (gcThread_.joinable()) {
    gcThread_.join();
  }

  // Make sure everything is shut down in reverse of construction order.
  // Cleanup is not necessary if overlay was not initialized
  if (!backingOverlay_.initialized()) {
    return;
  }

  // Since we are closing the overlay, no other threads can still be using
  // it. They must have used some external synchronization mechanism to
  // ensure this, so it is okay for us to still use relaxed access to
  // nextInodeNumber_.
  std::optional<InodeNumber> optNextInodeNumber;
  auto nextInodeNumber = nextInodeNumber_.load(std::memory_order_relaxed);
  if (nextInodeNumber) {
    optNextInodeNumber = InodeNumber{nextInodeNumber};
  }

  closeAndWaitForOutstandingIO();
#ifndef _WIN32
  inodeMetadataTable_.reset();
#endif // !_WIN32

  backingOverlay_.close(optNextInodeNumber);
}

bool Overlay::isClosed() {
  return outstandingIORequests_.load(std::memory_order_acquire) & ioClosedMask;
}

#ifndef _WIN32
struct statfs Overlay::statFs() {
  IORequest req{this};
  return backingOverlay_.statFs();
}
#endif // !_WIN32

folly::SemiFuture<Unit> Overlay::initialize(
    OverlayChecker::ProgressCallback&& progressCallback) {
  // The initOverlay() call is potentially slow, so we want to avoid
  // performing it in the current thread and blocking returning to our caller.
  //
  // We already spawn a separate thread for garbage collection.  It's convenient
  // to simply use this existing thread to perform the initialization logic
  // before waiting for GC work to do.
  auto [initPromise, initFuture] = folly::makePromiseContract<Unit>();

  gcThread_ = std::thread([this,
                           progressCallback = std::move(progressCallback),
                           promise = std::move(initPromise)]() mutable {
    try {
      initOverlay(progressCallback);
    } catch (std::exception& ex) {
      XLOG(ERR) << "overlay initialization failed for "
                << backingOverlay_.getLocalDir() << ": " << ex.what();
      promise.setException(
          folly::exception_wrapper(std::current_exception(), ex));
      return;
    }
    promise.setValue();
#ifndef _WIN32
    // TODO: On Windows files are cached by the ProjectedFS. We need to
    // clean the cached files while doing GC.

    gcThread();
#endif
  });
  return std::move(initFuture);
}

void Overlay::initOverlay(
    const OverlayChecker::ProgressCallback& progressCallback) {
  IORequest req{this};
  auto optNextInodeNumber = backingOverlay_.initOverlay(true);
  if (!optNextInodeNumber.has_value()) {
#ifndef _WIN32
    // If the next-inode-number data is missing it means that this overlay was
    // not shut down cleanly the last time it was used.  If this was caused by a
    // hard system reboot this can sometimes cause corruption and/or missing
    // data in some of the on-disk state.
    //
    // Use OverlayChecker to scan the overlay for any issues, and also compute
    // correct next inode number as it does so.
    XLOG(WARN) << "Overlay " << backingOverlay_.getLocalDir()
               << " was not shut down cleanly.  Performing fsck scan.";

    OverlayChecker checker(&backingOverlay_, std::nullopt);
    checker.scanForErrors(progressCallback);
    checker.repairErrors();

    optNextInodeNumber = checker.getNextInodeNumber();
#else
    // SqliteOverlay will always return the value of next Inode number, if we
    // end up here - it's a bug.
    EDEN_BUG() << "Sqlite Overlay is null value for NextInodeNumber";
#endif
  } else {
    hadCleanStartup_ = true;
  }
  nextInodeNumber_.store(optNextInodeNumber->get(), std::memory_order_relaxed);

#ifndef _WIN32
  // Open after infoFile_'s lock is acquired because the InodeTable acquires
  // its own lock, which should be released prior to infoFile_.
  inodeMetadataTable_ =
      InodeMetadataTable::open((backingOverlay_.getLocalDir() +
                                PathComponentPiece{FsOverlay::kMetadataFile})
                                   .c_str());
#endif // !_WIN32
}

InodeNumber Overlay::allocateInodeNumber() {
  // InodeNumber should generally be 64-bits wide, in which case it isn't even
  // worth bothering to handle the case where nextInodeNumber_ wraps.  We don't
  // need to bother checking for conflicts with existing inode numbers since
  // this can only happen if we wrap around.  We don't currently support
  // platforms with 32-bit inode numbers.
  static_assert(
      sizeof(nextInodeNumber_) == sizeof(InodeNumber),
      "expected nextInodeNumber_ and InodeNumber to have the same size");
  static_assert(
      sizeof(InodeNumber) >= 8, "expected InodeNumber to be at least 64 bits");

  // This could be a relaxed atomic operation.  It doesn't matter on x86 but
  // might on ARM.
  auto previous = nextInodeNumber_++;
#ifdef _WIN32
  backingOverlay_.updateUsedInodeNumber(previous);
#endif
  DCHECK_NE(0, previous) << "allocateInodeNumber called before initialize";
  return InodeNumber{previous};
}

optional<DirContents> Overlay::loadOverlayDir(InodeNumber inodeNumber) {
  IORequest req{this};
  auto dirData = backingOverlay_.loadOverlayDir(inodeNumber);
  if (!dirData.has_value()) {
    return std::nullopt;
  }
  const auto& dir = dirData.value();

  bool shouldMigrateToNewFormat = false;

  DirContents result;
  for (auto& iter : dir.entries) {
    const auto& name = iter.first;
    const auto& value = iter.second;

    bool isMaterialized =
        !value.hash_ref() || value.hash_ref().value_unchecked().empty();
    InodeNumber ino;
    if (value.inodeNumber) {
      ino = InodeNumber::fromThrift(value.inodeNumber);
    } else {
      ino = allocateInodeNumber();
      shouldMigrateToNewFormat = true;
    }

    if (isMaterialized) {
      result.emplace(PathComponentPiece{name}, value.mode, ino);
    } else {
      auto hash = Hash{folly::ByteRange{
          folly::StringPiece{value.hash_ref().value_unchecked()}}};
      result.emplace(PathComponentPiece{name}, value.mode, ino, hash);
    }
  }

  if (shouldMigrateToNewFormat) {
    saveOverlayDir(inodeNumber, result);
  }

  return optional<DirContents>{std::move(result)};
}

void Overlay::saveOverlayDir(InodeNumber inodeNumber, const DirContents& dir) {
  IORequest req{this};
  auto nextInodeNumber = nextInodeNumber_.load(std::memory_order_relaxed);
  CHECK_LT(inodeNumber.get(), nextInodeNumber)
      << "saveOverlayDir called with unallocated inode number";

  // TODO: T20282158 clean up access of child inode information.
  //
  // Translate the data to the thrift equivalents
  overlay::OverlayDir odir;

  for (auto& entIter : dir) {
    const auto& entName = entIter.first;
    const auto& ent = entIter.second;

    CHECK_NE(entName, "")
        << "saveOverlayDir called with entry with an empty path for directory with inodeNumber="
        << inodeNumber;
    CHECK_LT(ent.getInodeNumber().get(), nextInodeNumber)
        << "saveOverlayDir called with entry using unallocated inode number";

    overlay::OverlayEntry oent;
    // TODO: Eventually, we should merely serialize the child entry's dtype
    // into the Overlay. But, as of now, it's possible to create an inode under
    // a tree, serialize that tree into the overlay, then restart Eden. Since
    // writing mode bits into the InodeMetadataTable only occurs when the inode
    // is loaded, the initial mode bits must persist until the first load.
    oent.mode = ent.getInitialMode();
    oent.inodeNumber = ent.getInodeNumber().get();
    bool isMaterialized = ent.isMaterialized();
    if (!isMaterialized) {
      auto entHash = ent.getHash();
      auto bytes = entHash.getBytes();
      oent.hash_ref() = std::string{reinterpret_cast<const char*>(bytes.data()),

                                    bytes.size()};
    }

    odir.entries.emplace(
        std::make_pair(entName.stringPiece().str(), std::move(oent)));
  }

  backingOverlay_.saveOverlayDir(inodeNumber, odir);
}

void Overlay::removeOverlayData(InodeNumber inodeNumber) {
  IORequest req{this};

#ifndef _WIN32
  // TODO: batch request during GC
  getInodeMetadataTable()->freeInode(inodeNumber);
  backingOverlay_.removeOverlayFile(inodeNumber);
#else
  backingOverlay_.removeOverlayData(inodeNumber);
#endif // !_WIN32
}

#ifndef _WIN32
void Overlay::recursivelyRemoveOverlayData(InodeNumber inodeNumber) {
  IORequest req{this};
  auto dirData = backingOverlay_.loadOverlayDir(inodeNumber);

  // This inode's data must be removed from the overlay before
  // recursivelyRemoveOverlayData returns to avoid a race condition if
  // recursivelyRemoveOverlayData(I) is called immediately prior to
  // saveOverlayDir(I).  There's also no risk of violating our durability
  // guarantees if the process dies after this call but before the thread could
  // remove this data.
  removeOverlayData(inodeNumber);

  if (dirData) {
    gcQueue_.lock()->queue.emplace_back(std::move(*dirData));
    gcCondVar_.notify_one();
  }
}
#endif

#ifndef _WIN32
folly::Future<folly::Unit> Overlay::flushPendingAsync() {
  folly::Promise<folly::Unit> promise;
  auto future = promise.getFuture();
  gcQueue_.lock()->queue.emplace_back(std::move(promise));
  gcCondVar_.notify_one();
  return future;
}
#endif // !_WIN32

bool Overlay::hasOverlayData(InodeNumber inodeNumber) {
  IORequest req{this};
  return backingOverlay_.hasOverlayData(inodeNumber);
}

#ifndef _WIN32
// Helper function to open,validate,
// get file pointer of an overlay file
OverlayFile Overlay::openFile(
    InodeNumber inodeNumber,
    folly::StringPiece headerId) {
  IORequest req{this};
  return OverlayFile(
      backingOverlay_.openFile(inodeNumber, headerId), weak_from_this());
}

OverlayFile Overlay::openFileNoVerify(InodeNumber inodeNumber) {
  IORequest req{this};
  return OverlayFile(
      backingOverlay_.openFileNoVerify(inodeNumber), weak_from_this());
}

OverlayFile Overlay::createOverlayFile(
    InodeNumber inodeNumber,
    folly::ByteRange contents) {
  IORequest req{this};
  CHECK_LT(inodeNumber.get(), nextInodeNumber_.load(std::memory_order_relaxed))
      << "createOverlayFile called with unallocated inode number";
  return OverlayFile(
      backingOverlay_.createOverlayFile(inodeNumber, contents),
      weak_from_this());
}

OverlayFile Overlay::createOverlayFile(
    InodeNumber inodeNumber,
    const folly::IOBuf& contents) {
  IORequest req{this};
  CHECK_LT(inodeNumber.get(), nextInodeNumber_.load(std::memory_order_relaxed))
      << "createOverlayFile called with unallocated inode number";
  return OverlayFile(
      backingOverlay_.createOverlayFile(inodeNumber, contents),
      weak_from_this());
}

#endif // !_WIN32

InodeNumber Overlay::getMaxInodeNumber() {
  auto ino = nextInodeNumber_.load(std::memory_order_relaxed);
  CHECK_GT(ino, 1);
  return InodeNumber{ino - 1};
}

bool Overlay::tryIncOutstandingIORequests() {
  uint64_t currentOutstandingIO =
      outstandingIORequests_.load(std::memory_order_seq_cst);

  // Retry incrementing the IO count while we have not either successfully
  // updated outstandingIORequests_ or closed the overlay
  while (!(currentOutstandingIO & ioClosedMask)) {
    // If not closed, currentOutstandingIO now holds what
    // outstandingIORequests_ actually contained
    if (outstandingIORequests_.compare_exchange_weak(
            currentOutstandingIO,
            currentOutstandingIO + 1,
            std::memory_order_seq_cst)) {
      return true;
    }
  }

  // If we have broken out of the above loop, the overlay is closed and we
  // been unable to increment outstandingIORequests_.
  return false;
}

void Overlay::decOutstandingIORequests() {
  uint64_t outstanding =
      outstandingIORequests_.fetch_sub(1, std::memory_order_seq_cst);
  XCHECK_NE(0ull, outstanding) << "Decremented too far!";
  // If the overlay is closed and we just finished our last IO request (meaning
  // the previous value of outstandingIORequests_ was 1), then wake the waiting
  // thread.
  if ((outstanding & ioClosedMask) && (outstanding & ioCountMask) == 1) {
    lastOutstandingRequestIsComplete_.post();
  }
}

void Overlay::closeAndWaitForOutstandingIO() {
  uint64_t outstanding =
      outstandingIORequests_.fetch_or(ioClosedMask, std::memory_order_seq_cst);

  // If we have outstanding IO requests, wait for them. This should not block if
  // this baton has already been posted between the load in the fetch_or and
  // this if statement.
  if (outstanding & ioCountMask) {
    lastOutstandingRequestIsComplete_.wait();
  }
}

#ifndef _WIN32
// TODO: On Windows files are cached by the ProjectedFS. We need to clean that
// cache before doing GC.

void Overlay::gcThread() noexcept {
  for (;;) {
    std::vector<GCRequest> requests;
    {
      auto lock = gcQueue_.lock();
      while (lock->queue.empty()) {
        if (lock->stop) {
          return;
        }
        gcCondVar_.wait(lock.getUniqueLock());
        continue;
      }

      requests = std::move(lock->queue);
    }

    for (auto& request : requests) {
      try {
        handleGCRequest(request);
      } catch (const std::exception& e) {
        XLOG(ERR) << "handleGCRequest should never throw, but it did: "
                  << e.what();
      }
    }
  }
}

void Overlay::handleGCRequest(GCRequest& request) {
  IORequest req{this};
  if (request.flush) {
    request.flush->setValue();
    return;
  }

  // Should only include inode numbers for trees.
  std::queue<InodeNumber> queue;

  // TODO: For better throughput on large tree collections, it might make
  // sense to split this into two threads: one for traversing the tree and
  // another that makes the actual unlink calls.
  auto safeRemoveOverlayData = [&](InodeNumber inodeNumber) {
    try {
      removeOverlayData(inodeNumber);
    } catch (const std::exception& e) {
      XLOG(ERR) << "Failed to remove overlay data for inode " << inodeNumber
                << ": " << e.what();
    }
  };

  auto processDir = [&](const overlay::OverlayDir& dir) {
    for (const auto& entry : dir.entries) {
      const auto& value = entry.second;
      if (!value.inodeNumber) {
        // Legacy-only.  All new Overlay trees have inode numbers for all
        // children.
        continue;
      }
      auto ino = InodeNumber::fromThrift(value.inodeNumber);

      if (S_ISDIR(value.mode)) {
        queue.push(ino);
      } else {
        // No need to recurse, but delete any file at this inode.  Note that,
        // under normal operation, there should be nothing at this path
        // because files are only written into the overlay if they're
        // materialized.
        safeRemoveOverlayData(ino);
      }
    }
  };

  processDir(request.dir);

  while (!queue.empty()) {
    auto ino = queue.front();
    queue.pop();

    overlay::OverlayDir dir;
    try {
      auto dirData = backingOverlay_.loadOverlayDir(ino);
      if (!dirData.has_value()) {
        XLOG(DBG7) << "no dir data for inode " << ino;
        continue;
      } else {
        dir = std::move(*dirData);
      }
    } catch (const std::exception& e) {
      XLOG(ERR) << "While collecting, failed to load tree data for inode "
                << ino << ": " << e.what();
      continue;
    }

    safeRemoveOverlayData(ino);
    processDir(dir);
  }
}
#endif // !1

} // namespace eden
} // namespace facebook
