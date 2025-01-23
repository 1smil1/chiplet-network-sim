#pragma once
#include "group.h"
#include "packet.h"

class TrafficManager;

class System {
 public:
  System();
  static System* New(const std::string&);
  virtual ~System() {}
  virtual void reset();
  virtual void read_config() = 0; // Each system (topology) inport its own parameters.
  virtual void print_config() {
    std::cout << "Number of cores: " << num_cores_ << std::endl;
    std::cout << "Number of nodes: " << num_nodes_ << std::endl;
    std::cout << "Number of groups: " << num_groups_ << std::endl;
  }
  void update(Packet& s);
  void onestage(Packet& s);
  void twostage(Packet& s);
  void Threestage(Packet& s);
  // Three stages of the router
  void routing(Packet& s) const;
  void vc_allocate(Packet& s) const;
  void switch_allocate(Packet& s);
  virtual void routing_algorithm(Packet& s) const = 0;
  // Used in Traffic Generator, convert core_id (int) to NodeID
  virtual NodeID int_to_nodeid(int id) const {
    int node_id = id % groups_[0]->num_cores_;
    int group_id = id / groups_[0]->num_cores_;
    return NodeID(node_id, group_id);
  }
  virtual inline Group* get_group(int group_id) const { return groups_[group_id]; }
  virtual inline Group* get_group(NodeID id) const { return groups_[id.group_id]; }
  virtual inline Node* get_node(NodeID id) const {
    return groups_[id.group_id]->get_node(id.node_id);
  }

  int num_groups_;
  int num_nodes_;  // some nodes are only used for routing (e.g., switches), no packets injected
  int num_cores_;

  // router parameterss
  std::string router_stages_;
  std::string algorithm_;

  // simulation parameters
  int timeout_time_;

  friend TrafficManager;

 protected:
  std::vector<Group*> groups_;
};
