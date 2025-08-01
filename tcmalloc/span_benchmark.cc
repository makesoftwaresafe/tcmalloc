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

#include <stdlib.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "absl/random/random.h"
#include "absl/types/span.h"
#include "benchmark/benchmark.h"
#include "tcmalloc/common.h"
#include "tcmalloc/internal/allocation_guard.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

constexpr uint64_t kSpanAllocTime = 1234;

class RawSpan {
 public:
  void Init(size_t size_class) {
    size_t size = tc_globals.sizemap().class_to_size(size_class);
    TC_CHECK_GT(size, 0);
    auto npages = Length(tc_globals.sizemap().class_to_pages(size_class));
    size_t objects_per_span = npages.in_bytes() / size;

    void* mem;
    int res = posix_memalign(&mem, kPageSize, npages.in_bytes());
    TC_CHECK_EQ(res, 0);
    span_.emplace(Range(PageIdContaining(mem), npages));
    TC_CHECK_EQ(
        span_->BuildFreelist(size, objects_per_span, {}, kSpanAllocTime), 0);
  }

  ~RawSpan() {
    if (span_.has_value()) {
      free(span_->start_address());
    }
  }

  Span& span() { return *span_; }

 private:
  std::optional<Span> span_;
};

// BM_single_span repeatedly pushes and pops the same
// num_objects_to_move(size_class) objects from the span.
void BM_single_span(benchmark::State& state) {
  const int size_class = state.range(0);

  size_t size = tc_globals.sizemap().class_to_size(size_class);
  if (size == 0) {
    state.SkipWithMessage("Empty size class");
    return;
  }
  uint32_t reciprocal = Span::CalcReciprocal(size);
  size_t batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  RawSpan raw_span;
  raw_span.Init(size_class);
  Span& span = raw_span.span();

  void* batch[kMaxObjectsToMove];

  int64_t processed = 0;
  while (state.KeepRunningBatch(batch_size)) {
    int n = span.FreelistPopBatch(absl::MakeSpan(batch, batch_size), size);
    processed += n;

    for (int j = 0; j < n; j++) {
      (void)span.FreelistPushBatch({&batch[j], 1}, size, reciprocal);
    }
  }

  state.SetItemsProcessed(processed);
}

// BM_single_span_fulldrain alternates between fully draining and filling the
// span.
void BM_single_span_fulldrain(benchmark::State& state) {
  const int size_class = state.range(0);

  size_t size = tc_globals.sizemap().class_to_size(size_class);
  if (size == 0) {
    state.SkipWithMessage("Empty size class");
    return;
  }
  uint32_t reciprocal = Span::CalcReciprocal(size);
  size_t npages = tc_globals.sizemap().class_to_pages(size_class);
  size_t batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  size_t objects_per_span = npages * kPageSize / size;
  RawSpan raw_span;
  raw_span.Init(size_class);
  Span& span = raw_span.span();

  std::vector<void*> objects(objects_per_span, nullptr);
  size_t oindex = 0;

  size_t processed = 0;
  while (state.KeepRunningBatch(objects_per_span)) {
    // Drain span
    while (oindex < objects_per_span) {
      size_t popped = span.FreelistPopBatch(
          absl::MakeSpan(objects).subspan(oindex, batch_size), size);
      oindex += popped;
      processed += popped;
    }

    // Fill span
    while (oindex > 0) {
      void* p = objects[oindex - 1];
      if (!span.FreelistPushBatch({&p, 1}, size, reciprocal)) {
        break;
      }

      oindex--;
    }
  }

  state.SetItemsProcessed(processed);
}

BENCHMARK(BM_single_span)
    ->Arg(1)
    ->Arg(2)
    ->Arg(3)
    ->Arg(4)
    ->Arg(5)
    ->Arg(7)
    ->Arg(10)
    ->Arg(12)
    ->Arg(16)
    ->Arg(20)
    ->Arg(30)
    ->Arg(40)
    ->Arg(80);

BENCHMARK(BM_single_span_fulldrain)
    ->Arg(1)
    ->Arg(2)
    ->Arg(3)
    ->Arg(4)
    ->Arg(5)
    ->Arg(7)
    ->Arg(10)
    ->Arg(12)
    ->Arg(16)
    ->Arg(20)
    ->Arg(30)
    ->Arg(40)
    ->Arg(80);

void BM_NewDelete(benchmark::State& state) {
  constexpr SpanAllocInfo kSpanInfo = {/*objects_per_span=*/7,
                                       AccessDensityPrediction::kSparse};
  for (auto s : state) {
    Span* sp = tc_globals.page_allocator().New(Length(1), kSpanInfo,
                                               MemoryTag::kNormal);

    benchmark::DoNotOptimize(sp);

#ifdef TCMALLOC_INTERNAL_LEGACY_LOCKING
    PageHeapSpinLockHolder l;
    tc_globals.page_allocator().Delete(sp, MemoryTag::kNormal, kSpanInfo);
#else
    PageAllocatorInterface::AllocationState a{
        Range(sp->first_page(), sp->num_pages()),
        sp->donated(),
    };
    Span::Delete(sp);
    PageHeapSpinLockHolder l;
    tc_globals.page_allocator().Delete(a, MemoryTag::kNormal, kSpanInfo);
#endif
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_NewDelete);

void BM_multiple_spans(benchmark::State& state) {
  const int size_class = state.range(0);
  const size_t size = tc_globals.sizemap().class_to_size(size_class);
  if (size == 0) {
    state.SkipWithMessage("Empty size class");
    return;
  }

  // Should be large enough to cause cache misses
  const int num_spans =
      2 * benchmark::CPUInfo::Get().caches.back().size / sizeof(RawSpan);
  std::vector<RawSpan> spans(num_spans);
  uint32_t reciprocal = Span::CalcReciprocal(size);
  TC_CHECK_GT(size, 0);
  size_t batch_size = tc_globals.sizemap().num_objects_to_move(size_class);
  for (int i = 0; i < num_spans; i++) {
    spans[i].Init(size_class);
  }
  absl::BitGen rng;

  void* batch[kMaxObjectsToMove];

  int64_t processed = 0;
  while (state.KeepRunningBatch(batch_size)) {
    int current_span = absl::Uniform(rng, 0, num_spans);
    int n = spans[current_span].span().FreelistPopBatch(
        absl::MakeSpan(batch, batch_size), size);
    processed += n;

    for (int j = 0; j < n; j++) {
      (void)spans[current_span].span().FreelistPushBatch({&batch[j], 1}, size,
                                                         reciprocal);
    }
  }

  state.SetItemsProcessed(processed);
}

BENCHMARK(BM_multiple_spans)
    ->Arg(1)
    ->Arg(2)
    ->Arg(3)
    ->Arg(4)
    ->Arg(5)
    ->Arg(7)
    ->Arg(10)
    ->Arg(12)
    ->Arg(16)
    ->Arg(20)
    ->Arg(30)
    ->Arg(40)
    ->Arg(80);

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
