#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <regex>
#include <vector>
#include <string>

#include "bpftrace.h"
#include "btf.h"
#include "list.h"
#include "utils.h"

namespace bpftrace {

const std::string kprobe_path = "/sys/kernel/debug/tracing/available_filter_functions";
const std::string tp_path = "/sys/kernel/debug/tracing/events";

inline bool search_probe(const std::string &probe, const std::regex& re)
{
  try {
    if (std::regex_search(probe, re))
      return false;
    else
      return true;
   } catch(std::regex_error& e) {
       return true;
  }
}

void list_dir(const std::string path, std::vector<std::string> &files)
{
  // yes, I know about std::filesystem::directory_iterator, but no, it wasn't available
  DIR *dp;
  struct dirent *dep;
  if ((dp = opendir(path.c_str())) == NULL)
    return;

  while ((dep = readdir(dp)) != NULL)
    files.push_back(std::string(dep->d_name));

  closedir(dp);
}

void list_probes_from_list(const std::vector<ProbeListItem> &probes_list,
                           const std::string &probetype, const std::string &search,
                           const std::regex& re)
{
  std::string probe;

  for (auto &probeListItem : probes_list)
  {
    probe = probetype + ":" + probeListItem.path + ":";

    if (!search.empty())
    {
      if (search_probe(probe, re))
        continue;
    }

    std::cout << probe  << std::endl;
  }
}

void print_tracepoint_args(const std::string &category, const std::string &event)
{
  std::string format_file_path = tp_path + "/" + category + "/" + event + "/format";
  std::ifstream format_file(format_file_path.c_str());
  std::regex re("^\tfield:.*;$", std::regex::icase | std::regex::grep |
                                 std::regex::nosubs | std::regex::optimize);
  std::string line;

  if (format_file.fail())
  {
    std::cerr << "ERROR: tracepoint format file not found: " << format_file_path << std::endl;
    return;
  }

  // Skip lines until the first empty line
  do {
    getline(format_file, line);
  } while (line.length() > 0);

  for (; getline(format_file, line); )
  {
    try {
      if (std::regex_match(line, re))
      {
        unsigned idx = line.find(":") + 1;
        line = line.substr(idx);
        idx = line.find(";") + 1;
        line = line.substr(0, idx);
        std::cout << "    " << line << std::endl;
      }
    } catch(std::regex_error& e) {
      return;
    }
  }
}

void list_probes(const BPFtrace &bpftrace, const std::string &search_input)
{
  std::string search = search_input;
  std::string probe_name;
  std::regex re;

  std::smatch probe_match;
  std::regex probe_regex(":.*");
  std::regex_search ( search, probe_match, probe_regex );

  // replace alias name with full name
  if (probe_match.size())
  {
    auto pos = probe_match.position(0);
    probe_name =  probetypeName(search.substr(0, probe_match.position(0)));
    search = probe_name + search.substr(pos, search.length());
  }

  std::string s = "^";
  for (char c : search)
  {
    if (c == '*')
      s += ".*";
    else if (c == '?')
      s += '.';
    else
      s += c;
  }
  s += '$';
  try {
    re = std::regex(s, std::regex::icase | std::regex::grep | std::regex::nosubs | std::regex::optimize);
  } catch(std::regex_error& e) {
    std::cerr << "ERROR: invalid character in search expression." << std::endl;
    return;
  }

  // software
  list_probes_from_list(SW_PROBE_LIST, "software", search, re);

  // hardware
  list_probes_from_list(HW_PROBE_LIST, "hardware", search, re);

  // uprobe
  {
    std::unique_ptr<std::istream> symbol_stream;
    std::string executable;
    std::string absolute_exe;
    bool show_all = false;

    if (bpftrace.pid() > 0)
    {
      executable = get_pid_exe(bpftrace.pid());
      absolute_exe = path_for_pid_mountns(bpftrace.pid(), executable);
    } else if (probe_name == "uprobe")
    {
      executable = search.substr(search.find(":") + 1, search.size());
      show_all = executable.find(":") == std::string::npos;
      executable = executable.substr(0, executable.find(":"));

      auto paths = resolve_binary_path(executable);
      switch (paths.size())
      {
      case 0:
        std::cerr << "uprobe target '" << executable << "' does not exist or is not executable" << std::endl;
        return;
      case 1:
        absolute_exe = paths.front();
        break;
      default:
        std::cerr << "path '" << executable << "' must refer to a unique binary but matched " << paths.size() << std::endl;
        return;
      }
    }

    if (!executable.empty())
    {
      symbol_stream = std::make_unique<std::istringstream>(
          bpftrace.extract_func_symbols_from_path(absolute_exe));

      std::string line;
      while (std::getline(*symbol_stream, line))
      {
        std::string probe = "uprobe:" + absolute_exe + ":" + line;
        if (show_all || search.empty() || !search_probe(probe, re))
          std::cout << probe << std::endl;
      }
    }
  }

  // usdt
  usdt_probe_list usdt_probes;
  bool usdt_path_list = false;
  if (bpftrace.pid() > 0)
  {
    // PID takes precedence over path, so path from search expression will be ignored if pid specified
    usdt_probes = USDTHelper::probes_for_pid(bpftrace.pid());
  } else if (probe_name == "usdt") {
    // If the *full* path is provided as part of the search expression parse it out and use it
    std::string usdt_path = search.substr(search.find(":")+1, search.size());
    usdt_path_list = usdt_path.find(":") == std::string::npos;
    usdt_path = usdt_path.substr(0, usdt_path.find(":"));
    auto paths = resolve_binary_path(usdt_path, bpftrace.pid());
    switch (paths.size())
    {
    case 0:
      std::cerr << "usdt target '" << usdt_path << "' does not exist or is not executable" << std::endl;
      return;
    case 1:
      usdt_probes = USDTHelper::probes_for_path(paths.front());
      break;
    default:
      std::cerr << "usdt target '" << usdt_path << "' must refer to a unique binary but matched " << paths.size() << std::endl;
      return;
    }
  }

  for (auto const& usdt_probe : usdt_probes)
  {
    std::string path = usdt_probe.path;
    std::string provider = usdt_probe.provider;
    std::string fname = usdt_probe.name;
    std::string probe    = "usdt:" + path + ":" + provider + ":" + fname;
    if (usdt_path_list || search.empty() || !search_probe(probe, re))
      std::cout << probe << std::endl;
  }

  // tracepoints
  std::string probe;
  std::vector<std::string> cats;
  list_dir(tp_path, cats);
  for (const std::string &cat : cats)
  {
    if (cat == "." || cat == ".." || cat == "enable" || cat == "filter")
      continue;
    std::vector<std::string> events = std::vector<std::string>();
    list_dir(tp_path + "/" + cat, events);
    for (const std::string &event : events)
    {
      if (event == "." || event == ".." || event == "enable" || event == "filter")
        continue;
      probe = "tracepoint:" + cat + ":" + event;

      if (!search.empty())
      {
        if (search_probe(probe, re))
          continue;
      }

      std::cout << probe << std::endl;
      if (bt_verbose)
        print_tracepoint_args(cat, event);
    }
  }

  // Optimization: If the search expression starts with "t" (tracepoint) there is
  // no need to search for kprobes.
  if (search[0] == 't')
      return;

  // kprobes
  std::ifstream file(kprobe_path);
  if (file.fail())
  {
    std::cerr << strerror(errno) << ": " << kprobe_path << std::endl;
    return;
  }

  std::string line;
  size_t loc;
  while (std::getline(file, line))
  {
    loc = line.find_first_of(" ");
    if (loc == std::string::npos)
      probe = "kprobe:" + line;
    else
      probe = "kprobe:" + line.substr(0, loc);

    if (!search.empty())
    {
      if (search_probe(probe, re))
        continue;
    }

    std::cout << probe << std::endl;
  }

  // kfuncs
  bpftrace.btf_.display_funcs(search.empty() ? NULL : &re);
}

} // namespace bpftrace
