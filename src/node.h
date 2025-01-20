#pragma once

#include "config.h"

class Buffer;
class Group;

// NodeID is the unique identifier of a node in the network.
struct NodeID {
  explicit NodeID(int nodeid = -1, int groupid = -1) {
    node_id = nodeid;
    group_id = groupid;
  }
  int node_id;
  int group_id;
  inline bool operator==(const NodeID& id) const {
    return (node_id == id.node_id && group_id == id.group_id);
  };
  inline bool operator!=(const NodeID& id) const {
    return (node_id != id.node_id || group_id != id.group_id);
  }
  friend std::ostream& operator<<(std::ostream& s, const NodeID& id);
};

// A collection of { node_id, in_buffer, link_node, link_buffer }
struct Port {
  NodeID& node_id;
  Buffer*& in_buffer;
  NodeID& link_node;
  Buffer*& link_buffer;
  Port(NodeID& nid, Buffer*& ib, NodeID& ln, Buffer*& lb)
      : node_id(nid), in_buffer(ib), link_node(ln), link_buffer(lb) {}
  // Connecting two ports
  static void connect_port(Port*& p1, Port*& p2) {
    p1->link_node = p2->node_id;
    p1->link_buffer = p2->in_buffer;
    p2->link_node = p1->node_id;
    p2->link_buffer = p1->in_buffer;
  }
};


// Node class is the basic building block of networks.
// It contains several input ports with buffers.
class Node {
 public:
  Node(int radix, int vc_num, int buffer_size, Channel channel = default_channel);
  ~Node();

  virtual void set_node(Group* group, NodeID id);
  void reset();

  NodeID id_;
  int& node_id_ = id_.node_id;
  int& group_id_ = id_.group_id;
  Group* group_;
  int radix_;

  // Input buffers
  std::vector<Buffer*> in_buffers_;

  // ID of the node to which the output port goes.
  std::vector<NodeID> link_nodes_;

  // Point to input buffer connected to the output port.
  std::vector<Buffer*> link_buffers_;

  // A collection of { node_id, in_buffer, link_node, link_buffer }
  std::vector<Port*> ports_;
};
