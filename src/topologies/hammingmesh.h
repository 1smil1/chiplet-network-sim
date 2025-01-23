#pragma once
#include "system.h"
#include "railx.h"

class SWforHxMesh : public Group {
 public:
  SWforHxMesh(int num_sw_per_dir, int switch_radix, int num_vcs, int buffer_size,
              Channel local_channel);
  ~SWforHxMesh();
  inline Node* get_switch(int switch_id) const {
    return static_cast<Node*>(get_node(switch_id));
  }
  inline Node* get_x_switch(int x_id) const { return static_cast<Node*>(get_node(x_id)); }
  inline Node* get_y_switch(int y_id) const {
    return static_cast<Node*>(get_node(num_sw_per_dir_+ y_id));
  }
  int num_sw_per_dir_;
  int& num_switch_;
  int switch_radix_;
};

class HammingMesh : public System {
 public:
   HammingMesh();
  ~HammingMesh();

  void read_config() override;
  void print_config() override;

  void connect();
  void routing_algorithm(Packet& s) const override;
  void MIN_routing(Packet& s) const;
  void XY_routing(Packet& s, NodeID dest, int vcb) const;

  inline ChipletInMesh* get_chiplet(NodeID id) const {
    return static_cast<ChipletInMesh*>(System::get_node(id));
  }
  inline HBMesh* get_mesh(int mesh_id) const {
    return static_cast<HBMesh*>(System::get_group(mesh_id));
  }
  inline HBMesh* get_mesh(std::vector<int> coordinate) {
    return static_cast<HBMesh*>(
        System::get_group(coordinate[0] + coordinate[1] * switch_radix_ / 2));
  }
  inline SWforHxMesh* get_sw_layer() const {
    return static_cast<SWforHxMesh*>(System::get_group(num_mesh_));
  }
  inline Node* get_switch(NodeID id) const { 
    assert(id.group_id == num_mesh_);
    return get_sw_layer()->get_switch(id.node_id);
  }

  int m_scale_;
  int n_port_;
  int num_rails_;
  int num_sw_per_dir_;
  int num_switches_;
  int switch_radix_;
  Channel internal_HB_link;
  Channel external_link;
  int num_mesh_;
  std::vector<Group*>& meshes_;
};
