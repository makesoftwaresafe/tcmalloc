// Copyright 2021 The TCMalloc Authors
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

#include <stddef.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tcmalloc/internal/profile.pb.h"
#include "gtest/gtest.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/string_view.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/gzip_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "tcmalloc/malloc_extension.h"
#include "tcmalloc/profile_marshaler.h"
#include "tcmalloc/testing/testutil.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

TEST(ProfileTest, HeapProfile) {
#if ABSL_HAVE_ADDRESS_SANITIZER || ABSL_HAVE_MEMORY_SANITIZER || \
    ABSL_HAVE_THREAD_SANITIZER
  GTEST_SKIP() << "Skipping heap profile test under sanitizers.";
#endif

  constexpr int64_t kSamplingInterval = 1024 * 1024;
  ScopedProfileSamplingInterval s(kSamplingInterval);

  // Tweak alloc_size to make it more likely we can distinguish it from others.
  constexpr int kAllocs = 32;
  const size_t alloc_size = 64 * kSamplingInterval + 123;

  auto deleter = [](void* ptr) { ::operator delete(ptr); };
  std::vector<std::unique_ptr<void, decltype(deleter)>> allocs;
  // Sometimes the compiler duplicates the tcmalloc call depending on if the
  // fast path of the first `emplace_back` is taken. We reserve enough space for
  // all insertions so that all `emplace_back` calls go through the fast path
  // and there is only one stack trace for tcmalloc.
  allocs.reserve(3 * kAllocs);
  for (int i = 0; i < kAllocs; i++) {
    allocs.emplace_back(::operator new(alloc_size), deleter);
    allocs.emplace_back(::operator new(alloc_size, std::nothrow), deleter);
    allocs.emplace_back(__size_returning_new(alloc_size).p, deleter);
  }

  auto malloc_deleter = [](void* ptr) { free(ptr); };
  std::vector<std::unique_ptr<void, decltype(malloc_deleter)>> mallocs;
  mallocs.reserve(2 * kAllocs);
  for (int i = 0; i < kAllocs; i++) {
    mallocs.emplace_back(malloc(alloc_size), malloc_deleter);
    mallocs.emplace_back(aligned_alloc(ABSL_CACHELINE_SIZE, alloc_size),
                         malloc_deleter);
  }

  // Grab profile, encode, then decode to look for the allocations.
  Profile profile = MallocExtension::SnapshotCurrent(ProfileType::kHeap);
  absl::StatusOr<std::string> encoded_or = Marshal(profile);
  ASSERT_TRUE(encoded_or.ok());

  const absl::string_view encoded = *encoded_or;

  google::protobuf::io::ArrayInputStream stream(encoded.data(), encoded.size());
  google::protobuf::io::GzipInputStream gzip_stream(&stream);
  google::protobuf::io::CodedInputStream coded(&gzip_stream);

  perftools::profiles::Profile converted;
  ASSERT_TRUE(converted.ParseFromCodedStream(&coded));

  // Look for "request", "size_returning", "allocation_type", "new", "malloc",
  // "aligned_malloc" strings in string table.
  std::optional<int> request_id, size_returning_id, allocation_type_id, new_id,
      malloc_id, aligned_malloc_id;
  for (int i = 0, n = converted.string_table().size(); i < n; ++i) {
    if (converted.string_table(i) == "request") {
      request_id = i;
    } else if (converted.string_table(i) == "size_returning") {
      size_returning_id = i;
    } else if (converted.string_table(i) == "allocation type") {
      allocation_type_id = i;
    } else if (converted.string_table(i) == "new") {
      new_id = i;
    } else if (converted.string_table(i) == "malloc") {
      malloc_id = i;
    } else if (converted.string_table(i) == "aligned malloc") {
      aligned_malloc_id = i;
    }
  }

  EXPECT_TRUE(request_id.has_value());
  EXPECT_TRUE(size_returning_id.has_value());
  EXPECT_TRUE(allocation_type_id.has_value());
  EXPECT_TRUE(new_id.has_value());
  EXPECT_TRUE(malloc_id.has_value());
  EXPECT_TRUE(aligned_malloc_id.has_value());

  size_t count = 0, bytes = 0, samples = 0, size_returning_samples = 0,
         new_samples = 0, malloc_samples = 0, aligned_malloc_samples = 0;
  for (const auto& sample : converted.sample()) {
    count += sample.value(0);
    bytes += sample.value(1);

    // Count the number of times we saw an alloc_size-sized allocation.
    bool alloc_sized = false;
    for (const auto& label : sample.label()) {
      if (label.key() == request_id && label.num() == alloc_size) {
        alloc_sized = true;
        samples++;
      }
    }

    if (alloc_sized) {
      for (const auto& label : sample.label()) {
        if (label.key() == size_returning_id && label.num() > 0) {
          size_returning_samples++;
        }
      }

      // Count new versus malloc'd allocations.
      bool type_seen = false;
      for (const auto& label : sample.label()) {
        if (label.key() == allocation_type_id) {
          EXPECT_FALSE(type_seen);
          type_seen = true;

          if (label.str() == new_id) {
            new_samples++;
          } else if (label.str() == malloc_id) {
            malloc_samples++;
          } else if (label.str() == aligned_malloc_id) {
            aligned_malloc_samples++;
          } else {
            GTEST_FAIL() << "Unexpected string key: "
                         << converted.string_table(label.str()) << " ("
                         << label.str() << ")";
          }
        }
      }
    }
  }

  EXPECT_GT(count, 0);
  EXPECT_GE(bytes, 2 * alloc_size * kAllocs);
  // To minimize the size of profiles, we expect to coalesce similar allocations
  // (same call stack, size, alignment, etc.) during generation of the
  // profile.proto.  Since all of the calls to operator new(alloc_size) are
  // similar in these dimensions, we expect to see only 2 samples, one for
  // ::operator new and one for __size_returning_new.
  EXPECT_EQ(samples, 5);
  EXPECT_EQ(size_returning_samples, 1);
  EXPECT_EQ(new_samples, 3);
  EXPECT_EQ(malloc_samples, 1);
  EXPECT_EQ(aligned_malloc_samples, 1);

  // Dump the profile in case of failures so that it's possible to debug.
  // Since SCOPED_TRACE attaches output to every failure, we use ASSERTs below.
  SCOPED_TRACE(converted.DebugString());

  absl::flat_hash_map<int, const perftools::profiles::Mapping*> mappings;
  mappings.reserve(converted.mapping().size());
  for (const auto& mapping : converted.mapping()) {
    ASSERT_NE(mapping.id(), 0);
    ASSERT_TRUE(mappings.insert({mapping.id(), &mapping}).second);
  }

  absl::flat_hash_map<int, const perftools::profiles::Location*> locations;
  for (const auto& location : converted.location()) {
    ASSERT_NE(location.id(), 0);
    ASSERT_TRUE(locations.insert({location.id(), &location}).second);
  }

  // We can't unwind past optimized libstdc++.so, and as the result have some
  // bogus frames (random numbers), which don't have a mapping.
  absl::flat_hash_set<int> unreliable_locations;
  for (const auto& sample : converted.sample()) {
    bool unreliable = false;
    for (auto loc_id : sample.location_id()) {
      if (unreliable) {
        unreliable_locations.insert(loc_id);
        continue;
      }
      const auto* loc = locations[loc_id];
      ASSERT_NE(loc, nullptr);
      const auto* mapping = mappings[loc->mapping_id()];
      ASSERT_NE(mapping, nullptr);
      ASSERT_LT(mapping->filename(), converted.string_table().size());
      const auto& file = converted.string_table()[mapping->filename()];
      unreliable = absl::StrContains(file, "libstdc++.so");
    }
  }

  // Every reliable location should have a mapping.
  for (const auto& location : converted.location()) {
    if (unreliable_locations.contains(location.id())) {
      continue;
    }
    const int mapping_id = location.mapping_id();
    ASSERT_TRUE(mappings.contains(mapping_id)) << mapping_id;
  }
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
