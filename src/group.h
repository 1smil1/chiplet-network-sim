#pragma once
#include "node.h"

// Group class is an intermediate class between System and Node
// A group is a sub-network of the system network.
// In a multi-chiplet System, a Group is typically a network-on-chiplet.
// In a scale-out System, a Group is typically a package or a scale-up domain, and a Node is a chip.
class Group {
 public:
  Group();
  ~Group();

  virtual void set_group(System* system, int group_id_);
  void reset();

  virtual inline Node* get_node(int node_id) const { return nodes_[node_id]; }
  virtual inline Node* get_node(NodeID id) const  { return nodes_[id.node_id]; }

  System* system_;  // Point to the upper level group

  int group_id_;
  std::vector<int> group_coordinate_;
  int num_nodes_;
  int num_cores_;

  friend TrafficManager;
 protected:
  std::vector<Node*> nodes_;
};
