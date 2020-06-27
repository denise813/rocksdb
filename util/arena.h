//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Arena is an implementation of Allocator class. For a request of small size,
// it allocates a block with pre-defined block size. For a request of big
// size, it uses malloc to directly get the requested size.

#pragma once
#ifndef OS_WIN
#include <sys/mman.h>
#endif
#include <cstddef>
#include <cerrno>
#include <vector>
#include <assert.h>
#include <stdint.h>
#include "util/allocator.h"
#include "util/mutexlock.h"

namespace rocksdb {
/*
     我们知道，对于一个高性能的服务器端程序来说，内存的使用非常重要。C++提供了new/delete来管理内存的申请和释放，但是对于小对象来说，
 直接使用new/delete代价比较大，要付出额外的空间和时间，性价比不高。另外，我们也要避免多次的申请和释放引起的内存碎片。一旦碎片到
 达一定程度，即使剩余内存总量够用，但由于缺乏足够的连续空闲空间，导致内存不够用的假象。
     C++ STL为了避免内存碎片，实现一个复杂的内存池，rocksdb中则没有那么复杂，只是实现了一个"一次性"内存池arena。在rocksdb里面，并
 不是所有的地方都使用了这个内存池，主要是memtable使用，主要是用于临时存放用户的更新数据，由于更新的数据可能很小，所以这里使用内存
 池就很合适。
     为了避免小对象的频繁分配，需要减少对new的调用，最简单的做法就是申请大块的内存，多次分给客户。LevelDB用一个
 vector<char *>来保存所有的内存分配记录，默认每次申请4k的内存块，记录下当前内存块剩余指针和剩余内存字节数，每当有新的申请，如果
 当前剩余的字节能满足需要，则直接返回给用户。如果不能，对于超过1k的请求，直接new一个指定大小的内存块并返回，小于1K的请求，则申请
 一个新的4k内存块，从中分配一部分给用户。当内存池对象析构时，分配的内存均被释放，保证了内存不会泄漏。
*/
// arena是LevelDB内部实现的内存池。
class Arena : public Allocator {
 public:
  // No copying allowed
  Arena(const Arena&) = delete;
  void operator=(const Arena&) = delete;

  static const size_t kInlineSize = 2048;
  static const size_t kMinBlockSize;
  static const size_t kMaxBlockSize;

  // huge_page_size: if 0, don't use huge page TLB. If > 0 (should set to the
  // supported hugepage size of the system), block allocation will try huge
  // page TLB first. If allocation fails, will fall back to normal case.
  explicit Arena(size_t block_size = kMinBlockSize,
                 AllocTracker* tracker = nullptr, size_t huge_page_size = 0);
  ~Arena();
  // 分配bytes大小的内存块，返回指向该内存块的指针
  char* Allocate(size_t bytes) override;

  // huge_page_size: if >0, will try to allocate from huage page TLB.
  // The argument will be the size of the page size for huge page TLB. Bytes
  // will be rounded up to multiple of the page size to allocate through mmap
  // anonymous option with huge page on. The extra  space allocated will be
  // wasted. If allocation fails, will fall back to normal case. To enable it,
  // need to reserve huge pages for it to be allocated, like:
  //     sysctl -w vm.nr_hugepages=20
  // See linux doc Documentation/vm/hugetlbpage.txt for details.
  // huge page allocation can fail. In this case it will fail back to
  // normal cases. The messages will be logged to logger. So when calling with
  // huge_page_tlb_size > 0, we highly recommend a logger is passed in.
  // Otherwise, the error message will be printed out to stderr directly.
  // 基于malloc的字节对齐内存分配
  char* AllocateAligned(size_t bytes, size_t huge_page_size = 0,
                        Logger* logger = nullptr) override;

  // Returns an estimate of the total memory usage of data allocated
  // by the arena (exclude the space allocated but not yet used for future
  // allocations).
  // 返回整个内存池使用内存的总大小
  size_t ApproximateMemoryUsage() const {
    return blocks_memory_ + blocks_.capacity() * sizeof(char*) -
           alloc_bytes_remaining_;
  }

  size_t MemoryAllocatedBytes() const { return blocks_memory_; }

  size_t AllocatedAndUnused() const { return alloc_bytes_remaining_; }

  // If an allocation is too big, we'll allocate an irregular block with the
  // same size of that allocation.
  size_t IrregularBlockNum() const { return irregular_block_num; }

  size_t BlockSize() const override { return kBlockSize; }

  bool IsInInlineBlock() const {
    return blocks_.empty();
  }

 private:
  char inline_block_[kInlineSize] __attribute__((__aligned__(alignof(max_align_t))));
  // Number of bytes allocated in one block
  const size_t kBlockSize;
  
  // Array of new[] allocated memory blocks
  // 用来存储每一次向系统请求分配的内存块的指针
  typedef std::vector<char*> Blocks;
  Blocks blocks_;

  struct MmapInfo {
    void* addr_;
    size_t length_;

    MmapInfo(void* addr, size_t length) : addr_(addr), length_(length) {}
  };
  std::vector<MmapInfo> huge_blocks_;
  size_t irregular_block_num = 0;

  // Stats for current active block.
  // For each block, we allocate aligned memory chucks from one end and
  // allocate unaligned memory chucks from the other end. Otherwise the
  // memory waste for alignment will be higher if we allocate both types of
  // memory from one direction.

   // 当前内存块(block)偏移量指针，也就是未使用内存的首地址
  char* unaligned_alloc_ptr_ = nullptr;
  char* aligned_alloc_ptr_ = nullptr;
  
  // How many bytes left in currently active block?
  // 表示当前内存块(block)中未使用的空间大小
  size_t alloc_bytes_remaining_ = 0;

#ifdef MAP_HUGETLB
  size_t hugetlb_size_ = 0;
#endif  // MAP_HUGETLB
  char* AllocateFromHugePage(size_t bytes);
  char* AllocateFallback(size_t bytes, bool aligned);
  char* AllocateNewBlock(size_t block_bytes);

  // Bytes of memory in blocks allocated so far
  // 迄今为止分配的内存块的总大小
  size_t blocks_memory_ = 0;
  AllocTracker* tracker_;
};

inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) { //直接使用可用的空间，不alloc
    unaligned_alloc_ptr_ -= bytes;
    alloc_bytes_remaining_ -= bytes;
    return unaligned_alloc_ptr_;
  }

  // 因为alloc_bytes_remaining_初始为0，因此第一次调用Allocate实际上直接调用的是AllocateFallback
  // 如果需求的内存大于内存块中剩余的内存，也会调用AllocateFallback
  return AllocateFallback(bytes, false /* unaligned */);
}

// check and adjust the block_size so that the return value is
//  1. in the range of [kMinBlockSize, kMaxBlockSize].
//  2. the multiple of align unit.
extern size_t OptimizeBlockSize(size_t block_size);

}  // namespace rocksdb
