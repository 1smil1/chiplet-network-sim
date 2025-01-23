#pragma once
#include "system.h"
#include "railx.h"

class RailX2DHyperX : public System {
 public:
  RailX2DHyperX();
  ~RailX2DHyperX();

  void read_config() override;

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

  int m_scale_;
  int n_port_;
  int num_rails_;
  Channel internal_HB_link;
  Channel external_link;
  int& num_mesh_;
  std::vector<Group*>& meshes_;
  std::vector<std::vector<int>> hamilton_decomp_;
};