#pragma once
#include "system.h"

class Leaf : public Group {
 public:
  Leaf(int num_rails, int num_endpoints, int up_links, int num_vcs, int buffer_size,
       Channel local_channel);
  ~Leaf();
  inline Node* get_leaf_sw(int rail_id) const { return nodes_[num_endpoints_ + rail_id]; }
  void set_group(System* system, int group_id) override;
  int num_rails_;
  int& num_endpoints_;
  int num_up_links_;
  int& leaf_id_;
};

class Spine : public Group {
 public:
  Spine(int num_rails, int num_spine_sw, int down_links, int num_vcs, int buffer_size,
        Channel global_channel);
  ~Spine();
  inline Node* get_spine_sw(int sw_id, int rail_id) { return nodes_[sw_id * num_rails_ + rail_id]; }
  int num_rails_;
  int switch_radix_;
  int& num_spine_sw_;
};

class FatTree : public System {
 public:
  FatTree();
  ~FatTree();
  void read_config() override;
  void print_config() override;
  void connect();
  void routing_algorithm(Packet& s) const override;
  void MIN_routing(Packet& s) const;
  inline Leaf* get_leaf(int leaf_id) const {
    return static_cast<Leaf*>(System::get_group(leaf_id));
  }
  inline Leaf* get_leaf(NodeID id) const {
    return static_cast<Leaf*>(System::get_group(id.group_id));
  }
  inline Spine* get_spine() const {
    return static_cast<Spine*>(System::get_group(num_leaf_sw_));
  }
  inline Spine* get_spine(NodeID id) const {
    return static_cast<Spine*>(System::get_group(id.group_id));
  }
  std::string algorithm_;
  
  int num_rails_;
  int switch_radix_;
  int down_to_up_ratio_;
  int endpoints_per_leaf_;
  int num_leaf_sw_;
  int num_spine_sw_;
  int& num_endpoints_;
  Channel local_channel_;
  Channel global_channel_;
};