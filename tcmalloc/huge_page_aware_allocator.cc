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

#include "tcmalloc/huge_page_aware_allocator.h"

#include <cstddef>
#include <optional>

#include "absl/base/attributes.h"
#include "absl/strings/string_view.h"
#include "tcmalloc/arena.h"
#include "tcmalloc/error_reporting.h"
#include "tcmalloc/huge_pages.h"
#include "tcmalloc/huge_region.h"
#include "tcmalloc/internal/config.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/logging.h"
#include "tcmalloc/internal/memory_tag.h"
#include "tcmalloc/pagemap.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"
#include "tcmalloc/static_vars.h"
#include "tcmalloc/system-alloc.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

ABSL_ATTRIBUTE_WEAK int default_subrelease();

namespace huge_page_allocator_internal {

bool decide_subrelease() {
  const char* e = thread_safe_getenv("TCMALLOC_HPAA_CONTROL");
  if (e) {
    switch (e[0]) {
      case '0':
        // If we're forcing HPAA on, we want to converge towards our default
        // of subrelease on, rather than off (where it is moot without HPAA).
        break;
      case '1':
        return false;
      case '2':
        return true;
      default:
        TC_BUG("bad env var '%s'", e);
    }
  }

  if (default_subrelease != nullptr) {
    const int decision = default_subrelease();
    if (decision != 0) {
      return decision > 0;
    }
  }

  return true;
}

extern "C" ABSL_ATTRIBUTE_WEAK bool
default_want_disable_huge_region_more_often();

bool use_huge_region_more_often() {
  // Disable huge regions more often feature if built against an opt-out.
  if (default_want_disable_huge_region_more_often != nullptr) {
    return false;
  }

  // TODO(b/296281171): Remove this opt-out.
  const char* e =
      thread_safe_getenv("TCMALLOC_USE_HUGE_REGION_MORE_OFTEN_DISABLE");
  if (e) {
    switch (e[0]) {
      case '0':
        return true;
      case '1':
        return false;
      default:
        TC_BUG("bad env var '%s'", e);
    }
  }

  return true;
}

HugeRegionUsageOption huge_region_option() {
  // By default, we use slack to determine when to use HugeRegion. When slack is
  // greater than 64MB (to ignore small binaries), and greater than the number
  // of small allocations, we allocate large allocations from HugeRegion.
  //
  // When huge-region-more-often feature is enabled, we use number of abandoned
  // pages in addition to slack to make a decision. If the size of abandoned
  // pages plus slack exceeds 64MB (to ignore small binaries), we use HugeRegion
  // for large allocations. This results in using HugeRegions for all the large
  // allocations once the size exceeds 64MB.
  return use_huge_region_more_often()
             ? HugeRegionUsageOption::kUseForAllLargeAllocs
             : HugeRegionUsageOption::kDefault;
}

Arena& StaticForwarder::arena() { return tc_globals.arena(); }

void* StaticForwarder::GetHugepage(HugePage p) {
  return tc_globals.pagemap().GetHugepage(p.first_page());
}

bool StaticForwarder::Ensure(Range r) { return tc_globals.pagemap().Ensure(r); }

void StaticForwarder::Set(PageId page, Span* span) {
  tc_globals.pagemap().Set(page, span);
}

void StaticForwarder::SetHugepage(HugePage p, void* pt) {
  tc_globals.pagemap().SetHugepage(p.first_page(), pt);
}

void StaticForwarder::ShrinkToUsageLimit(Length n) {
  tc_globals.page_allocator().ShrinkToUsageLimit(n);
}

Span* StaticForwarder::NewSpan(Range r) {
  // TODO(b/134687001):  Delete this when span_allocator moves.
  return Span::New(r);
}

void StaticForwarder::DeleteSpan(Span* span) { Span::Delete(span); }

AddressRange StaticForwarder::AllocatePages(size_t bytes, size_t align,
                                            MemoryTag tag) {
  return tc_globals.system_allocator().Allocate(bytes, align, tag);
}

void StaticForwarder::Back(Range r) {
  tc_globals.system_allocator().Back(r.start_addr(), r.in_bytes());
}

bool StaticForwarder::ReleasePages(Range r) {
  return tc_globals.system_allocator().Release(r.start_addr(), r.in_bytes());
}

void StaticForwarder::ReportDoubleFree(void* ptr) {
  ::tcmalloc::tcmalloc_internal::ReportDoubleFree(tc_globals, ptr);
}

bool StaticForwarder::CollapsePages(Range r) {
  return tc_globals.system_allocator().Collapse(r.start_addr(), r.in_bytes());
}

void StaticForwarder::SetAnonVmaName(Range r,
                                     std::optional<absl::string_view> name) {
  tc_globals.system_allocator().SetAnonVmaName(r.start_addr(), r.in_bytes(),
                                               name);
}

}  // namespace huge_page_allocator_internal

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
