#include "traffic_manager.h"
#include "railx_2d_hyperx.h"

void TrafficManager::torus_hirechical_reduce_mess(std::vector<Packet*>& packets) {
  //int core_per_chip = network->groups_[0]->num_cores_;
  //int dest1, dest2;
  //for (pkt_for_injection_ += message_per_cycle(); pkt_for_injection_ > traffic_scale_ * 4;
  //     pkt_for_injection_ -= traffic_scale_ * 4) {
  //  if (param->topology == "RailX2DHyperX") {
  //    RailX2DHyperX* network_hyperx = static_cast<RailX2DHyperX*>(network);
  //    for (int src = 0; src < traffic_scale_; src++) {
  //      NodeID src_id = network_hyperx->int_to_nodeid(src);
  //      HBMesh* src_mesh = network_hyperx->get_mesh(src_id.group_id);
  //      ChipletInMesh* src_chiplet = network_hyperx->get_chiplet(src_id);
  //      NodeID dest1 = src_chiplet->xneg_link_nodes_[0];
  //      NodeID dest2 = src_chiplet->xpos_link_nodes_[0];
  //      NodeID dest3 = src_chiplet->yneg_link_nodes_[0];
  //      NodeID dest4 = src_chiplet->ypos_link_nodes_[0];
  //      Packet* mess = new Packet(src_id, dest1, message_length_);
  //      packets.push_back(mess);
  //      mess = new Packet(src_id, dest2, message_length_);
  //      packets.push_back(mess);
  //      mess = new Packet(src_id, dest3, message_length_);
  //      packets.push_back(mess);
  //      mess = new Packet(src_id, dest4, message_length_);
  //      packets.push_back(mess);
  //      all_message_num_ += 4;
  //    }
  //  }
  //}
}