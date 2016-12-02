// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_STORE_BUFFER_H_
#define V8_STORE_BUFFER_H_

#include "src/allocation.h"
#include "src/base/logging.h"
#include "src/base/platform/platform.h"
#include "src/cancelable-task.h"
#include "src/globals.h"
#include "src/heap/remembered-set.h"
#include "src/heap/slot-set.h"

namespace v8 {
namespace internal {

// Intermediate buffer that accumulates old-to-new stores from the generated
// code. Moreover, it stores invalid old-to-new slots with two entries.
// The first is a tagged address of the start of the invalid range, the second
// one is the end address of the invalid range or null if there is just one slot
// that needs to be removed from the remembered set. On buffer overflow the
// slots are moved to the remembered set.
class StoreBuffer {
 public:
  static const int kStoreBufferSize = 1 << (11 + kPointerSizeLog2);
  static const int kStoreBufferMask = kStoreBufferSize - 1;
  static const int kStoreBuffers = 2;
  static const intptr_t kDeletionTag = 1;

  V8_EXPORT_PRIVATE static void StoreBufferOverflow(Isolate* isolate);

  explicit StoreBuffer(Heap* heap);
  void SetUp();
  void TearDown();

  // Used to add entries from generated code.
  inline Address* top_address() { return reinterpret_cast<Address*>(&top_); }

  // Moves entries from a specific store buffer to the remembered set. This
  // method takes a lock.
  void MoveEntriesToRememberedSet(int index);

  // This method ensures that all used store buffer entries are transfered to
  // the remembered set.
  void MoveAllEntriesToRememberedSet();

  inline bool IsDeletionAddress(Address address) const {
    return reinterpret_cast<intptr_t>(address) & kDeletionTag;
  }

  inline Address MarkDeletionAddress(Address address) {
    return reinterpret_cast<Address>(reinterpret_cast<intptr_t>(address) |
                                     kDeletionTag);
  }

  inline Address UnmarkDeletionAddress(Address address) {
    return reinterpret_cast<Address>(reinterpret_cast<intptr_t>(address) &
                                     ~kDeletionTag);
  }

  // If we only want to delete a single slot, end should be set to null which
  // will be written into the second field. When processing the store buffer
  // the more efficient Remove method will be called in this case.
  void DeleteEntry(Address start, Address end = nullptr);

  void InsertEntry(Address slot) {
    // Insertions coming from the GC are directly inserted into the remembered
    // set. Insertions coming from the runtime are added to the store buffer to
    // allow concurrent processing.
    if (heap_->gc_state() == Heap::NOT_IN_GC) {
      if (top_ + sizeof(Address) > limit_[current_]) {
        StoreBufferOverflow(heap_->isolate());
      }
      *top_ = slot;
      top_++;
    } else {
      // In GC the store buffer has to be empty at any time.
      DCHECK(Empty());
      RememberedSet<OLD_TO_NEW>::Insert(Page::FromAddress(slot), slot);
    }
  }

  // Used by the concurrent processing thread to transfer entries from the
  // store buffer to the remembered set.
  void ConcurrentlyProcessStoreBuffer();

  bool Empty() {
    for (int i = 0; i < kStoreBuffers; i++) {
      if (lazy_top_[i]) {
        return false;
      }
    }
    return top_ == start_[current_];
  }

 private:
  // There are two store buffers. If one store buffer fills up, the main thread
  // publishes the top pointer of the store buffer that needs processing in its
  // global lazy_top_ field. After that it start the concurrent processing
  // thread. The concurrent processing thread uses the pointer in lazy_top_.
  // It will grab the given mutex and transfer its entries to the remembered
  // set. If the concurrent thread does not make progress, the main thread will
  // perform the work.
  // Important: there is an ordering constrained. The store buffer with the
  // older entries has to be processed first.
  class Task : public CancelableTask {
   public:
    Task(Isolate* isolate, StoreBuffer* store_buffer)
        : CancelableTask(isolate), store_buffer_(store_buffer) {}
    virtual ~Task() {}

   private:
    void RunInternal() override {
      store_buffer_->ConcurrentlyProcessStoreBuffer();
    }
    StoreBuffer* store_buffer_;
    DISALLOW_COPY_AND_ASSIGN(Task);
  };

  void FlipStoreBuffers();

  Heap* heap_;

  Address* top_;

  // The start and the limit of the buffer that contains store slots
  // added from the generated code. We have two chunks of store buffers.
  // Whenever one fills up, we notify a concurrent processing thread and
  // use the other empty one in the meantime.
  Address* start_[kStoreBuffers];
  Address* limit_[kStoreBuffers];

  // At most one lazy_top_ pointer is set at any time.
  Address* lazy_top_[kStoreBuffers];
  base::Mutex mutex_;

  // We only want to have at most one concurrent processing tas running.
  bool task_running_;

  // Points to the current buffer in use.
  int current_;

  base::VirtualMemory* virtual_memory_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_STORE_BUFFER_H_
