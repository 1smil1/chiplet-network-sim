#include "hammingmesh.h"

SWforHxMesh::SWforHxMesh(int num_sw_per_dir, int switch_radix, int num_vcs, int buffer_size,
                         Channel external_link)
    : num_switch_(num_nodes_) {
  switch_radix_ = switch_radix;
  num_sw_per_dir_ = num_sw_per_dir;
  num_switch_ = num_sw_per_dir_ * 2;
  // 0..num_sw_per_dir_-1: x switches; num_sw_per_dir_..num_switch_-1: y switches
  // 0..switch_radix_-1: x switches (rail 0)
  // switch_raidx_..2*switch_radix_-1: x switches (rail 1)
  // num_sw_per_dir_..num_sw_per_dir_+switch_radix_-1: y switches (rail 0)
  // num_sw_per_dir_+switch_radix_..num_switch_-1: y switches (rail 1)
  for (int i = 0; i < num_switch_; i++) {
    nodes_.push_back(new Node(switch_radix, num_vcs, buffer_size, external_link));
  }
}

SWforHxMesh::~SWforHxMesh() {
  for (auto node : nodes_) {
    delete node;
  }
  nodes_.clear();
}

HammingMesh::HammingMesh() : meshes_(groups_) {
  read_config();
  num_mesh_ = (switch_radix_ / 2) * (switch_radix_ / 2);
  num_groups_ = num_mesh_ + 1;
  num_rails_ = m_scale_ * n_port_;
  num_sw_per_dir_ = (switch_radix_ / 2) * num_rails_;
  num_switches_ = num_sw_per_dir_ * 2;
  num_cores_ = num_mesh_ * m_scale_ * m_scale_;
  num_nodes_ = num_cores_ + num_switches_;
  groups_.reserve(num_groups_);
  for (int mesh_id = 0; mesh_id < num_mesh_; mesh_id++) {
    meshes_.push_back(new HBMesh(m_scale_, n_port_, param->vc_number, param->buffer_size,
                                 internal_HB_link, external_link));
    meshes_[mesh_id]->set_group(this, mesh_id);
    get_mesh(mesh_id)->coordinate_ = {mesh_id % (switch_radix_ / 2), mesh_id / (switch_radix_ / 2)};
  }
  groups_.push_back(new SWforHxMesh(num_sw_per_dir_, switch_radix_, param->vc_number,
                                    param->buffer_size, external_link));
  groups_[num_mesh_]->set_group(this, num_mesh_);
  connect();
  print_config();
}

HammingMesh::~HammingMesh() {
  for (auto mesh : meshes_) {
    delete mesh;
  }
  meshes_.clear();
}

void HammingMesh::read_config() {
  m_scale_ = param->params_ptree.get<int>("Network.m_scale", 4);
  n_port_ = param->params_ptree.get<int>("Network.n_port", 2);
  switch_radix_ = param->params_ptree.get<int>("Network.switch_radix", 18);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "MIN");
  int internal_bandiwdth = param->params_ptree.get<int>("Network.internal_bandwidth", 2);
  int external_latency = param->params_ptree.get<int>("Network.external_latency", 4);
  internal_HB_link = Channel(internal_bandiwdth, 1);
  external_link = Channel(1, external_latency);
}

void HammingMesh::print_config() {
  std::cout << "Number of cores: " << num_cores_ << std::endl;
  std::cout << "Number of nodes: " << num_nodes_ << std::endl;
  std::cout << "Number of groups: " << num_groups_ << std::endl;
  std::cout << "HammingMesh parameters: " << std::endl;
  std::cout << "m_scale: " << m_scale_ << std::endl;
  std::cout << "n_port: " << n_port_ << std::endl;
  std::cout << "switch_radix: " << switch_radix_ << std::endl;
  std::cout << "switch_per_dirction: " << num_sw_per_dir_ << std::endl;
}

void HammingMesh::connect() {
  for (int x_mesh = 0; x_mesh < switch_radix_ / 2; ++x_mesh) {
    for (int y_mesh = 0; y_mesh < switch_radix_ / 2; ++y_mesh) {
      HBMesh* mesh = get_mesh({x_mesh, y_mesh});
      for (int rail = 0; rail < num_rails_; ++rail) {
        Node* x_switch = get_sw_layer()->get_x_switch(y_mesh * num_rails_ + rail);
        // connect x switches
        Port* x_neg_port = mesh->xneg_rail_ports_[rail];
        Port* x_pos_port = mesh->xpos_rail_ports_[rail];
        Port* sw_port_1 = x_switch->ports_[x_mesh * 2];
        Port* sw_port_2 = x_switch->ports_[x_mesh * 2 + 1];
        Port::connect_port(x_neg_port, sw_port_1);
        Port::connect_port(x_pos_port, sw_port_2);
        // connect y switches
        Node* y_switch = get_sw_layer()->get_y_switch(x_mesh * num_rails_ + rail);
        Port* y_neg_port = mesh->yneg_rail_ports_[rail];
        Port* y_pos_port = mesh->ypos_rail_ports_[rail];
        Port* sw_port_3 = y_switch->ports_[y_mesh * 2];
        Port* sw_port_4 = y_switch->ports_[y_mesh * 2 + 1];
        Port::connect_port(y_neg_port, sw_port_3);
        Port::connect_port(y_pos_port, sw_port_4);
      }
    }
  }
}

void HammingMesh::routing_algorithm(Packet& s) const {
  if (algorithm_ == "MIN") {
    MIN_routing(s);
  } else {
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
  }
}

void HammingMesh::MIN_routing(Packet& s) const {
  ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
  HBMesh* dest_mesh = get_mesh(s.destination_.group_id);
  int dest_mesh_x = dest_mesh->coordinate_[0];
  int dest_mesh_y = dest_mesh->coordinate_[1];
  int dest_chip_x = destination_chiplet->x_;
  int dest_chip_y = destination_chiplet->y_;

  
  if (s.head_trace().id.group_id == num_mesh_) { // routing in switch layer
    SWforHxMesh* sw_layer = get_sw_layer();
    Node* cur_switch = get_switch(s.head_trace().id);
    if (cur_switch->node_id_ < num_sw_per_dir_) {  // x switches
      if (dest_chip_x * 2 == m_scale_ - 1) {
        int port_id = dest_mesh_x * 2;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 1));
        port_id = dest_mesh_x * 2 + 1;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 1));
      }
      else if (dest_chip_x < m_scale_ / 2) {
        int port_id = dest_mesh_x * 2;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 1));
      } else {  // dest_chip_x >= m_scale_ / 2
        int port_id = dest_mesh_x * 2 + 1;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 1));
      }
    } else {  // cur_switch->node_id_ >= num_sw_per_dir_  y switches
      if (dest_chip_y * 2 == m_scale_ - 1) {
        int port_id = dest_mesh_y * 2;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 2));
        port_id = dest_mesh_y * 2 + 1;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 2));
      }
      else if (dest_chip_y < m_scale_ / 2) {
        int port_id = dest_mesh_y * 2;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 2));
      } else {  // dest_chip_y >= m_scale_ / 2
        int port_id = dest_mesh_y * 2 + 1;
        s.candidate_channels_.push_back(VCInfo(cur_switch->ports_[port_id]->link_buffer, 2));
      }
    }
  } else {
    // routing in mesh
    ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
    HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
    int cur_mesh_x = current_mesh->coordinate_[0];
    int cur_mesh_y = current_mesh->coordinate_[1];
    int cur_chip_x = current_chiplet->x_;
    int cur_chip_y = current_chiplet->y_;

    if (current_mesh->mesh_id_ == dest_mesh->mesh_id_) {
      XY_routing(s, s.destination_, 2);
    } else {
      int rantint = rand();
      if (cur_mesh_x != dest_mesh_x) {  // x-first
        if (cur_chip_x == 0) {          // x-neg edge to the fat-tree
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[plane], 1));
          }
        } else if (cur_chip_x == m_scale_ - 1) {  // x-pos edge to the fat-tree
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[plane], 1));
          }
        } else if (cur_chip_x * 2 == m_scale_ - 1) {
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[plane], 1));
          }
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[plane], 1));
          }
        }
        else if (cur_chip_x < m_scale_ / 2) {  // to x-neg edge
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[plane], 0));
          }
        } else if (cur_chip_x >= m_scale_ / 2) {  // to x-pos edge
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[plane], 0));
          }
        }
      } else {
        assert(cur_mesh_x == dest_mesh_x);
        if (cur_chip_x < dest_chip_x) {
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[plane], 0));
          }
        } else if (cur_chip_x > dest_chip_x) {
          for (int i = 0; i < n_port_; i++) {
            int plane = (i + rantint) % n_port_;
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[plane], 0));
          }
        } else {  // in same y-column
          assert(cur_chip_x == dest_chip_x);
          assert(cur_mesh_y != dest_mesh_y);
        //{
          if (cur_chip_y == 0) {  // y-neg edge to the fat-tree
            for (int i = 0; i < n_port_; i++) {
              int plane = (i + rantint) % n_port_;
              s.candidate_channels_.push_back(
                  VCInfo(current_chiplet->yneg_link_buffers_[plane], 2));
            }
          } else if (cur_chip_y == m_scale_ - 1) {  // y-pos edge to the fat-tree
            for (int i = 0; i < n_port_; i++) {
              int plane = (i + rantint) % n_port_;
              s.candidate_channels_.push_back(
                  VCInfo(current_chiplet->ypos_link_buffers_[plane], 2));
            }
          } else if (cur_chip_y < m_scale_ / 2) {  // to y-neg edge
            for (int i = 0; i < n_port_; i++) {
              int plane = (i + rantint) % n_port_;
              s.candidate_channels_.push_back(
                  VCInfo(current_chiplet->yneg_link_buffers_[plane], 1));
            }
          } else if (cur_chip_y >= m_scale_ / 2) {  // to y-pos edge
            for (int i = 0; i < n_port_; i++) {
              int plane = (i + rantint) % n_port_;
              s.candidate_channels_.push_back(
                  VCInfo(current_chiplet->ypos_link_buffers_[plane], 1));
            }
          }
        }
      }
    }
  
  }
}

void HammingMesh::XY_routing(Packet& s, NodeID dest, int vcb) const {
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

void HammingMesh::torus_routing(Packet& s) const {}
