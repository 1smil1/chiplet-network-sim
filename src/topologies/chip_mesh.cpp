#include "chip_mesh.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>

// Initialize static members
std::map<int, std::pair<int, int>> NodeMesh::position_cache_;
std::string NodeMesh::cached_position_file_ = "";

NodeMesh::NodeMesh(int k_node, int vc_num, int buffer_size)
    : Node(4, vc_num, buffer_size),
      xneg_in_buffer_(in_buffers_[0]),
      xpos_in_buffer_(in_buffers_[1]),
      yneg_in_buffer_(in_buffers_[2]),
      ypos_in_buffer_(in_buffers_[3]),
      xneg_link_node_(link_nodes_[0]),
      xpos_link_node_(link_nodes_[1]),
      yneg_link_node_(link_nodes_[2]),
      ypos_link_node_(link_nodes_[3]),
      xneg_link_buffer_(link_buffers_[0]),
      xpos_link_buffer_(link_buffers_[1]),
      yneg_link_buffer_(link_buffers_[2]),
      ypos_link_buffer_(link_buffers_[3]) {
  x_ = 0;
  y_ = 0;
  k_node_ = k_node;
}

void NodeMesh::set_node(Chip* chip, NodeID id) {
  chip_ = chip;
  id_ = id;
  x_ = id.node_id % k_node_;
  y_ = id.node_id / k_node_;

  // VERIFICATION DEBUG OUTPUT
  ChipMesh* chip_mesh = dynamic_cast<ChipMesh*>(chip_);
  if (chip_mesh && id.chip_id < 5) {  // First 5 chips only
    int chip_x = chip_mesh->chip_coordinate_[0];
    int chip_y = chip_mesh->chip_coordinate_[1];
    int global_x = chip_x * k_node_ + x_;
    int global_y = chip_y * k_node_ + y_;

    if (id.node_id < 4) {  // First 4 nodes per chip
      fprintf(stderr, "[VERIFY] NodeMesh::set_node - chip_id=%d, node_id=%d, "
              "local_pos=(%d,%d), chip_pos=(%d,%d), global_pos=(%d,%d)\n",
              id.chip_id, id.node_id, x_, y_, chip_x, chip_y, global_x, global_y);
    }
  }

  // Load custom positions if position_file is specified
  if (!param->position_file.empty()) {
    load_custom_positions(param->position_file);
  }
}

std::map<int, std::pair<int, int>> NodeMesh::load_all_positions(const std::string& position_file) {
  std::map<int, std::pair<int, int>> positions;

  std::ifstream file(position_file);
  if (!file.is_open()) {
    std::cerr << "Warning: Cannot open position file: " << position_file << std::endl;
    return positions;
  }

  std::string line;
  int loaded_count = 0;
  while (std::getline(file, line)) {
    // Skip comments and empty lines
    if (line.empty() || line[0] == '#') continue;

    std::istringstream iss(line);
    int node_id, x, y;
    if (iss >> node_id >> x >> y) {
      positions[node_id] = std::make_pair(x, y);
      loaded_count++;
    }
  }
  file.close();

  if (loaded_count > 0) {
    std::cout << "[NodeMesh] Loaded " << loaded_count << " custom positions from " << position_file << std::endl;
  }

  return positions;
}

void NodeMesh::load_custom_positions(const std::string& position_file) {
  if (position_file.empty()) return;

  // Load cache if needed (only once for all nodes)
  if (cached_position_file_ != position_file) {
    position_cache_ = load_all_positions(position_file);
    cached_position_file_ = position_file;
  }

  // Calculate global node ID for MultiChipMesh
  // For SingleChipMesh: chip_id = -1, so use node_id directly
  // For MultiChipMesh: global_id = chip_id * nodes_per_chip + node_id
  int global_id;
  if (id_.chip_id >= 0) {
    // MultiChipMesh: calculate global ID
    int nodes_per_chip = k_node_ * k_node_;
    global_id = id_.chip_id * nodes_per_chip + id_.node_id;
  } else {
    // SingleChipMesh: use node_id directly
    global_id = id_.node_id;
  }

  // Look up this node's position in the cache using global ID
  auto it = position_cache_.find(global_id);
  if (it != position_cache_.end()) {
    x_ = it->second.first;
    y_ = it->second.second;
  }
}

ChipMesh::ChipMesh(int k_node, int vc_num, int buffer_size) {
  k_node_ = k_node;
  number_nodes_ = k_node_ * k_node_;
  number_cores_ = number_nodes_;
  // port_number_ = Knode_ * 4 - 4;
  nodes_.reserve(number_nodes_);
  chip_coordinate_.resize(2);
  for (int node_id = 0; node_id < number_nodes_; node_id++) {
    nodes_.push_back(new NodeMesh(k_node_, vc_num, buffer_size));
  }
}

ChipMesh::~ChipMesh() {
  for (auto node : nodes_) {
    delete node;
  }
  nodes_.clear();
}

void ChipMesh::build_position_index() {
  position_to_node_id_.clear();
  for (int node_id = 0; node_id < number_nodes_; node_id++) {
    NodeMesh* node = get_node(node_id);
    std::pair<int, int> pos = std::make_pair(node->x_, node->y_);
    position_to_node_id_[pos] = node_id;
  }
  std::cout << "[ChipMesh] Built position index: " << position_to_node_id_.size() << " nodes" << std::endl;
}

int ChipMesh::find_node_at(int x, int y) {
  auto it = position_to_node_id_.find(std::make_pair(x, y));
  if (it != position_to_node_id_.end()) {
    return it->second;
  }
  return -1;  // Not found
}

void ChipMesh::set_chip(System* system, int chip_id) {
  Chip::set_chip(system, chip_id);  // This loads custom positions via NodeMesh::set_node()

  // NEW: Build spatial index after positions are loaded
  build_position_index();

  // Build links based on COORDINATE PROXIMITY, not node_id offset
  for (int node_id = 0; node_id < number_nodes_; node_id++) {
    NodeMesh* node = get_node(node_id);
    int x = node->x_;
    int y = node->y_;

    // Find left neighbor (x-1, y)
    int left_id = find_node_at(x - 1, y);
    if (left_id >= 0) {
      node->xneg_link_node_ = NodeID(left_id, chip_id);
      node->xneg_link_buffer_ = get_node(left_id)->xpos_in_buffer_;
    }

    // Find right neighbor (x+1, y)
    int right_id = find_node_at(x + 1, y);
    if (right_id >= 0) {
      node->xpos_link_node_ = NodeID(right_id, chip_id);
      node->xpos_link_buffer_ = get_node(right_id)->xneg_in_buffer_;
    }

    // Find bottom neighbor (x, y-1)
    int bottom_id = find_node_at(x, y - 1);
    if (bottom_id >= 0) {
      node->yneg_link_node_ = NodeID(bottom_id, chip_id);
      node->yneg_link_buffer_ = get_node(bottom_id)->ypos_in_buffer_;
    }

    // Find top neighbor (x, y+1)
    int top_id = find_node_at(x, y + 1);
    if (top_id >= 0) {
      node->ypos_link_node_ = NodeID(top_id, chip_id);
      node->ypos_link_buffer_ = get_node(top_id)->yneg_in_buffer_;
    }
  }
}