#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

#if defined __linux__

#include "sys/sysinfo.h"
#include "sys/times.h"

#elif defined __APPLE__

#include "sys/sysctl.h"
#include "sys/resource.h"
#include "mach/mach_init.h"
#include "mach/mach_error.h"
#include "mach/mach_host.h"
#include "mach/vm_map.h"
#include "mach/mach_time.h"
#include "mach/mach.h"
#include "mach/vm_statistics.h"

#endif

#include "sys/types.h"

#include "meta_table_manager.hpp"

#include "constant_mappings.hpp"
#include "hyrise.hpp"
#include "resolve_type.hpp"
#include "statistics/table_statistics.hpp"
#include "storage/base_encoded_segment.hpp"
#include "storage/dictionary_segment.hpp"
#include "storage/fixed_string_dictionary_segment.hpp"
#include "storage/segment_iterables/any_segment_iterable.hpp"
#include "storage/table.hpp"
#include "storage/table_column_definition.hpp"

namespace {

using namespace opossum;  // NOLINT

// TODO(anyone): #1968 introduced this namespace. With the expected growth of the meta table manager of time, there
//               might be a large number of helper function that are only loosely related to the core functionality
//               of the MetaTableManager. If this becomes the case, restructure and move the functions to other files.
size_t get_distinct_value_count(const std::shared_ptr<BaseSegment>& segment) {
  auto distinct_value_count = size_t{0};
  resolve_data_type(segment->data_type(), [&](auto type) {
    using ColumnDataType = typename decltype(type)::type;

    // For dictionary segments, an early (and much faster) exit is possible by using the dictionary size
    if (const auto dictionary_segment = std::dynamic_pointer_cast<const DictionarySegment<ColumnDataType>>(segment)) {
      distinct_value_count = dictionary_segment->dictionary()->size();
      return;
    } else if (const auto fs_dictionary_segment =
                   std::dynamic_pointer_cast<const FixedStringDictionarySegment<pmr_string>>(segment)) {
      distinct_value_count = fs_dictionary_segment->fixed_string_dictionary()->size();
      return;
    }

    std::unordered_set<ColumnDataType> distinct_values;
    auto iterable = create_any_segment_iterable<ColumnDataType>(*segment);
    iterable.with_iterators([&](auto it, auto end) {
      for (; it != end; ++it) {
        const auto segment_item = *it;
        if (!segment_item.is_null()) {
          distinct_values.insert(segment_item.value());
        }
      }
    });
    distinct_value_count = distinct_values.size();
  });
  return distinct_value_count;
}

auto gather_segment_meta_data(const std::shared_ptr<Table>& meta_table, const MemoryUsageCalculationMode mode) {
  for (const auto& [table_name, table] : Hyrise::get().storage_manager.tables()) {
    for (auto chunk_id = ChunkID{0}; chunk_id < table->chunk_count(); ++chunk_id) {
      for (auto column_id = ColumnID{0}; column_id < table->column_count(); ++column_id) {
        const auto& chunk = table->get_chunk(chunk_id);
        const auto& segment = chunk->get_segment(column_id);

        const auto data_type = pmr_string{data_type_to_string.left.at(table->column_data_type(column_id))};

        const auto estimated_size = segment->memory_usage(mode);
        AllTypeVariant encoding = NULL_VALUE;
        AllTypeVariant vector_compression = NULL_VALUE;
        if (const auto& encoded_segment = std::dynamic_pointer_cast<BaseEncodedSegment>(segment)) {
          encoding = pmr_string{encoding_type_to_string.left.at(encoded_segment->encoding_type())};

          if (encoded_segment->compressed_vector_type()) {
            std::stringstream ss;
            ss << *encoded_segment->compressed_vector_type();
            vector_compression = pmr_string{ss.str()};
          }
        }

        if (mode == MemoryUsageCalculationMode::Full) {
          const auto distinct_value_count = static_cast<int64_t>(get_distinct_value_count(segment));
          meta_table->append({pmr_string{table_name}, static_cast<int32_t>(chunk_id), static_cast<int32_t>(column_id),
                              pmr_string{table->column_name(column_id)}, data_type, distinct_value_count, encoding,
                              vector_compression, static_cast<int64_t>(estimated_size)});
        } else {
          meta_table->append({pmr_string{table_name}, static_cast<int32_t>(chunk_id), static_cast<int32_t>(column_id),
                              pmr_string{table->column_name(column_id)}, data_type, encoding, vector_compression,
                              static_cast<int64_t>(estimated_size)});
        }
      }
    }
  }
}

struct LoadAvg {
  float load_1_min;
  float load_5_min;
  float load_15_min;
};

LoadAvg get_load_avg() {
#if defined __linux__

  std::ifstream load_avg_file;
  load_avg_file.open("/proc/loadavg", std::ifstream::in);

  std::string load_avg_value;
  std::vector<float> load_avg_values;
  for (int value_index = 0; value_index < 3; ++value_index) {
    std::getline(load_avg_file, load_avg_value, ' ');
    load_avg_values.push_back(std::stof(load_avg_value));
  }
  load_avg_file.close();

  return {load_avg_values[0], load_avg_values[1], load_avg_values[2]};

#elif defined __APPLE__

  loadavg load_avg;
  size_t size = sizeof(load_avg);
  if (sysctlbyname("vm.loadavg", &load_avg, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl vm.loadavg");
  }

  return {static_cast<float>(load_avg.ldavg[0]) / static_cast<float>(load_avg.fscale),
          static_cast<float>(load_avg.ldavg[1]) / static_cast<float>(load_avg.fscale),
          static_cast<float>(load_avg.ldavg[2]) / static_cast<float>(load_avg.fscale)};

#endif

  Fail("Method not implemented for this platform");
}

int get_cpu_count() {
#if defined __linux__

  std::ifstream cpu_info_file;
  cpu_info_file.open("/proc/cpuinfo", std::ifstream::in);

  uint32_t processors = 0;
  for (std::string cpu_info_line; std::getline(cpu_info_file, cpu_info_line);) {
    if (cpu_info_line.rfind("processor", 0) == 0) ++processors;
  }

  cpu_info_file.close();

  return processors;

#elif defined __APPLE__

  uint32_t processors;
  size_t size = sizeof(processors);
  if (sysctlbyname("hw.ncpu", &processors, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl hw.ncpu");
  }

  return processors;
#endif

  Fail("Method not implemented for this platform");
}

float get_system_cpu_usage() {
#if defined __linux__

  static uint64_t last_user_time = 0u, last_user_nice_time = 0u, last_kernel_time = 0u, last_idle_time = 0u;

  std::ifstream stat_file;
  stat_file.open("/proc/stat", std::ifstream::in);

  std::string cpu_line;
  std::getline(stat_file, cpu_line);
  uint64_t user_time, user_nice_time, kernel_time, idle_time;
  std::sscanf(cpu_line.c_str(), "cpu %lu %lu %lu %lu", &user_time, &user_nice_time, &kernel_time, &idle_time);
  stat_file.close();

  auto used = (user_time - last_user_time) + (user_nice_time - last_user_nice_time) + (kernel_time - last_kernel_time);
  auto total = used + (idle_time - last_idle_time);

  last_user_time = user_time;
  last_user_nice_time = user_nice_time;
  last_kernel_time = kernel_time;
  last_idle_time = idle_time;

  auto cpus = get_cpu_count();

  return (100.0 * used) / (total * cpus);

#elif defined __APPLE__

  static uint64_t last_total_ticks = 0u, last_idle_ticks = 0u;

  host_cpu_load_info_data_t cpu_info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu_info), &count) != KERN_SUCCESS) {
    Fail("Unable to access host_statistics");
  }

  uint64_t total_ticks = 0;
  for (int cpu_state = 0; cpu_state <= CPU_STATE_MAX; ++cpu_state) {
    total_ticks += cpu_info.cpu_ticks[cpu_state];
  }
  auto idle_ticks = cpu_info.cpu_ticks[CPU_STATE_IDLE];

  auto total = total_ticks - last_total_ticks;
  auto idle = idle_ticks - last_idle_ticks;

  last_total_ticks = total_ticks;
  last_idle_ticks = idle_ticks;

  auto cpus = get_cpu_count();

  return 100.0f * (1.0f - (static_cast<float>(idle) / static_cast<float>(total))) / cpus;

#endif

  Fail("Method not implemented for this platform");
}


float get_process_cpu_usage() {
#if defined __linux__

  static clock_t last_clock_time = 0u, last_kernel_time = 0u, last_user_time = 0u;
  struct tms timeSample;

  auto clock_time = times(&timeSample);
  auto kernel_time = timeSample.tms_stime;
  auto user_time = timeSample.tms_utime;

  auto used = (user_time - last_user_time) + (kernel_time - last_kernel_time);
  auto total = clock_time - last_clock_time;

  last_user_time = user_time;
  last_kernel_time = kernel_time;
  last_clock_time = clock_time;

  auto cpus = get_cpu_count();

  return (100.0 * used) / (total * cpus);

#elif defined __APPLE__

  static uint64_t last_clock_time = 0u, last_system_time = 0u, last_user_time = 0u;

  mach_timebase_info_data_t info;
  mach_timebase_info(&info);

  uint64_t clock_time = mach_absolute_time();

  struct rusage resource_usage;
  if (getrusage(RUSAGE_SELF, &resource_usage)) {
    Fail("Unable to access rusage");
  }

  uint32_t nano = 1'000'000'000, micro = 1'000;
  uint64_t system_time = resource_usage.ru_stime.tv_sec * nano + resource_usage.ru_stime.tv_usec * micro;
  uint64_t user_time = resource_usage.ru_utime.tv_sec * nano + resource_usage.ru_utime.tv_usec * micro;

  auto used = (user_time - last_user_time) + (system_time - last_system_time);
  auto total = (clock_time - last_clock_time) * info.numer / info.denom;

  last_clock_time = clock_time;
  last_user_time = user_time;
  last_system_time = system_time;

  return (100.0f * used) / (total);

#endif

  Fail("Method not implemented for this platform");
}


struct SystemMemoryUsage {
  int64_t total_ram;
  int64_t total_swap;
  int64_t total_memory;
  int64_t free_ram;
  int64_t free_swap;
  int64_t free_memory;
};

SystemMemoryUsage get_system_memory_usage() {
#if defined __linux__

  struct sysinfo memory_info;
  sysinfo(&memory_info);

  SystemMemoryUsage memory_usage;
  memory_usage.total_ram = memory_info.totalram * memory_info.mem_unit;
  memory_usage.total_swap = memory_info.totalswap * memory_info.mem_unit;
  memory_usage.free_ram = memory_info.freeram * memory_info.mem_unit;
  memory_usage.free_swap = memory_info.freeswap * memory_info.mem_unit;
  memory_usage.total_memory = memory_usage.total_ram + memory_usage.total_swap;
  memory_usage.free_memory = memory_usage.free_ram + memory_usage.free_swap;

  return memory_usage;

#elif defined __APPLE__

  int64_t physical_memory;
  size_t size = sizeof(physical_memory);
  if (sysctlbyname("hw.memsize", &physical_memory, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl hw.memsize");
  }

  // Attention: total swap might change if more swap is needed
  xsw_usage swap_usage;
  size = sizeof(swap_usage);
  if (sysctlbyname("vm.swapusage", &swap_usage, &size, nullptr, 0) != 0) {
    Fail("Unable to call sysctl vm.swapusage");
  }

  vm_size_t page_size;
  vm_statistics64_data_t vm_statistics;
  mach_msg_type_number_t count = sizeof(vm_statistics) / sizeof(natural_t);

  if (host_page_size(mach_host_self(), &page_size) != KERN_SUCCESS ||
      host_statistics64(mach_host_self(), HOST_VM_INFO, reinterpret_cast<host_info64_t>(&vm_statistics), &count) != KERN_SUCCESS) {
    Fail("Unable to access host_page_size or host_statistics64");
  }

  SystemMemoryUsage memory_usage;
  memory_usage.total_ram = physical_memory;
  memory_usage.total_swap = swap_usage.xsu_total;
  memory_usage.free_swap = swap_usage.xsu_avail;
  memory_usage.free_ram = vm_statistics.free_count * page_size;

  // auto used = (vm_statistics.active_couunt + vm_statistice.inactive_count + vm_statistics.wire_count) * page_size;

  return memory_usage;

#endif

  Fail("Method not implemented for this platform");
}

#if defined __linux__
int64_t int_from_string(std::string input_string) {
  size_t index = 0;
  size_t begin = 0, end = input_string.length() - 1;

  for (; index < input_string.length(); ++index) {
    if (isdigit(input_string[index])) {
      begin = index;
      break;
    }
  }
  for (; index < input_string.length(); ++index) {
    if (!isdigit(input_string[index])) {
      end = index;
      break;
    }
  }
  return std::stol(input_string.substr(begin, end - begin));
}
#endif

struct ProcessMemoryUsage {
  int64_t virtual_memory;
  int64_t physical_memory;
};

ProcessMemoryUsage get_process_memory_usage() {
#if defined __linux__

  std::ifstream self_status_file;
  self_status_file.open("/proc/self/status", std::ifstream::in);

  ProcessMemoryUsage memory_usage;
  for (std::string self_status_line; std::getline(self_status_file, self_status_line);) {
    if (self_status_line.rfind("VmSize", 0) == 0) {
      memory_usage.virtual_memory = int_from_string(self_status_line) * 1000;
    } else if (self_status_line.rfind("VmRSS", 0) == 0) {
      memory_usage.physical_memory = int_from_string(self_status_line) * 1000;
    }
  }

  self_status_file.close();

  return memory_usage;

#elif defined __APPLE__

  struct task_basic_info info;
  mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    Fail("Unable to access task_info");
  }

  return {static_cast<int64_t>(info.virtual_size), static_cast<int64_t>(info.resident_size)};

#endif

  Fail("Method not implemented for this platform");
}

auto gather_workload_meta_data(const std::shared_ptr<Table>& meta_table) {
  auto system_cpu_usage = get_system_cpu_usage();
  auto process_cpu_usage = get_process_cpu_usage();
  auto load_avg = get_load_avg();
  auto system_memory_usage = get_system_memory_usage();
  auto process_memory_usage = get_process_memory_usage();

  meta_table->append({system_cpu_usage, process_cpu_usage, load_avg.load_1_min, load_avg.load_5_min,
                      load_avg.load_15_min, system_memory_usage.total_ram, system_memory_usage.free_ram,
                      process_memory_usage.virtual_memory, process_memory_usage.physical_memory});

}

}  // namespace

namespace opossum {

MetaTableManager::MetaTableManager() {
  _methods["tables"] = &MetaTableManager::generate_tables_table;
  _methods["columns"] = &MetaTableManager::generate_columns_table;
  _methods["chunks"] = &MetaTableManager::generate_chunks_table;
  _methods["chunk_sort_orders"] = &MetaTableManager::generate_chunk_sort_orders_table;
  _methods["segments"] = &MetaTableManager::generate_segments_table;
  _methods["workload"] = &MetaTableManager::generate_workload_table;
  _methods["segments_accurate"] = &MetaTableManager::generate_accurate_segments_table;

  _table_names.reserve(_methods.size());
  for (const auto& [table_name, _] : _methods) {
    _table_names.emplace_back(table_name);
  }
  std::sort(_table_names.begin(), _table_names.end());

  get_system_cpu_usage();
  get_process_cpu_usage();
}

const std::vector<std::string>& MetaTableManager::table_names() const { return _table_names; }

std::shared_ptr<Table> MetaTableManager::generate_table(const std::string& table_name) const {
  const auto table = _methods.at(table_name)();
  table->set_table_statistics(TableStatistics::from_table(*table));
  return table;
}

std::shared_ptr<Table> MetaTableManager::generate_tables_table() {
  const auto columns = TableColumnDefinitions{{"table_name", DataType::String, false},
                                              {"column_count", DataType::Int, false},
                                              {"row_count", DataType::Long, false},
                                              {"chunk_count", DataType::Int, false},
                                              {"max_chunk_size", DataType::Long, false}};
  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);

  for (const auto& [table_name, table] : Hyrise::get().storage_manager.tables()) {
    output_table->append({pmr_string{table_name}, static_cast<int32_t>(table->column_count()),
                          static_cast<int64_t>(table->row_count()), static_cast<int32_t>(table->chunk_count()),
                          static_cast<int64_t>(table->max_chunk_size())});
  }

  return output_table;
}

std::shared_ptr<Table> MetaTableManager::generate_columns_table() {
  const auto columns = TableColumnDefinitions{{"table_name", DataType::String, false},
                                              {"column_name", DataType::String, false},
                                              {"data_type", DataType::String, false},
                                              {"nullable", DataType::Int, false}};
  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);

  for (const auto& [table_name, table] : Hyrise::get().storage_manager.tables()) {
    for (auto column_id = ColumnID{0}; column_id < table->column_count(); ++column_id) {
      output_table->append({pmr_string{table_name}, static_cast<pmr_string>(table->column_name(column_id)),
                            static_cast<pmr_string>(data_type_to_string.left.at(table->column_data_type(column_id))),
                            static_cast<int32_t>(table->column_is_nullable(column_id))});
    }
  }

  return output_table;
}

std::shared_ptr<Table> MetaTableManager::generate_chunks_table() {
  const auto columns = TableColumnDefinitions{{"table_name", DataType::String, false},
                                              {"chunk_id", DataType::Int, false},
                                              {"row_count", DataType::Long, false},
                                              {"invalid_row_count", DataType::Long, false},
                                              {"cleanup_commit_id", DataType::Long, true}};
  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);

  for (const auto& [table_name, table] : Hyrise::get().storage_manager.tables()) {
    for (auto chunk_id = ChunkID{0}; chunk_id < table->chunk_count(); ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      const auto cleanup_commit_id = chunk->get_cleanup_commit_id()
                                         ? AllTypeVariant{static_cast<int64_t>(*chunk->get_cleanup_commit_id())}
                                         : NULL_VALUE;
      output_table->append({pmr_string{table_name}, static_cast<int32_t>(chunk_id), static_cast<int64_t>(chunk->size()),
                            static_cast<int64_t>(chunk->invalid_row_count()), cleanup_commit_id});
    }
  }

  return output_table;
}

/**
 * At the moment, each chunk can be sorted by exactly one column or none. Hence, having a column within the chunk table
 * would be sufficient. However, this will change in the near future (e.g., when a sort-merge join evicts a chunk that
 * is sorted on two columns). To prepare for this change, this additional table stores the sort orders and allows a
 * chunk to have multiple sort orders. Cascading sort orders for chunks are currently not planned.
 */
std::shared_ptr<Table> MetaTableManager::generate_chunk_sort_orders_table() {
  const auto columns = TableColumnDefinitions{{"table_name", DataType::String, false},
                                              {"chunk_id", DataType::Int, false},
                                              {"column_id", DataType::Int, false},
                                              {"order_mode", DataType::String, false}};
  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);

  for (const auto& [table_name, table] : Hyrise::get().storage_manager.tables()) {
    for (auto chunk_id = ChunkID{0}; chunk_id < table->chunk_count(); ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      const auto ordered_by = chunk->ordered_by();
      if (ordered_by) {
        std::stringstream order_by_mode_steam;
        order_by_mode_steam << ordered_by->second;
        output_table->append({pmr_string{table_name}, static_cast<int32_t>(chunk_id),
                              static_cast<int32_t>(ordered_by->first), pmr_string{order_by_mode_steam.str()}});
      }
    }
  }

  return output_table;
}

std::shared_ptr<Table> MetaTableManager::generate_workload_table() {
  const auto columns = TableColumnDefinitions{{"cpu_system_usage", DataType::Float, false},
                                              {"cpu_process_usage", DataType::Float, false},
                                              {"load_average_1_min", DataType::Float, false},
                                              {"load_average_5_min", DataType::Float, false},
                                              {"load_average_15_min", DataType::Float, false},
                                              {"system_ram_total", DataType::Long, false},
                                              {"system_ram_free", DataType::Long, false},
                                              {"process_virtual_memory", DataType::Long, false},
                                              {"process_physical_memory", DataType::Long, false}};

  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);

  gather_workload_meta_data(output_table);

  return output_table;
}

std::shared_ptr<Table> MetaTableManager::generate_segments_table() {
  const auto columns = TableColumnDefinitions{{"table_name", DataType::String, false},
                                              {"chunk_id", DataType::Int, false},
                                              {"column_id", DataType::Int, false},
                                              {"column_name", DataType::String, false},
                                              {"column_data_type", DataType::String, false},
                                              {"encoding_type", DataType::String, true},
                                              {"vector_compression_type", DataType::String, true},
                                              {"estimated_size_in_bytes", DataType::Long, false}};

  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);
  gather_segment_meta_data(output_table, MemoryUsageCalculationMode::Sampled);

  return output_table;
}

std::shared_ptr<Table> MetaTableManager::generate_accurate_segments_table() {
  PerformanceWarning("Accurate segment information are expensive to gather. Use with caution.");
  const auto columns = TableColumnDefinitions{
      {"table_name", DataType::String, false},       {"chunk_id", DataType::Int, false},
      {"column_id", DataType::Int, false},           {"column_name", DataType::String, false},
      {"column_data_type", DataType::String, false}, {"distinct_value_count", DataType::Long, false},
      {"encoding_type", DataType::String, true},     {"vector_compression_type", DataType::String, true},
      {"size_in_bytes", DataType::Long, false}};

  auto output_table = std::make_shared<Table>(columns, TableType::Data, std::nullopt, UseMvcc::Yes);
  gather_segment_meta_data(output_table, MemoryUsageCalculationMode::Full);

  return output_table;
}

bool MetaTableManager::is_meta_table_name(const std::string& name) {
  const auto prefix_len = META_PREFIX.size();
  return name.size() > prefix_len && std::string_view{&name[0], prefix_len} == MetaTableManager::META_PREFIX;
}

}  // namespace opossum
