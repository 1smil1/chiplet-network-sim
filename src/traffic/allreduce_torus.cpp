#include "traffic_manager.h"
#include "railx_2d_hyperx.h"
#include "railx_2d_torus.h"

#include <cmath>
#include <algorithm>

void TrafficManager::allreduce_torus(std::vector<Packet*>& packets){
  static int stage = 0;
  // int packet_length = std::min(32, (int)(data_size / (int)sqrt(traffic_scale_) / 4));
  // int packet_num = data_size / traffic_scale_ / 4 / packet_length;
  if (stage < ((int)sqrt(traffic_scale_)-1) && packets.size()==0) {
    if (param->topology == "RailX2DTorus") {
      RailX2DTorus* network_torus = static_cast<RailX2DTorus*>(network);
      for (int src = 0; src < traffic_scale_; src++) {
        NodeID src_id = network_torus->int_to_nodeid(src);
        ChipletInMesh* src_chiplet = network_torus->get_chiplet(src_id);
        NodeID dest1 = src_chiplet->xneg_link_nodes_[0];
        NodeID dest2 = src_chiplet->xpos_link_nodes_[0];
        NodeID dest3 = src_chiplet->yneg_link_nodes_[0];
        NodeID dest4 = src_chiplet->ypos_link_nodes_[0];
        int packet_length = std::min(param->buffer_size/2, (int)(data_size / (int)sqrt(traffic_scale_) / 4));
        int packet_num = data_size / (int)sqrt(traffic_scale_) / 4 / packet_length;
        for (int i =0; i < packet_num; i++){
          packets.push_back(new Packet(src_id, dest1, packet_length));
          packets.push_back(new Packet(src_id, dest2, packet_length));
          packets.push_back(new Packet(src_id, dest3, packet_length));
          packets.push_back(new Packet(src_id, dest4, packet_length));
          all_message_num_ += 4;
        }
      }
    }
    stage++;
  }
  else if (stage < ((int)sqrt(traffic_scale_)-1)*2 && packets.size()==0) {
    if (param->topology == "RailX2DTorus") {
      RailX2DTorus* network_torus = static_cast<RailX2DTorus*>(network);
      for (int src = 0; src < traffic_scale_; src++) {
        NodeID src_id = network_torus->int_to_nodeid(src);
        ChipletInMesh* src_chiplet = network_torus->get_chiplet(src_id);
        NodeID dest1 = src_chiplet->xneg_link_nodes_[0];
        NodeID dest2 = src_chiplet->xpos_link_nodes_[0];
        NodeID dest3 = src_chiplet->yneg_link_nodes_[0];
        NodeID dest4 = src_chiplet->ypos_link_nodes_[0];
        int packet_length = std::min(param->buffer_size/2, (int)(data_size / traffic_scale_ / 4));
        int packet_num = data_size / traffic_scale_ / 4 / packet_length;
        for (int i =0; i < packet_num; i++){
          packets.push_back(new Packet(src_id, dest1, packet_length));
          packets.push_back(new Packet(src_id, dest2, packet_length));
          packets.push_back(new Packet(src_id, dest3, packet_length));
          packets.push_back(new Packet(src_id, dest4, packet_length));
          all_message_num_ += 4;
        }
      }
    }
    stage++;
  }
  else if(stage == ((int)sqrt(traffic_scale_)-1)*2 && packets.size()==0){
    stage = 0;
    is_done = true;
    throughput = (double) data_size / cycles;
  }
}

void TrafficManager::torus_all_reduce_mess(std::vector<Packet*>& packets) {

  int dest1, dest2;
  for (pkt_for_injection_ += message_per_cycle(); pkt_for_injection_ > traffic_scale_ * 4;
       pkt_for_injection_ -= traffic_scale_ * 4) {
    if (param->topology == "RailX2DHyperX") {
      RailX2DHyperX* network_hyperx = static_cast<RailX2DHyperX*>(network);
      for (int src = 0; src < traffic_scale_; src++) {
            NodeID src_id = network_hyperx->int_to_nodeid(src);
            ChipletInMesh* src_chiplet = network_hyperx->get_chiplet(src_id);
            NodeID dest1 = src_chiplet->xneg_link_nodes_[0];
            NodeID dest2 = src_chiplet->xpos_link_nodes_[0];
            NodeID dest3 = src_chiplet->yneg_link_nodes_[0];
            NodeID dest4 = src_chiplet->ypos_link_nodes_[0];
            Packet* mess = new Packet(src_id, dest1, message_length_);
            packets.push_back(mess);
            mess = new Packet(src_id, dest2, message_length_);
            packets.push_back(mess);
            mess = new Packet(src_id, dest3, message_length_);
            packets.push_back(mess);
            mess = new Packet(src_id, dest4, message_length_);
            packets.push_back(mess);
            all_message_num_ += 4;
      }
    }
  }
}

