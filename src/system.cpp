#include "system.h"

#include "dragonfly_chiplet.h"
#include "dragonfly_sw.h"
#include "multiple_chip_mesh.h"
#include "multiple_chip_torus.h"
#include "single_chip_mesh.h"
#include "traffic_manager.h"

// External variable to track current simulation cycle
extern std::atomic_uint64_t current_simulation_cycle;

System::System() {
  num_chips_ = 0;
  num_nodes_ = 0;
  num_cores_ = 0;

  // router parameters
  router_stages_ = param->router_stages;

  // simulation parameters
  timeout_time_ = param->timeout_threshold;
}

System* System::New(const std::string& topology) {
  System* sys_ptr;
  if (topology == "SingleChipMesh")
    sys_ptr = new SingleChipMesh;
  else if (topology == "MultiChipMesh")
    sys_ptr = new MultiChipMesh;
  else if (topology == "MultiChipTorus")
    sys_ptr = new MultiChipTorus;
  else if (topology == "DragonflySW")
    sys_ptr = new DragonflySW;
  else if (topology == "DragonflyChiplet")
    sys_ptr = new DragonflyChiplet;
  else {
    std::cerr << "No such a topology!" << std::endl;
    return nullptr;
  }
  return sys_ptr;
}

void System::reset() {
  for (auto chip : chips_) {
    chip->reset();
  }
}

void System::onestage(Packet& p) {
  if (p.candidate_channels_.empty()) routing(p);
  if (!p.candidate_channels_.empty() && p.next_vc_.buffer == nullptr)  // VC Allocating Stage
    vc_allocate(p);
  if (p.next_vc_.buffer != nullptr && p.switch_allocated_ == false)  // Switch Allocating Stage
    switch_allocate(p);
}

void System::twostage(Packet& p) {
  if (p.candidate_channels_.empty()) routing(p);
  if (!p.candidate_channels_.empty() && p.next_vc_.buffer == nullptr)  // VC Allocating Stage
    vc_allocate(p);
  else if (p.next_vc_.buffer != nullptr && p.switch_allocated_ == false)  // Switch Allocating Stage
    switch_allocate(p);
}

void System::Threestage(Packet& p) {
  if (p.candidate_channels_.empty())  // Routing Stage
    routing(p);
  else if (!p.candidate_channels_.empty() && p.next_vc_.buffer == nullptr)  // VC Allocating Stage
    vc_allocate(p);
  else if (p.next_vc_.buffer != nullptr && p.switch_allocated_ == false)  // Switch Allocating Stage
    switch_allocate(p);
}

void System::routing(Packet& p) const {
  assert(p.candidate_channels_.empty());
  routing_algorithm(p);
  assert(!p.candidate_channels_.empty());
}

void System::vc_allocate(Packet& p) const {
  VCInfo current_vc = p.head_trace();
  static int vc_debug_count = 0;
  bool should_debug = (param->online_debug &&
                       p.phase_id_ >= 0 && p.phase_id_ <= 2 && vc_debug_count < 5);

  if (current_vc.buffer == nullptr ||
      current_vc.head_packet() == &p) {  // the packet is at the source or at the front of the queue
    if (should_debug) {
      printf("[VC_ALLOC] phase=%d candidates=%zu\n", p.phase_id_, p.candidate_channels_.size());
    }
    for (auto& vc : p.candidate_channels_) {
      if (vc.buffer->is_empty(vc.vcb))                        // try to allocate a empty vc
        if (vc.buffer->allocate_buffer(vc.vcb, p.length_)) {  // packet switching
          // allocating sucessed
          if (should_debug) printf("[VC_ALLOC] SUCCESS phase=%d allocated empty VC\n", p.phase_id_);
          p.next_vc_ = vc;
          vc_debug_count++;
          return;
        }
    }
    // no empty vc, try to allocate a free vc
    for (auto& vc : p.candidate_channels_) {
      if (vc.buffer->allocate_buffer(vc.vcb, p.length_)) {  // packet switching
        // allocating sucessed
        if (should_debug) printf("[VC_ALLOC] SUCCESS phase=%d allocated non-empty VC\n", p.phase_id_);
        p.next_vc_ = vc;
        vc_debug_count++;
        return;
      }
    }
    if (should_debug) printf("[VC_ALLOC] FAILED phase=%d no VC available\n", p.phase_id_);
    vc_debug_count++;
  }
}

void System::switch_allocate(Packet& p) {
  VCInfo current_vc = p.head_trace();
  static int sw_debug_count = 0;
  bool should_debug = (param->online_debug &&
                       p.phase_id_ >= 0 && p.phase_id_ <= 2 && sw_debug_count < 10);

  if (current_vc.buffer == nullptr) {              // the packet is at the source
    if (p.next_vc_.buffer->allocate_in_link(p)) {  // wait for link to the next buffer
      p.switch_allocated_ = true;
      if (should_debug) printf("[SW_ALLOC] SUCCESS phase=%d at source, next_vc_latency=%d\n", p.phase_id_, p.next_vc_.buffer->channel_.latency);
    } else {
      if (should_debug) printf("[SW_ALLOC] FAILED phase=%d at source, no in_link\n", p.phase_id_);
    }
    sw_debug_count++;
  } else if (current_vc.head_packet() == &p) {
    if (current_vc.buffer->allocate_sw_link()) {     // try to allocate the link to the switch
      if (p.next_vc_.buffer->allocate_in_link(p)) {  // wait for link to the next buffer
        p.switch_allocated_ = true;
        if (should_debug) printf("[SW_ALLOC] SUCCESS phase=%d at network, next_vc_latency=%d\n", p.phase_id_, p.next_vc_.buffer->channel_.latency);
      } else {
        if (should_debug) printf("[SW_ALLOC] FAILED phase=%d no in_link\n", p.phase_id_);
        current_vc.buffer->release_sw_link();
      }
    } else {
      if (should_debug) printf("[SW_ALLOC] FAILED phase=%d no sw_link\n", p.phase_id_);
    }
    sw_debug_count++;
  }
}

void System::update(Packet& p) {
  // A packet cannot be sent to itself
  assert(p.link_timer_ > 0 || p.destination_ != p.tail_trace().id);

  p.trans_timer_++;
  if (p.wait_timer_ == timeout_time_)  // timeout
    TM->message_timeout_++;

  // Processing at source node before transmission (Packetization, injection, etc.)
  if (p.head_trace().id == p.source_ && p.process_timer_ > 0) {
    p.process_timer_--;
    return;
  }

  // Routing -> VC allocating -> Switch allocating -> Transmission
  // switch_allocated_ is the final credit for message forwarding.
  if (p.link_timer_ == 0) {                     // reach the input buffer
    if (p.head_trace().id != p.destination_) {  // not reach destination
      if (router_stages_ == "OneStage") {
        onestage(p);
      } else if (router_stages_ == "TwoStage") {
        twostage(p);
      } else if (router_stages_ == "ThreeStage") {
        Threestage(p);
      } else {
        std::cerr << "No such a microarchitecture!" << std::endl;
      }
      if (!p.switch_allocated_) p.wait_timer_++;
    }
  } else {  // flying in the link
    p.link_timer_--;
  }

  VCInfo temp1, temp2;
  int i = 0;

  if (p.switch_allocated_) {
    temp1 = p.next_vc_;
    p.wait_timer_ = 0;
    p.link_timer_ = p.next_vc_.buffer->channel_.latency;
    static int sw_success_debug = 0;
    if (param->online_debug && sw_success_debug < 5 &&
        p.phase_id_ >= 0 && p.phase_id_ <= 2) {
      printf("[UPDATE] switch_allocated=true phase=%d, setting link_timer=%d, latency=%d, buffer=%p\n",
             p.phase_id_, p.link_timer_, p.next_vc_.buffer->channel_.latency, (void*)p.next_vc_.buffer);
      sw_success_debug++;
    }
#ifdef DEBUG
    TM->traffic_map_[temp1.buffer]++;
#endif  // DEBUG
    if (temp1.buffer->channel_ == on_chip_channel)
      p.internal_hops_++;
    else if (temp1.buffer->channel_ == off_chip_parallel_channel)
      p.parallel_hops_++;
    else if (temp1.buffer->channel_ == off_chip_serial_channel)
      p.serial_hops_++;
    else
      p.other_hops_++;
    p.candidate_channels_.clear();
    p.next_vc_ = VCInfo();
    p.switch_allocated_ = false;

    // DEBUG: After clearing switch_allocated
    static int after_clear_debug = 0;
    if (param->online_debug && after_clear_debug < 5 &&
        p.phase_id_ >= 0 && p.phase_id_ <= 2) {
      printf("[AFTER_CLEAR] phase=%d link_timer=%d wait=%d next_vc_buf=%p\n",
             p.phase_id_, p.link_timer_, p.wait_timer_, (void*)p.next_vc_.buffer);
      after_clear_debug++;
    }
  } else {
    temp1 = p.head_trace();
    // find the flit that fall behind the head flit
    while (i < p.length_ && p.flit_trace_[i].id == temp1.id) i++;
  }

  if (i < p.length_) {  // there is flits fall behind
    temp2 = p.flit_trace_[i];
    int k = temp1.buffer->channel_.width;  // linkwidth
    int j = 0;
    while (i < p.length_) {
      if (p.flit_trace_[i].id == temp2.id && j < k) {
        assert(p.flit_trace_[i].id != temp1.id);
        p.flit_trace_[i] = temp1;
        j++;
      } else {
        if (p.flit_trace_[i].id != temp2.id) {
          temp1 = temp2;
          temp2 = p.flit_trace_[i];
          k = temp1.buffer->channel_.width;
          j = 0;
          assert(p.flit_trace_[i].id != temp1.id);
          p.flit_trace_[i] = temp1;
          j++;
        } else {
          assert(j == k);
        }
      }
      i++;
    }
    // If last flit shift, realease link
    if (temp2.id != p.tail_trace().id) {
      p.releaselink_ = true;
      p.leaving_vc_ = temp2;
      if (temp2.buffer != nullptr) {
        temp2.buffer->release_buffer(temp2.vcb, p.length_);
      }
    }
  }
  // If the last flit reach destination, delete message
  if (p.link_timer_ == 0 && p.tail_trace().id == p.destination_) {
    VCInfo dest_vc = p.tail_trace();
    dest_vc.buffer->release_buffer(dest_vc.vcb, p.length_);
    p.finished_ = true;
    TM->message_arrived_++;
    TM->total_cycles_ += p.trans_timer_;
    TM->total_parallel_hops_ += p.parallel_hops_;
    TM->total_serial_hops_ += p.serial_hops_;
    TM->total_internal_hops_ += p.internal_hops_;
    TM->total_other_hops_ += p.other_hops_;

    // NEW: Update last arrival cycle (thread-safe compare-and-swap)
    uint64_t current_cycle = current_simulation_cycle.load();
    uint64_t prev_last = TM->last_arrival_cycle_.load();
    while (prev_last < current_cycle && !TM->last_arrival_cycle_.compare_exchange_weak(prev_last, current_cycle));

    // NEW: Track slowest packet (thread-safe)
    uint64_t prev_slowest = TM->slowest_packet_latency_.load();
    if (p.trans_timer_ > (int)prev_slowest) {
      // Try to update slowest packet if this one is slower
      if (TM->slowest_packet_latency_.compare_exchange_strong(prev_slowest, p.trans_timer_)) {
        // Successfully updated latency, now update other fields
        TM->slowest_packet_src_.store(p.source_.chip_id * 100 + p.source_.node_id);
        TM->slowest_packet_dst_.store(p.destination_.chip_id * 100 + p.destination_.node_id);
        TM->slowest_packet_length_.store(p.length_);
        TM->slowest_packet_hops_.store(p.internal_hops_);
      }
    }

    // DEBUG: Print packet state at end of update
    static int end_of_update_debug = 0;
    if (param->online_debug && end_of_update_debug < 20 &&
        p.phase_id_ >= 0 && p.phase_id_ <= 2) {
      printf("[END_OF_UPDATE] phase=%d link_timer=%d switch_alloc=%d wait=%d finished=%d next_vc_buf=%p tail_dst=(%d:%d)\n",
             p.phase_id_, p.link_timer_, p.switch_allocated_, p.wait_timer_, p.finished_,
             (void*)p.next_vc_.buffer, p.tail_trace().id.chip_id, p.tail_trace().id.node_id);
      end_of_update_debug++;
    }

    return;
  }
}
