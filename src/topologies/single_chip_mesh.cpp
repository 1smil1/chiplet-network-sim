#include "single_chip_mesh.h"

NodeMesh::NodeMesh(int k_node, int vc_num, int buffer_size)
    : Node(4, vc_num, buffer_size),
      chip_(group_),
      xneg_in_buffer_(in_buffers_[0]),
      xpos_in_buffer_(in_buffers_[1]),
      yneg_in_buffer_(in_buffers_[2]),
      ypos_in_buffer_(in_buffers_[3]),
      xneg_link_node_(link_nodes_[0]),
      xpos_link_node_(link_nodes_[1]),
      yneg_link_node_(link_nodes_[2]),
      ypos_link_node_(link_nodes_[3]),
      xneg_link_buffer_(link_buffers_[0]),
      xpos_link_buffer_(link_buffers_[1]),
      yneg_link_buffer_(link_buffers_[2]),
      ypos_link_buffer_(link_buffers_[3]) {
  x_ = 0;
  y_ = 0;
  k_node_ = k_node;
}

void NodeMesh::set_node(Group *chip, NodeID id) {
  chip_ = chip;
  id_ = id;
  x_ = id.node_id % k_node_;
  y_ = id.node_id / k_node_;
}

ChipMesh::ChipMesh(int k_node, int vc_num, int buffer_size) : chip_coordinate_(group_coordinate_) {
  k_node_ = k_node;
  number_nodes_ = k_node_ * k_node_;
  number_cores_ = number_nodes_;
  nodes_.reserve(number_nodes_);
  for (int node_id = 0; node_id < number_nodes_; node_id++) {
    nodes_.push_back(new NodeMesh(k_node_, vc_num, buffer_size));
  }
}

ChipMesh::~ChipMesh() {
  for (auto node : nodes_) {
    delete node;
  }
  nodes_.clear();
}

void ChipMesh::set_group(System *system, int chip_id) {
  Group::set_group(system, chip_id);
  for (int node_id = 0; node_id < number_nodes_; node_id++) {
    NodeMesh *node = get_node(node_id);
    if (node->x_ != 0) {
      node->xneg_link_node_ = NodeID(node_id - 1, chip_id);
      node->xneg_link_buffer_ = get_node(node->xneg_link_node_)->xpos_in_buffer_;
    }
    if (node->x_ != k_node_ - 1) {
      node->xpos_link_node_ = NodeID(node_id + 1, chip_id);
      node->xpos_link_buffer_ = get_node(node->xpos_link_node_)->xneg_in_buffer_;
    }
    if (node->y_ != 0) {
      node->yneg_link_node_ = NodeID(node_id - k_node_, chip_id);
      node->yneg_link_buffer_ = get_node(node->yneg_link_node_)->ypos_in_buffer_;
    }
    if (node->y_ != k_node_ - 1) {
      node->ypos_link_node_ = NodeID(node_id + k_node_, chip_id);
      node->ypos_link_buffer_ = get_node(node->ypos_link_node_)->yneg_in_buffer_;
    }
  }
}

SingleChipMesh::SingleChipMesh() : chips_(groups_) {
  read_config();
  num_groups_ = 1;
  num_nodes_ = k_node_ * k_node_;
  num_cores_ = num_nodes_;
  chips_.push_back(new ChipMesh(k_node_, param->vc_number, param->buffer_size));
  chips_[0]->set_group(this, 0);
}

SingleChipMesh::~SingleChipMesh() {
  delete chips_[0];
  chips_.clear();
}

void SingleChipMesh::read_config() {
  k_node_ = param->params_ptree.get<int>("Network.scale", 4);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "XY");
  if (algorithm_ == "NFR_adaptive") assert(param->vc_number >= 2);
  printf("Single Chip 2D-mesh, %ix%i\n", k_node_, k_node_);
}

void SingleChipMesh::routing_algorithm(Packet &s) const {
  if (algorithm_ == "XY")
    XY_routing(s);
  else if (algorithm_ == "NFR")
    NFR_routing(s);
  else if (algorithm_ == "NFR_adaptive")
    NFR_adaptive_routing(s);
  else
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}

void SingleChipMesh::XY_routing(Packet &s) const {
  NodeMesh *current_node = get_node(s.head_trace().id);
  NodeMesh *destination_node = get_node(s.destination_);

  int cur_x = current_node->x_;
  int cur_y = current_node->y_;
  int dest_x = destination_node->x_;
  int dest_y = destination_node->y_;
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  if (dis_x < 0)  // first x
    for (int i = 0; i < current_node->xneg_link_buffer_->vc_num_; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->xneg_link_buffer_, i));
  else if (dis_x > 0)
    for (int i = 0; i < current_node->xpos_link_buffer_->vc_num_; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->xpos_link_buffer_, i));
  else if (dis_x == 0) {
    if (dis_y < 0)  // then y
      for (int i = 0; i < current_node->yneg_link_buffer_->vc_num_; i++)
        s.candidate_channels_.push_back(VCInfo(current_node->yneg_link_buffer_, i));
    else if (dis_y > 0)
      for (int i = 0; i < current_node->ypos_link_buffer_->vc_num_; i++)
        s.candidate_channels_.push_back(VCInfo(current_node->ypos_link_buffer_, i));
  }
}

void SingleChipMesh::NFR_routing(Packet &s) const {
  NodeMesh *current_node = get_node(s.head_trace().id);
  NodeMesh *destination_node = get_node(s.destination_);

  int cur_x = current_node->x_;
  int cur_y = current_node->y_;
  int dest_x = destination_node->x_;
  int dest_y = destination_node->y_;
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  // Baseline routing: negative-first
  if (dis_x < 0 || dis_y < 0) {
    if (dis_x < 0)
      for (int i = 0; i < current_node->xneg_link_buffer_->vc_num_; i++)
        s.candidate_channels_.push_back(VCInfo(current_node->xneg_link_buffer_, i));
    if (dis_y < 0)
      for (int i = 0; i < current_node->yneg_link_buffer_->vc_num_; i++)
        s.candidate_channels_.push_back(VCInfo(current_node->yneg_link_buffer_, i));
  } else {
    if (dis_x > 0)
      for (int i = 0; i < current_node->xpos_link_buffer_->vc_num_; i++)
        s.candidate_channels_.push_back(VCInfo(current_node->xpos_link_buffer_, i));
    if (dis_y > 0)
      for (int i = 0; i < current_node->ypos_link_buffer_->vc_num_; i++)
        s.candidate_channels_.push_back(VCInfo(current_node->ypos_link_buffer_, i));
  }
}

void SingleChipMesh::NFR_adaptive_routing(Packet &s) const {
  NodeMesh *current_node = get_node(s.head_trace().id);
  NodeMesh *destination_node = get_node(s.destination_);

  int cur_x = current_node->x_;
  int cur_y = current_node->y_;
  int dest_x = destination_node->x_;
  int dest_y = destination_node->y_;
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  // Adaptive Routing Channels
  if (dis_x < 0)
    for (int i = 0; i < current_node->xneg_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->xneg_link_buffer_, i));
  else if (dis_x > 0)
    for (int i = 0; i < current_node->xpos_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->xpos_link_buffer_, i));
  if (dis_y < 0)
    for (int i = 0; i < current_node->yneg_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->yneg_link_buffer_, i));
  else if (dis_y > 0)
    for (int i = 0; i < current_node->ypos_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->ypos_link_buffer_, i));

  // Baseline routing: negative-first
  if (dis_x < 0 || dis_y < 0) {
    if (dis_x < 0)
      s.candidate_channels_.push_back(
          VCInfo(current_node->xneg_link_buffer_, current_node->xneg_link_buffer_->vc_num_ - 1));
    if (dis_y < 0)
      s.candidate_channels_.push_back(
          VCInfo(current_node->yneg_link_buffer_, current_node->yneg_link_buffer_->vc_num_ - 1));
  } else {
    if (dis_x > 0)
      s.candidate_channels_.push_back(
          VCInfo(current_node->xpos_link_buffer_, current_node->xpos_link_buffer_->vc_num_ - 1));
    if (dis_y > 0)
      s.candidate_channels_.push_back(
          VCInfo(current_node->ypos_link_buffer_, current_node->ypos_link_buffer_->vc_num_ - 1));
  }
}