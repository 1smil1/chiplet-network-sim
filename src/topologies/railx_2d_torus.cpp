#include "railx_2d_torus.h"

RailX2DTorus::RailX2DTorus() : num_mesh_(num_groups_), meshes_(groups_) {
  read_config();
  num_mesh_ = x_scale_ * y_scale_;
  num_nodes_ = num_mesh_ * (m_scale_ * m_scale_);
  num_cores_ = num_nodes_;
  meshes_.reserve(num_mesh_);
  for (int mesh_id = 0; mesh_id < num_mesh_; mesh_id++) {
    meshes_.push_back(new HBMesh(m_scale_, n_port_, param->vc_number, param->buffer_size,
                                 internal_HB_link, external_link));
    meshes_[mesh_id]->set_group(this, mesh_id);
    get_mesh(mesh_id)->coordinate_ = {mesh_id % x_scale_, mesh_id / x_scale_};
  }
  connect();
}

RailX2DTorus::~RailX2DTorus() {
  for (auto mesh : meshes_) delete mesh;
  meshes_.clear();
}

void RailX2DTorus::read_config() {
  m_scale_ = param->params_ptree.get<int>("Network.m_scale", 8);
  n_port_ = param->params_ptree.get<int>("Network.n_port", 1);
  x_scale_ = param->params_ptree.get<int>("Network.x_scale", 1);
  y_scale_ = param->params_ptree.get<int>("Network.y_scale", 1);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "XY");
  int internal_bandiwdth = param->params_ptree.get<int>("Network.internal_bandwidth", 1);
  int external_latency = param->params_ptree.get<int>("Network.external_latency", 1);
  internal_HB_link = Channel(internal_bandiwdth, 1);
  external_link = Channel(1, external_latency);
}

void RailX2DTorus::connect() {
  // torus
  for (int mesh_id = 0; mesh_id < num_mesh_; ++mesh_id) {
    HBMesh* mesh = get_mesh(mesh_id);
    int mesh_x = mesh->coordinate_[0];
    int mesh_y = mesh->coordinate_[1];
    ChipletInMesh* chiplet;
    int link_chiplet_id, link_mesh_id;
    for (int chiplet_y = 0; chiplet_y < m_scale_; ++chiplet_y) {  
      chiplet = mesh->get_chiplet(chiplet_y * m_scale_);     // x-neg
      link_chiplet_id = chiplet_y * m_scale_ + m_scale_ - 1; // x_chiplet = m_scale_ -1
      link_mesh_id = mesh_y * x_scale_ + (mesh_id - 1 + x_scale_) % x_scale_;
      for (int i = 0; i < n_port_; ++i) {
        chiplet->xneg_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
        chiplet->xneg_link_buffers_[i] =
            get_chiplet(chiplet->xneg_link_nodes_[i])->xpos_in_buffers_[i];
      }
      chiplet = mesh->get_chiplet(chiplet_y * m_scale_ + m_scale_ - 1);  // x-pos
      link_chiplet_id = chiplet_y * m_scale_;                            // x_chiplet = 0
      link_mesh_id = mesh_y * x_scale_ + (mesh_id + 1) % x_scale_;
      for (int i = 0; i < n_port_; ++i) {
        chiplet->xpos_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
        chiplet->xpos_link_buffers_[i] =
            get_chiplet(chiplet->xpos_link_nodes_[i])->xneg_in_buffers_[i];
      }
    }
    for (int chiplet_x = 0; chiplet_x < m_scale_; ++chiplet_x) {  
      chiplet = mesh->get_chiplet(chiplet_x);                   // y-neg
      link_chiplet_id = chiplet_x + (m_scale_ - 1) * m_scale_;  // y_chiplet = m_scale_ -1
      link_mesh_id = (mesh_y - 1 + y_scale_) % y_scale_ * x_scale_ + mesh_x;
      for (int i = 0; i < n_port_; ++i) {
        chiplet->yneg_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
        chiplet->yneg_link_buffers_[i] =
            get_chiplet(chiplet->yneg_link_nodes_[i])->ypos_in_buffers_[i];
      }
      chiplet = mesh->get_chiplet(chiplet_x + (m_scale_ - 1) * m_scale_);  // y-pos
      link_chiplet_id = chiplet_x;                                         // y_chiplet = 0
      link_mesh_id = (mesh_y + 1) % y_scale_ * x_scale_ + mesh_x;
      for (int i = 0; i < n_port_; ++i) {
        chiplet->ypos_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
        chiplet->ypos_link_buffers_[i] =
            get_chiplet(chiplet->ypos_link_nodes_[i])->yneg_in_buffers_[i];
      }
    }
  }
}

void RailX2DTorus::routing_algorithm(Packet& s) const {
  if (algorithm_ == "XY")
    XY_routing(s);
  else if (algorithm_ == "Chip_XY")
    Chip_XY_routing(s);
  else if (algorithm_ == "CLUE")
    clue_routing(s);
  else
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}

void RailX2DTorus::XY_routing(Packet& s) const {
  ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
  ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
  HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
  HBMesh* dest_mesh = get_mesh(s.destination_.group_id);

  int cur_x = current_mesh->coordinate_[0] * m_scale_ + current_chiplet->x_;
  int cur_y = current_mesh->coordinate_[1] * m_scale_ + current_chiplet->y_;
  int dest_x = dest_mesh->coordinate_[0] * m_scale_ + destination_chiplet->x_;
  int dest_y = dest_mesh->coordinate_[1] * m_scale_ + destination_chiplet->y_;
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset
  int K_x = x_scale_ * m_scale_;    
  int K_y = y_scale_ * m_scale_;

  // x first
  if (dis_x < 0 && dis_x >= -K_x / 2)
    for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 0));
    }
  else if (dis_x > K_x / 2) 
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 1));
    }
  else if (dis_x > 0 && dis_x <= K_x / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 0));
    }
  else if (dis_x < -K_x / 2) {
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 1));
    }
  }
  else  { // then y
    assert(dis_x == 0);
    if (dis_y < 0 && dis_y >= -K_y / 2)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 0));
      }
    else if (dis_y > K_y / 2) 
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 1));
      }
    else if (dis_y > 0 && dis_y <= K_y / 2)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 0));
      }
    else if (dis_y < -K_y / 2) 
        for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 1));
      }
    else {
      std::cerr << "The source and destination are the same!" << std::endl;
    }
  }
}

void RailX2DTorus::Chip_XY_routing(Packet& s) const {
  ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
  ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
  HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
  HBMesh* dest_mesh = get_mesh(s.destination_.group_id);

  int dis_x_hbmesh = dest_mesh->coordinate_[0] - current_mesh->coordinate_[0];
  int dis_y_hbmesh = dest_mesh->coordinate_[1] - current_mesh->coordinate_[1];
  int dis_x_in_mesh = destination_chiplet->x_ - current_chiplet->x_;
  int dis_y_in_mesh = destination_chiplet->y_ - current_chiplet->y_;

  if (dis_x_hbmesh < 0 && dis_x_hbmesh >= -x_scale_ / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 0));
    }
  else if (dis_x_hbmesh > x_scale_ / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 1));
    }
  else if (dis_x_hbmesh > 0 && dis_x_hbmesh <= x_scale_ / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 0));
    }
  else if (dis_x_hbmesh < -x_scale_ / 2) {
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 1));
    }
  }
  else {
    assert(dis_x_hbmesh == 0);
    if (dis_x_in_mesh < 0)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 0));
      }
    else if (dis_x_in_mesh > 0)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 0));
      }
    else {
      assert(dis_x_in_mesh == 0);
      if (dis_y_hbmesh < 0 && dis_y_hbmesh >= -y_scale_ / 2)
        for (int i = 0; i < n_port_; i++) {
          s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 0));
        }
      else if (dis_y_hbmesh > y_scale_ / 2)
        for (int i = 0; i < n_port_; i++) {
          s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 1));
        }
      else if (dis_y_hbmesh > 0 && dis_y_hbmesh <= y_scale_ / 2)
        for (int i = 0; i < n_port_; i++) {
          s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 0));
        }
      else if (dis_y_hbmesh < -y_scale_ / 2)
        for (int i = 0; i < n_port_; i++) {
          s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 1));
        }
      else {
        assert(dis_y_hbmesh == 0);
        if (dis_y_in_mesh < 0)
          for (int i = 0; i < n_port_; i++) {
            s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 0));
          }
        else if (dis_y_in_mesh > 0)
          for (int i = 0; i < n_port_; i++) {
            s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 0));
          }
        else {
          assert(dis_y_in_mesh == 0);
          std::cerr << "The source and destination are the same!" << std::endl;
        }
      }
    }
  }
}

void RailX2DTorus::clue_routing(Packet& s) const {
  ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
  ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
  HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
  HBMesh* dest_mesh = get_mesh(s.destination_.group_id);

  int cur_x = current_mesh->coordinate_[0] * m_scale_ + current_chiplet->x_;
  int cur_y = current_mesh->coordinate_[1] * m_scale_ + current_chiplet->y_;
  int dest_x = dest_mesh->coordinate_[0] * m_scale_ + destination_chiplet->x_;
  int dest_y = dest_mesh->coordinate_[1] * m_scale_ + destination_chiplet->y_;
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset
  int K_x = x_scale_ * m_scale_;
  int K_y = y_scale_ * m_scale_;

  // VC-0 Adaptive Routing
  if (-K_x / 2 <= dis_x && dis_x < 0 || dis_x > K_x / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 0));
    }
  else if (0 < dis_x && dis_x <= K_x / 2 || dis_x < -K_x / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 0));
    }

  if (-K_y / 2 <= dis_y && dis_y < 0 || dis_y > K_y / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 0));
    }
  else if (0 < dis_y && dis_y <= K_y / 2 || dis_y < -K_y / 2)
    for (int i = 0; i < n_port_; i++) {
      s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 0));
    }

  // VC-1 
  if (abs(dis_x) <= K_x / 2 && abs(dis_y) <= K_y / 2) {
    // negative-first
    if (dis_x < 0 || dis_y < 0) {
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 1));
      }
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 1));
      }
    } else {
      if (dis_x > 0)
        for (int i = 0; i < n_port_; i++) {
          s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 1));
        }
      if (dis_y > 0)
        for (int i = 0; i < n_port_; i++) {
          s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 1));
        }
    }
  } else if (abs(dis_x) > K_x / 2) {
    if (cur_x == 0)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], 1));
      }
    else if (cur_x == K_x - 1)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], 1));
      }
  } else if (abs(dis_y) > K_y / 2) {
    if (cur_y == 0)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], 1));
      }
    else if (cur_y == K_y - 1)
      for (int i = 0; i < n_port_; i++) {
        s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], 1));
      }
  }
}