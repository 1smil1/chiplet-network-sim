#include "fat_tree.h"
#include <algorithm>
#include <random>

Leaf::Leaf(int num_rails, int num_endpoints, int num_up_links, int num_vcs, int buffer_size,
           Channel local_channel, Channel global_channel)
    : leaf_id_(group_id_), num_endpoints_(num_cores_) {
  num_rails_ = num_rails;
  num_endpoints_ = num_endpoints;
  num_nodes_ = num_endpoints_ + num_rails_;
  num_up_links_ = num_up_links;
  for (int i = 0; i < num_endpoints_; i++) {
    nodes_.push_back(new Node(num_rails, num_vcs, buffer_size, local_channel));
  }
  for (int i = 0; i < num_rails; i++) {
    nodes_.push_back(new Node(num_endpoints + num_up_links, num_vcs, buffer_size, local_channel));
  }
  for (int rail = 0; rail < num_rails_; rail++) {
    Node* sw = get_node(num_endpoints_ + rail);
    for (int i = 0; i < num_up_links; i++) {
      sw->in_buffers_[num_endpoints_ + i]->channel_ = global_channel;
    }
  }
}

Leaf::~Leaf() {
  for (auto node : nodes_) {
    delete node;
  }
  nodes_.clear();
}

void Leaf::set_group(System* system, int leaf_id) {
  Group::set_group(system, leaf_id);
  // connect endpoints to switch
  for (int rail = 0; rail < num_rails_; rail++) {
    Node* sw = get_node(num_endpoints_ + rail);
    for (int i = 0; i < num_endpoints_; i++) {
      Node* endpoint = get_node(i);
      Port::connect_port(endpoint->ports_[rail], sw->ports_[i]);
    }
  }
}

Spine::Spine(int num_rails, int num_spine_sw_per_rail, int num_down_links, int num_vcs, int buffer_size, Channel global_channel)
    : num_spine_sw_(num_nodes_) {
  num_rails_ = num_rails;
  switch_radix_ = num_down_links;
  num_spine_sw_per_rail_ = num_spine_sw_per_rail;
  num_spine_sw_ = num_spine_sw_per_rail_ * num_rails_;
  for (int rail = 0; rail < num_rails; rail++) {
    for (int i = 0; i < num_spine_sw_per_rail; i++) {
      nodes_.push_back(new Node(switch_radix_, num_vcs, buffer_size, global_channel));
    }
  }
}

Spine::~Spine() {
  for (auto node : nodes_) {
    delete node;
  }
  nodes_.clear();
}

FatTree::FatTree() : num_endpoints_(num_cores_) {
  read_config();
  num_leaf_sw_per_rail_ = switch_radix_;
  num_spine_sw_per_rail_ = std::floor(switch_radix_ / (down_to_up_ratio_ + 1));
  num_leaf_sw_ = num_leaf_sw_per_rail_ * num_rails_;
  num_spine_sw_ = num_spine_sw_per_rail_ * num_rails_;
  endpoints_per_leaf_ = switch_radix_ - num_spine_sw_per_rail_;
  num_endpoints_ = num_leaf_sw_per_rail_ * endpoints_per_leaf_;
  num_nodes_ = num_endpoints_ + num_leaf_sw_ + num_spine_sw_;
  num_groups_ = num_leaf_sw_per_rail_ + 1;
  groups_.reserve(num_groups_);
  for (int leaf_id = 0; leaf_id < num_leaf_sw_per_rail_; leaf_id++) {
    groups_.push_back(new Leaf(num_rails_, endpoints_per_leaf_, num_spine_sw_per_rail_, param->vc_number,
                               param->buffer_size, local_channel_, global_channel_));
    groups_[leaf_id]->set_group(this, leaf_id);
  }
  groups_.push_back(new Spine(num_rails_, num_spine_sw_per_rail_, num_leaf_sw_per_rail_, param->vc_number,
                              param->buffer_size, global_channel_));
  groups_[num_leaf_sw_per_rail_]->set_group(this, num_leaf_sw_per_rail_);
  connect();
  print_config();
}

FatTree::~FatTree() {
  for (auto group : groups_) {
    delete group;
  }
  groups_.clear();
}

void FatTree::read_config() {
  switch_radix_ = param->params_ptree.get<int>("Network.switch_radix", 64);
  down_to_up_ratio_ = param->params_ptree.get<int>("Network.down_to_up_ratio", 1);
  num_rails_ = param->params_ptree.get<int>("Network.num_rails", 1);
  //assert(switch_radix_ % (down_to_up_ratio_ + 1) == 0);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "MIN");
  int local_latency = param->params_ptree.get<int>("Network.local_latency", 4);
  int global_latency = param->params_ptree.get<int>("Network.global_latency", 10);
  local_channel_ = Channel(1, local_latency);
  global_channel_ = Channel(1, global_latency);
}

void FatTree::print_config() {
  std::cout << "Number of cores: " << num_cores_ << std::endl;
  std::cout << "Number of nodes: " << num_nodes_ << std::endl;
  std::cout << "Number of groups: " << num_groups_ << std::endl;
  std::cout << "FatTree parameters: " << std::endl;
  std::cout << "switch_radix: " << switch_radix_ << std::endl;
  std::cout << "down_to_up_ratio: " << down_to_up_ratio_ << std::endl;
  std::cout << "num_rails: " << num_rails_ << std::endl;
  std::cout << "num_leaf_sw: " << num_leaf_sw_per_rail_ << std::endl;
  std::cout << "num_spine_sw: " << num_spine_sw_per_rail_ << std::endl;
  std::cout << "endpoints_per_leaf: " << endpoints_per_leaf_ << std::endl;
}



void FatTree::connect() {
  // connect leaf switches to spine switches
  Spine* spine = get_spine();
  NodeID spine_sw_id = NodeID(0, num_groups_);
  for (int rail = 0; rail < num_rails_; rail++) {
    for (int i = 0; i < spine->num_spine_sw_per_rail_; i++) {
      for (int j = 0; j < num_leaf_sw_per_rail_; j++) {
        Port* port1 = spine->get_spine_sw(i, rail)->ports_[j];
        // 0..num_endpoints_per_leaf_ - 1 ports are endpoints
        Port* port2 = get_leaf(j)->get_leaf_sw(rail)->ports_[endpoints_per_leaf_ + i];
        Port::connect_port(port1, port2);
      }
    }
  }
}

void FatTree::routing_algorithm(Packet& s) const {
  if (algorithm_ == "MIN")
    MIN_routing(s);
  else
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}

void FatTree::MIN_routing(Packet& s) const { 
  Node* cur_node = get_node(s.head_trace().id);
  Node* dest_node = get_node(s.destination_);

  if (cur_node->group_id_ < num_leaf_sw_per_rail_ &&
      cur_node->node_id_ < endpoints_per_leaf_) {  // current node is endpoint
    for (int r = 0; r < num_rails_; r++) {
      int rail = (rand() + r) % num_rails_;
      Buffer* next_buffer = cur_node->link_buffers_[rail];
      for (int i = 0; i < param->vc_number; i++) {
        s.candidate_channels_.push_back(VCInfo(next_buffer, i));
      }
    }
  } 
  // current node is switch
  else if (cur_node->group_id_ == dest_node->group_id_) { // same leaf
    assert(cur_node->node_id_ >= endpoints_per_leaf_);       // current node is leaf switch
    Buffer* next_buffer = cur_node->link_buffers_[dest_node->node_id_];
    for (int i = 0; i < param->vc_number; i++) {
      s.candidate_channels_.push_back(VCInfo(next_buffer, i));
    }
  }
  else if (cur_node->group_id_ == num_groups_ - 1) { // current node is spine switch
    assert(cur_node->group_id_ == num_leaf_sw_per_rail_);           
    //std::cout << "spine: " << cur_node->node_id_ << std::endl;
    Buffer* next_buffer = cur_node->link_buffers_[dest_node->group_id_];
    for (int i = 0; i < param->vc_number; i++) {
      s.candidate_channels_.push_back(VCInfo(next_buffer, i));
    }
  } 
  else if (cur_node->group_id_ != dest_node->group_id_) {  // different leaf
    assert(cur_node->node_id_ >= endpoints_per_leaf_);         // current node is leaf switch
    int randint = rand();
    for (int j = 0; j < num_spine_sw_per_rail_; ++j) {
      int spine = (j + randint) % num_spine_sw_per_rail_;
      // 0..num_endpoints_per_leaf_ - 1 ports are endpoints
      Buffer* next_buffer = cur_node->link_buffers_[endpoints_per_leaf_ + spine];
      for (int i = 0; i < param->vc_number; i++) {
        s.candidate_channels_.push_back(VCInfo(next_buffer, i));
      }
    }
  }
}