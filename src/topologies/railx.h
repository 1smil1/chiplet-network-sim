#pragma once
#include "System.h"

class HBMesh;
class RailX2DHyperX;

std::vector<std::vector<int>> gen_rc_square(int n);
std::vector<std::vector<int>> gen_hamilton_decomp_odd(int n);
std::vector<std::vector<int>> gen_hamilton_decomp_4(int n);
int in_which_cycle(std::pair<int, int> link, const std::vector<std::vector<int>>& hamilton_decomp);
bool check_hamilton_decomp(const std::vector<std::vector<int>>& hcycle, int n);

class ChipletInMesh : public Node {
 public:
  ChipletInMesh(int m_scale, int n_port, int vc_num, int buffer_size, Channel internal_channel);
  void set_node(Group* HB_mesh, NodeID id) override;
  inline static int man_distance(ChipletInMesh* chiplet_1, ChipletInMesh* chiplet_2) {
    assert(chiplet_1->id_.group_id == chiplet_2->id_.group_id);
    return abs(chiplet_1->x_ - chiplet_2->x_) + abs(chiplet_1->y_ - chiplet_2->y_);
  }
  Group*& HB_mesh_;
  // chiplet coordinate in local mesh
  int x_, y_;
  // number of physical channels
  int n_port_;
  // scale of the 2D-mesh
  int m_scale_;

  // Input buffers for the on-chip 2D-mesh
  std::vector<Buffer*> xneg_in_buffers_;
  std::vector<Buffer*> xpos_in_buffers_;
  std::vector<Buffer*> yneg_in_buffers_;
  std::vector<Buffer*> ypos_in_buffers_;

  // ID of the node to which the output port goes.
  std::vector<std::reference_wrapper<NodeID>> xneg_link_nodes_;
  std::vector<std::reference_wrapper<NodeID>> xpos_link_nodes_;
  std::vector<std::reference_wrapper<NodeID>> yneg_link_nodes_;
  std::vector<std::reference_wrapper<NodeID>> ypos_link_nodes_;

  // Point to input buffer to which the output port goes.
  std::vector<Buffer*> xneg_link_buffers_;
  std::vector<Buffer*> xpos_link_buffers_;
  std::vector<Buffer*> yneg_link_buffers_;
  std::vector<Buffer*> ypos_link_buffers_;

  std::vector<Port*> xneg_ports_;
  std::vector<Port*> xpos_ports_;
  std::vector<Port*> yneg_ports_;
  std::vector<Port*> ypos_ports_;
};

// Chiplets are locally connected by a 2D-mesh with high-bandwidth low-ltency links.
class HBMesh : public Group {
 public:
  HBMesh(int m_scale, int n_port, int vc_num, int buffer_size, Channel internal_channel,
         Channel external_channel);
  ~HBMesh();

  void set_group(System* system, int mesh_id) override;
  inline ChipletInMesh* get_chiplet(int chiplet_id) const {
    return static_cast<ChipletInMesh*>(Group::get_node(chiplet_id));
  }
  inline ChipletInMesh* get_chiplet(NodeID id) const {
    return static_cast<ChipletInMesh*>(Group::get_node(id));
  }

  System*& railx_;
  Channel external_channel_;
  int& num_chiplets_;
  std::vector<int>& coordinate_;
  std::vector<Port*> xneg_rail_ports_;
  std::vector<Port*> xpos_rail_ports_;
  std::vector<Port*> yneg_rail_ports_;
  std::vector<Port*> ypos_rail_ports_;
  int m_scale_;
  int n_port_;
  int num_rail_;
  int& mesh_id_;
};