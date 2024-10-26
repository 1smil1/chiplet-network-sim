#include "group.h"
#include "system.h"

Group::Group() {
	system_ = nullptr;
	chip_id_ = 0;
	number_nodes_ = 0;
    number_cores_ = 0;
}

Group::~Group()
{
	for (auto node : nodes_) {
		delete node;
	}
	nodes_.clear();
}

void Group::set_group(System* system, int chip_id)
{
	system_ = system;
	chip_id_ = chip_id;
	for (int node_id = 0; node_id < number_nodes_; node_id++) {
		NodeID id(node_id, chip_id_);
		nodes_[node_id]->set_node(this, id);
	}
}

void Group::reset() {
	for (auto node : nodes_) {
		node->reset();
	}
}
