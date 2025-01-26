#include "railx_2d_hyperx.h"

RailX2DHyperX::RailX2DHyperX() : num_mesh_(num_groups_), meshes_(groups_) {
  read_config();
  num_rails_ = m_scale_ * n_port_;
  num_mesh_ = (num_rails_ + 1) * (num_rails_ + 1);
  num_nodes_ = num_mesh_ * (m_scale_ * m_scale_);
  num_cores_ = num_nodes_;
  meshes_.reserve(num_mesh_);
  for (int mesh_id = 0; mesh_id < num_mesh_; mesh_id++) {
    meshes_.push_back(new HBMesh(m_scale_, n_port_, param->vc_number, param->buffer_size,
                                 internal_HB_link, external_link));
    meshes_[mesh_id]->set_group(this, mesh_id);
  }
  if ((num_rails_+1) % 4 == 0) {
    hamilton_decomp_ = gen_hamilton_decomp_4(num_rails_ + 1);
  } else if (num_rails_ % 2 == 0) {
    hamilton_decomp_ = gen_hamilton_decomp_odd(num_rails_ + 1);
  }
  for (auto& v : hamilton_decomp_) {
    for (auto& i : v) {
      std::cout << i << " ";
    }
    std::cout << std::endl;
  }
  connect();
}

RailX2DHyperX::~RailX2DHyperX() {
  for (auto mesh : meshes_) delete mesh;
  meshes_.clear();
}

void RailX2DHyperX::read_config() {
  m_scale_ = param->params_ptree.get<int>("Network.m_scale", 4);
  n_port_ = param->params_ptree.get<int>("Network.n_port", 2);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "MIN");
  int internal_bandiwdth = param->params_ptree.get<int>("Network.internal_bandwidth", 2);
  int external_latency = param->params_ptree.get<int>("Network.external_latency", 10);
  internal_HB_link = Channel(internal_bandiwdth, 1);
  external_link = Channel(1, external_latency);
}

void RailX2DHyperX::connect() {
  // rail-ring-based all-to-all connection
  for (int i = 0; i < num_rails_; i++) { // rail-i
    for (int j = 0; j < num_rails_ + 1; j++) {    // rail-ring
      for (int k = 0; k < num_rails_ + 1; k++) {  // for each row/column
        // X-rails
        int mesh_id_1 = k * (num_rails_ + 1) + hamilton_decomp_[i][j];
        int mesh_id_2 = k * (num_rails_ + 1) + hamilton_decomp_[i][(j + 1) % (num_rails_ + 1)];
        //std::cout << "rail:" << i << " mesh_id_1:" << mesh_id_1 << " mesh_id_2:" << mesh_id_2 << std::endl;
        Port* port1 = get_mesh(mesh_id_1)->xpos_rail_ports_[i];
        Port* port2 = get_mesh(mesh_id_2)->xneg_rail_ports_[i];
        Port::connect_port(port1, port2);
        // Y-rails
        mesh_id_1 = hamilton_decomp_[i][j] * (num_rails_ + 1) + k;
        mesh_id_2 = hamilton_decomp_[i][(j + 1) % (num_rails_ + 1)] * (num_rails_ + 1) + k;
        port1 = get_mesh(mesh_id_1)->ypos_rail_ports_[i];
        port2 = get_mesh(mesh_id_2)->yneg_rail_ports_[i];
        Port::connect_port(port1, port2);
      }
    }
  }
}

void RailX2DHyperX::routing_algorithm(Packet& s) const {
  if (algorithm_ == "MIN")
    MIN_routing(s);
  else
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}

void RailX2DHyperX::MIN_routing(Packet& s) const {
  ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
  ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
  
  HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
  HBMesh* dest_mesh = get_mesh(s.destination_.group_id);

  if (current_mesh->mesh_id_ == dest_mesh->mesh_id_) {
    XY_routing(s, s.destination_, 2);
  } else {
    int cur_mesh_x = current_mesh->coordinate_[0];
    int dest_mesh_x = dest_mesh->coordinate_[0];
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
      int cur_mesh_y = current_mesh->coordinate_[1];
      int dest_mesh_y = dest_mesh->coordinate_[1];
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

void RailX2DHyperX::XY_routing(Packet& s, NodeID dest, int vcb) const {
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
