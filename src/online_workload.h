#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/property_tree/ptree.hpp>

#include "packet.h"

struct OnlinePacketTemplate {
  int packet_id = -1;
  int src_node = -1;
  int dst_node = -1;
  int size_bytes = 0;
};

struct OnlineGroupTemplate {
  int group_id = -1;
  std::string bz2_path;
  std::vector<OnlinePacketTemplate> packets;
};

struct OnlinePhase {
  int phase_id = -1;
  int input_id = -1;
  int layer_id = -1;
  uint64_t compute_latency_cycles = 0;
  std::vector<int> dep_phase_ids;
  std::vector<int> resource_ids;
  int group_id = -1;
};

struct OnlineInputSummary {
  int input_id = -1;
  int total_phase_count = 0;
  int done_phase_count = 0;
  bool first_injection_seen = false;
  bool all_comm_done_seen = false;
  bool input_done_seen = false;
  uint64_t first_injection_cycle = 0;
  uint64_t all_comm_done_cycle = 0;
  uint64_t input_done_cycle = 0;
};

enum class OnlinePhaseStatus {
  waiting,
  computing,
  ready_to_inject,
  communicating,
  done
};

struct OnlinePhaseState {
  OnlinePhase def;
  OnlinePhaseStatus status = OnlinePhaseStatus::waiting;
  int dep_remaining = 0;
  uint64_t compute_start_cycle = 0;
  uint64_t compute_end_cycle = 0;
  uint64_t comm_start_cycle = 0;
  uint64_t comm_end_cycle = 0;
  int run_id = -1;
};

struct OnlineGroupRunSummary {
  int run_id = -1;
  int group_id = -1;
  int phase_id = -1;
  int input_id = -1;
  std::string template_bz2;
  uint64_t inject_cycle = 0;
  uint64_t finish_cycle = 0;
  int remaining_packet_count = 0;
  int packet_count = 0;
  bool done = false;
};

class OnlineWorkloadScheduler {
 public:
  bool LoadFromFile(const std::string& workload_file, int num_network_nodes, std::string* error);
  bool AllDone() const;
  uint64_t current_cycle() const { return current_cycle_; }
  void ReclaimCompletedPackets(std::vector<Packet*>& packets);
  void ProcessFinishedPackets(std::vector<Packet*>& packets);
  void StartNewComputePhases();
  void PromoteFinishedComputePhases();
  int InjectReadyPackets(std::vector<Packet*>& packets);
  void AdvanceOneCycle();
  bool FastForwardIfIdle();
  void PrintPauseEvents(size_t inflight_packet_count) const;
  void PrintProgressSummary(size_t inflight_packet_count) const;
  void PrintFinalSummary() const;
  bool WriteResultsJson(const std::string& json_path, std::string* error) const;

 private:
  bool LoadFromLegacyJson(const boost::property_tree::ptree& root, std::string* error);
  bool LoadFromManifest(const boost::property_tree::ptree& root,
                        const std::string& workload_file,
                        std::string* error);
  bool LoadGroupTemplate(int group_id, const std::string& bz2_path, std::string* error);
  bool Validate(std::string* error) const;
  bool ResourcesAvailable(const OnlinePhaseState& phase_state) const;
  uint64_t EarliestResourceReadyCycle(const OnlinePhaseState& phase_state) const;
  void MarkPhaseDone(OnlinePhaseState& phase_state);
  void OnGroupRunDone(OnlineGroupRunSummary& run_state, uint64_t finish_cycle);
  void MaybePauseOnFirstInjection(int input_id, size_t inflight_packet_count) const;
  void MaybePauseOnInputDone(int input_id, size_t inflight_packet_count) const;
  static int BytesToFlits(int size_bytes);
  static std::string ParentDir(const std::string& path);
  static std::string JoinPath(const std::string& base_dir, const std::string& child);
  static std::string ReplaceExtension(const std::string& path, const std::string& new_ext);

  int num_inputs_ = 0;
  int num_resources_ = 0;
  int num_network_nodes_ = 0;
  uint64_t current_cycle_ = 0;
  int next_run_id_ = 0;
  std::vector<uint64_t> resource_release_cycle_;
  std::vector<int> resource_owner_phase_;
  std::vector<OnlinePhaseState> phases_;
  std::vector<OnlineGroupTemplate> group_templates_;
  std::vector<OnlineGroupRunSummary> group_runs_;
  std::vector<OnlineInputSummary> inputs_;
  std::unordered_map<int, int> phase_id_to_index_;
  std::unordered_map<int, std::vector<int> > dependents_;
  std::unordered_map<int, int> group_id_to_index_;
  std::unordered_map<int, int> run_id_to_index_;
};
