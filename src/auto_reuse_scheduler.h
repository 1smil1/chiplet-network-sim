#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>
#include <boost/property_tree/ptree.hpp>

// Mapping policy only: the online kernel owns dependencies and compute leases.
// A binding reserves an address, never a FIFO position or a compute lease.
class AutoReuseScheduler {
 public:
  struct Layer {
    int block;
    std::vector<int> virtuals, canonical;
    std::vector<std::vector<int> > candidates;
  };
  struct Binding { int pool; uint64_t cycle; };
  bool enabled = false;
  bool dynamic = false;
  int resolve_mode = 1;
  int pools = 0;
  std::map<int, Layer> layers;
  std::map<int, int> virtual_layer, physical_cnode;
  std::map<int, int> physical_tier;
  std::map<std::pair<int, int>, Binding> bindings;

  static std::vector<int> Ints(const boost::property_tree::ptree& root, const char* key) {
    std::vector<int> result;
    for (const auto& item : root.get_child(key)) result.push_back(item.second.get_value<int>());
    return result;
  }

  void Load(const boost::property_tree::ptree& root, int num_resources, int num_nodes) {
    enabled = true;
    dynamic = root.get<int>("auto_reuse") == 1;
    resolve_mode = root.get<int>("resolve_mode");
    pools = root.get<int>("num_block_resources");
    if (root.get<int>("version") != 1 || pools <= 0 || (resolve_mode != 0 && resolve_mode != 1))
      throw std::runtime_error("Invalid auto reuse catalog version/pools/resolve mode");
    for (const auto& item : root.get_child("physical_nodes")) {
      int p = item.second.get<int>("physical_id"), c = item.second.get<int>("c_node_id");
      int tier = item.second.get<int>("tier_id");
      if (tier < 0) throw std::runtime_error("Negative physical tier");
      physical_tier.emplace(p, tier);
      if (p < 0 || p >= num_resources || c < 0 || c >= num_nodes || !physical_cnode.emplace(p, c).second)
        throw std::runtime_error("Invalid/duplicate catalog physical node");
    }
    for (const auto& item : root.get_child("layers")) {
      int id = item.second.get<int>("layer_id");
      Layer layer;
      layer.block = item.second.get<int>("block_id");
      layer.virtuals = Ints(item.second, "virtual_ids");
      layer.canonical = Ints(item.second, "canonical");
      for (const auto& candidate : item.second.get_child("candidates")) {
        std::vector<int> bank;
        for (const auto& p : candidate.second) bank.push_back(p.second.get_value<int>());
        layer.candidates.push_back(bank);
      }
      if (layer.virtuals.empty() || layer.virtuals.size() != layer.canonical.size() ||
          layer.candidates.size() != static_cast<size_t>(layer.block < 0 ? 1 : pools))
        throw std::runtime_error("Invalid catalog layer shape");
      for (const auto& candidate : layer.candidates) {
        if (candidate.size() != layer.virtuals.size() || std::set<int>(candidate.begin(), candidate.end()).size() != candidate.size())
          throw std::runtime_error("Invalid catalog candidate shape");
        for (int p : candidate) if (!physical_cnode.count(p)) throw std::runtime_error("Missing physical position");
      }
      for (int v : layer.virtuals)
        if (v < 0 || !virtual_layer.emplace(v, id).second) throw std::runtime_error("Duplicate virtual endpoint");
      if (!layers.emplace(id, layer).second) throw std::runtime_error("Duplicate catalog layer");
    }
  }

  bool IsDynamic(int lid) const { return enabled && dynamic && layers.at(lid).block >= 0; }

  void Bind(int input, int lid, uint64_t now,
            const std::function<std::pair<uint64_t, uint64_t>(const std::vector<int>&)>& estimate) {
    if (!IsDynamic(lid)) return;
    const Layer& layer = layers.at(lid);
    const auto key = std::make_pair(input, layer.block);
    if (bindings.count(key)) return;
    int best = 0;
    auto score = estimate(layer.candidates[0]);
    for (int p = 1; p < pools; ++p) {
      auto candidate = estimate(layer.candidates[p]);
      if (candidate < score) { best = p; score = candidate; }
    }
    bindings.emplace(key, Binding{best, now});
  }

  const std::vector<int>& Resources(int input, int lid) const {
    const Layer& layer = layers.at(lid);
    if (!IsDynamic(lid)) return layer.canonical;
    return layer.candidates.at(bindings.at(std::make_pair(input, layer.block)).pool);
  }

  int Physical(int input, int virtual_id) const {
    int lid = virtual_layer.at(virtual_id);
    const Layer& layer = layers.at(lid);
    auto offset = std::find(layer.virtuals.begin(), layer.virtuals.end(), virtual_id) - layer.virtuals.begin();
    return Resources(input, lid).at(offset);
  }

  int Endpoint(int input, int virtual_id) const {
    int physical = Physical(input, virtual_id);
    return resolve_mode == 0 ? physical_cnode.at(physical) : physical;
  }
};
