/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/convert/xplane_to_dcn_collective_stats.h"

#include <memory>
#include <string>
#include <utility>

#include "absl/strings/match.h"
#include "tensorflow/core/platform/statusor.h"
#include "tensorflow/core/profiler/convert/dcn_slack_analysis_combiner.h"
#include "tensorflow/core/profiler/convert/repository.h"
#include "tensorflow/core/profiler/convert/xspace_to_dcn_slack_analysis.h"
#include "tensorflow/core/profiler/protobuf/dcn_slack_analysis.pb.h"
#include "tensorflow/core/profiler/utils/xplane_schema.h"
#include "tensorflow/core/profiler/utils/xplane_utils.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/statusor.h"
#include "tensorflow/tsl/profiler/protobuf/xplane.pb.h"

namespace tensorflow {
namespace profiler {

namespace {

StatusOr<bool> HasDcnCollectiveStats(const SessionSnapshot& session_snapshot) {
  for (int idx = 0; idx < session_snapshot.XSpaceSize(); idx++) {
    std::string hostname = session_snapshot.GetHostname(idx);
    TF_ASSIGN_OR_RETURN(std::unique_ptr<XSpace> xspace,
                        session_snapshot.GetXSpace(idx));

    if (const tensorflow::profiler::XPlane* xplane = FindPlaneWithName(
            *xspace, tensorflow::profiler::kHostThreadsPlaneName);
        xplane != nullptr) {
      for (const auto& [_, metadata] : xplane->event_metadata()) {
        if (absl::StartsWith(metadata.name(), "MegaScale:")) {
          return true;
        }
      }
    }
  }
  return false;
}

StatusOr<bool> GetDcnCollectiveStatsFromMultiXSpaceAndSaveToFile(
    const SessionSnapshot& session_snapshot) {
  TF_ASSIGN_OR_RETURN(bool hasDcnCollectiveStats,
                      HasDcnCollectiveStats(session_snapshot));

  // The profile does not have dcn collective stats.
  if (!hasDcnCollectiveStats) {
    DcnSlackAnalysis dcnSlackAnalysis;
    TF_RETURN_IF_ERROR(WriteBinaryProto(session_snapshot,
                                        StoredDataType::DCN_COLLECTIVE_STATS,
                                        kNoHostIdentifier, dcnSlackAnalysis));
    return false;
  }

  DcnSlackAnalysisCombiner combiner;
  for (int idx = 0; idx < session_snapshot.XSpaceSize(); idx++) {
    std::string hostname = session_snapshot.GetHostname(idx);
    TF_ASSIGN_OR_RETURN(std::unique_ptr<XSpace> xspace,
                        session_snapshot.GetXSpace(idx));

    DcnSlackAnalysis dcnSlackAnalysis =
        ConvertXSpaceToDcnSlackAnalysis(*xspace);

    TF_RETURN_IF_ERROR(WriteBinaryProto(session_snapshot,
                                        StoredDataType::DCN_COLLECTIVE_STATS,
                                        hostname, dcnSlackAnalysis));

    combiner.Combine(dcnSlackAnalysis);
  }

  DcnSlackAnalysis dcnSlackAnalysis = combiner.Finalize();
  TF_RETURN_IF_ERROR(WriteBinaryProto(session_snapshot,
                                      StoredDataType::DCN_COLLECTIVE_STATS,
                                      kAllHostsIdentifier, dcnSlackAnalysis));

  // The profile has dcn collective stats.
  return true;
}

}  // namespace

StatusOr<bool> HasDcnCollectiveStatsInMultiXSpace(
    const SessionSnapshot& session_snapshot) {
  std::pair<bool, std::string> hasCacheFile;
  TF_ASSIGN_OR_RETURN(hasCacheFile, session_snapshot.HasCacheFile(
                                        StoredDataType::DCN_COLLECTIVE_STATS));

  // Cache file not present, check if trace contains dcn collective stats.
  if (!hasCacheFile.first) {
    return HasDcnCollectiveStats(session_snapshot);
  }

  if (hasCacheFile.second.empty()) {
    // If the profiler finds a file NO_HOST.dcn_collective_stats.pb, this means
    // dcn collective stats are not present in the profile.
    return false;
  } else {
    // If the profiler finds a file ALL_HOSTS.dcn_collective_stats.pb, this
    // means dcn collective stats are present in the profile.
    return true;
  }
}

StatusOr<bool> ConvertMultiXSpaceToDcnCollectiveStats(
    const SessionSnapshot& session_snapshot) {
  std::pair<bool, std::string> hasCacheFile;
  TF_ASSIGN_OR_RETURN(hasCacheFile, session_snapshot.HasCacheFile(
                                        StoredDataType::DCN_COLLECTIVE_STATS));

  // Cache file not present, generate dcn collective stats.
  if (!hasCacheFile.first) {
    return GetDcnCollectiveStatsFromMultiXSpaceAndSaveToFile(session_snapshot);
  }

  if (hasCacheFile.second.empty()) {
    // If the profiler finds a file NO_HOST.dcn_collective_stats.pb, this means
    // dcn collective stats are not present in the profile.
    return false;
  } else {
    // If the profiler finds a file ALL_HOSTS.dcn_collective_stats.pb, this
    // means dcn collective stats are present in the profile.
    return true;
  }
}

}  // namespace profiler
}  // namespace tensorflow
