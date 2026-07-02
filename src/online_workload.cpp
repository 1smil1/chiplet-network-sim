#include "online_workload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <unordered_map>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "config.h"
extern "C" {
#include "netrace.h"
}
#include "system.h"
#include "traffic_manager.h"

using boost::property_tree::ptree;

namespace {

std::vector<int> ReadIntArray(const ptree& parent, const std::string& key) {
  std::vector<int> values;
  auto child_opt = parent.get_child_optional(key);
  if (!child_opt) {
    return values;
  }
  for (const auto& item : *child_opt) {
    values.push_back(item.second.get_value<int>());
  }
  return values;
}

std::string PhaseStatusToString(OnlinePhaseStatus status) {
  switch (status) {
    case OnlinePhaseStatus::waiting:
      return "waiting";
    case OnlinePhaseStatus::computing:
      return "computing";
    case OnlinePhaseStatus::ready_to_inject:
      return "ready_to_inject";
    case OnlinePhaseStatus::communicating:
      return "communicating";
    case OnlinePhaseStatus::done:
      return "done";
  }
  return "unknown";
}

}  // namespace

std::string OnlineWorkloadScheduler::ParentDir(const std::string& path) {
  const size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return "";
  }
  return path.substr(0, pos);
}

std::string OnlineWorkloadScheduler::JoinPath(const std::string& base_dir, const std::string& child) {
  if (child.empty()) {
    return base_dir;
  }
  if (child.size() >= 2 && child[1] == ':') {
    return child;
  }
  if (!child.empty() && (child[0] == '/' || child[0] == '\\')) {
    return child;
  }
  if (base_dir.empty()) {
    return child;
  }
  const char last = base_dir[base_dir.size() - 1];
  if (last == '/' || last == '\\') {
    return base_dir + child;
  }
  return base_dir + "/" + child;
}

std::string OnlineWorkloadScheduler::ReplaceExtension(const std::string& path, const std::string& new_ext) {
  const size_t slash_pos = path.find_last_of("/\\");
  const size_t dot_pos = path.find_last_of('.');
  if (dot_pos == std::string::npos || (slash_pos != std::string::npos && dot_pos < slash_pos)) {
    return path + new_ext;
  }
  return path.substr(0, dot_pos) + new_ext;
}

bool OnlineWorkloadScheduler::LoadFromLegacyJson(const ptree& root, std::string* error) {
  num_inputs_ = root.get<int>("num_inputs");
  num_resources_ = root.get<int>("num_resources");
  current_cycle_ = 0;
  next_run_id_ = 0;
  next_fanout_id_ = 0;
  resource_release_cycle_.assign(std::max(0, num_resources_), 0);
  resource_owner_phase_.assign(std::max(0, num_resources_), -1);
  phase_id_to_index_.clear();
  dependents_.clear();
  group_id_to_index_.clear();
  run_id_to_index_.clear();
  phases_.clear();
  group_templates_.clear();
  group_runs_.clear();
  pending_fanouts_.clear();
  inputs_.assign(std::max(0, num_inputs_), OnlineInputSummary());
  for (int i = 0; i < num_inputs_; ++i) {
    inputs_[i].input_id = i;
  }

  std::vector<OnlinePacketTemplate> legacy_packets;
  for (const auto& item : root.get_child("packet_templates")) {
    OnlinePacketTemplate packet;
    packet.packet_id = item.second.get<int>("packet_id");
    packet.src_node = item.second.get<int>("src_node");
    packet.dst_node = item.second.get<int>("dst_node");
    packet.size_bytes = item.second.get<int>("size_bytes");
    packet.shared_key = item.second.get<int>("shared_key", -1);
    legacy_packets.push_back(packet);
  }

  for (const auto& item : root.get_child("comm_tasks")) {
    const int task_id = item.second.get<int>("task_id");
    const int packet_begin = item.second.get<int>("packet_begin");
    const int packet_end = item.second.get<int>("packet_end");
    if (task_id < 0 || packet_begin < 0 || packet_end < packet_begin ||
        packet_end > static_cast<int>(legacy_packets.size())) {
      if (error) *error = "Legacy comm_task range invalid";
      return false;
    }
    OnlineGroupTemplate group;
    group.group_id = task_id;
    group.bz2_path = "legacy_json_inline";
    group.shared_prefix_policy = 0;
    for (int i = packet_begin; i < packet_end; ++i) {
      group.packets.push_back(legacy_packets[i]);
    }
    group_id_to_index_[group.group_id] = static_cast<int>(group_templates_.size());
    group_templates_.push_back(group);
  }

  for (const auto& item : root.get_child("phases")) {
    OnlinePhaseState state;
    state.def.phase_id = item.second.get<int>("phase_id");
    state.def.input_id = item.second.get<int>("input_id");
    state.def.layer_id = item.second.get<int>("layer_id");
    state.def.compute_latency_cycles = item.second.get<uint64_t>("compute_latency_cycles");
    state.def.dep_phase_ids = ReadIntArray(item.second, "dep_phase_ids");
    state.def.resource_ids = ReadIntArray(item.second, "resource_ids");
    const int begin = item.second.get<int>("comm_task_begin");
    const int end = item.second.get<int>("comm_task_end");
    state.def.group_id = (begin < end) ? begin : -1;
    state.dep_remaining = static_cast<int>(state.def.dep_phase_ids.size());
    phase_id_to_index_[state.def.phase_id] = static_cast<int>(phases_.size());
    phases_.push_back(state);
    if (state.def.input_id >= 0 && state.def.input_id < num_inputs_) {
      inputs_[state.def.input_id].total_phase_count++;
    }
  }

  for (size_t i = 0; i < phases_.size(); ++i) {
    const std::vector<int>& deps = phases_[i].def.dep_phase_ids;
    for (size_t j = 0; j < deps.size(); ++j) {
      dependents_[deps[j]].push_back(static_cast<int>(i));
    }
  }

  return Validate(error);
}

bool OnlineWorkloadScheduler::LoadGroupTemplate(int group_id,
                                                const std::string& bz2_path,
                                                std::string* error) {
  nt_context_t ctx;
  std::memset(&ctx, 0, sizeof(ctx));
  nt_open_trfile(&ctx, bz2_path.c_str());
  nt_disable_dependencies(&ctx);

  OnlineGroupTemplate group;
  group.group_id = group_id;
  group.bz2_path = bz2_path;
  group.shared_prefix_policy = 0;
  nt_packet_t* packet = NULL;
  while ((packet = nt_read_packet(&ctx)) != NULL) {
    OnlinePacketTemplate templ;
    templ.packet_id = static_cast<int>(packet->id);
    templ.src_node = static_cast<int>(packet->src);
    templ.dst_node = static_cast<int>(packet->dst);
    templ.size_bytes = nt_get_packet_size(packet);
    group.packets.push_back(templ);
    nt_packet_free(packet);
  }
  nt_close_trfile(&ctx);

  const std::string sidecar_path = ReplaceExtension(bz2_path, ".json");
  try {
    ptree sidecar;
    read_json(sidecar_path, sidecar);
    group.shared_prefix_policy =
        sidecar.get<int>("metadata.shared_prefix_policy", 0);
    if (group.shared_prefix_policy < 0 || group.shared_prefix_policy > 2) {
      group.shared_prefix_policy = 0;
    }
    std::unordered_map<int, int> shared_by_id;
    for (const auto& item : sidecar.get_child("packets")) {
      const int packet_id = item.second.get<int>("id");
      const int shared_key = item.second.get<int>("shared_key", -1);
      shared_by_id[packet_id] = shared_key;
    }
    for (size_t i = 0; i < group.packets.size(); ++i) {
      std::unordered_map<int, int>::const_iterator it =
          shared_by_id.find(group.packets[i].packet_id);
      if (it != shared_by_id.end()) {
        group.packets[i].shared_key = it->second;
      }
    }
  } catch (const std::exception&) {
    group.shared_prefix_policy = 0;
    for (size_t i = 0; i < group.packets.size(); ++i) {
      group.packets[i].shared_key = -1;
    }
  }

  if (group.packets.empty()) {
    if (error) *error = "Group template has no packets: " + bz2_path;
    return false;
  }

  group_id_to_index_[group_id] = static_cast<int>(group_templates_.size());
  group_templates_.push_back(group);
  return true;
}

bool OnlineWorkloadScheduler::LoadFromManifest(const ptree& root,
                                               const std::string& workload_file,
                                               std::string* error) {
  num_inputs_ = root.get<int>("num_inputs");
  num_resources_ = root.get<int>("num_resources");
  current_cycle_ = 0;
  next_run_id_ = 0;
  next_fanout_id_ = 0;
  resource_release_cycle_.assign(std::max(0, num_resources_), 0);
  resource_owner_phase_.assign(std::max(0, num_resources_), -1);
  phase_id_to_index_.clear();
  dependents_.clear();
  group_id_to_index_.clear();
  run_id_to_index_.clear();
  phases_.clear();
  group_templates_.clear();
  group_runs_.clear();
  pending_fanouts_.clear();
  nonuniform_chiplets_.clear();
  use_nonuniform_tier_grid_ = false;
  inputs_.assign(std::max(0, num_inputs_), OnlineInputSummary());
  for (int i = 0; i < num_inputs_; ++i) {
    inputs_[i].input_id = i;
  }

  if (param != NULL && !param->nonuniform_tier_grid_file.empty()) {
    if (!LoadNonuniformTierGrid(param->nonuniform_tier_grid_file, error)) {
      return false;
    }
  }

  const std::string manifest_dir = ParentDir(workload_file);
  for (const auto& item : root.get_child("template_groups")) {
    const int group_id = item.second.get<int>("group_id");
    const std::string rel_path = item.second.get<std::string>("bz2_path");
    const std::string full_path = JoinPath(manifest_dir, rel_path);
    if (!LoadGroupTemplate(group_id, full_path, error)) {
      return false;
    }
  }

  for (const auto& item : root.get_child("phases")) {
    OnlinePhaseState state;
    state.def.phase_id = item.second.get<int>("phase_id");
    state.def.input_id = item.second.get<int>("input_id");
    state.def.layer_id = item.second.get<int>("layer_id");
    state.def.compute_latency_cycles = item.second.get<uint64_t>("compute_latency_cycles");
    state.def.dep_phase_ids = ReadIntArray(item.second, "dep_phase_ids");
    state.def.resource_ids = ReadIntArray(item.second, "resource_ids");
    state.def.group_id = item.second.get<int>("group_id", -1);
    state.dep_remaining = static_cast<int>(state.def.dep_phase_ids.size());
    phase_id_to_index_[state.def.phase_id] = static_cast<int>(phases_.size());
    phases_.push_back(state);
    if (state.def.input_id >= 0 && state.def.input_id < num_inputs_) {
      inputs_[state.def.input_id].total_phase_count++;
    }
  }

  for (size_t i = 0; i < phases_.size(); ++i) {
    const std::vector<int>& deps = phases_[i].def.dep_phase_ids;
    for (size_t j = 0; j < deps.size(); ++j) {
      dependents_[deps[j]].push_back(static_cast<int>(i));
    }
  }

  return Validate(error);
}

bool OnlineWorkloadScheduler::LoadNonuniformTierGrid(const std::string& path,
                                                     std::string* error) {
  try {
    ptree root;
    read_json(path, root);
    const bool enabled = root.get<bool>("enabled", false);
    if (!enabled) {
      return true;
    }
    nonuniform_chiplets_.clear();
    for (const auto& item : root.get_child("chiplets")) {
      const int chip_id = std::stoi(item.first);
      OnlineChipletGrid grid;
      grid.chip_id = chip_id;
      grid.tier_id = item.second.get<int>("tier_id", -1);
      grid.chip_x = item.second.get<int>("chip_x");
      grid.chip_y = item.second.get<int>("chip_y");
      std::vector<int> dims;
      for (const auto& dim_item : item.second.get_child("grid")) {
        dims.push_back(dim_item.second.get_value<int>());
      }
      if (dims.size() < 2 || dims[0] <= 0 || dims[1] <= 0) {
        if (error) *error = "Invalid nonuniform tier grid dimensions";
        return false;
      }
      grid.grid_x = dims[0];
      grid.grid_y = dims[1];
      nonuniform_chiplets_[chip_id] = grid;
    }
    use_nonuniform_tier_grid_ = !nonuniform_chiplets_.empty();
    if (param->online_debug && use_nonuniform_tier_grid_) {
      std::cerr << "[ONLINE WORKLOAD] Loaded nonuniform tier grid: "
                << nonuniform_chiplets_.size() << " chiplets from " << path << std::endl;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = std::string("Failed to load nonuniform tier grid: ") + ex.what();
    }
    return false;
  }
}

bool OnlineWorkloadScheduler::LoadFromFile(const std::string& workload_file,
                                           int num_network_nodes,
                                           std::string* error) {
  try {
    ptree root;
    boost::property_tree::read_json(workload_file, root);
    num_network_nodes_ = num_network_nodes;
    if (root.get_child_optional("template_groups")) {
      return LoadFromManifest(root, workload_file, error);
    }
    return LoadFromLegacyJson(root, error);
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  }
}

bool OnlineWorkloadScheduler::Validate(std::string* error) const {
  for (size_t i = 0; i < phases_.size(); ++i) {
    const OnlinePhaseState& phase_state = phases_[i];
    if (phase_state.def.input_id < 0 || phase_state.def.input_id >= num_inputs_) {
      if (error) *error = "Phase input_id out of range";
      return false;
    }
    for (size_t j = 0; j < phase_state.def.resource_ids.size(); ++j) {
      const int resource_id = phase_state.def.resource_ids[j];
      if (resource_id < 0 || resource_id >= num_resources_) {
        if (error) *error = "Phase resource_id out of range";
        return false;
      }
    }
    for (size_t j = 0; j < phase_state.def.dep_phase_ids.size(); ++j) {
      const int dep_id = phase_state.def.dep_phase_ids[j];
      if (phase_id_to_index_.count(dep_id) == 0) {
        if (error) *error = "Phase dependency not found";
        return false;
      }
    }
    if (phase_state.def.group_id >= 0 && group_id_to_index_.count(phase_state.def.group_id) == 0) {
      if (error) *error = "Phase group_id not found";
      return false;
    }
  }

  for (size_t i = 0; i < group_templates_.size(); ++i) {
    const OnlineGroupTemplate& group = group_templates_[i];
    for (size_t j = 0; j < group.packets.size(); ++j) {
      const OnlinePacketTemplate& packet = group.packets[j];
      if (packet.src_node < 0 || packet.src_node >= num_network_nodes_ ||
          packet.dst_node < 0 || packet.dst_node >= num_network_nodes_) {
        if (error) *error = "Packet node id out of range";
        return false;
      }
      if (packet.size_bytes <= 0) {
        if (error) *error = "Packet size must be positive";
        return false;
      }
    }
  }

  return true;
}

bool OnlineWorkloadScheduler::AllDone() const {
  for (size_t i = 0; i < phases_.size(); ++i) {
    if (phases_[i].status != OnlinePhaseStatus::done) {
      return false;
    }
  }
  return true;
}

void OnlineWorkloadScheduler::ReclaimCompletedPackets(std::vector<Packet*>& packets) {
  size_t write_index = 0;
  for (size_t i = 0; i < packets.size(); ++i) {
    Packet* packet = packets[i];
    if (packet->finished_ && !packet->releaselink_) {
      delete packet;
      continue;
    }
    packets[write_index++] = packet;
  }
  packets.resize(write_index);
}

void OnlineWorkloadScheduler::OnGroupRunDone(OnlineGroupRunSummary& run_state, uint64_t finish_cycle) {
  if (run_state.done) {
    return;
  }
  run_state.done = true;
  run_state.finish_cycle = finish_cycle;
  OnlinePhaseState& phase_state = phases_[phase_id_to_index_.at(run_state.phase_id)];
  phase_state.comm_end_cycle = finish_cycle;
  MarkPhaseDone(phase_state);
}

void OnlineWorkloadScheduler::MarkPhaseDone(OnlinePhaseState& phase_state) {
  if (phase_state.status == OnlinePhaseStatus::done) {
    return;
  }
  phase_state.status = OnlinePhaseStatus::done;
  if (phase_state.comm_end_cycle < phase_state.compute_end_cycle) {
    phase_state.comm_end_cycle = phase_state.compute_end_cycle;
  }

  OnlineInputSummary& input_summary = inputs_[phase_state.def.input_id];
  input_summary.done_phase_count++;
  if (!input_summary.input_done_seen &&
      input_summary.done_phase_count == input_summary.total_phase_count) {
    input_summary.input_done_seen = true;
    input_summary.input_done_cycle = phase_state.comm_end_cycle;
  }
  if (!input_summary.all_comm_done_seen &&
      input_summary.done_phase_count == input_summary.total_phase_count) {
    input_summary.all_comm_done_seen = true;
    input_summary.all_comm_done_cycle = phase_state.comm_end_cycle;
  }

  std::unordered_map<int, std::vector<int> >::iterator dependent_it =
      dependents_.find(phase_state.def.phase_id);
  if (dependent_it != dependents_.end()) {
    for (size_t i = 0; i < dependent_it->second.size(); ++i) {
      int dependent_index = dependent_it->second[i];
      if (phases_[dependent_index].dep_remaining > 0) {
        --phases_[dependent_index].dep_remaining;
      }
    }
  }
}

NodeID OnlineWorkloadScheduler::ComputeDestinationBoundary(NodeID src, NodeID dst) const {
  if (src.chip_id == dst.chip_id) {
    return dst;
  }

  NodeID nonuniform_boundary;
  if (ComputeNonuniformDestinationBoundary(src, dst, &nonuniform_boundary)) {
    return nonuniform_boundary;
  }

  const int k_node = param->params_ptree.get<int>("Network.k_node", 4);
  const int chip_w = param->params_ptree.get<int>(
      "Network.chip_w",
      param->params_ptree.get<int>("Network.k_chip", 2));

  const int src_chip_x = src.chip_id % chip_w;
  const int src_chip_y = src.chip_id / chip_w;
  const int dst_chip_x = dst.chip_id % chip_w;
  const int dst_chip_y = dst.chip_id / chip_w;

  const int src_local_y = src.node_id / k_node;
  const int dst_local_x = dst.node_id % k_node;

  if (src_chip_y == dst_chip_y && src_chip_x != dst_chip_x) {
    const int boundary_x = (dst_chip_x > src_chip_x) ? 0 : (k_node - 1);
    const int boundary_y = src_local_y;
    return NodeID(boundary_y * k_node + boundary_x, dst.chip_id);
  }

  const int boundary_x = dst_local_x;
  const int boundary_y = (dst_chip_y > src_chip_y) ? 0 : (k_node - 1);
  return NodeID(boundary_y * k_node + boundary_x, dst.chip_id);
}

bool OnlineWorkloadScheduler::ComputeNonuniformDestinationBoundary(
    NodeID src, NodeID dst, NodeID* boundary) const {
  if (!use_nonuniform_tier_grid_ || boundary == NULL || src.chip_id == dst.chip_id) {
    return false;
  }

  std::unordered_map<int, OnlineChipletGrid>::const_iterator src_it =
      nonuniform_chiplets_.find(src.chip_id);
  std::unordered_map<int, OnlineChipletGrid>::const_iterator dst_it =
      nonuniform_chiplets_.find(dst.chip_id);
  if (src_it == nonuniform_chiplets_.end() || dst_it == nonuniform_chiplets_.end()) {
    return false;
  }

  const int k_node = param->params_ptree.get<int>("Network.k_node", 4);
  if (k_node <= 0) {
    return false;
  }
  const OnlineChipletGrid& src_grid = src_it->second;
  const OnlineChipletGrid& dst_grid = dst_it->second;
  if (dst_grid.grid_x <= 0 || dst_grid.grid_y <= 0 ||
      dst_grid.grid_x > k_node || dst_grid.grid_y > k_node) {
    return false;
  }

  const int src_local_y = src.node_id / src_grid.grid_x;
  const int dst_local_x = dst.node_id % dst_grid.grid_x;
  int boundary_x = 0;
  int boundary_y = 0;

  if (src_grid.chip_y == dst_grid.chip_y && src_grid.chip_x != dst_grid.chip_x) {
    boundary_x = (dst_grid.chip_x > src_grid.chip_x) ? 0 : (dst_grid.grid_x - 1);
    boundary_y = std::max(0, std::min(src_local_y, dst_grid.grid_y - 1));
  } else {
    boundary_x = std::max(0, std::min(dst_local_x, dst_grid.grid_x - 1));
    boundary_y = (dst_grid.chip_y > src_grid.chip_y) ? 0 : (dst_grid.grid_y - 1);
  }

  *boundary = NodeID(boundary_y * dst_grid.grid_x + boundary_x, dst.chip_id);
  return true;
}

void OnlineWorkloadScheduler::InjectBoundaryFanout(const Packet& prefix_packet,
                                                   std::vector<Packet*>& packets,
                                                   uint64_t finish_cycle) {
  std::unordered_map<int, OnlinePendingFanout>::iterator fanout_it =
      pending_fanouts_.find(prefix_packet.shared_fanout_id_);
  if (fanout_it == pending_fanouts_.end()) {
    return;
  }

  OnlinePendingFanout pending = fanout_it->second;
  pending_fanouts_.erase(fanout_it);

  std::unordered_map<int, int>::iterator run_it = run_id_to_index_.find(pending.run_id);
  if (run_it == run_id_to_index_.end()) {
    return;
  }
  OnlineGroupRunSummary& run_state = group_runs_[run_it->second];
  const NodeID boundary = prefix_packet.destination_;
  int immediate_completions = 0;

  for (size_t i = 0; i < pending.packets.size(); ++i) {
    const OnlinePacketTemplate& templ = pending.packets[i];
    const NodeID dst_nid = network->id2nodeid(templ.dst_node);
    if (dst_nid == boundary) {
      ++immediate_completions;
      continue;
    }

    Packet* packet = new Packet(boundary, dst_nid, BytesToFlits(templ.size_bytes));
    packet->process_timer_ = 0;
    packet->input_id_ = pending.input_id;
    packet->phase_id_ = pending.phase_id;
    packet->task_id_ = pending.run_id;
    packet->template_packet_id_ = templ.packet_id;
    packets.push_back(packet);
    TM->all_message_num_++;
  }

  if (immediate_completions > 0 && run_state.remaining_packet_count > 0) {
    run_state.remaining_packet_count =
        std::max(0, run_state.remaining_packet_count - immediate_completions);
  }
  if (run_state.remaining_packet_count == 0) {
    OnGroupRunDone(run_state, finish_cycle);
  }
}

void OnlineWorkloadScheduler::ProcessFinishedPackets(std::vector<Packet*>& packets) {
  const uint64_t finish_cycle = current_cycle_ == 0 ? 0 : current_cycle_ - 1;
  const size_t packet_count_at_start = packets.size();
  for (size_t i = 0; i < packet_count_at_start; ++i) {
    Packet* packet = packets[i];
    if (!packet->finished_ || packet->completion_recorded_) {
      continue;
    }
    packet->completion_recorded_ = true;
    if (packet->task_id_ < 0) {
      continue;
    }
    std::unordered_map<int, int>::iterator run_it = run_id_to_index_.find(packet->task_id_);
    if (run_it == run_id_to_index_.end()) {
      continue;
    }
    if (packet->shared_fanout_id_ >= 0) {
      InjectBoundaryFanout(*packet, packets, finish_cycle);
      continue;
    }
    OnlineGroupRunSummary& run_state = group_runs_[run_it->second];
    const int logical_completions = 1 + std::max(0, packet->shared_follower_count_);
    if (run_state.remaining_packet_count > 0) {
      run_state.remaining_packet_count =
          std::max(0, run_state.remaining_packet_count - logical_completions);
    }
    if (run_state.remaining_packet_count == 0) {
      OnGroupRunDone(run_state, finish_cycle);
    }
  }
}

bool OnlineWorkloadScheduler::ResourcesAvailable(const OnlinePhaseState& phase_state) const {
  for (size_t i = 0; i < phase_state.def.resource_ids.size(); ++i) {
    const int resource_id = phase_state.def.resource_ids[i];
    if (resource_release_cycle_[resource_id] > current_cycle_ ||
        resource_owner_phase_[resource_id] != -1) {
      return false;
    }
  }
  return true;
}

uint64_t OnlineWorkloadScheduler::EarliestResourceReadyCycle(const OnlinePhaseState& phase_state) const {
  uint64_t earliest = current_cycle_;
  for (size_t i = 0; i < phase_state.def.resource_ids.size(); ++i) {
    const int resource_id = phase_state.def.resource_ids[i];
    if (resource_owner_phase_[resource_id] != -1) {
      return std::numeric_limits<uint64_t>::max();
    }
    earliest = std::max(earliest, resource_release_cycle_[resource_id]);
  }
  return earliest;
}

void OnlineWorkloadScheduler::StartNewComputePhases() {
  for (size_t i = 0; i < phases_.size(); ++i) {
    OnlinePhaseState& phase_state = phases_[i];
    if (phase_state.status != OnlinePhaseStatus::waiting) {
      continue;
    }
    if (phase_state.dep_remaining != 0) {
      continue;
    }
    if (!ResourcesAvailable(phase_state)) {
      continue;
    }
    phase_state.status = OnlinePhaseStatus::computing;
    phase_state.compute_start_cycle = current_cycle_;
    phase_state.compute_end_cycle = current_cycle_ + phase_state.def.compute_latency_cycles;
    for (size_t j = 0; j < phase_state.def.resource_ids.size(); ++j) {
      const int resource_id = phase_state.def.resource_ids[j];
      resource_owner_phase_[resource_id] = phase_state.def.phase_id;
    }
  }
}

void OnlineWorkloadScheduler::PromoteFinishedComputePhases() {
  for (size_t i = 0; i < phases_.size(); ++i) {
    OnlinePhaseState& phase_state = phases_[i];
    if (phase_state.status != OnlinePhaseStatus::computing) {
      continue;
    }
    if (phase_state.compute_end_cycle > current_cycle_) {
      continue;
    }
    for (size_t j = 0; j < phase_state.def.resource_ids.size(); ++j) {
      const int resource_id = phase_state.def.resource_ids[j];
      resource_release_cycle_[resource_id] = phase_state.compute_end_cycle;
      resource_owner_phase_[resource_id] = -1;
    }
    if (phase_state.def.group_id < 0) {
      phase_state.comm_start_cycle = phase_state.compute_end_cycle;
      phase_state.comm_end_cycle = phase_state.compute_end_cycle;
      MarkPhaseDone(phase_state);
      continue;
    }
    phase_state.status = OnlinePhaseStatus::ready_to_inject;
  }
}

int OnlineWorkloadScheduler::BytesToFlits(int size_bytes) {
  const int bytes_per_flit = 16;
  return std::max(1, static_cast<int>(std::ceil(static_cast<double>(size_bytes) / bytes_per_flit)));
}

int OnlineWorkloadScheduler::InjectReadyPackets(std::vector<Packet*>& packets) {
  int injected = 0;
  for (size_t i = 0; i < phases_.size(); ++i) {
    OnlinePhaseState& phase_state = phases_[i];
    if (phase_state.status != OnlinePhaseStatus::ready_to_inject) {
      continue;
    }
    phase_state.comm_start_cycle = current_cycle_;
    phase_state.status = OnlinePhaseStatus::communicating;

    OnlineInputSummary& input_summary = inputs_[phase_state.def.input_id];
    if (!input_summary.first_injection_seen) {
      input_summary.first_injection_seen = true;
      input_summary.first_injection_cycle = current_cycle_;
    }

    const OnlineGroupTemplate& group_template = group_templates_[group_id_to_index_.at(phase_state.def.group_id)];
    const int shared_prefix_policy = group_template.shared_prefix_policy;
    std::unordered_map<int, int> shared_leader_index;
    std::unordered_map<int, int> shared_follower_count;
    std::unordered_map<int, std::vector<int> > shared_members;
    for (size_t j = 0; j < group_template.packets.size(); ++j) {
      const OnlinePacketTemplate& templ = group_template.packets[j];
      if (shared_prefix_policy == 0 || templ.shared_key < 0) {
        continue;
      }
      if (shared_leader_index.find(templ.shared_key) == shared_leader_index.end()) {
        shared_leader_index[templ.shared_key] = static_cast<int>(j);
      } else {
        shared_follower_count[templ.shared_key]++;
      }
      shared_members[templ.shared_key].push_back(static_cast<int>(j));
    }

    int physical_packet_count = 0;
    int boundary_fanout_packet_count = 0;
    int shared_follower_total = 0;
    for (size_t j = 0; j < group_template.packets.size(); ++j) {
      const OnlinePacketTemplate& templ = group_template.packets[j];
      if (shared_prefix_policy == 1 &&
          templ.shared_key >= 0 &&
          shared_leader_index[templ.shared_key] != static_cast<int>(j)) {
        continue;
      }
      if (shared_prefix_policy == 2 &&
          templ.shared_key >= 0 &&
          shared_leader_index[templ.shared_key] != static_cast<int>(j)) {
        continue;
      }
      ++physical_packet_count;
      if (shared_prefix_policy == 2 && templ.shared_key >= 0) {
        const NodeID src_nid = network->id2nodeid(templ.src_node);
        const NodeID boundary = ComputeDestinationBoundary(src_nid, network->id2nodeid(templ.dst_node));
        const std::vector<int>& members = shared_members[templ.shared_key];
        for (size_t m = 0; m < members.size(); ++m) {
          const OnlinePacketTemplate& member = group_template.packets[members[m]];
          if (network->id2nodeid(member.dst_node) != boundary) {
            ++physical_packet_count;
            ++boundary_fanout_packet_count;
          }
        }
      }
    }
    for (std::unordered_map<int, int>::const_iterator it = shared_follower_count.begin();
         it != shared_follower_count.end(); ++it) {
      shared_follower_total += it->second;
    }

    OnlineGroupRunSummary run_state;
    run_state.run_id = next_run_id_++;
    run_state.group_id = group_template.group_id;
    run_state.phase_id = phase_state.def.phase_id;
    run_state.input_id = phase_state.def.input_id;
    run_state.template_bz2 = group_template.bz2_path;
    run_state.inject_cycle = current_cycle_;
    run_state.packet_count = static_cast<int>(group_template.packets.size());
    run_state.remaining_packet_count = run_state.packet_count;
    run_state.physical_packet_count = physical_packet_count;
    run_state.shared_follower_count = shared_follower_total;
    run_state.boundary_fanout_packet_count = boundary_fanout_packet_count;
    phase_state.run_id = run_state.run_id;
    run_id_to_index_[run_state.run_id] = static_cast<int>(group_runs_.size());
    group_runs_.push_back(run_state);

    if (run_state.packet_count == 0) {
      group_runs_.back().done = true;
      group_runs_.back().finish_cycle = current_cycle_;
      phase_state.comm_end_cycle = current_cycle_;
      MarkPhaseDone(phase_state);
      continue;
    }

    for (size_t j = 0; j < group_template.packets.size(); ++j) {
      const OnlinePacketTemplate& templ = group_template.packets[j];
      if (shared_prefix_policy == 1 &&
          templ.shared_key >= 0 &&
          shared_leader_index[templ.shared_key] != static_cast<int>(j)) {
        continue;
      }
      if (shared_prefix_policy == 2 &&
          templ.shared_key >= 0 &&
          shared_leader_index[templ.shared_key] != static_cast<int>(j)) {
        continue;
      }
      NodeID src_nid = network->id2nodeid(templ.src_node);
      NodeID dst_nid = network->id2nodeid(templ.dst_node);
      int fanout_id = -1;
      if (shared_prefix_policy == 2 && templ.shared_key >= 0) {
        dst_nid = ComputeDestinationBoundary(src_nid, dst_nid);
        OnlinePendingFanout pending;
        pending.fanout_id = next_fanout_id_++;
        pending.run_id = run_state.run_id;
        pending.input_id = phase_state.def.input_id;
        pending.phase_id = phase_state.def.phase_id;
        const std::vector<int>& members = shared_members[templ.shared_key];
        for (size_t m = 0; m < members.size(); ++m) {
          pending.packets.push_back(group_template.packets[members[m]]);
        }
        fanout_id = pending.fanout_id;
        pending_fanouts_[fanout_id] = pending;
      }
      Packet* packet =
          new Packet(src_nid, dst_nid, BytesToFlits(templ.size_bytes));
      packet->input_id_ = phase_state.def.input_id;
      packet->phase_id_ = phase_state.def.phase_id;
      packet->task_id_ = run_state.run_id;
      packet->template_packet_id_ = templ.packet_id;
      packet->shared_follower_count_ =
          (shared_prefix_policy == 1 && templ.shared_key >= 0) ? shared_follower_count[templ.shared_key] : 0;
      packet->shared_fanout_id_ = fanout_id;
      packets.push_back(packet);
      TM->all_message_num_++;
      ++injected;
    }
  }
  return injected;
}

void OnlineWorkloadScheduler::AdvanceOneCycle() { ++current_cycle_; }

bool OnlineWorkloadScheduler::FastForwardIfIdle() {
  uint64_t next_cycle = std::numeric_limits<uint64_t>::max();

  for (size_t i = 0; i < phases_.size(); ++i) {
    const OnlinePhaseState& phase_state = phases_[i];
    if (phase_state.status == OnlinePhaseStatus::computing) {
      next_cycle = std::min(next_cycle, phase_state.compute_end_cycle);
    } else if (phase_state.status == OnlinePhaseStatus::waiting && phase_state.dep_remaining == 0) {
      next_cycle = std::min(next_cycle, EarliestResourceReadyCycle(phase_state));
    }
  }

  if (next_cycle == std::numeric_limits<uint64_t>::max() || next_cycle <= current_cycle_) {
    return false;
  }

  current_cycle_ = next_cycle;
  return true;
}

void OnlineWorkloadScheduler::MaybePauseOnFirstInjection(int input_id, size_t inflight_packet_count) const {
  if (!param->pause_on_first_injection) {
    return;
  }
  const OnlineInputSummary& input_summary = inputs_[input_id];
  if (!input_summary.first_injection_seen || input_summary.first_injection_cycle != current_cycle_) {
    return;
  }
  std::cout << "[PAUSE] cycle=" << current_cycle_ << " input=" << input_id
            << " event=first_injection inflight_packets=" << inflight_packet_count
            << " phases_done=" << input_summary.done_phase_count << "/" << input_summary.total_phase_count
            << " network_idle=" << (inflight_packet_count == 0 ? "true" : "false") << std::endl;
}

void OnlineWorkloadScheduler::MaybePauseOnInputDone(int input_id, size_t inflight_packet_count) const {
  if (!param->pause_on_input_done) {
    return;
  }
  const OnlineInputSummary& input_summary = inputs_[input_id];
  if (!input_summary.input_done_seen ||
      input_summary.input_done_cycle + 1 != current_cycle_) {
    return;
  }
  std::cout << "[PAUSE] cycle=" << current_cycle_ << " input=" << input_id
            << " event=input_done inflight_packets=" << inflight_packet_count
            << " phases_done=" << input_summary.done_phase_count << "/" << input_summary.total_phase_count
            << " network_idle=" << (inflight_packet_count == 0 ? "true" : "false") << std::endl;
}

void OnlineWorkloadScheduler::PrintPauseEvents(size_t inflight_packet_count) const {
  for (size_t i = 0; i < inputs_.size(); ++i) {
    MaybePauseOnFirstInjection(inputs_[i].input_id, inflight_packet_count);
    MaybePauseOnInputDone(inputs_[i].input_id, inflight_packet_count);
  }
}

void OnlineWorkloadScheduler::PrintProgressSummary(size_t inflight_packet_count) const {
  size_t waiting = 0;
  size_t computing = 0;
  size_t ready_to_inject = 0;
  size_t communicating = 0;
  size_t done = 0;
  for (size_t i = 0; i < phases_.size(); ++i) {
    switch (phases_[i].status) {
      case OnlinePhaseStatus::waiting:
        ++waiting;
        break;
      case OnlinePhaseStatus::computing:
        ++computing;
        break;
      case OnlinePhaseStatus::ready_to_inject:
        ++ready_to_inject;
        break;
      case OnlinePhaseStatus::communicating:
        ++communicating;
        break;
      case OnlinePhaseStatus::done:
        ++done;
        break;
    }
  }

  size_t groups_done = 0;
  size_t groups_active = 0;
  for (size_t i = 0; i < group_runs_.size(); ++i) {
    if (group_runs_[i].done) {
      ++groups_done;
    } else {
      ++groups_active;
    }
  }

  std::cout << "[ONLINE PROGRESS] cycle=" << current_cycle_
            << " inflight_packets=" << inflight_packet_count
            << " phases(wait=" << waiting
            << ", compute=" << computing
            << ", ready=" << ready_to_inject
            << ", comm=" << communicating
            << ", done=" << done << "/" << phases_.size() << ")"
            << " groups(active=" << groups_active
            << ", done=" << groups_done << "/" << group_runs_.size() << ")"
            << std::endl;

  if (groups_active > 0) {
    std::cout << "  [ONLINE PROGRESS] active_groups:";
    int printed = 0;
    for (size_t i = 0; i < group_runs_.size(); ++i) {
      const OnlineGroupRunSummary& run = group_runs_[i];
      if (run.done) {
        continue;
      }
      std::cout << " {phase=" << run.phase_id
                << ", group=" << run.group_id
                << ", input=" << run.input_id
                << ", inject=" << run.inject_cycle
                << ", remaining_packets=" << run.remaining_packet_count
                << "}";
      ++printed;
      if (printed >= 4) {
        break;
      }
    }
    std::cout << std::endl;
  }
}

void OnlineWorkloadScheduler::PrintFinalSummary() const {
  std::cout << "\n[ONLINE WORKLOAD] Simulation complete at cycle " << current_cycle_ << std::endl;
  for (size_t i = 0; i < inputs_.size(); ++i) {
    const OnlineInputSummary& input_summary = inputs_[i];
    std::cout << "  Input " << input_summary.input_id
              << ": first_injection="
              << (input_summary.first_injection_seen ? std::to_string(input_summary.first_injection_cycle) : "N/A")
              << ", all_comm_done="
              << (input_summary.all_comm_done_seen ? std::to_string(input_summary.all_comm_done_cycle) : "N/A")
              << ", input_done="
              << (input_summary.input_done_seen ? std::to_string(input_summary.input_done_cycle) : "N/A")
              << ", phases_done=" << input_summary.done_phase_count << "/" << input_summary.total_phase_count
              << std::endl;
  }
  for (size_t i = 0; i < group_runs_.size(); ++i) {
    const OnlineGroupRunSummary& run = group_runs_[i];
    std::cout << "  GroupRun phase=" << run.phase_id << " group=" << run.group_id
              << " inject=" << run.inject_cycle << " finish="
              << (run.done ? std::to_string(run.finish_cycle) : "N/A")
              << " packets=" << run.packet_count << std::endl;
  }
}

bool OnlineWorkloadScheduler::WriteResultsJson(const std::string& json_path, std::string* error) const {
  try {
    ptree root;
    root.put("mode", "online");
    uint64_t final_cycle = 0;
    for (size_t i = 0; i < phases_.size(); ++i) {
      final_cycle = std::max(final_cycle, phases_[i].comm_end_cycle);
    }
    root.put("final_cycle", final_cycle);
    root.put("cycle_frequency_ghz", param->cycle_frequency_ghz);
    std::ostringstream time_base;
    time_base << "1cycle=" << std::setprecision(6)
              << (param->cycle_frequency_ghz > 0.0 ? (1.0 / param->cycle_frequency_ghz) : 0.0)
              << "ns @ " << param->cycle_frequency_ghz << "GHz";
    root.put("time_base", time_base.str());

    ptree inputs_node;
    for (size_t i = 0; i < inputs_.size(); ++i) {
      const OnlineInputSummary& input = inputs_[i];
      ptree entry;
      entry.put("input_id", input.input_id);
      if (input.first_injection_seen) {
        entry.put("first_injection_cycle", input.first_injection_cycle);
      }
      if (input.all_comm_done_seen) {
        entry.put("all_comm_done_cycle", input.all_comm_done_cycle);
      }
      if (input.input_done_seen) {
        entry.put("input_done_cycle", input.input_done_cycle);
      }
      inputs_node.push_back(std::make_pair("", entry));
    }
    root.add_child("inputs", inputs_node);

    ptree groups_node;
    for (size_t i = 0; i < group_runs_.size(); ++i) {
      const OnlineGroupRunSummary& run = group_runs_[i];
      ptree entry;
      entry.put("run_id", run.run_id);
      entry.put("group_id", run.group_id);
      entry.put("phase_id", run.phase_id);
      entry.put("input_id", run.input_id);
      entry.put("template_bz2", run.template_bz2);
      entry.put("inject_cycle", run.inject_cycle);
      if (run.done) {
        entry.put("finish_cycle", run.finish_cycle);
      }
      entry.put("packet_count", run.packet_count);
      entry.put("physical_packet_count", run.physical_packet_count);
      entry.put("shared_follower_count", run.shared_follower_count);
      entry.put("boundary_fanout_packet_count", run.boundary_fanout_packet_count);
      groups_node.push_back(std::make_pair("", entry));
    }
    root.add_child("groups", groups_node);

    ptree phases_node;
    for (size_t i = 0; i < phases_.size(); ++i) {
      const OnlinePhaseState& phase = phases_[i];
      ptree entry;
      entry.put("phase_id", phase.def.phase_id);
      entry.put("input_id", phase.def.input_id);
      entry.put("layer_id", phase.def.layer_id);
      ptree deps_node;
      for (size_t j = 0; j < phase.def.dep_phase_ids.size(); ++j) {
        ptree dep_entry;
        dep_entry.put("", phase.def.dep_phase_ids[j]);
        deps_node.push_back(std::make_pair("", dep_entry));
      }
      entry.add_child("dep_phase_ids", deps_node);
      ptree resources_node;
      for (size_t j = 0; j < phase.def.resource_ids.size(); ++j) {
        ptree res_entry;
        res_entry.put("", phase.def.resource_ids[j]);
        resources_node.push_back(std::make_pair("", res_entry));
      }
      entry.add_child("resource_ids", resources_node);
      entry.put("compute_latency_cycles", phase.def.compute_latency_cycles);
      entry.put("compute_start_cycle", phase.compute_start_cycle);
      entry.put("compute_end_cycle", phase.compute_end_cycle);
      entry.put("comm_start_cycle", phase.comm_start_cycle);
      entry.put("comm_end_cycle", phase.comm_end_cycle);
      entry.put("state_final", PhaseStatusToString(phase.status));
      entry.put("has_group", phase.def.group_id >= 0);
      phases_node.push_back(std::make_pair("", entry));
    }
    root.add_child("phases", phases_node);

    root.put("message_arrived", TM->message_arrived_.load());
    root.put("average_latency",
             TM->message_arrived_.load() == 0
                 ? 0.0
                 : static_cast<double>(TM->total_cycles_.load()) / TM->message_arrived_.load());
    boost::property_tree::write_json(json_path, root);
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  }
}
