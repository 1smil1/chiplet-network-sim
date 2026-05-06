#include "online_workload.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

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
  resource_release_cycle_.assign(std::max(0, num_resources_), 0);
  resource_owner_phase_.assign(std::max(0, num_resources_), -1);
  phase_id_to_index_.clear();
  dependents_.clear();
  group_id_to_index_.clear();
  run_id_to_index_.clear();
  phases_.clear();
  group_templates_.clear();
  group_runs_.clear();
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
  nt_packet_t* packet = NULL;
  while ((packet = nt_read_packet(&ctx)) != NULL) {
    OnlinePacketTemplate templ;
    templ.packet_id = static_cast<int>(packet->id);
    templ.src_node = static_cast<int>(packet->src);
    templ.dst_node = static_cast<int>(packet->dst);
    templ.size_bytes = nt_get_packet_size(packet);
    // DEBUG: print raw values read from .bz2
    if (group.packets.size() < 10) {
      printf("[LOAD_BZ2] group=%d pkt=%zu: raw src=%u dst=%u size=%d (type=%d addr=%u)\n",
             group_id, group.packets.size(), (unsigned)packet->src, (unsigned)packet->dst,
             templ.size_bytes, (int)packet->type, packet->addr);
    }
    group.packets.push_back(templ);
    nt_packet_free(packet);
  }
  nt_close_trfile(&ctx);

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
  resource_release_cycle_.assign(std::max(0, num_resources_), 0);
  resource_owner_phase_.assign(std::max(0, num_resources_), -1);
  phase_id_to_index_.clear();
  dependents_.clear();
  group_id_to_index_.clear();
  run_id_to_index_.clear();
  phases_.clear();
  group_templates_.clear();
  group_runs_.clear();
  inputs_.assign(std::max(0, num_inputs_), OnlineInputSummary());
  for (int i = 0; i < num_inputs_; ++i) {
    inputs_[i].input_id = i;
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

void OnlineWorkloadScheduler::ProcessFinishedPackets(std::vector<Packet*>& packets) {
  const uint64_t finish_cycle = current_cycle_ == 0 ? 0 : current_cycle_ - 1;
  for (size_t i = 0; i < packets.size(); ++i) {
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
    OnlineGroupRunSummary& run_state = group_runs_[run_it->second];
    if (run_state.remaining_packet_count > 0) {
      --run_state.remaining_packet_count;
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
    OnlineGroupRunSummary run_state;
    run_state.run_id = next_run_id_++;
    run_state.group_id = group_template.group_id;
    run_state.phase_id = phase_state.def.phase_id;
    run_state.input_id = phase_state.def.input_id;
    run_state.template_bz2 = group_template.bz2_path;
    run_state.inject_cycle = current_cycle_;
    run_state.packet_count = static_cast<int>(group_template.packets.size());
    run_state.remaining_packet_count = run_state.packet_count;
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
      NodeID src_nid = network->id2nodeid(templ.src_node);
      NodeID dst_nid = network->id2nodeid(templ.dst_node);
      // DEBUG: print id2nodeid conversion for first packets of each phase
      if (j < 5 || phase_state.def.phase_id == 0) {
        printf("[INJECT_PKT] phase=%d pkt=%zu: templ_src=%d -> nid=(%d:%d), templ_dst=%d -> nid=(%d:%d)\n",
               phase_state.def.phase_id, (int)j,
               templ.src_node, src_nid.node_id, src_nid.chip_id,
               templ.dst_node, dst_nid.node_id, dst_nid.chip_id);
      }
      Packet* packet =
          new Packet(src_nid, dst_nid, BytesToFlits(templ.size_bytes));
      packet->input_id_ = phase_state.def.input_id;
      packet->phase_id_ = phase_state.def.phase_id;
      packet->task_id_ = run_state.run_id;
      packet->template_packet_id_ = templ.packet_id;
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
