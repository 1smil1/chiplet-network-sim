#include "railx_2d_hyperx.h"

ChipletInMesh::ChipletInMesh(int m_scale, int n_port, int vc_num, int buffer_size,
                             Channel internal_channel)
    : Node(4 * n_port, vc_num, buffer_size), HB_mesh_(group_) {
  HB_mesh_ = nullptr;
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
    xneg_link_buffers_.push_back(link_buffers_[j]);
    xpos_link_buffers_.push_back(link_buffers_[n_port + j]);
    yneg_link_buffers_.push_back(link_buffers_[2 * n_port + j]);
    ypos_link_buffers_.push_back(link_buffers_[3 * n_port + j]);
    xneg_ports_.push_back(ports_[j]);
    xpos_ports_.push_back(ports_[n_port + j]);
    yneg_ports_.push_back(ports_[2 * n_port + j]);
    ypos_ports_.push_back(ports_[3 * n_port + j]);
  }
  //for (int j = 0; j < n_port; j++) {
  //  std::cout << "xneg_ports [" << j << "]:" << xneg_ports_[j]->node_id
  //            << " link_node:" << xneg_ports_[j]->link_node << std::endl;
  //  std::cout << "xpos_ports [" << j << "]:" << xpos_ports_[j]->node_id
  //            << " link_node:" << xpos_ports_[j]->link_node << std::endl;
  //  std::cout << "yneg_ports [" << j << "]:" << yneg_ports_[j]->node_id
  //            << " link_node:" << yneg_ports_[j]->link_node << std::endl;
  //  std::cout << "ypos_ports [" << j << "]:" << ypos_ports_[j]->node_id
  //            << " link_node:" << ypos_ports_[j]->link_node << std::endl;
  //}
}

void ChipletInMesh::set_node(Group* HB_mesh, NodeID id) {
  assert(HB_mesh != nullptr);
  id_ = id;
  HB_mesh_ = dynamic_cast<HBMesh*>(HB_mesh);
  x_ = id_.node_id % m_scale_;
  y_ = id_.node_id / m_scale_;
}

int ChipletInMesh::man_distance(ChipletInMesh* chiplet_1, ChipletInMesh* chiplet_2) { 
  assert(chiplet_1->id_.group_id == chiplet_2->id_.group_id);
  return abs(chiplet_1->x_ - chiplet_2->x_) + abs(chiplet_1->y_ - chiplet_2->y_);
}

HBMesh::HBMesh(int m_scale, int n_port, int vc_num, int buffer_size, Channel internal_channel,
               Channel external_channel)
    : num_chiplets_(num_nodes_), mesh_id_(group_id_), hyperx_coordinate_(group_coordinate_) {
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
  railx_ = dynamic_cast<RailX*>(system_);
  hyperx_coordinate_.resize(2);
  hyperx_coordinate_[0] = mesh_id % (num_rail_ + 1);
  hyperx_coordinate_[1] = mesh_id / (num_rail_ + 1);
  for (int node_id = 0; node_id < num_nodes_; node_id++) {
    ChipletInMesh* chiplet = get_chiplet(node_id);
    if (chiplet->x_ != 0) {
      for (int i = 0; i < n_port_; i++) {
        chiplet->xneg_link_nodes_[i].get() = NodeID(node_id - 1, mesh_id);
        chiplet->xneg_link_buffers_[i] =
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
        chiplet->xpos_link_buffers_[i] =
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
        chiplet->yneg_link_buffers_[i] =
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
        chiplet->ypos_link_buffers_[i] =
            get_chiplet(chiplet->ypos_link_nodes_[i])->yneg_in_buffers_[i];
      }
    } else {
      for (int i = 0; i < n_port_; i++) {
        chiplet->ypos_in_buffers_[i]->channel_ = external_channel_;
        ypos_rail_ports_[chiplet->x_ * n_port_ + i] = chiplet->ypos_ports_[i];
      }
    }
  }
  //for (int i = 0; i < num_rail_; i++) {
  //  std::cout << "xneg_rail_ports_[" << i << "]:" << xneg_rail_ports_[i]->node_id
  //            << " link_node:" << xneg_rail_ports_[i]->link_node.node_id << std::endl;
  //  std::cout << "xpos_rail_ports_[" << i << "]:" << xpos_rail_ports_[i]->node_id
  //            << " link_node:" << xpos_rail_ports_[i]->link_node.node_id << std::endl;
  //  std::cout << "yneg_rail_ports_[" << i << "]:" << yneg_rail_ports_[i]->node_id
  //            << " link_node:" << yneg_rail_ports_[i]->link_node.node_id << std::endl;
  //  std::cout << "ypos_rail_ports_[" << i << "]:" << ypos_rail_ports_[i]->node_id
  //            << " link_node:" << ypos_rail_ports_[i]->link_node.node_id << std::endl;
  //}
 }

RailX::RailX() : num_mesh_(num_groups_), meshes_(groups_) {
  read_config();
  num_rail_ = m_scale_ * n_port_;
  num_mesh_ = (num_rail_ + 1) * (num_rail_ + 1);
  num_nodes_ = num_mesh_ * (m_scale_ * m_scale_);
  num_cores_ = num_nodes_;
  meshes_.reserve(num_mesh_);
  for (int mesh_id = 0; mesh_id < num_mesh_; mesh_id++) {
    meshes_.push_back(new HBMesh(m_scale_, n_port_, param->vc_number, param->buffer_size,
                                 internal_HB_link, external_link));
    meshes_[mesh_id]->set_group(this, mesh_id);
  }
  hamilton_decomp_ = gen_hamilton_decomp_odd(num_rail_ + 1);
  for (auto& v : hamilton_decomp_) {
    for (auto& i : v) {
      std::cout << i << " ";
    }
    std::cout << std::endl;
  }
  connect();
}

RailX::~RailX() {
  for (auto mesh : meshes_) delete mesh;
  meshes_.clear();
}

void RailX::read_config() {
  m_scale_ = param->params_ptree.get<int>("Network.m_scale", 4);
  n_port_ = param->params_ptree.get<int>("Network.n_port", 4);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "MIN");
  int internal_bandiwdth = param->params_ptree.get<int>("Network.internal_bandwidth", 1);
  int external_latency = param->params_ptree.get<int>("Network.external_latency", 4);
  internal_HB_link = Channel(internal_bandiwdth, 1);
  external_link = Channel(1, external_latency);
}

void RailX::connect() {
  // rail-ring-based all-to-all connection
  for (int i = 0; i < num_rail_; i++) { // rail-i
    for (int j = 0; j < num_rail_ + 1; j++) {    // rail-ring
      for (int k = 0; k < num_rail_ + 1; k++) {  // for each row/column
        // X-rails
        int mesh_id_1 = k * (num_rail_ + 1) + hamilton_decomp_[i][j];
        int mesh_id_2 = k * (num_rail_ + 1) + hamilton_decomp_[i][(j + 1) % (num_rail_ + 1)];
        //std::cout << "rail:" << i << " mesh_id_1:" << mesh_id_1 << " mesh_id_2:" << mesh_id_2 << std::endl;
        Port* port1 = get_mesh(mesh_id_1)->xpos_rail_ports_[i];
        Port* port2 = get_mesh(mesh_id_2)->xneg_rail_ports_[i];
        Port::connect_port(port1, port2);
        // Y-rails
        mesh_id_1 = hamilton_decomp_[i][j] * (num_rail_ + 1) + k;
        mesh_id_2 = hamilton_decomp_[i][(j + 1) % (num_rail_ + 1)] * (num_rail_ + 1) + k;
        port1 = get_mesh(mesh_id_1)->ypos_rail_ports_[i];
        port2 = get_mesh(mesh_id_2)->yneg_rail_ports_[i];
        Port::connect_port(port1, port2);
      }
    }
  }
}

void RailX::routing_algorithm(Packet& s) const {
  if (algorithm_ == "MIN")
    MIN_routing(s);
  else
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}

void RailX::MIN_routing(Packet& s) const {
  ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
  ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
  
  HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
  HBMesh* dest_mesh = get_mesh(s.destination_.group_id);

  if (current_mesh->mesh_id_ == dest_mesh->mesh_id_) {
    XY_routing(s, s.destination_, 2);
  } else {
    int cur_mesh_x = current_mesh->hyperx_coordinate_[0];
    int dest_mesh_x = dest_mesh->hyperx_coordinate_[0];
    if (cur_mesh_x != dest_mesh_x) {  // X-rail
      int pos_rail_x = in_which_cycle({cur_mesh_x, dest_mesh_x}, hamilton_decomp_);
      int neg_rail_x = in_which_cycle({dest_mesh_x, cur_mesh_x}, hamilton_decomp_);
      Port* pos_rail_port = current_mesh->xpos_rail_ports_[pos_rail_x];
      Port* neg_rail_port = current_mesh->xneg_rail_ports_[neg_rail_x];
      ChipletInMesh* pos_rail_chiplet = get_chiplet(pos_rail_port->node_id);
      ChipletInMesh* neg_rail_chiplet = get_chiplet(neg_rail_port->node_id);
      int man_dis_pos = ChipletInMesh::man_distance(current_chiplet, pos_rail_chiplet);
      int man_dis_neg = ChipletInMesh::man_distance(current_chiplet, neg_rail_chiplet);
      if (man_dis_pos == 0) {
        assert(current_chiplet->id_ == pos_rail_chiplet->id_);
        s.candidate_channels_.push_back(VCInfo(pos_rail_port->link_buffer, 1));
      } else if (man_dis_neg == 0) {
        assert(current_chiplet->id_ == neg_rail_chiplet->id_);
        s.candidate_channels_.push_back(VCInfo(neg_rail_port->link_buffer, 1));
      } else if (man_dis_pos < man_dis_neg)
        XY_routing(s, pos_rail_port->node_id, 0);
      else
        XY_routing(s, neg_rail_port->node_id, 0);
    } else {  // Y-rail
      int cur_mesh_y = current_mesh->hyperx_coordinate_[1];
      int dest_mesh_y = dest_mesh->hyperx_coordinate_[1];
      int pos_rail_y = in_which_cycle({cur_mesh_y, dest_mesh_y}, hamilton_decomp_);
      int neg_rail_y = in_which_cycle({dest_mesh_y, cur_mesh_y}, hamilton_decomp_);
      Port* pos_rail_port = current_mesh->ypos_rail_ports_[pos_rail_y];
      Port* neg_rail_port = current_mesh->yneg_rail_ports_[neg_rail_y];
      ChipletInMesh* pos_rail_chiplet = get_chiplet(pos_rail_port->node_id);
      ChipletInMesh* neg_rail_chiplet = get_chiplet(neg_rail_port->node_id);
      int man_dis_pos = ChipletInMesh::man_distance(current_chiplet, pos_rail_chiplet);
      int man_dis_neg = ChipletInMesh::man_distance(current_chiplet, neg_rail_chiplet);
      if (man_dis_pos == 0) {
        assert(current_chiplet->id_ == pos_rail_chiplet->id_);
        s.candidate_channels_.push_back(VCInfo(pos_rail_port->link_buffer, 2));
      } else if (man_dis_neg == 0) {
        assert(current_chiplet->id_ == neg_rail_chiplet->id_);
        s.candidate_channels_.push_back(VCInfo(neg_rail_port->link_buffer, 2));
      } else if (man_dis_pos < man_dis_neg)
        XY_routing(s, pos_rail_port->node_id, 1);
      else
        XY_routing(s, neg_rail_port->node_id, 1);
    }
  }
}

void RailX::XY_routing(Packet& s, NodeID dest, int vcb) const {
  ChipletInMesh* current_node = get_chiplet(s.head_trace().id);
  ChipletInMesh* destination_node = get_chiplet(dest);

  int cur_x = current_node->x_;
  int cur_y = current_node->y_;
  int dest_x = destination_node->x_;
  int dest_y = destination_node->y_;
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  if (dis_x < 0)  // first x
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_node->xneg_link_buffers_[i], vcb));
    }
  else if (dis_x > 0)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_node->xpos_link_buffers_[i], vcb));
    }
  else if (dis_x == 0) {
    if (dis_y < 0)  // then y
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_node->yneg_link_buffers_[i], vcb));
      }
    else if (dis_y > 0)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_node->ypos_link_buffers_[i], vcb));
      }
  }
}
