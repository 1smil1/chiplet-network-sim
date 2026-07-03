#include "multiple_chip_mesh.h"
#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <fstream>

MultiChipMesh::MultiChipMesh() {
  // topology parameters
  read_config();
  num_chips_ = chip_w_ * chip_h_;
  if (use_nonuniform_grid_) {
    int max_chip_id = -1;
    for (std::unordered_map<int, ChipletGridInfo>::const_iterator it = chiplet_grids_.begin();
         it != chiplet_grids_.end(); ++it) {
      max_chip_id = std::max(max_chip_id, it->first);
    }
    num_chips_ = max_chip_id + 1;
    chip_h_ = (num_chips_ + chip_w_ - 1) / chip_w_;
  }
  num_nodes_ = 0;
  for (int chip_id = 0; chip_id < num_chips_; chip_id++) {
    ChipletGridInfo grid = grid_for_chip(chip_id);
    chips_.push_back(new ChipMesh(k_node_, param->vc_number, param->buffer_size,
                                  grid.grid_x, grid.grid_y));
    get_chip(chip_id)->chip_coordinate_[0] = chip_id % chip_w_;
    get_chip(chip_id)->chip_coordinate_[1] = chip_id / chip_w_;
    get_chip(chip_id)->set_chip(this, chip_id);
    num_nodes_ += get_chip(chip_id)->number_nodes_;
  }
  num_cores_ = num_nodes_;
  connect_chiplets();
}

MultiChipMesh::~MultiChipMesh() {
  for (auto chiplet : chips_) delete chiplet;
  chips_.clear();
}

void MultiChipMesh::read_config() {
  k_node_ = param->params_ptree.get<int>("Network.k_node", 4);

  // NEW: Support chip_w and chip_h for rectangular chiplet grids
  // Fallback to k_chip for backward compatibility (square grids)
  auto chip_w_opt = param->params_ptree.get_optional<int>("Network.chip_w");
  auto chip_h_opt = param->params_ptree.get_optional<int>("Network.chip_h");

  if (chip_w_opt && chip_h_opt) {
    // Use new rectangular grid format
    chip_w_ = chip_w_opt.get();
    chip_h_ = chip_h_opt.get();
    k_chip_ = chip_w_;  // For backward compatibility
    printf("Multi Chip 2D-mesh, %ix%i chiplets (rectangular grid), each chiplet %ix%i nodes\n",
           chip_w_, chip_h_, k_node_, k_node_);
  } else {
    // Fallback to square grid (k_chip)
    k_chip_ = param->params_ptree.get<int>("Network.k_chip", 2);
    chip_w_ = k_chip_;
    chip_h_ = k_chip_;
    printf("Multi Chip 2D-mesh, %ix%i chiplets (square grid), each chiplet %ix%i nodes\n",
           k_chip_, k_chip_, k_node_, k_node_);
  }

  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "XY");
  if (algorithm_ == "NFR_adaptive") assert(param->vc_number >= 2);
  d2d_IF_ = param->params_ptree.get<std::string>("Network.d2d_IF", "off_chip_parallel");

  // Load position cache for Method 2 (py_node_id based)
  if (!param->position_file.empty()) {
    NodeMesh::position_cache_ = NodeMesh::load_all_positions(param->position_file);
    NodeMesh::cached_position_file_ = param->position_file;
    fprintf(stderr, "[MultiChipMesh] Loaded position cache for Method 2: %zu entries\n",
            NodeMesh::position_cache_.size());
  }

  load_nonuniform_tier_grid();
}

void MultiChipMesh::load_nonuniform_tier_grid() {
  chiplet_grids_.clear();
  use_nonuniform_grid_ = false;

  if (param->nonuniform_tier_grid_file.empty()) {
    return;
  }

  std::ifstream file(param->nonuniform_tier_grid_file);
  if (!file.good()) {
    std::cerr << "[MultiChipMesh] Warning: cannot open nonuniform grid file: "
              << param->nonuniform_tier_grid_file << std::endl;
    return;
  }
  file.close();

  boost::property_tree::ptree root;
  try {
    boost::property_tree::read_json(param->nonuniform_tier_grid_file, root);
    const bool enabled = root.get<bool>("enabled", root.get<bool>("metadata.enabled", false));
    if (!enabled) {
      return;
    }
    for (const auto& item : root.get_child("chiplets")) {
      const int chip_id = std::stoi(item.first);
      ChipletGridInfo grid;
      boost::property_tree::ptree origin_node = item.second.get_child("origin");
      boost::property_tree::ptree::const_iterator origin_it = origin_node.begin();
      if (origin_it == origin_node.end()) {
        continue;
      }
      grid.origin_x = origin_it->second.get_value<int>();
      ++origin_it;
      if (origin_it == origin_node.end()) {
        continue;
      }
      grid.origin_y = origin_it->second.get_value<int>();
      grid.node_id_base = item.second.get<int>("node_id_base");
      boost::property_tree::ptree grid_node = item.second.get_child("grid");
      if (grid_node.empty()) {
        continue;
      }
      boost::property_tree::ptree::const_iterator grid_it = grid_node.begin();
      grid.grid_x = grid_it->second.get_value<int>();
      ++grid_it;
      if (grid_it == grid_node.end()) {
        continue;
      }
      grid.grid_y = grid_it->second.get_value<int>();
      if (grid.grid_x <= 0 || grid.grid_y <= 0 || grid.node_id_base < 0) {
        std::cerr << "[MultiChipMesh] Ignoring invalid nonuniform grid for chip "
                  << chip_id << ": " << grid.grid_x << "x" << grid.grid_y << std::endl;
        continue;
      }
      chiplet_grids_[chip_id] = grid;
    }
  } catch (const std::exception& e) {
    std::cerr << "[MultiChipMesh] Warning: failed to read nonuniform grid file "
              << param->nonuniform_tier_grid_file << ": " << e.what() << std::endl;
    chiplet_grids_.clear();
  }

  use_nonuniform_grid_ = !chiplet_grids_.empty();
  if (use_nonuniform_grid_) {
    std::cerr << "[MultiChipMesh] Loaded nonuniform active grids for "
              << chiplet_grids_.size() << " chiplets" << std::endl;
  }
}

ChipletGridInfo MultiChipMesh::grid_for_chip(int chip_id) const {
  std::unordered_map<int, ChipletGridInfo>::const_iterator it = chiplet_grids_.find(chip_id);
  if (it != chiplet_grids_.end()) {
    return it->second;
  }
  if (use_nonuniform_grid_) {
    ChipletGridInfo grid;
    return grid;
  }
  ChipletGridInfo grid;
  grid.origin_x = (chip_id % chip_w_) * k_node_;
  grid.origin_y = (chip_id / chip_w_) * k_node_;
  grid.grid_x = k_node_;
  grid.grid_y = k_node_;
  grid.node_id_base = chip_id * k_node_ * k_node_;
  return grid;
}

bool MultiChipMesh::position_to_nodeid(int x, int y, int* chip_id, int* node_id) const {
  if (x < 0 || y < 0 || chip_id == nullptr || node_id == nullptr) {
    return false;
  }

  if (use_nonuniform_grid_) {
    for (std::unordered_map<int, ChipletGridInfo>::const_iterator it = chiplet_grids_.begin();
         it != chiplet_grids_.end(); ++it) {
      const ChipletGridInfo& grid = it->second;
      const int local_x = x - grid.origin_x;
      const int local_y = y - grid.origin_y;
      if (local_x >= 0 && local_x < grid.grid_x &&
          local_y >= 0 && local_y < grid.grid_y) {
        *chip_id = it->first;
        *node_id = local_x + local_y * grid.grid_x;
        return true;
      }
    }
    return false;
  }

  const int chip_x = x / k_node_;
  const int chip_y = y / k_node_;
  if (chip_x < 0 || chip_x >= chip_w_ || chip_y < 0 || chip_y >= chip_h_) {
    return false;
  }
  *chip_id = chip_x + chip_y * chip_w_;
  *node_id = (x % k_node_) + (y % k_node_) * k_node_;
  return true;
}

NodeID MultiChipMesh::dense_id_to_nodeid(int id) const {
  for (std::unordered_map<int, ChipletGridInfo>::const_iterator it = chiplet_grids_.begin();
       it != chiplet_grids_.end(); ++it) {
    const ChipletGridInfo& grid = it->second;
    const int begin = grid.node_id_base;
    const int end = begin + grid.grid_x * grid.grid_y;
    if (id >= begin && id < end) {
      return NodeID(id - begin, it->first);
    }
  }
  std::cerr << "[MultiChipMesh] Invalid nonuniform c_node_id: " << id << std::endl;
  return NodeID(0, 0);
}

int MultiChipMesh::global_x(NodeID id) const {
  ChipletGridInfo grid = grid_for_chip(id.chip_id);
  if (grid.grid_x <= 0) {
    return 0;
  }
  return grid.origin_x + (id.node_id % grid.grid_x);
}

int MultiChipMesh::global_y(NodeID id) const {
  ChipletGridInfo grid = grid_for_chip(id.chip_id);
  if (grid.grid_x <= 0) {
    return 0;
  }
  return grid.origin_y + (id.node_id / grid.grid_x);
}

bool MultiChipMesh::is_active_node(NodeID id) const {
  if (id.chip_id < 0 || id.chip_id >= num_chips_ ||
      id.node_id < 0) {
    return false;
  }
  ChipletGridInfo grid = grid_for_chip(id.chip_id);
  if (grid.grid_x <= 0 || grid.grid_y <= 0) {
    return false;
  }
  const int x = id.node_id % grid.grid_x;
  const int y = id.node_id / grid.grid_x;
  return x < grid.grid_x && y < grid.grid_y;
}

void MultiChipMesh::connect_chiplets() {
  for (int chip_id = 0; chip_id < num_chips_; ++chip_id) {
    ChipMesh* chip = get_chip(chip_id);
    int chip_x = chip->chip_coordinate_[0];
    int chip_y = chip->chip_coordinate_[1];
    ChipletGridInfo grid = grid_for_chip(chip_id);
    if (chip_x != 0 && chip_id - 1 >= 0) {
      ChipletGridInfo left_grid = grid_for_chip(chip_id - 1);
      if (left_grid.grid_x > 0 && left_grid.grid_y > 0) {
        for (int y = 0; y < grid.grid_y; ++y) {
          NodeMesh* node = chip->get_node(y * grid.grid_x);
          const int left_y = std::max(0, std::min(y, left_grid.grid_y - 1));
          node->xneg_link_node_ = NodeID(left_y * left_grid.grid_x + left_grid.grid_x - 1, chip_id - 1);
          node->xneg_link_buffer_ = get_node(node->xneg_link_node_)->xpos_in_buffer_;
          if (d2d_IF_ == "off_chip_parallel")
            node->xneg_in_buffer_->channel_ = off_chip_parallel_channel;
          else if (d2d_IF_ == "off_chip_serial")
            node->xneg_in_buffer_->channel_ = off_chip_serial_channel;
          else
            std::cerr << "Unknown d2d interface: " << d2d_IF_ << std::endl;
        }
      }
    }
    if (chip_x != chip_w_ - 1 && chip_id + 1 < num_chips_) {
      ChipletGridInfo right_grid = grid_for_chip(chip_id + 1);
      if (right_grid.grid_x > 0 && right_grid.grid_y > 0) {
        for (int y = 0; y < grid.grid_y; ++y) {
          NodeMesh* node = chip->get_node(y * grid.grid_x + grid.grid_x - 1);
          const int right_y = std::max(0, std::min(y, right_grid.grid_y - 1));
          node->xpos_link_node_ = NodeID(right_y * right_grid.grid_x, chip_id + 1);
          node->xpos_link_buffer_ = get_node(node->xpos_link_node_)->xneg_in_buffer_;
          if (d2d_IF_ == "off_chip_parallel")
            node->xpos_in_buffer_->channel_ = off_chip_parallel_channel;
          else if (d2d_IF_ == "off_chip_serial")
            node->xpos_in_buffer_->channel_ = off_chip_serial_channel;
          else
            std::cerr << "Unknown d2d interface: " << d2d_IF_ << std::endl;
        }
      }
    }
    if (chip_y != 0 && chip_id - chip_w_ >= 0) {
      ChipletGridInfo bottom_grid = grid_for_chip(chip_id - chip_w_);
      if (bottom_grid.grid_x > 0 && bottom_grid.grid_y > 0) {
        for (int x = 0; x < grid.grid_x; ++x) {
          NodeMesh* node = chip->get_node(x);
          const int bottom_x = std::max(0, std::min(x, bottom_grid.grid_x - 1));
          node->yneg_link_node_ = NodeID(bottom_x + (bottom_grid.grid_y - 1) * bottom_grid.grid_x, chip_id - chip_w_);
          node->yneg_link_buffer_ = get_node(node->yneg_link_node_)->ypos_in_buffer_;
          if (d2d_IF_ == "off_chip_parallel")
            node->yneg_in_buffer_->channel_ = off_chip_parallel_channel;
          else if (d2d_IF_ == "off_chip_serial")
            node->yneg_in_buffer_->channel_ = off_chip_serial_channel;
          else
            std::cerr << "Unknown d2d interface: " << d2d_IF_ << std::endl;
        }
      }
    }
    if (chip_y != chip_h_ - 1 && chip_id + chip_w_ < num_chips_) {
      ChipletGridInfo top_grid = grid_for_chip(chip_id + chip_w_);
      if (top_grid.grid_x > 0 && top_grid.grid_y > 0) {
        for (int x = 0; x < grid.grid_x; ++x) {
          NodeMesh* node = chip->get_node(x + (grid.grid_y - 1) * grid.grid_x);
          const int top_x = std::max(0, std::min(x, top_grid.grid_x - 1));
          node->ypos_link_node_ = NodeID(top_x, chip_id + chip_w_);
          node->ypos_link_buffer_ = get_node(node->ypos_link_node_)->yneg_in_buffer_;
          if (d2d_IF_ == "off_chip_parallel")
            node->ypos_in_buffer_->channel_ = off_chip_parallel_channel;
          else if (d2d_IF_ == "off_chip_serial")
            node->ypos_in_buffer_->channel_ = off_chip_serial_channel;
          else
            std::cerr << "Unknown d2d interface: " << d2d_IF_ << std::endl;
        }
      }
    }
  }
}

void MultiChipMesh::routing_algorithm(Packet& s) const {
  if (algorithm_ == "XY")
    XY_routing(s);
  else if (algorithm_ == "NFR")
    NFR_routing(s);
  else if (algorithm_ == "NFR_adaptive")
    NFR_adaptive_routing(s);
  else
    std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}

void MultiChipMesh::add_channels(Buffer* buffer, Packet& s) const {
  if (buffer == nullptr) {
    return;
  }
  for (int i = 0; i < buffer->vc_num_; i++) {
    s.candidate_channels_.push_back(VCInfo(buffer, i));
  }
}

bool MultiChipMesh::route_toward_overlap(Packet& s, int dx, int dy) const {
  NodeMesh* current_node = get_node(s.head_trace().id);
  const int current_chip_id = s.head_trace().id.chip_id;
  const int local_x = current_node->x_;
  const int local_y = current_node->y_;
  ChipletGridInfo current_grid = grid_for_chip(current_chip_id);

  if (dx != 0) {
    const int boundary_x = (dx > 0) ? (current_grid.grid_x - 1) : 0;
    if (local_x < boundary_x) {
      add_channels(current_node->xpos_link_buffer_, s);
      return !s.candidate_channels_.empty();
    }
    if (local_x > boundary_x) {
      add_channels(current_node->xneg_link_buffer_, s);
      return !s.candidate_channels_.empty();
    }

    const int neighbor_chip_id = current_chip_id + (dx > 0 ? 1 : -1);
    if (neighbor_chip_id < 0 || neighbor_chip_id >= num_chips_) {
      return false;
    }
    ChipletGridInfo neighbor_grid = grid_for_chip(neighbor_chip_id);
    const int target_y = std::max(0, std::min(local_y, neighbor_grid.grid_y - 1));
    if (local_y < target_y) {
      add_channels(current_node->ypos_link_buffer_, s);
    } else if (local_y > target_y) {
      add_channels(current_node->yneg_link_buffer_, s);
    }
    if (s.candidate_channels_.empty()) {
      add_channels(dx > 0 ? current_node->xpos_link_buffer_ : current_node->xneg_link_buffer_, s);
    }
    return !s.candidate_channels_.empty();
  }

  if (dy != 0) {
    const int boundary_y = (dy > 0) ? (current_grid.grid_y - 1) : 0;
    if (local_y < boundary_y) {
      add_channels(current_node->ypos_link_buffer_, s);
      return !s.candidate_channels_.empty();
    }
    if (local_y > boundary_y) {
      add_channels(current_node->yneg_link_buffer_, s);
      return !s.candidate_channels_.empty();
    }

    const int neighbor_chip_id = current_chip_id + (dy > 0 ? chip_w_ : -chip_w_);
    if (neighbor_chip_id < 0 || neighbor_chip_id >= num_chips_) {
      return false;
    }
    ChipletGridInfo neighbor_grid = grid_for_chip(neighbor_chip_id);
    const int target_x = std::max(0, std::min(local_x, neighbor_grid.grid_x - 1));
    if (local_x < target_x) {
      add_channels(current_node->xpos_link_buffer_, s);
    } else if (local_x > target_x) {
      add_channels(current_node->xneg_link_buffer_, s);
    }
    if (s.candidate_channels_.empty()) {
      add_channels(dy > 0 ? current_node->ypos_link_buffer_ : current_node->yneg_link_buffer_, s);
    }
    return !s.candidate_channels_.empty();
  }

  return false;
}

void MultiChipMesh::XY_routing(Packet& s) const {
  ChipMesh* current_chip = get_chip(s.head_trace().id);
  ChipMesh* destination_chip = get_chip(s.destination_);
  NodeMesh* current_node = get_node(s.head_trace().id);
  NodeMesh* destination_node = get_node(s.destination_);

  int cur_x = global_x(s.head_trace().id);
  int cur_y = global_y(s.head_trace().id);
  int dest_x = global_x(s.destination_);
  int dest_y = global_y(s.destination_);
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  // DEBUG: Print routing info for problematic packets
  static int debug_count = 0;
  if (param->online_debug && debug_count < 50 &&
      (s.wait_timer_ > 1000 || (s.phase_id_ >= 0 && s.phase_id_ <= 2 && debug_count < 10))) {
    printf("[XY_ROUTING_DEBUG] task=%d phase=%d src=(%d:%d) dst=(%d:%d) head=(%d:%d) cur_pos=(%d,%d) dest_pos=(%d,%d) dis=(%d,%d) wait=%d\n",
           s.task_id_, s.phase_id_,
           s.source_.chip_id, s.source_.node_id,
           s.destination_.chip_id, s.destination_.node_id,
           s.head_trace().id.chip_id, s.head_trace().id.node_id,
           cur_x, cur_y, dest_x, dest_y, dis_x, dis_y, s.wait_timer_);

    // Check link buffers
    printf("[XY_ROUTING_DEBUG]   xneg_buf=%p xpos_buf=%p yneg_buf=%p ypos_buf=%p\n",
           (void*)current_node->xneg_link_buffer_, (void*)current_node->xpos_link_buffer_,
           (void*)current_node->yneg_link_buffer_, (void*)current_node->ypos_link_buffer_);
    debug_count++;
  }

  const int chip_dis_x = destination_chip->chip_coordinate_[0] - current_chip->chip_coordinate_[0];
  const int chip_dis_y = destination_chip->chip_coordinate_[1] - current_chip->chip_coordinate_[1];

  if (chip_dis_x < 0) {
    route_toward_overlap(s, -1, 0);
    if (!s.candidate_channels_.empty()) return;
  } else if (chip_dis_x > 0) {
    route_toward_overlap(s, 1, 0);
    if (!s.candidate_channels_.empty()) return;
  } else if (chip_dis_y < 0) {
    route_toward_overlap(s, 0, -1);
    if (!s.candidate_channels_.empty()) return;
  } else if (chip_dis_y > 0) {
    route_toward_overlap(s, 0, 1);
    if (!s.candidate_channels_.empty()) return;
  }

  if (dis_x < 0) {
    add_channels(current_node->xneg_link_buffer_, s);
  } else if (dis_x > 0) {
    add_channels(current_node->xpos_link_buffer_, s);
  }
  else if (dis_x == 0) {
    if (dis_y < 0) {
      add_channels(current_node->yneg_link_buffer_, s);
    }
    else if (dis_y > 0) {
      add_channels(current_node->ypos_link_buffer_, s);
    }
  }

  // DEBUG: Check if no candidate channels found
  if (param->online_debug && debug_count <= 10 &&
      s.wait_timer_ > 100000 && s.candidate_channels_.empty()) {
    printf("[XY_ROUTING_ERROR] No candidate channels! dis_x=%d dis_y=%d\n", dis_x, dis_y);
  }
  static int no_candidate_debug = 0;
  if (param->online_debug && s.candidate_channels_.empty() && no_candidate_debug < 40) {
    printf("[XY_NO_CANDIDATE] task=%d phase=%d head=(%d:%d) dst=(%d:%d) "
           "cur_local=(%d,%d) dst_local=(%d,%d) dis=(%d,%d) "
           "links[xn=%p xp=%p yn=%p yp=%p]\n",
           s.task_id_, s.phase_id_,
           s.head_trace().id.chip_id, s.head_trace().id.node_id,
           s.destination_.chip_id, s.destination_.node_id,
           current_node->x_, current_node->y_,
           destination_node->x_, destination_node->y_,
           dis_x, dis_y,
           (void*)current_node->xneg_link_buffer_,
           (void*)current_node->xpos_link_buffer_,
           (void*)current_node->yneg_link_buffer_,
           (void*)current_node->ypos_link_buffer_);
    ++no_candidate_debug;
  }
}

void MultiChipMesh::NFR_routing(Packet& s) const {
  ChipMesh* current_chip = get_chip(s.head_trace().id);
  ChipMesh* destination_chip = get_chip(s.destination_);
  NodeMesh* current_node = get_node(s.head_trace().id);
  NodeMesh* destination_node = get_node(s.destination_);

  int cur_x = global_x(s.head_trace().id);
  int cur_y = global_y(s.head_trace().id);
  int dest_x = global_x(s.destination_);
  int dest_y = global_y(s.destination_);
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  // Baseline routing: negative-first
  if (dis_x < 0 || dis_y < 0) {
    if (dis_x < 0) add_channels(current_node->xneg_link_buffer_, s);
    if (dis_y < 0) add_channels(current_node->yneg_link_buffer_, s);
  } else {
    if (dis_x > 0) add_channels(current_node->xpos_link_buffer_, s);
    if (dis_y > 0) add_channels(current_node->ypos_link_buffer_, s);
  }
  if (s.candidate_channels_.empty()) {
    if (dis_x != 0) route_toward_overlap(s, dis_x, 0);
    else if (dis_y != 0) route_toward_overlap(s, 0, dis_y);
  }
}

void MultiChipMesh::NFR_adaptive_routing(Packet& s) const {
  ChipMesh* current_chip = get_chip(s.head_trace().id);
  ChipMesh* destination_chip = get_chip(s.destination_);
  NodeMesh* current_node = get_node(s.head_trace().id);
  NodeMesh* destination_node = get_node(s.destination_);

  int cur_x = global_x(s.head_trace().id);
  int cur_y = global_y(s.head_trace().id);
  int dest_x = global_x(s.destination_);
  int dest_y = global_y(s.destination_);
  int dis_x = dest_x - cur_x;  // x offset
  int dis_y = dest_y - cur_y;  // y offset

  // Adaptive Routing Channels
  if (dis_x < 0 && current_node->xneg_link_buffer_ != nullptr)
    for (int i = 0; i < current_node->xneg_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->xneg_link_buffer_, i));
  else if (dis_x > 0 && current_node->xpos_link_buffer_ != nullptr)
    for (int i = 0; i < current_node->xpos_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->xpos_link_buffer_, i));
  if (dis_y < 0 && current_node->yneg_link_buffer_ != nullptr)
    for (int i = 0; i < current_node->yneg_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->yneg_link_buffer_, i));
  else if (dis_y > 0 && current_node->ypos_link_buffer_ != nullptr)
    for (int i = 0; i < current_node->ypos_link_buffer_->vc_num_ - 1; i++)
      s.candidate_channels_.push_back(VCInfo(current_node->ypos_link_buffer_, i));

  // Baseline routing: negative-first
  if (dis_x < 0 || dis_y < 0) {
    if (dis_x < 0 && current_node->xneg_link_buffer_ != nullptr)
      s.candidate_channels_.push_back(
          VCInfo(current_node->xneg_link_buffer_, current_node->xneg_link_buffer_->vc_num_ - 1));
    if (dis_y < 0 && current_node->yneg_link_buffer_ != nullptr)
      s.candidate_channels_.push_back(
          VCInfo(current_node->yneg_link_buffer_, current_node->yneg_link_buffer_->vc_num_ - 1));
  } else {
    if (dis_x > 0 && current_node->xpos_link_buffer_ != nullptr)
      s.candidate_channels_.push_back(
          VCInfo(current_node->xpos_link_buffer_, current_node->xpos_link_buffer_->vc_num_ - 1));
    if (dis_y > 0 && current_node->ypos_link_buffer_ != nullptr)
      s.candidate_channels_.push_back(
          VCInfo(current_node->ypos_link_buffer_, current_node->ypos_link_buffer_->vc_num_ - 1));
  }
  if (s.candidate_channels_.empty()) {
    if (dis_x != 0) route_toward_overlap(s, dis_x, 0);
    else if (dis_y != 0) route_toward_overlap(s, 0, dis_y);
  }
}
