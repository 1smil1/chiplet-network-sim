#include <condition_variable>
#include <chrono>
#include <mutex>
#include <thread>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "online_workload.h"
#include "traffic_manager.h"

// global variables
Parameters* param;
TrafficManager* TM;
System* network;
boost::mt19937 gen;  // random number generator
std::atomic_uint64_t current_simulation_cycle;  // track current cycle for arrival time recording

// multi-threading variables
static std::vector<std::thread> threads;
static volatile bool finished = false;
static std::condition_variable cv;
static std::atomic_uint64_t pkt_i;
static std::mutex* mtxs;
static volatile bool* thread_ready;
static volatile bool* worker_launch;

static std::string ReplaceExtension(const std::string& path, const std::string& new_ext) {
  size_t slash_pos = path.find_last_of("/\\");
  size_t dot_pos = path.find_last_of('.');
  if (dot_pos == std::string::npos || (slash_pos != std::string::npos && dot_pos < slash_pos)) {
    return path + new_ext;
  }
  return path.substr(0, dot_pos) + new_ext;
}

static bool WriteOfflineResultsJson(const std::string& json_path, std::string* error) {
  try {
    boost::property_tree::ptree root;
    root.put("mode", "offline");
    root.put("task_completion_cycle", TM->last_arrival_cycle_.load());
    root.put("message_arrived", TM->message_arrived_.load());
    root.put("average_latency",
             TM->message_arrived_.load() == 0
                 ? 0.0
                 : static_cast<double>(TM->total_cycles_.load()) / TM->message_arrived_.load());
    root.put("slowest_packet.latency", TM->slowest_packet_latency_.load());
    root.put("slowest_packet.src", TM->slowest_packet_src_.load());
    root.put("slowest_packet.dst", TM->slowest_packet_dst_.load());
    root.put("slowest_packet.length", TM->slowest_packet_length_.load());
    root.put("slowest_packet.hops", TM->slowest_packet_hops_.load());
    root.put("hop_summary.internal", TM->total_internal_hops_.load());
    root.put("hop_summary.parallel", TM->total_parallel_hops_.load());
    root.put("hop_summary.serial", TM->total_serial_hops_.load());
    root.put("hop_summary.other", TM->total_other_hops_.load());
    boost::property_tree::write_json(json_path, root);
    return true;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = ex.what();
    }
    return false;
  }
}

static void update_packets(std::vector<Packet*>& packets, System* system) {
  uint64_t i = pkt_i.load();
  uint64_t vec_size = packets.size();
  static int issue_width = param->issue_width;
  while (i < vec_size) {
    if (pkt_i.compare_exchange_strong(i, i + issue_width)) {
      uint64_t max_i = std::min(i + issue_width, vec_size);
      do {
        system->update(*packets[i]);
      } while (++i < max_i);
      i = pkt_i.load();
    }
  }
}

static void worker(std::vector<Packet*>& packets, System* s, int id) {
  std::unique_lock<std::mutex> lk(mtxs[id]);
  thread_ready[id] = true;
  cv.wait(lk);
  while (!finished) {
    update_packets(packets, s);
    worker_launch[id] = false;
    while (!worker_launch[id] && !finished) {
      cv.wait(lk);
    }
  }
}

static void run_one_cycle(std::vector<Packet*>& vec_pkts, System* system) {
  uint64_t j = 0;
  uint64_t vecsize = vec_pkts.size();
  for (uint64_t i = 0; i < vecsize; ++i) {
    Packet*& pkt = vec_pkts[i];
    if (pkt->releaselink_ == true) {
      pkt->tail_trace().buffer->release_in_link(*pkt);
      if (pkt->leaving_vc_.buffer != nullptr)
        pkt->leaving_vc_.buffer->release_sw_link();
      else
        assert(pkt->leaving_vc_.id == pkt->source_);
      pkt->releaselink_ = false;
    }
    if (pkt->finished_) {
      delete pkt;
    } else {
      vec_pkts[j] = pkt;
      j++;
    }
  }
  vec_pkts.resize(j);

  pkt_i.store(0);
  if (vec_pkts.size() < static_cast<size_t>(param->threads) || param->threads < 2) {
    update_packets(vec_pkts, system);
  } else {
    for (int i = 0; i < param->threads; ++i) {
      worker_launch[i] = true;
      mtxs[i].unlock();
    }
    cv.notify_all();
    for (int i = 0; i < param->threads; ++i) {
      while (worker_launch[i]) {
      }
      mtxs[i].lock();
    }
  }
}

int main(int argc, char* argv[]) {
  std::string config_file;
  if (argc > 1) config_file = argv[1];
  param = new Parameters(config_file);
  network = System::New(param->topology);
  TM = new TrafficManager();
  gen.seed(1);

  uint64_t timeout_limit = param->timeout_limit;
  double maximum_receiving_rate = 0;
  std::vector<Packet*> all_packets;

  if (param->threads > 1) {
    mtxs = new std::mutex[param->threads];
    thread_ready = new bool[param->threads];
    worker_launch = new bool[param->threads];
    pkt_i.store(0);
    for (int i = 0; i < param->threads; ++i) {
      thread_ready[i] = false;
      threads.push_back(std::thread(worker, std::ref(all_packets), network, i));
    }
    for (int i = 0; i < param->threads; ++i) {
      while (!thread_ready[i]) {
      }
      mtxs[i].lock();
    }
  }

  if (param->traffic == "online_workload") {
    OnlineWorkloadScheduler scheduler;
    std::string load_error;
    if (!scheduler.LoadFromFile(param->workload_file, network->num_cores_, &load_error)) {
      std::cerr << "[ONLINE WORKLOAD] Failed to load workload: " << load_error << std::endl;
      return 1;
    }
    std::chrono::steady_clock::time_point progress_last = std::chrono::steady_clock::now();
    const std::chrono::seconds progress_interval(5);

    while (!scheduler.AllDone()) {
      scheduler.ProcessFinishedPackets(all_packets);
      scheduler.ReclaimCompletedPackets(all_packets);
      scheduler.PromoteFinishedComputePhases();
      scheduler.StartNewComputePhases();
      int injected_packets = scheduler.InjectReadyPackets(all_packets);

      size_t unfinished_packets = 0;
      for (size_t i = 0; i < all_packets.size(); ++i) {
        if (!all_packets[i]->finished_) {
          ++unfinished_packets;
        }
      }
      const std::chrono::steady_clock::time_point progress_now =
          std::chrono::steady_clock::now();
      if (param->online_debug && progress_now - progress_last >= progress_interval) {
        scheduler.PrintProgressSummary(unfinished_packets);
        int debug_printed = 0;
        std::cout << "  [ONLINE PROGRESS] sample_packets:";
        for (size_t i = 0; i < all_packets.size(); ++i) {
          Packet* packet = all_packets[i];
          if (packet->finished_) {
            continue;
          }
          const VCInfo& head = packet->head_trace();
          std::cout << " {task=" << packet->task_id_
                    << ", phase=" << packet->phase_id_
                    << ", src=(" << packet->source_.chip_id << ":" << packet->source_.node_id << ")"
                    << ", dst=(" << packet->destination_.chip_id << ":" << packet->destination_.node_id << ")"
                    << ", head=(" << head.id.chip_id << ":" << head.id.node_id << ")"
                    << ", wait=" << packet->wait_timer_
                    << ", link=" << packet->link_timer_
                    << ", proc=" << packet->process_timer_
                    << ", trans=" << packet->trans_timer_
                    << "}";
          ++debug_printed;
          if (debug_printed >= 4) {
            break;
          }
        }
        std::cout << std::endl;
        progress_last = progress_now;
      }

      scheduler.PrintPauseEvents(unfinished_packets);
      if (unfinished_packets == 0 && injected_packets == 0) {
        if (!scheduler.FastForwardIfIdle()) {
          break;
        }
        continue;
      }

      current_simulation_cycle.store(scheduler.current_cycle());
      run_one_cycle(all_packets, network);

      // DEBUG: Print packet state after update
      static int after_update_debug = 0;
      if (after_update_debug < 10) {
        std::cout << "[AFTER_UPDATE] cycle=" << current_simulation_cycle.load() << " ";
        for (size_t i = 0; i < all_packets.size(); ++i) {
          Packet* packet = all_packets[i];
          if (!packet->finished_ && packet->phase_id_ == 0 && packet->task_id_ == 0) {
            std::cout << "phase0_pkt: link_timer=" << packet->link_timer_
                      << " switch_alloc=" << packet->switch_allocated_
                      << " wait=" << packet->wait_timer_
                      << " next_vc_buf=" << packet->next_vc_.buffer
                      << std::endl;
            after_update_debug++;
            break;
          }
        }
      }

      scheduler.AdvanceOneCycle();
    }

    scheduler.ProcessFinishedPackets(all_packets);
    scheduler.ReclaimCompletedPackets(all_packets);
    scheduler.PrintFinalSummary();
    TM->print_statistics();

    std::string json_error;
    if (!scheduler.WriteResultsJson(ReplaceExtension(param->output_file, ".json"), &json_error)) {
      std::cerr << "[ONLINE WORKLOAD] Failed to write JSON results: " << json_error << std::endl;
    }
  } else if (param->traffic == "netrace") {
    TM->injection_rate_ = (double)TM->CTX->input_trheader->num_packets /
                          TM->CTX->input_trheader->num_cycles / network->num_cores_;
    for (uint64_t i = 0; i < TM->CTX->input_trheader->num_cycles + 1000; i++) {
      current_simulation_cycle.store(i);
      TM->genMes(all_packets, i);
      run_one_cycle(all_packets, network);
    }
    TM->print_statistics();
    std::string json_error;
    if (!WriteOfflineResultsJson(ReplaceExtension(param->output_file, ".json"), &json_error)) {
      std::cerr << "[NETRACE] Failed to write JSON results: " << json_error << std::endl;
    }
    nt_close_trfile(TM->CTX);
  } else {
    bool saturated = false;
    while (!saturated) {
      TM->injection_rate_ += param->injection_increment;
      for (uint64_t i = 0; i < param->simulation_time / 2; i++) {
        TM->genMes(all_packets);
        run_one_cycle(all_packets, network);
      }
      TM->reset();
      for (uint64_t i = 0; i < param->simulation_time && TM->message_timeout_ < timeout_limit; i++) {
        TM->genMes(all_packets);
        run_one_cycle(all_packets, network);
      }
      TM->print_statistics();
      if (TM->receiving_rate() > maximum_receiving_rate) {
        maximum_receiving_rate = TM->receiving_rate();
      }
      if (TM->message_arrived_ < (TM->message_timeout_ + all_packets.size()) * 5) {
        std::cout << std::endl
                  << "Saturation point!" << std::endl
                  << "Maximum average receiving traffic: " << maximum_receiving_rate
                  << " flits/(node*cycle)" << std::endl;
        saturated = true;
#ifdef DEBUG
        for (uint64_t i = 0; i < param->simulation_time * 2; i++) {
          run_one_cycle(all_packets, network);
          if (all_packets.size() == 0) {
            std::cerr << "No deadlock!" << std::endl;
            break;
          }
        }
        if (all_packets.size() != 0) std::cerr << "Possible Deadlock!" << std::endl;
#endif
      }
      for (size_t i = 0; i < all_packets.size(); ++i) delete all_packets[i];
      all_packets.clear();
      network->reset();
      gen.seed(1);
    }
  }

  if (param->threads > 1) {
    finished = true;
    for (int i = 0; i < param->threads; ++i) {
      mtxs[i].unlock();
    }
    cv.notify_all();
    for (int i = 0; i < param->threads; ++i) {
      threads[i].join();
    }
    delete[] mtxs;
    delete[] thread_ready;
    delete[] worker_launch;
  }
  delete TM;
  delete network;
  delete param;
  return 0;
}
