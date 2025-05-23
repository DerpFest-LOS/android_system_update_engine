//
// Copyright (C) 2012 The Android Open Source Project
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
//

#include "update_engine/payload_generator/delta_diff_generator.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/logging.h>
#include <base/threading/simple_thread.h>

#include "update_engine/common/utils.h"
#include "update_engine/payload_consumer/file_descriptor.h"
#include "update_engine/payload_generator/ab_generator.h"
#include "update_engine/payload_generator/annotated_operation.h"
#include "update_engine/payload_generator/blob_file_writer.h"
#include "update_engine/payload_generator/cow_size_estimator.h"
#include "update_engine/payload_generator/delta_diff_utils.h"
#include "update_engine/payload_generator/full_update_generator.h"
#include "update_engine/payload_generator/merge_sequence_generator.h"
#include "update_engine/payload_generator/payload_file.h"
#include "update_engine/update_metadata.pb.h"

using std::string;
using std::unique_ptr;
using std::vector;

namespace chromeos_update_engine {

// bytes
const size_t kRootFSPartitionSize = static_cast<size_t>(2) * 1024 * 1024 * 1024;

class PartitionProcessor : public base::DelegateSimpleThread::Delegate {
  bool IsDynamicPartition(const std::string& partition_name) {
    for (const auto& group :
         config_.target.dynamic_partition_metadata->groups()) {
      const auto& names = group.partition_names();
      if (std::find(names.begin(), names.end(), partition_name) !=
          names.end()) {
        return true;
      }
    }
    return false;
  }

 public:
  explicit PartitionProcessor(
      const PayloadGenerationConfig& config,
      const PartitionConfig& old_part,
      const PartitionConfig& new_part,
      BlobFileWriter* file_writer,
      std::vector<AnnotatedOperation>* aops,
      std::vector<CowMergeOperation>* cow_merge_sequence,
      android::snapshot::CowSizeInfo* cow_info,
      std::unique_ptr<chromeos_update_engine::OperationsGenerator> strategy)
      : config_(config),
        old_part_(old_part),
        new_part_(new_part),
        file_writer_(file_writer),
        aops_(aops),
        cow_merge_sequence_(cow_merge_sequence),
        cow_info_(cow_info),
        strategy_(std::move(strategy)) {}
  PartitionProcessor(PartitionProcessor&&) noexcept = default;

  void Run() override {
    LOG(INFO) << "Started an async task to process partition "
              << new_part_.name;
    bool success = strategy_->GenerateOperations(
        config_, old_part_, new_part_, file_writer_, aops_);
    if (!success) {
      // ABORT the entire process, so that developer can look
      // at recent logs and diagnose what happened
      LOG(FATAL) << "GenerateOperations(" << old_part_.name << ", "
                 << new_part_.name << ") failed";
    }

    bool snapshot_enabled =
        config_.target.dynamic_partition_metadata &&
        config_.target.dynamic_partition_metadata->snapshot_enabled();
    if (!snapshot_enabled || !IsDynamicPartition(new_part_.name)) {
      return;
    }
    // Skip cow size estimation if VABC isn't enabled
    if (!config_.target.dynamic_partition_metadata->vabc_enabled()) {
      return;
    }
    if (!old_part_.path.empty()) {
      auto generator = MergeSequenceGenerator::Create(*aops_, new_part_.name);
      if (!generator || !generator->Generate(cow_merge_sequence_)) {
        LOG(FATAL) << "Failed to generate merge sequence";
      }
    }

    LOG(INFO) << "Estimating COW size for partition: " << new_part_.name;
    // Need the contents of source/target image bytes when doing
    // dry run.
    auto target_fd = std::make_unique<EintrSafeFileDescriptor>();
    target_fd->Open(new_part_.path.c_str(), O_RDONLY);

    google::protobuf::RepeatedPtrField<InstallOperation> operations;

    for (const AnnotatedOperation& aop : *aops_) {
      *operations.Add() = aop.op;
    }

    FileDescriptorPtr source_fd = std::make_shared<EintrSafeFileDescriptor>();
    source_fd->Open(old_part_.path.c_str(), O_RDONLY);

    *cow_info_ = EstimateCowSizeInfo(
        std::move(source_fd),
        std::move(target_fd),
        std::move(operations),
        {cow_merge_sequence_->begin(), cow_merge_sequence_->end()},
        config_.block_size,
        config_.target.dynamic_partition_metadata->vabc_compression_param(),
        new_part_.size,
        old_part_.size,
        config_.enable_vabc_xor,
        config_.target.dynamic_partition_metadata->cow_version(),
        config_.target.dynamic_partition_metadata->compression_factor());

    // add a 1% overhead to our estimation
    cow_info_->cow_size = cow_info_->cow_size * 1.01;
    if (config_.target.dynamic_partition_metadata->cow_version() >= 3) {
      cow_info_->op_count_max = std::max(int(cow_info_->op_count_max), 25);
    }
    // ops buffer size == 0 for v2 version of cow format
    LOG(INFO) << "Estimated COW size for partition: " << new_part_.name << " "
              << cow_info_->cow_size
              << " ops buffer size: " << cow_info_->op_count_max;
  }

 private:
  const PayloadGenerationConfig& config_;
  const PartitionConfig& old_part_;
  const PartitionConfig& new_part_;
  BlobFileWriter* file_writer_;
  std::vector<AnnotatedOperation>* aops_;
  std::vector<CowMergeOperation>* cow_merge_sequence_;
  android::snapshot::CowSizeInfo* cow_info_;
  std::unique_ptr<chromeos_update_engine::OperationsGenerator> strategy_;
  DISALLOW_COPY_AND_ASSIGN(PartitionProcessor);
};

bool GenerateUpdatePayloadFile(const PayloadGenerationConfig& config,
                               const string& output_path,
                               const string& private_key_path,
                               uint64_t* metadata_size) {
  if (!config.version.Validate()) {
    LOG(ERROR) << "Unsupported major.minor version: " << config.version.major
               << "." << config.version.minor;
    return false;
  }

  // Create empty payload file object.
  PayloadFile payload;
  TEST_AND_RETURN_FALSE(payload.Init(config));

  ScopedTempFile data_file("CrAU_temp_data.XXXXXX", true);
  {
    off_t data_file_size = 0;
    BlobFileWriter blob_file(data_file.fd(), &data_file_size);
    if (config.is_delta) {
      TEST_EQ(config.source.partitions.size(), config.target.partitions.size());
    }
    PartitionConfig empty_part("");
    std::vector<std::vector<AnnotatedOperation>> all_aops;
    all_aops.resize(config.target.partitions.size());

    std::vector<std::vector<CowMergeOperation>> all_merge_sequences;
    all_merge_sequences.resize(config.target.partitions.size());

    std::vector<android::snapshot::CowSizeInfo> all_cow_info(
        config.target.partitions.size());

    std::vector<PartitionProcessor> partition_tasks{};
    auto thread_count = std::min<size_t>(diff_utils::GetMaxThreads(),
                                         config.target.partitions.size());
    if (thread_count > config.max_threads && config.max_threads > 0) {
      thread_count = config.max_threads;
    }
    if (thread_count < 1) {
      thread_count = 1;
    }
    base::DelegateSimpleThreadPool thread_pool{"partition-thread-pool",
                                               static_cast<int>(thread_count)};
    LOG(INFO) << "Using " << thread_count << " threads to process "
              << config.target.partitions.size() << " partitions";
    for (size_t i = 0; i < config.target.partitions.size(); i++) {
      const PartitionConfig& old_part =
          config.is_delta ? config.source.partitions[i] : empty_part;
      const PartitionConfig& new_part = config.target.partitions[i];
      LOG(INFO) << "Partition name: " << new_part.name;
      LOG(INFO) << "Partition size: " << new_part.size;
      LOG(INFO) << "Block count: " << new_part.size / config.block_size;

      // Select payload generation strategy based on the config.
      unique_ptr<OperationsGenerator> strategy;
      if (!old_part.path.empty()) {
        // Delta update.
        LOG(INFO) << "Using generator ABGenerator() for partition "
                  << new_part.name;
        strategy.reset(new ABGenerator());
      } else {
        LOG(INFO) << "Using generator FullUpdateGenerator() for partition "
                  << new_part.name;
        strategy.reset(new FullUpdateGenerator());
      }

      // Generate the operations using the strategy we selected above.
      partition_tasks.push_back(PartitionProcessor(config,
                                                   old_part,
                                                   new_part,
                                                   &blob_file,
                                                   &all_aops[i],
                                                   &all_merge_sequences[i],
                                                   &all_cow_info[i],
                                                   std::move(strategy)));
    }
    thread_pool.Start();
    for (auto& processor : partition_tasks) {
      thread_pool.AddWork(&processor);
    }
    thread_pool.JoinAll();

    for (size_t i = 0; i < config.target.partitions.size(); i++) {
      const PartitionConfig& old_part =
          config.is_delta ? config.source.partitions[i] : empty_part;
      const PartitionConfig& new_part = config.target.partitions[i];
      TEST_AND_RETURN_FALSE(
          payload.AddPartition(old_part,
                               new_part,
                               std::move(all_aops[i]),
                               std::move(all_merge_sequences[i]),
                               all_cow_info[i]));
    }
  }
  data_file.CloseFd();

  LOG(INFO) << "Writing payload file...";
  // Write payload file to disk.
  TEST_AND_RETURN_FALSE(payload.WritePayload(
      output_path, data_file.path(), private_key_path, metadata_size));

  LOG(INFO) << "All done. Successfully created delta file with "
            << "metadata size = " << *metadata_size;
  return true;
}

};  // namespace chromeos_update_engine
