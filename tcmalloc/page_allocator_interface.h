// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_PAGE_ALLOCATOR_INTERFACE_H_
#define TCMALLOC_PAGE_ALLOCATOR_INTERFACE_H_

#include <stddef.h>

#include "absl/base/thread_annotations.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/pageflags.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/stats.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

class PageMap;

class PageAllocatorInterface {
 public:
  PageAllocatorInterface(const char* label, MemoryTag tag);
  virtual ~PageAllocatorInterface() = default;
  // Allocate a run of "n" pages. These pages would be allocated to a total of
  // 'objects_per_span' objects. Returns zero if out of memory.  Caller should
  // not pass "n == 0" -- instead, n should have been rounded up already.
  virtual Span* New(Length n, SpanAllocInfo span_alloc_info)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) = 0;

  // As New, but the returned span is aligned to a <align>-page boundary.
  // <align> must be a power of two.
  virtual Span* NewAligned(Length n, Length align,
                           SpanAllocInfo span_alloc_info)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) = 0;

  // Delete the span "[p, p+n-1]".
  // REQUIRES: span was returned by earlier call to New() and
  //           has not yet been deleted.
  //
  // TODO(b/175334169): Prefer the AllocationState-ful API.
#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
  virtual void Delete(Span* span, SpanAllocInfo span_alloc_info)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;
#endif  // TCMALLOC_INTERNAL_LEGACY_LOCKING

  struct AllocationState {
    Range r;
    bool donated;

    operator bool() const { return ABSL_PREDICT_TRUE(r.p != PageId{0}); }
  };

  virtual void Delete(AllocationState s, SpanAllocInfo span_alloc_info)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  virtual BackingStats stats() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  virtual void GetSmallSpanStats(SmallSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  virtual void GetLargeSpanStats(LargeSpanStats* result)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  // Try to release at least num_pages for reuse by the OS.  Returns
  // the actual number of pages released, which may be less than
  // num_pages if there weren't enough pages to release. The result
  // may also be larger than num_pages since page_heap might decide to
  // release one large range instead of fragmenting it into two
  // smaller released and unreleased ranges.
  virtual Length ReleaseAtLeastNPages(Length num_pages,
                                      PageReleaseReason reason)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  // Returns the number of pages that have been released from this page
  // allocator.
  virtual PageReleaseStats GetReleaseStats() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) = 0;

  virtual void TreatHugepageTrackers(bool enable_collapse,
                                     bool enable_release_free_swapped)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) = 0;

  // Prints stats about the page heap to *out.
  virtual void Print(Printer& out, PageFlagsBase& pageflags)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) = 0;

  // Prints stats about the page heap in pbtxt format.
  virtual void PrintInPbtxt(PbtxtRegion& region, PageFlagsBase& pageflags)
      ABSL_LOCKS_EXCLUDED(pageheap_lock) = 0;

  const PageAllocInfo& info() const
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(pageheap_lock) {
    return info_;
  }

 protected:
  PageAllocInfo info_ ABSL_GUARDED_BY(pageheap_lock);

  const MemoryTag tag_;  // The type of tagged memory this heap manages
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END

#endif  // TCMALLOC_PAGE_ALLOCATOR_INTERFACE_H_
