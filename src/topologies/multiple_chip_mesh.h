#pragma once
#include "chip_mesh.h"
#include "system.h"
#include <set>
#include <unordered_map>

struct ChipletGridInfo {
  int chip_x = 0;
  int chip_y = 0;
  int grid_x = 0;
  int grid_y = 0;
};

class MultiChipMesh : public System {
 public:
  MultiChipMesh();
  ~MultiChipMesh();

  void read_config() override;

  inline NodeID id2nodeid(int id) const override {
    // Check if position cache is loaded (Method 2)
    if (!NodeMesh::position_cache_.empty()) {
      // Method 2: id is py_node_id, lookup from cache
      auto pos_iter = NodeMesh::position_cache_.find(id);
      if (pos_iter != NodeMesh::position_cache_.end()) {
        int x = pos_iter->second.first;
        int y = pos_iter->second.second;

        int chip_id = 0;
        int node_id = 0;
        if (!position_to_nodeid(x, y, &chip_id, &node_id)) {
          chip_id = (x / k_node_) + (y / k_node_) * chip_w_;
          ChipletGridInfo grid = grid_for_chip(chip_id);
          node_id = (x % k_node_) + (y % k_node_) * grid.grid_x;
        }

        // Debug output
        static std::set<int> printed_ids;
        if (param->online_debug && printed_ids.size() < 100 && printed_ids.find(id) == printed_ids.end()) {
          fprintf(stderr, "[METHOD2] py_node_id=%d → pos=(%d,%d) → chip_id=%d, node_id=%d\n",
                  id, x, y, chip_id, node_id);
          printed_ids.insert(id);
        }

        return NodeID(node_id, chip_id);
      }
    }

    // Method 1: id is c_node_id, auto-calculate
    int K_x = k_node_ * chip_w_;
    int x = id % K_x;
    int y = id / K_x;
    int chip_id = 0;
    int node_id = 0;
    if (!position_to_nodeid(x, y, &chip_id, &node_id)) {
      chip_id = (x / k_node_) + (y / k_node_) * chip_w_;
      ChipletGridInfo grid = grid_for_chip(chip_id);
      node_id = (x % k_node_) + (y % k_node_) * grid.grid_x;
    }

    // Debug output for Method 1
    static std::set<int> printed_ids_m1;
    if (param->online_debug && printed_ids_m1.size() < 100 && printed_ids_m1.find(id) == printed_ids_m1.end()) {
      fprintf(stderr, "[METHOD1] c_node_id=%d → pos=(%d,%d) → chip_id=%d, node_id=%d\n",
              id, x, y, chip_id, node_id);
      printed_ids_m1.insert(id);
    }

    return NodeID(node_id, chip_id);
  }
  inline NodeMesh* get_node(NodeID id) const override {
    return dynamic_cast<NodeMesh*>(System::get_node(id));
  }
  inline ChipMesh* get_chip(int chip_id) const override {
    return dynamic_cast<ChipMesh*>(chips_[chip_id]);
  }
  inline ChipMesh* get_chip(NodeID id) const override {
    return dynamic_cast<ChipMesh*>(chips_[id.chip_id]);
  }
  void connect_chiplets();
  bool position_to_nodeid(int x, int y, int* chip_id, int* node_id) const;
  bool is_active_node(NodeID id) const;
  ChipletGridInfo grid_for_chip(int chip_id) const;
  void load_nonuniform_tier_grid();
  void add_channels(Buffer* buffer, Packet& s) const;
  bool route_toward_overlap(Packet& s, int dx, int dy) const;

  void routing_algorithm(Packet& s) const override;
  void XY_routing(Packet& s) const;
  void NFR_routing(Packet& s) const;
  void NFR_adaptive_routing(Packet& s) const;

  std::string algorithm_;

  int k_node_;
  int k_chip_;      // Deprecated: kept for backward compatibility
  int chip_w_;      // Chiplet grid width (NEW)
  int chip_h_;      // Chiplet grid height (NEW)
  bool use_nonuniform_grid_ = false;
  std::unordered_map<int, ChipletGridInfo> chiplet_grids_;

  std::string d2d_IF_;
};
