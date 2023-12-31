/* Copyright 2016 The TensorFlow Authors All Rights Reserved.

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

#include "tensorflow/core/profiler/internal/tfprof_op.h"

#include <stdio.h>

#include <algorithm>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "tensorflow/core/platform/regexp.h"
#include "tensorflow/core/profiler/internal/tfprof_constants.h"
#include "tensorflow/core/profiler/internal/tfprof_tensor.h"

namespace tensorflow {
namespace tfprof {
namespace {
string FormatToalExecTime(const ShowMultiNode* node,
                          const ShowMultiNode* root) {
  double accu_pct = 0.0;
  double pct = 0.0;
  if (node->proto().total_exec_micros() > 0) {
    accu_pct = 100.0 * node->proto().total_exec_micros() /
               root->proto().total_exec_micros();
    pct =
        100.0 * node->proto().exec_micros() / root->proto().total_exec_micros();
  }

  return absl::StrFormat(
      "%30s",
      absl::StrFormat("%s (%.2f%%, %.2f%%)",
                      FormatTime(node->proto().exec_micros()), accu_pct, pct));
}
string FormatCPUExecTime(const ShowMultiNode* node, const ShowMultiNode* root) {
  double accu_pct = 0.0;
  double pct = 0.0;
  if (node->proto().total_cpu_exec_micros() > 0) {
    accu_pct = 100.0 * node->proto().total_cpu_exec_micros() /
               root->proto().total_cpu_exec_micros();
    pct = 100.0 * node->proto().cpu_exec_micros() /
          root->proto().total_cpu_exec_micros();
  }

  return absl::StrFormat(
      "%30s", absl::StrFormat("%s (%.2f%%, %.2f%%)",
                              FormatTime(node->proto().cpu_exec_micros()),
                              accu_pct, pct));
}
string FormatAcceleratorExecTime(const ShowMultiNode* node,
                                 const ShowMultiNode* root) {
  double accu_pct = 0.0;
  double pct = 0.0;
  if (node->proto().total_accelerator_exec_micros() > 0) {
    accu_pct = 100.0 * node->proto().total_accelerator_exec_micros() /
               root->proto().total_accelerator_exec_micros();
    pct = 100.0 * node->proto().accelerator_exec_micros() /
          root->proto().total_accelerator_exec_micros();
  }

  return absl::StrFormat(
      "%30s",
      absl::StrFormat("%s (%.2f%%, %.2f%%)",
                      FormatTime(node->proto().accelerator_exec_micros()),
                      accu_pct, pct));
}
}  // namespace

void TFOp::AddNode(TFGraphNode* node) {
  const string& op = node->op();
  if (tfcnodes_map_.find(op) == tfcnodes_map_.end()) {
    tfcnodes_map_[op] = std::make_unique<TFMultiGraphNode>(op);
  }
  TFMultiGraphNode* tfcnode = tfcnodes_map_[op].get();
  tfcnode->AddGraphNode(node);
}

void TFOp::Build() {
  for (auto& tn : tfcnodes_map_) {
    cnodes_map_[tn.first] = std::make_unique<OpNode>(tn.second.get());
  }

  tfcnodes_map_[kTFProfRoot] = std::make_unique<TFMultiGraphNode>(kTFProfRoot);
  root_ = std::make_unique<OpNode>(tfcnodes_map_[kTFProfRoot].get());
}

const ShowMultiNode* TFOp::ShowInternal(const Options& opts,
                                        Timeline* timeline) {
  root_->ResetTotalStats();
  if (opts.output_type == kOutput[3]) {
    absl::FPrintF(stderr, "Only 'code' view supports pprof output now.\n");
    return root_.get();
  }
  if (opts.output_type == kOutput[1] || opts.output_type == kOutput[2]) {
    root_->formatted_str = FormatNode(root_.get(), root_.get(), opts);
  }
  if (timeline) {
    absl::FPrintF(stderr,
                  "op view doesn't support timeline yet. "
                  "Consider graph/scope/code view.\n");
    return root_.get();
  }
  if (cnodes_map_.empty()) {
    return root_.get();
  }

  std::vector<OpNode*> nodes;
  for (auto& n : cnodes_map_) {
    n.second->account = ReAccount(n.second.get(), opts);
    n.second->ResetTotalStats();
    n.second->AddSelfToTotalStats();
    nodes.push_back(n.second.get());
  }
  nodes = SortNodes(nodes, opts);
  // pre keeps track of previous visited node.
  OpNode* pre = nullptr;
  std::vector<OpNode*> account_nodes;
  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
    if ((*it)->account) {
      if (pre) (*it)->AggregateTotalStats(pre);
      account_nodes.push_back(*it);
      pre = *it;
    }
  }
  std::reverse(std::begin(account_nodes), std::end(account_nodes));
  if (pre) {
    root_->AggregateTotalStats(pre);
  }

  // Perform the display and optionally redo accounting.
  int64_t depth = 0;
  std::vector<OpNode*> show_nodes;
  int64_t start = SearchRoot(account_nodes, opts.start_name_regexes);
  for (int64_t i = start, end = account_nodes.size(); i < end; ++i, ++depth) {
    OpNode* n = account_nodes[i];
    if (ShouldTrim(n, opts.trim_name_regexes) || depth > opts.max_depth) {
      break;
    }
    n->show = ShouldShow(n, opts, depth);
    if (n->show) show_nodes.push_back(n);
  }

  pre = nullptr;
  for (auto it = show_nodes.rbegin(); it != show_nodes.rend(); ++it) {
    if (opts.account_displayed_op_only) {
      (*it)->ResetTotalStats();
      (*it)->AddSelfToTotalStats();
      if (pre) (*it)->AggregateTotalStats(pre);
    }
    pre = *it;
  }
  if (opts.account_displayed_op_only) {
    root_->ResetTotalStats();
    if (pre) {
      root_->AggregateTotalStats(pre);
    }
  }
  if (opts.output_type == kOutput[1] || opts.output_type == kOutput[2]) {
    string display_str = FormatLegend(opts);
    for (OpNode* node : show_nodes) {
      display_str += FormatNode(node, root_.get(), opts);
    }
    // In op view, we don't show root (total). But it will still in proto.
    // TODO(xpan): Is it the right choice?
    root_->formatted_str = display_str;
  }
  // Populate the children field.
  auto* pre_pb = root_->mutable_proto();
  for (auto& show_node : show_nodes) {
    pre_pb->clear_children();
    pre_pb->add_children()->Swap(show_node->mutable_proto());
    pre_pb = pre_pb->mutable_children(0);
  }
  return root_.get();
}

int64_t TFOp::SearchRoot(const std::vector<OpNode*> nodes,
                         const std::vector<string>& regexes) {
  if (regexes.empty() || (regexes.size() == 1 && regexes[0] == ".*")) {
    return 0;
  }
  int64_t i = 0;
  const int64_t nodes_size = nodes.size();
  for (; i < nodes_size; ++i) {
    for (const string& regex : regexes) {
      if (RE2::FullMatch(nodes[i]->name(), regex)) {
        return i;
      }
    }
  }
  return i;
}

string TFOp::FormatMemoryNode(int64_t node_total_bytes,
                              int64_t root_total_bytes,
                              int64_t node_bytes) const {
  double accu_pct = 0.0;
  double pct = 0.0;
  if (node_bytes > 0) {
    accu_pct = 100.0 * node_total_bytes / root_total_bytes;
    pct = 100.0 * node_bytes / root_total_bytes;
  }
  return absl::StrFormat(
      "%30s", absl::StrFormat("%s (%.2f%%, %.2f%%)", FormatMemory(node_bytes),
                              accu_pct, pct));
}

string TFOp::FormatNode(OpNode* node, OpNode* root, const Options& opts) const {
  std::vector<string> attrs;

  if (opts.select.find(kShown[0]) != opts.select.end()) {
    attrs.push_back(FormatMemoryNode(node->proto().total_requested_bytes(),
                                     root->proto().total_requested_bytes(),
                                     node->proto().requested_bytes()));
  }

  if (opts.select.find(kShown[11]) != opts.select.end()) {
    attrs.push_back(FormatMemoryNode(node->proto().total_peak_bytes(),
                                     root->proto().total_peak_bytes(),
                                     node->proto().peak_bytes()));
  }

  if (opts.select.find(kShown[12]) != opts.select.end()) {
    attrs.push_back(FormatMemoryNode(node->proto().total_residual_bytes(),
                                     root->proto().total_residual_bytes(),
                                     node->proto().residual_bytes()));
  }
  if (opts.select.find(kShown[13]) != opts.select.end()) {
    attrs.push_back(FormatMemoryNode(node->proto().total_output_bytes(),
                                     root->proto().total_output_bytes(),
                                     node->proto().output_bytes()));
  }

  if (opts.select.find(kShown[1]) != opts.select.end()) {
    attrs.push_back(FormatToalExecTime(node, root));
    attrs.push_back(FormatAcceleratorExecTime(node, root));
    attrs.push_back(FormatCPUExecTime(node, root));
  }
  if (opts.select.find(kShown[9]) != opts.select.end() &&
      opts.select.find(kShown[1]) == opts.select.end()) {
    attrs.push_back(FormatAcceleratorExecTime(node, root));
  }
  if (opts.select.find(kShown[10]) != opts.select.end() &&
      opts.select.find(kShown[1]) == opts.select.end()) {
    attrs.push_back(FormatCPUExecTime(node, root));
  }
  if (opts.select.find(kShown[2]) != opts.select.end()) {
    double accu_pct = 0.0;
    double pct = 0.0;
    if (node->proto().total_parameters() > 0) {
      accu_pct = 100.0 * node->proto().total_parameters() /
                 root->proto().total_parameters();
      pct =
          100.0 * node->proto().parameters() / root->proto().total_parameters();
    }
    attrs.push_back(absl::StrFormat(
        "%30s", absl::StrFormat("%s params (%.2f%%, %.2f%%)",
                                FormatNumber(node->proto().parameters()),
                                accu_pct, pct)));
  }

  if (opts.select.find(kShown[3]) != opts.select.end()) {
    double accu_pct = 0.0;
    double pct = 0.0;
    if (node->proto().total_float_ops() > 0) {
      accu_pct = 100.0 * node->proto().total_float_ops() /
                 root->proto().total_float_ops();
      pct = 100.0 * node->proto().float_ops() / root->proto().total_float_ops();
    }

    attrs.push_back(absl::StrFormat(
        "%30s", absl::StrFormat("%s float_ops (%.2f%%, %.2f%%)",
                                FormatNumber(node->proto().float_ops()),
                                accu_pct, pct)));
  }

  if (opts.select.find(kShown[5]) != opts.select.end()) {
    attrs.push_back(absl::StrJoin(node->node->devices(), "|"));
  }

  if (opts.select.find(kShown[6]) != opts.select.end()) {
    std::set<string> op_types = node->node->op_types();
    attrs.push_back(absl::StrJoin(op_types, "|"));
  }

  if (opts.select.find(kShown[7]) != opts.select.end()) {
    int64_t total_runs = 0;
    for (const auto& gnode : node->proto().graph_nodes()) {
      total_runs += gnode.run_count();
    }
    attrs.push_back(absl::StrFormat(
        "%10s", absl::StrFormat("%d|%d", total_runs,
                                node->proto().graph_nodes_size())));
  }

  string node_str =
      absl::StrFormat("%-25s%s\n", node->name(), absl::StrJoin(attrs, ", "));

  if (opts.select.find(kShown[8]) != opts.select.end()) {
    string input_shape_str = FormatInputShapes(node->proto());
    if (!input_shape_str.empty()) {
      node_str = absl::StrFormat("%s\n%s\n\n", node_str, input_shape_str);
    }
  }
  return node_str;
}
}  // namespace tfprof
}  // namespace tensorflow
