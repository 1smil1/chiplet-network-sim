#include "node.h"
#include "buffer.h"

std::ostream& operator<<(std::ostream& s, const NodeID& id) {
  s << "NodeID:" << id.node_id << " GroupID:" << id.group_id;
  return s;
}

Node::Node(int radix, int vc_num, int buffer_size, Channel channel) {
  group_ = nullptr;
  id_ = NodeID();
  radix_ = radix;
  for (int i = 0; i < radix_; i++) {
    in_buffers_.push_back(new Buffer(this, vc_num, buffer_size, channel));
    link_nodes_.push_back(NodeID());
    link_buffers_.push_back(nullptr);
    ports_.push_back(Port{id_, in_buffers_[i], link_nodes_[i], link_buffers_[i]});
  }
}

Node::~Node() {
  for (auto in_buffer : in_buffers_) {
    delete in_buffer;
  }
  in_buffers_.clear();
}

void Node::set_node(Group* group, NodeID id) {
  group_ = group;
  id_ = id;
}

void Node::reset() {
  for (auto in_buffer : in_buffers_) {
    in_buffer->reset();
  }
}
