// Copyright 2019 The MediaPipe Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>

// TODO: Move protos in another CL after the C++ code migration.
#include "absl/strings/substitute.h"
#include "absl/synchronization/mutex.h"
#include "mediapipe/framework/input_stream_handler.h"
#include "mediapipe/framework/mediapipe_options.pb.h"
#include "mediapipe/framework/packet_set.h"
#include "mediapipe/framework/stream_handler/sync_set_input_stream_handler.pb.h"
#include "mediapipe/framework/timestamp.h"
#include "mediapipe/framework/tool/tag_map.h"

namespace mediapipe {

// An input stream handler which separates the inputs into sets which
// are each independently synchronized.  For example, if 5 inputs are
// present, then the first three can be grouped (and will be synchronized
// as if they were in a calculator with only those three streams) and the
// remaining 2 streams can be independently grouped.  The calculator will
// always be called with all the available packets from a single sync set
// (never more than one).  The input timestamps seen by the calculator
// will be ordered sequentially for each sync set but may jump around
// between sync sets.
class SyncSetInputStreamHandler : public InputStreamHandler {
 public:
  SyncSetInputStreamHandler() = delete;
  SyncSetInputStreamHandler(std::shared_ptr<tool::TagMap> tag_map,
                            CalculatorContextManager* cc_manager,
                            const MediaPipeOptions& extendable_options,
                            bool calculator_run_in_parallel);

  void PrepareForRun(
      std::function<void()> headers_ready_callback,
      std::function<void()> notification_callback,
      std::function<void(CalculatorContext*)> schedule_callback,
      std::function<void(::mediapipe::Status)> error_callback) override;

 protected:
  // In SyncSetInputStreamHandler, a node is "ready" if any
  // of its sync sets are ready in the traditional sense (See
  // DefaultInputStreamHandler).
  NodeReadiness GetNodeReadiness(Timestamp* min_stream_timestamp) override;

  // Only invoked when associated GetNodeReadiness() returned kReadyForProcess.
  void FillInputSet(Timestamp input_timestamp,
                    InputStreamShardSet* input_set) override;

 private:
  absl::Mutex mutex_;
  // The ids of each set of inputs.
  std::vector<std::vector<CollectionItemId>> sync_sets_ GUARDED_BY(mutex_);
  // The index of the ready sync set.  A value of -1 indicates that no
  // sync sets are ready.
  int ready_sync_set_index_ GUARDED_BY(mutex_) = -1;
  // The timestamp at which the sync set is ready.  If no sync set is
  // ready then this variable should be Timestamp::Done() .
  Timestamp ready_timestamp_ GUARDED_BY(mutex_);
};

REGISTER_INPUT_STREAM_HANDLER(SyncSetInputStreamHandler);

SyncSetInputStreamHandler::SyncSetInputStreamHandler(
    std::shared_ptr<tool::TagMap> tag_map, CalculatorContextManager* cc_manager,
    const MediaPipeOptions& extendable_options, bool calculator_run_in_parallel)
    : InputStreamHandler(std::move(tag_map), cc_manager, extendable_options,
                         calculator_run_in_parallel) {}

void SyncSetInputStreamHandler::PrepareForRun(
    std::function<void()> headers_ready_callback,
    std::function<void()> notification_callback,
    std::function<void(CalculatorContext*)> schedule_callback,
    std::function<void(::mediapipe::Status)> error_callback) {
  const auto& handler_options =
      options_.GetExtension(SyncSetInputStreamHandlerOptions::ext);
  {
    absl::MutexLock lock(&mutex_);
    sync_sets_.clear();
    std::set<CollectionItemId> used_ids;
    for (const auto& sync_set : handler_options.sync_set()) {
      sync_sets_.emplace_back();
      CHECK_LT(0, sync_set.tag_index_size());
      for (const auto& tag_index : sync_set.tag_index()) {
        std::string tag;
        int index;
        MEDIAPIPE_CHECK_OK(tool::ParseTagIndex(tag_index, &tag, &index));
        CollectionItemId id = input_stream_managers_.GetId(tag, index);
        CHECK(id.IsValid()) << "stream \"" << tag_index << "\" is not found.";
        CHECK(!::mediapipe::ContainsKey(used_ids, id))
            << "stream \"" << tag_index << "\" is in more than one sync set.";
        used_ids.insert(id);
        sync_sets_.back().push_back(id);
      }
    }
    std::vector<CollectionItemId> remaining_ids;
    for (CollectionItemId id = input_stream_managers_.BeginId();
         id < input_stream_managers_.EndId(); ++id) {
      if (!::mediapipe::ContainsKey(used_ids, id)) {
        remaining_ids.push_back(id);
      }
    }
    if (!remaining_ids.empty()) {
      sync_sets_.push_back(std::move(remaining_ids));
    }
    ready_sync_set_index_ = -1;
    ready_timestamp_ = Timestamp::Done();
  }

  InputStreamHandler::PrepareForRun(
      std::move(headers_ready_callback), std::move(notification_callback),
      std::move(schedule_callback), std::move(error_callback));
}

NodeReadiness SyncSetInputStreamHandler::GetNodeReadiness(
    Timestamp* min_stream_timestamp) {
  DCHECK(min_stream_timestamp);
  absl::MutexLock lock(&mutex_);
  if (ready_sync_set_index_ >= 0) {
    *min_stream_timestamp = ready_timestamp_;
    return NodeReadiness::kReadyForProcess;
  }
  for (int sync_set_index = 0; sync_set_index < sync_sets_.size();
       ++sync_set_index) {
    const std::vector<CollectionItemId>& sync_set = sync_sets_[sync_set_index];
    *min_stream_timestamp = Timestamp::Done();
    Timestamp min_bound = Timestamp::Done();
    for (CollectionItemId id : sync_set) {
      const auto& stream = input_stream_managers_.Get(id);
      bool empty;
      Timestamp stream_timestamp = stream->MinTimestampOrBound(&empty);
      if (empty) {
        min_bound = std::min(min_bound, stream_timestamp);
      }
      *min_stream_timestamp = std::min(*min_stream_timestamp, stream_timestamp);
    }

    if (*min_stream_timestamp == Timestamp::Done()) {
      // This sync set is done, remove it.  Note that this invalidates
      // sync set indexes higher than sync_set_index.  However, we are
      // guaranteed that we were not ready before entering the outer
      // loop, so even if we are ready now, ready_sync_set_index_ must
      // be less than the current value of sync_set_index.
      sync_sets_.erase(sync_sets_.begin() + sync_set_index);
      --sync_set_index;
      continue;
    }

    if (min_bound > *min_stream_timestamp) {
      if (*min_stream_timestamp < ready_timestamp_) {
        // Store the timestamp and corresponding sync set index for the
        // sync set with the earliest arrival timestamp.
        ready_timestamp_ = *min_stream_timestamp;
        ready_sync_set_index_ = sync_set_index;
      }
    } else {
      CHECK_EQ(min_bound, *min_stream_timestamp);
    }
  }
  if (ready_sync_set_index_ >= 0) {
    *min_stream_timestamp = ready_timestamp_;
    return NodeReadiness::kReadyForProcess;
  }
  if (sync_sets_.empty()) {
    *min_stream_timestamp = Timestamp::Done();
    return NodeReadiness::kReadyForClose;
  }
  // TODO The value of *min_stream_timestamp is undefined in this case.
  return NodeReadiness::kNotReady;
}

void SyncSetInputStreamHandler::FillInputSet(Timestamp input_timestamp,
                                             InputStreamShardSet* input_set) {
  // Assume that all current packets are already cleared.
  CHECK(input_timestamp.IsAllowedInStream());
  CHECK(input_set);
  absl::MutexLock lock(&mutex_);
  CHECK_LE(0, ready_sync_set_index_);
  CHECK_EQ(input_timestamp, ready_timestamp_);
  // Set the input streams for the ready sync set.
  for (CollectionItemId id : sync_sets_[ready_sync_set_index_]) {
    const auto& stream = input_stream_managers_.Get(id);
    int num_packets_dropped = 0;
    bool stream_is_done = false;
    Packet current_packet = stream->PopPacketAtTimestamp(
        input_timestamp, &num_packets_dropped, &stream_is_done);
    CHECK_EQ(num_packets_dropped, 0)
        << absl::Substitute("Dropped $0 packet(s) on input stream \"$1\".",
                            num_packets_dropped, stream->Name());
    AddPacketToShard(&input_set->Get(id), std::move(current_packet),
                     stream_is_done);
  }
  ready_sync_set_index_ = -1;
  ready_timestamp_ = Timestamp::Done();
}

}  // namespace mediapipe
