#include "railx.h"

ChipletInMesh::ChipletInMesh(int m_scale, int n_port, int vc_num, int buffer_size,
                             Channel internal_channel)
    : Node(4 * n_port, vc_num, buffer_size), HB_mesh_(group_) {
  HB_mesh_ = nullptr;
  n_port_ = n_port;
  m_scale_ = m_scale;
  x_ = 0;
  y_ = 0;
  for (int i = 0; i < 4; i++) {
    in_buffers_[i]->channel_ = internal_channel;
    for (int j = 0; j < n_port; j++) {
      in_buffers_[i * n_port + j]->channel_ = internal_channel;
    }
  }
  for (int j = 0; j < n_port; j++) {
    xneg_in_buffers_.push_back(in_buffers_[j]);
    xpos_in_buffers_.push_back(in_buffers_[n_port + j]);
    yneg_in_buffers_.push_back(in_buffers_[2 * n_port + j]);
    ypos_in_buffers_.push_back(in_buffers_[3 * n_port + j]);
    xneg_link_nodes_.push_back(std::ref(link_nodes_[j]));
    xpos_link_nodes_.push_back(std::ref(link_nodes_[n_port + j]));
    yneg_link_nodes_.push_back(std::ref(link_nodes_[2 * n_port + j]));
    ypos_link_nodes_.push_back(std::ref(link_nodes_[3 * n_port + j]));
    xneg_link_buffers_.push_back(std::ref(link_buffers_[j]));
    xpos_link_buffers_.push_back(std::ref(link_buffers_[n_port + j]));
    yneg_link_buffers_.push_back(std::ref(link_buffers_[2 * n_port + j]));
    ypos_link_buffers_.push_back(std::ref(link_buffers_[3 * n_port + j]));
    xneg_ports_.push_back(ports_[j]);
    xpos_ports_.push_back(ports_[n_port + j]);
    yneg_ports_.push_back(ports_[2 * n_port + j]);
    ypos_ports_.push_back(ports_[3 * n_port + j]);
  }
}

void ChipletInMesh::set_node(Group* HB_mesh, NodeID id) {
  assert(HB_mesh != nullptr);
  id_ = id;
  HB_mesh_ = dynamic_cast<HBMesh*>(HB_mesh);
  x_ = id_.node_id % m_scale_;
  y_ = id_.node_id / m_scale_;
}

HBMesh::HBMesh(int m_scale, int n_port, int vc_num, int buffer_size, Channel internal_channel,
               Channel external_channel)
    : num_chiplets_(num_nodes_), mesh_id_(group_id_), coordinate_(group_coordinate_), railx_(system_) {
  external_channel_ = external_channel;
  m_scale_ = m_scale;
  n_port_ = n_port;
  num_rail_ = m_scale_ * n_port_;
  num_chiplets_ = m_scale * m_scale;
  num_cores_ = num_chiplets_;
  xneg_rail_ports_.resize(num_rail_);
  xpos_rail_ports_.resize(num_rail_);
  yneg_rail_ports_.resize(num_rail_);
  ypos_rail_ports_.resize(num_rail_);
  for (int i = 0; i < num_chiplets_; i++) {
    nodes_.push_back(new ChipletInMesh(m_scale_, n_port_, vc_num, buffer_size, internal_channel));
  }
}

HBMesh::~HBMesh() {
  for (auto chiplet : nodes_) delete chiplet;
  nodes_.clear();
}

void HBMesh::set_group(System* system, int mesh_id) {
  Group::set_group(system, mesh_id);
  railx_ = system_;
  coordinate_.resize(2);
  coordinate_[0] = mesh_id % (num_rail_ + 1);
  coordinate_[1] = mesh_id / (num_rail_ + 1);
  for (int node_id = 0; node_id < num_nodes_; node_id++) {
    ChipletInMesh* chiplet = get_chiplet(node_id);
    if (chiplet->x_ != 0) {
      for (int i = 0; i < n_port_; i++) {
        chiplet->xneg_link_nodes_[i].get() = NodeID(node_id - 1, mesh_id);
        chiplet->xneg_link_buffers_[i].get() =
            get_chiplet(chiplet->xneg_link_nodes_[i])->xpos_in_buffers_[i];
      }
    } else {
      for (int i = 0; i < n_port_; i++) {
        chiplet->xneg_in_buffers_[i]->channel_ = external_channel_;
        xneg_rail_ports_[chiplet->y_ * n_port_ + i] = chiplet->xneg_ports_[i];
      }
    }
    if (chiplet->x_ != m_scale_ - 1) {
      for (int i = 0; i < n_port_; i++) {
        chiplet->xpos_link_nodes_[i].get() = NodeID(node_id + 1, mesh_id);
        chiplet->xpos_link_buffers_[i].get() =
            get_chiplet(chiplet->xpos_link_nodes_[i])->xneg_in_buffers_[i];
      }
    } else {
      for (int i = 0; i < n_port_; i++) {
        chiplet->xpos_in_buffers_[i]->channel_ = external_channel_;
        xpos_rail_ports_[chiplet->y_ * n_port_ + i] = chiplet->xpos_ports_[i];
      }
    }
    if (chiplet->y_ != 0) {
      for (int i = 0; i < n_port_; i++) {
        chiplet->yneg_link_nodes_[i].get() = NodeID(node_id - m_scale_, mesh_id);
        chiplet->yneg_link_buffers_[i].get() =
            get_chiplet(chiplet->yneg_link_nodes_[i])->ypos_in_buffers_[i];
      }
    } else {
      for (int i = 0; i < n_port_; i++) {
        chiplet->yneg_in_buffers_[i]->channel_ = external_channel_;
        yneg_rail_ports_[chiplet->x_ * n_port_ + i] = chiplet->yneg_ports_[i];
      }
    }
    if (chiplet->y_ != m_scale_ - 1) {
      for (int i = 0; i < n_port_; i++) {
        chiplet->ypos_link_nodes_[i].get() = NodeID(node_id + m_scale_, mesh_id);
        chiplet->ypos_link_buffers_[i].get() =
            get_chiplet(chiplet->ypos_link_nodes_[i])->yneg_in_buffers_[i];
      }
    } else {
      for (int i = 0; i < n_port_; i++) {
        chiplet->ypos_in_buffers_[i]->channel_ = external_channel_;
        ypos_rail_ports_[chiplet->x_ * n_port_ + i] = chiplet->ypos_ports_[i];
      }
    }
  }
}