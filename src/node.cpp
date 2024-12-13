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
  in_buffers_.resize(radix_);
  link_nodes_.resize(radix_);
  link_buffers_.resize(radix_);
  ports_.resize(radix_);
  for (int i = 0; i < radix_; i++) {
    in_buffers_[i] = new Buffer(this, vc_num, buffer_size, channel);
    link_nodes_[i] = NodeID();
    link_buffers_[i] = nullptr;
    ports_[i] = new Port(id_, in_buffers_[i], link_nodes_[i], link_buffers_[i]);
  }
}

Node::~Node() {
  for (auto in_buffer : in_buffers_) {
    delete in_buffer;
  }
  in_buffers_.clear();
  for (auto port : ports_) {
    delete port;
  }
  ports_.clear();
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
