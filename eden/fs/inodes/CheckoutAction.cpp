/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "eden/fs/inodes/CheckoutAction.h"

#include "eden/fs/inodes/CheckoutContext.h"
#include "eden/fs/inodes/FileInode.h"
#include "eden/fs/inodes/InodeBase.h"
#include "eden/fs/inodes/TreeInode.h"
#include "eden/fs/model/Blob.h"
#include "eden/fs/model/Tree.h"
#include "eden/fs/model/TreeEntry.h"
#include "eden/fs/service/gen-cpp2/eden_types.h"
#include "eden/fs/store/ObjectStore.h"

using folly::exception_wrapper;
using folly::Future;
using folly::makeFuture;
using folly::Unit;
using std::make_shared;
using std::vector;

namespace facebook {
namespace eden {

CheckoutAction::CheckoutAction(
    CheckoutContext* ctx,
    const TreeEntry& oldScmEntry,
    const TreeEntry& newScmEntry,
    InodePtr&& inode)
    : ctx_(ctx),
      oldScmEntry_(oldScmEntry),
      newScmEntry_(newScmEntry),
      inode_(std::move(inode)) {}

CheckoutAction::CheckoutAction(
    CheckoutContext* ctx,
    const TreeEntry& oldScmEntry,
    InodePtr&& inode)
    : ctx_(ctx), oldScmEntry_(oldScmEntry), inode_(std::move(inode)) {}

CheckoutAction::CheckoutAction(
    InternalConstructor,
    CheckoutContext* ctx,
    const TreeEntry& oldScmEntry,
    const folly::Optional<TreeEntry>& newScmEntry,
    folly::Future<InodePtr> inodeFuture)
    : ctx_(ctx),
      oldScmEntry_(oldScmEntry),
      newScmEntry_(newScmEntry),
      inodeFuture_(std::move(inodeFuture)) {}

CheckoutAction::~CheckoutAction() {}

PathComponentPiece CheckoutAction::getEntryName() const {
  return oldScmEntry_.getName();
}

class CheckoutAction::LoadingRefcount {
 public:
  explicit LoadingRefcount(CheckoutAction* action) : action_(action) {
    action_->numLoadsPending_.fetch_add(1);
  }
  LoadingRefcount(LoadingRefcount&& other) noexcept : action_(other.action_) {
    other.action_ = nullptr;
  }
  LoadingRefcount& operator=(LoadingRefcount&& other) noexcept {
    decref();
    action_ = other.action_;
    other.action_ = nullptr;
    return *this;
  }
  ~LoadingRefcount() {
    decref();
  }

  /**
   * Implement the arrow operator, so that LoadingRefcount can be used like a
   * pointer.  This allows users to easily call through it into the underlying
   * CheckoutAction methods.
   */
  CheckoutAction* operator->() const {
    return action_;
  }

 private:
  void decref() {
    if (action_) {
      auto oldCount = action_->numLoadsPending_.fetch_sub(1);
      if (oldCount == 1) {
        // We were the last load to complete.  We can perform the action now.
        action_->allLoadsComplete();
      }
    }
  }

  CheckoutAction* action_;
};

Future<Unit> CheckoutAction::run(
    CheckoutContext* /* ctx */,
    ObjectStore* store) {
  // Immediately create one LoadingRefcount, to ensure that our
  // numLoadsPending_ refcount does not drop to 0 until after we have started
  // all required load operations.
  //
  // Even if all loads complete immediately, allLoadsComplete() won't be called
  // until this LoadingRefcount is destroyed.
  LoadingRefcount refcount{this};

  try {
    // Load the Blob or Tree for the old TreeEntry.
    if (oldScmEntry_.getType() == TreeEntryType::TREE) {
      store->getTreeFuture(oldScmEntry_.getHash())
          .then([rc = LoadingRefcount(this)](std::unique_ptr<Tree> oldTree) {
            rc->setOldTree(std::move(oldTree));
          })
          .onError([rc = LoadingRefcount(this)](const exception_wrapper& ew) {
            rc->error("error getting old tree", ew);
          });
    } else {
      store->getBlobFuture(oldScmEntry_.getHash())
          .then([rc = LoadingRefcount(this)](std::unique_ptr<Blob> oldBlob) {
            rc->setOldBlob(std::move(oldBlob));
          })
          .onError([rc = LoadingRefcount(this)](const exception_wrapper& ew) {
            rc->error("error getting old blob", ew);
          });
    }

    // If we have a new TreeEntry, load the corresponding Blob or Tree
    if (newScmEntry_.hasValue()) {
      const auto& newEntry = newScmEntry_.value();
      if (newEntry.getType() == TreeEntryType::TREE) {
        store->getTreeFuture(newEntry.getHash())
            .then([rc = LoadingRefcount(this)](std::unique_ptr<Tree> newTree) {
              rc->setNewTree(std::move(newTree));
            })
            .onError([rc = LoadingRefcount(this)](const exception_wrapper& ew) {
              rc->error("error getting new tree", ew);
            });
      } else {
        store->getBlobFuture(newEntry.getHash())
            .then([rc = LoadingRefcount(this)](std::unique_ptr<Blob> newBlob) {
              rc->setNewBlob(std::move(newBlob));
            })
            .onError([rc = LoadingRefcount(this)](const exception_wrapper& ew) {
              rc->error("error getting new blob", ew);
            });
      }
    }

    // If we were constructed with a Future<InodePtr>, wait for it.
    if (!inode_) {
      CHECK(inodeFuture_.hasValue());
      inodeFuture_.value()
          .then([rc = LoadingRefcount(this)](InodePtr inode) {
            rc->setInode(std::move(inode));
          })
          .onError([rc = LoadingRefcount(this)](const exception_wrapper& ew) {
            rc->error("error getting inode", ew);
          });
    }
  } catch (const std::exception& ex) {
    exception_wrapper ew{std::current_exception(), ex};
    refcount->error("error preparing to load data for checkout action", ew);
  }

  return promise_.getFuture();
}

void CheckoutAction::setOldTree(std::unique_ptr<Tree> tree) {
  CHECK(!oldTree_);
  CHECK(!oldBlob_);
  oldTree_ = std::move(tree);
}

void CheckoutAction::setOldBlob(std::unique_ptr<Blob> blob) {
  CHECK(!oldTree_);
  CHECK(!oldBlob_);
  oldBlob_ = std::move(blob);
}

void CheckoutAction::setNewTree(std::unique_ptr<Tree> tree) {
  CHECK(!newTree_);
  CHECK(!newBlob_);
  newTree_ = std::move(tree);
}

void CheckoutAction::setNewBlob(std::unique_ptr<Blob> blob) {
  CHECK(!newTree_);
  CHECK(!newBlob_);
  newBlob_ = std::move(blob);
}

void CheckoutAction::setInode(InodePtr inode) {
  CHECK(!inode_);
  inode_ = std::move(inode);
}

void CheckoutAction::error(
    folly::StringPiece msg,
    const folly::exception_wrapper& ew) {
  LOG(ERROR) << "error performing checkout action: " << msg << ": "
             << folly::exceptionStr(ew);
  errors_.push_back(ew);
}

void CheckoutAction::allLoadsComplete() noexcept {
  if (!ensureDataReady()) {
    // ensureDataReady() will fulfilled promise_ with an exception
    return;
  }

  try {
    doAction().then(
        [this](folly::Try<Unit>&& t) { this->promise_.setTry(std::move(t)); });
  } catch (const std::exception& ex) {
    exception_wrapper ew{std::current_exception(), ex};
    promise_.setException(ew);
  }
}

bool CheckoutAction::ensureDataReady() noexcept {
  if (!errors_.empty()) {
    // If multiple errors occurred, we log them all, but only propagate
    // up the first one.  If necessary we could change this to create
    // a single exception that contains all of the messages concatenated
    // together.
    if (errors_.size() > 1) {
      LOG(ERROR) << "multiple errors while attempting to load data for "
                    "checkout action:";
      for (const auto& ew : errors_) {
        LOG(ERROR) << "CheckoutAction error: " << folly::exceptionStr(ew);
      }
    }
    promise_.setException(errors_[0]);
    return false;
  }

  // Make sure we actually have all the data we need.
  // (Just in case something went wrong when wiring up the callbacks in such a
  // way that we also failed to call error().)
  if (!oldTree_ && !oldBlob_) {
    promise_.setException(
        std::runtime_error("failed to load data for old TreeEntry"));
    return false;
  }
  if (newScmEntry_.hasValue() && (!newTree_ && !newBlob_)) {
    promise_.setException(
        std::runtime_error("failed to load data for new TreeEntry"));
    return false;
  }
  if (!inode_) {
    promise_.setException(std::runtime_error("failed to load affected inode"));
    return false;
  }

  return true;
}

Future<Unit> CheckoutAction::doAction() {
  // All the data is ready and we're ready to go!
  //
  // Unfortunately there are a lot of combinations of cases to handle:
  // - What's the status of the entry in the old source control tree?
  //   (blob, tree, not present)
  // - What's the desired status of the entry in the new source control tree?
  //   (blob, tree, not present)
  // - What's the current status of the inode in the file system?
  //   (file, tree)
  //   Fortunately we don't have to worry about not present here.
  //   That case is already handled immediately in TreeInode::checkout()
  //   TODO: this isn't the case for forceUpdate()
  // - What type of checkout are we performing?
  //   (merge, check-only, force)

  // Check for conflicts first.
  if (hasConflict() && !ctx_->forceUpdate()) {
    // hasConflict will have added the conflict information to ctx_
    return makeFuture();
  }

  // Switch on the desired entry type
  if (newTree_) {
    return performTreeCheckout();
  } else if (newBlob_) {
    return performBlobCheckout();
  } else {
    return performRemoval();
  }
}

bool CheckoutAction::hasConflict() {
  if (oldTree_) {
    auto treeInode = inode_.asTreePtrOrNull();
    if (!treeInode) {
      // This was a directory, but has been replaced with a file on disk
      ctx_->addConflict(ConflictType::MODIFIED, inode_.get());
      return true;
    }

    // TODO: check for permissions changes

    // We don't check if this tree is unmodified from the old tree or not here.
    // We simply apply the checkout to the tree in this case, so that we report
    // conflicts for individual leaf inodes that were modified, and not for the
    // parent directories.
    return false;
  }

  // If we are still here this used to be a file.
  auto fileInode = inode_.asFilePtrOrNull();
  if (!fileInode) {
    // This was a file, but has been replaced with a directory on disk
    ctx_->addConflict(ConflictType::MODIFIED, inode_.get());
    return true;
  }

  // Check that the file contents are the same as the old source control entry
  if (!fileInode->isSameAs(*oldBlob_, oldScmEntry_.getMode())) {
    // The file contents or mode bits are different
    ctx_->addConflict(ConflictType::MODIFIED, inode_.get());
    return true;
  }

  // This file is the same as the old source control state.
  return false;
}

Future<Unit> CheckoutAction::performTreeCheckout() {
  auto treeInode = inode_.asTreePtrOrNull();
  if (treeInode) {
    // When going from a tree to a tree, use TreeInode::checkout()
    //
    // Note that we always pass in oldTree_ here, even if the old value might
    // have been a blob instead.  Calling checkout() with the old tree value as
    // null will do the right thing here.
    return treeInode->checkout(ctx_, std::move(oldTree_), std::move(newTree_));
  } else {
    // When going from a file to a tree, ask the parent to do the replacement.
    auto parent = inode_->getParent(ctx_->renameLock());
    return parent->checkoutReplaceEntry(ctx_, inode_, newScmEntry_.value());
  }
}

Future<Unit> CheckoutAction::performBlobCheckout() {
  // As the parent TreeInode to replace whatever is currently at this location
  // with the new blob.
  auto parent = inode_->getParent(ctx_->renameLock());
  return parent->checkoutReplaceEntry(ctx_, inode_, newScmEntry_.value());
}

Future<Unit> CheckoutAction::performRemoval() {
  // As the parent TreeInode to remove this entry
  auto parent = inode_->getParent(ctx_->renameLock());
  return parent->checkoutRemoveChild(ctx_, oldScmEntry_.getName(), inode_);
}
}
}
