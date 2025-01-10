#pragma once
#include "railx.h"
#include "system.h"

class RailX2DTorus : public System {
 public:
  RailX2DTorus();
  ~RailX2DTorus();

  void read_config() override;

  void connect();
  void routing_algorithm(Packet& s) const override;
  void MIN_routing(Packet& s) const;
  void XY_routing(Packet& s) const;
  void clue_routing(Packet& s) const;
  void Chip_XY_routing(Packet& s) const;    

  inline ChipletInMesh* get_chiplet(NodeID id) const {
    return static_cast<ChipletInMesh*>(System::get_node(id));
  }
  inline HBMesh* get_mesh(int mesh_id) const {
    return static_cast<HBMesh*>(System::get_group(mesh_id));
  }
  inline HBMesh* get_mesh(std::vector<int>& hyperx_coordinate) const {
    return get_mesh(hyperx_coordinate[0] + hyperx_coordinate[1] * m_scale_);
  }

  std::string algorithm_;

  int m_scale_;
  int n_port_;
  int num_rail_;
  int x_scale_;  // gloabal x scale of torus
  int y_scale_;  // gloabal y scale of torus
  Channel internal_HB_link;
  Channel external_link;
  int num_chiplets_per_mesh_;
  int& num_mesh_;
  std::vector<Group*>& meshes_;
};