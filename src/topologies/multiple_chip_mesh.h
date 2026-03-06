#pragma once
#include "chip_mesh.h"
#include "system.h"
#include <set>

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

        // Calculate chip_id and node_id from position
        int chip_x = x / k_node_;
        int chip_y = y / k_node_;
        int chip_id = chip_x + chip_y * chip_w_;

        int local_x = x % k_node_;
        int local_y = y % k_node_;
        int node_id = local_x + local_y * k_node_;

        // Debug output
        static std::set<int> printed_ids;
        if (printed_ids.size() < 100 && printed_ids.find(id) == printed_ids.end()) {
          fprintf(stderr, "[METHOD2] py_node_id=%d → pos=(%d,%d) → chip_id=%d, node_id=%d\n",
                  id, x, y, chip_id, node_id);
          printed_ids.insert(id);
        }

        return NodeID(node_id, chip_id);
      }
    }

    // Method 1: id is c_node_id, auto-calculate
    int K_x = k_node_ * chip_w_;
    int K_y = k_node_ * chip_h_;
    int x = id % K_x;
    int y = id / K_x;
    int node_id = x % k_node_ + (y % k_node_) * k_node_;
    int chip_id = x / k_node_ + (y / k_node_) * chip_w_;

    // Debug output for Method 1
    static std::set<int> printed_ids_m1;
    if (printed_ids_m1.size() < 100 && printed_ids_m1.find(id) == printed_ids_m1.end()) {
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

  void routing_algorithm(Packet& s) const override;
  void XY_routing(Packet& s) const;
  void NFR_routing(Packet& s) const;
  void NFR_adaptive_routing(Packet& s) const;

  std::string algorithm_;

  int k_node_;
  int k_chip_;      // Deprecated: kept for backward compatibility
  int chip_w_;      // Chiplet grid width (NEW)
  int chip_h_;      // Chiplet grid height (NEW)

  std::string d2d_IF_;
};