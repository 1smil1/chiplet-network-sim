#include <boost/property_tree/json_parser.hpp>
#include <stdexcept>
#include "config.h"

namespace {

bool IsAbsolutePath(const std::string& path) {
  if (path.empty()) return false;
  if (path[0] == '/' || path[0] == '\\') return true;
  return path.size() >= 2 &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':';
}

std::string ConfigDirectory(const std::string& config_file) {
  const size_t slash = config_file.find_last_of("/\\");
  if (slash == std::string::npos) return "";
  return config_file.substr(0, slash);
}

std::string ResolveConfigPath(const std::string& config_file,
                              const std::string& value) {
  if (value.empty() || IsAbsolutePath(value)) return value;
  const std::string directory = ConfigDirectory(config_file);
  if (directory.empty()) return value;
  const char separator =
      directory.find('\\') != std::string::npos ? '\\' : '/';
  return directory + separator + value;
}

}  // namespace

// paser parameters from config file
Parameters::Parameters(const std::string &config_file) {
  params_ptree = boost::property_tree::ptree();
  if (!config_file.empty()) boost::property_tree::ini_parser::read_ini(config_file, params_ptree);
  config_file_path = config_file;

  topology = params_ptree.get<std::string>("Network.topology", "SingleChipMesh");
  buffer_size = params_ptree.get<int>("Network.buffer_size", 16);
  vc_number = params_ptree.get<int>("Network.vc_number", 1);
  on_chip_width_flits = params_ptree.get<int>("Network.on_chip_width_flits", 64);
  off_chip_serial_width_flits = params_ptree.get<int>("Network.off_chip_serial_width_flits", 64);
  off_chip_serial_latency_cycles = params_ptree.get<int>("Network.off_chip_serial_latency_cycles", 8);
  if (on_chip_width_flits <= 0 || off_chip_serial_width_flits <= 0 ||
      off_chip_serial_latency_cycles <= 0) {
    throw std::invalid_argument("Network channel width and latency must be positive");
  }
  router_stages = params_ptree.get<std::string>("Network.router_stages", "ThreeStage");
  processing_time = params_ptree.get<int>("Network.processing_time", 2);

  traffic = params_ptree.get<std::string>("Workload.traffic", "uniform");
  traffic_scale = params_ptree.get<int>("Workload.traffic_scale", 0);
  packet_length = params_ptree.get<int>("Workload.packet_length", 5);

  injection_increment = params_ptree.get<double>("Simulation.injection_increment", 0.1);
  simulation_time = params_ptree.get<uint64_t>("Simulation.simulation_time", 10000);
  timeout_threshold = params_ptree.get<int>("Simulation.timeout_threshold", 500);
  timeout_limit = params_ptree.get<int>("Simulation.timeout_limit", 100);
  threads = params_ptree.get<int>("Simulation.threads", 2);
  if (threads >= 2)
    issue_width = params_ptree.get<int>("Simulation.issue_width", 10);
  else
    issue_width = 1000;

  if (traffic == "sd_traces")
    trace_file = params_ptree.get<std::string>("Files.trace_file");
  else if (traffic == "netrace")
    netrace_file = params_ptree.get<std::string>("Files.netrace_file");
  else if (traffic == "online_workload")
    workload_file = params_ptree.get<std::string>("Files.workload_file");
  output_file = params_ptree.get<std::string>("Files.output_file", "../../output/output.csv");
  log_file = params_ptree.get<std::string>("Files.log_file", "../../output/log.txt");
  position_file = params_ptree.get<std::string>("Network.position_file", "");
  nonuniform_tier_grid_file = params_ptree.get<std::string>("Network.nonuniform_tier_grid_file", "");
  trace_file = ResolveConfigPath(config_file, trace_file);
  netrace_file = ResolveConfigPath(config_file, netrace_file);
  workload_file = ResolveConfigPath(config_file, workload_file);
  output_file = ResolveConfigPath(config_file, output_file);
  log_file = ResolveConfigPath(config_file, log_file);
  position_file = ResolveConfigPath(config_file, position_file);
  nonuniform_tier_grid_file = ResolveConfigPath(config_file, nonuniform_tier_grid_file);
  pause_on_first_injection = params_ptree.get<bool>("Simulation.pause_on_first_injection", true);
  pause_on_input_done = params_ptree.get<bool>("Simulation.pause_on_input_done", true);
  interactive_pause = params_ptree.get<bool>("Simulation.interactive_pause", false);
  online_debug = params_ptree.get<bool>("Simulation.online_debug", false);
  cycle_frequency_ghz = params_ptree.get<double>("Simulation.cycle_frequency_ghz", 1.0);

  print_params();
}
