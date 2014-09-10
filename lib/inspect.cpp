#include "causal/inspect.h"

#include <cxxabi.h>
#include <elf.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <link.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>

#include <libelfin/dwarf/dwarf++.hh>
#include <libelfin/elf/elf++.hh>

#include "ccutil/log.h"

using boost::is_any_of;
using boost::split;

using boost::filesystem::absolute;
using boost::filesystem::canonical;
using boost::filesystem::exists;
using boost::filesystem::path;

using std::ios;
using std::map;
using std::pair;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::system_error;
using std::vector;

/**
 * Locate the build ID encoded in an ELF file and return it as a formatted string
 */
static string find_build_id(elf::elf& f) {
  for(auto& section : f.sections()) {
    if(section.get_hdr().type == elf::sht::note) {
      uintptr_t base = reinterpret_cast<uintptr_t>(section.data());
      size_t offset = 0;
      while(offset < section.size()) {
        Elf64_Nhdr* hdr = reinterpret_cast<Elf64_Nhdr*>(base + offset);
      
        if(hdr->n_type == NT_GNU_BUILD_ID) {
          // Found the build-id note
          stringstream ss;
          uintptr_t desc_base = base + offset + sizeof(Elf64_Nhdr) + hdr->n_namesz;
          uint8_t* build_id = reinterpret_cast<uint8_t*>(desc_base);
          for(size_t i = 0; i < hdr->n_descsz; i++) {
            ss.flags(ios::hex);
            ss.width(2);
            ss.fill('0');
            ss << static_cast<size_t>(build_id[i]);
          }
          return ss.str();
        
        } else {
          // Advance to the next note header
          offset += sizeof(Elf64_Nhdr) + hdr->n_namesz + hdr->n_descsz;
        }
      }
    }
  }
  return "";
}

/**
 * Get the full path to a file specified via absolute path, relative path, or raw name
 * resolved via the PATH variable.
 */
static const string get_full_path(const string filename) {
  if(filename[0] == '/') {
    return filename;
  
  } else if(filename.find('/') != string::npos) {
    return canonical(filename).string();
  
  } else {  
    // Search the environment's path for the first match
    const string path_env = getenv("PATH");
    vector<string> search_dirs;
    split(search_dirs, path_env, is_any_of(":"));
  
    for(const string& dir : search_dirs) {
      auto p = path(dir) / filename;
      if(exists(p)) {
        return p.string();
      }
    }
  }

  return "";
}

/**
 * Locate an ELF file that contains debug symbols for the file provided by name.
 * This will work for files specified by relative path, absolute path, or raw name
 * resolved via the PATH variable.
 */
static elf::elf locate_debug_executable(const string filename) {
  elf::elf f;

  const string full_path = get_full_path(filename);

  // If a full path wasn't found, return the invalid ELF file
  if(full_path.length() == 0) {
    return f;
  }

  int fd = open(full_path.c_str(), O_RDONLY);

  // If the file couldn't be opened, return the invalid ELF file
  if(fd < 0) {
    return f;
  }

  // Load the opened ELF file
  f = elf::elf(elf::create_mmap_loader(fd));

  // If this file has a .debug_info section, return it
  if(f.get_section(".debug_info").valid()) {
    return f;
  }

  // If there isn't a .debug_info section, check for the .gnu_debuglink section
  auto& link_section = f.get_section(".gnu_debuglink");

  // Store the full path to the executable and its directory name
  string directory = full_path.substr(0, full_path.find_last_of('/'));

  // Build a vector of paths to search for a debug version of the file
  vector<string> search_paths;

  // Check for a build-id section
  string build_id = find_build_id(f);
  if(build_id.length() > 0) {
    string prefix = build_id.substr(0, 2);
    string suffix = build_id.substr(2);
  
    auto p = path("/usr/lib/debug/.build-id") / prefix / (suffix + ".debug");
    search_paths.push_back(p.string());
  }

  // Check for a debug_link section
  if(link_section.valid()) {
    string link_name = reinterpret_cast<const char*>(link_section.data());
  
    search_paths.push_back(directory + "/" + link_name);
    search_paths.push_back(directory + "/.debug/" + link_name);
    search_paths.push_back("/usr/lib/debug" + directory + "/" + link_name);
  }
  
  // Clear the loaded file so if we have to return it, it won't be valid()
  f = elf::elf();

  // Try all the usable search paths
  for(const string& path : search_paths) {
    fd = open(path.c_str(), O_RDONLY);
    if(fd >= 0) {
      f = elf::elf(elf::create_mmap_loader(fd));
      if(f.get_section(".debug_info").valid()) {
        break;
      }
      f = elf::elf();
    }
  }

  return f;
}

map<string, uintptr_t> get_loaded_files(bool include_libs) {
  map<string, uintptr_t> result;

  FILE* map = fopen("/proc/self/maps", "r");
  int rc;
  do {
    // If we aren't including libraries, exit once there's a single executable mapping
    if(!include_libs && result.size() == 1) {
      break;
    }
    
    uintptr_t base, limit;
    char perms[4];
    size_t offset;
    uint8_t dev_major, dev_minor;
    int inode;
    char path[512];
    rc = fscanf(map, "%lx-%lx %s %lx %hhx:%hhx %d %s\n",
      &base, &limit, perms, &offset, &dev_major, &dev_minor, &inode, path);
    
    // If the mapping parsed correctly, check whether this is an executable file:
    // Executables are mapped at offset 0, mapped executable, and have a corresponding absolute path
    if(rc == 8 && offset == 0 && perms[2] == 'x' && path[0] == '/') {
      result[string(path)] = base;
    }
  } while(rc == 8);
  fclose(map);
  
  return result;
}

void memory_map::build(const vector<string>& scope, bool include_libs) {
  for(const auto& f : get_loaded_files(include_libs)) {
    try {
      if(process_file(f.first, f.second, scope)) {
        INFO << "Including lines from " << f.first;
      } else {
        INFO << "Unable to locate debug information for " << f.first;
      }
    } catch(const system_error& e) {
      WARNING << "Processing file \"" << f.first << "\" failed: " << e.what();
    }
  }
}

bool in_scope(string file, const vector<string>& scope) {
  for(const string& s : scope) {
    if(path(file).normalize().string().find(s) == 0) {
      return true;
    }
  }
  return false;
}

dwarf::value find_attribute(const dwarf::die& d, dwarf::DW_AT attr) {
  if(!d.valid())
    return dwarf::value();
  
  if(d.has(attr))
    return d[attr];
  
  if(d.has(dwarf::DW_AT::abstract_origin)) {
    const dwarf::die child = d.resolve(dwarf::DW_AT::abstract_origin).as_reference();
    dwarf::value v = find_attribute(child, attr);
    if(v.valid())
      return v;
  }
  
  if(d.has(dwarf::DW_AT::specification)) {
    const dwarf::die child = d.resolve(dwarf::DW_AT::specification).as_reference();
    dwarf::value v = find_attribute(child, attr);
    if(v.valid())
      return v;
  }
  
  return dwarf::value();
}

void memory_map::add_range(std::string filename, size_t line_no, interval range) {
  shared_ptr<file> f = get_file(filename);
  shared_ptr<line> l = f->get_line(line_no);
  
  /*auto iter = _ranges.find(range);
  if(iter != _ranges.end() && iter->second != l) {
    WARNING << "Overlapping entries for lines " 
      << f->get_name() << ":" << l->get_line() << " and " 
      << iter->second->get_file()->get_name() << ":" << iter->second->get_line();
  }*/

  // Add the entry
  _ranges.emplace(range, l);
}

void memory_map::process_inlines(const dwarf::die& d,
                                 const dwarf::line_table& table,
                                 const vector<string>& scope,
                                 uintptr_t load_address) {
  if(!d.valid())
    return;
  
  if(d.tag == dwarf::DW_TAG::inlined_subroutine) {
    string name;
    dwarf::value name_val = find_attribute(d, dwarf::DW_AT::name);
    if(name_val.valid()) {
      name = name_val.as_string();
    }
    
    string decl_file;
    dwarf::value decl_file_val = find_attribute(d, dwarf::DW_AT::decl_file);
    if(decl_file_val.valid() && table.valid()) {
      decl_file = table.get_file(decl_file_val.as_uconstant())->path;
    }
    
    size_t decl_line = 0;
    dwarf::value decl_line_val = find_attribute(d, dwarf::DW_AT::decl_line);
    if(decl_line_val.valid())
      decl_line = decl_line_val.as_uconstant();
    
    string call_file;
    if(d.has(dwarf::DW_AT::call_file) && table.valid()) {
      call_file = table.get_file(d[dwarf::DW_AT::call_file].as_uconstant())->path;
    }
    
    size_t call_line = 0;
    if(d.has(dwarf::DW_AT::call_line)) {
      call_line = d[dwarf::DW_AT::call_line].as_uconstant();
    }
    
    // If the call location is in scope but the function is not, add an entry 
    if(decl_file.size() > 0 && call_file.size() > 0) {
      if(!in_scope(decl_file, scope) && in_scope(call_file, scope)) {
        // Does this inline have separate ranges?
        dwarf::value ranges_val = find_attribute(d, dwarf::DW_AT::ranges);
        if(ranges_val.valid()) {
          // Add each range
          for(auto r : ranges_val.as_rangelist()) {
            add_range(call_file,
                      call_line,
                      interval(r.low, r.high) + load_address);
          }
        } else {
          // Must just be one range. Add it
          dwarf::value low_pc_val = find_attribute(d, dwarf::DW_AT::low_pc);
          dwarf::value high_pc_val = find_attribute(d, dwarf::DW_AT::high_pc);

          if(low_pc_val.valid() && high_pc_val.valid()) {
            uint64_t low_pc;
            uint64_t high_pc;
            
            if(low_pc_val.get_type() == dwarf::value::type::address)
              low_pc = low_pc_val.as_address();
            else if(low_pc_val.get_type() == dwarf::value::type::uconstant)
              low_pc = low_pc_val.as_uconstant();
            else if(low_pc_val.get_type() == dwarf::value::type::sconstant)
              low_pc = low_pc_val.as_sconstant();
            
            if(high_pc_val.get_type() == dwarf::value::type::address)
              high_pc = high_pc_val.as_address();
            else if(high_pc_val.get_type() == dwarf::value::type::uconstant)
              high_pc = high_pc_val.as_uconstant();
            else if(high_pc_val.get_type() == dwarf::value::type::sconstant)
              high_pc = high_pc_val.as_sconstant();
            
            add_range(call_file,
                      call_line,
                      interval(low_pc, high_pc) + load_address);
          }
        }
      }
    }
  }
  
  for(const auto& child : d) {
    process_inlines(child, table, scope, load_address);
  }
}

bool memory_map::process_file(const string& name, uintptr_t load_address,
                              const vector<string>& scope) {
  elf::elf f = locate_debug_executable(name);
  // If a debug version of the file could not be located, return false
  if(!f.valid()) {
    return false;
  }
  
  // Read the DWARF information from the chosen file
  dwarf::dwarf d(dwarf::elf::create_loader(f));
  
  // Walk through the compilation units (source files) in the executable
  for(auto unit : d.compilation_units()) {
    
    string prev_filename;
    size_t prev_line;
    uintptr_t prev_address = 0;
    // Walk through the line instructions in the DWARF line table
    for(auto& line_info : unit.get_line_table()) {
      // Insert an entry if this isn't the first line command in the sequence
      if(prev_address != 0 && in_scope(prev_filename, scope)) {
        add_range(prev_filename,
                  prev_line, 
                  interval(prev_address, line_info.address) + load_address);
      }
    
      if(line_info.end_sequence) {
        prev_address = 0;
      } else {
        prev_filename = path(line_info.file->path).normalize().string();
        prev_line = line_info.line;
        prev_address = line_info.address;
      }
    }
    process_inlines(unit.root(), unit.get_line_table(), scope, load_address);
  }
  
  return true;
}

shared_ptr<line> memory_map::find_line(const string& name) {
  string::size_type colon_pos = name.find_first_of(':');
  if(colon_pos == string::npos) {
    WARNING << "Could not identify file name in input " << name;
    return shared_ptr<line>();
  }
  
  string filename = name.substr(0, colon_pos);
  string line_no_str = name.substr(colon_pos + 1);
  
  size_t line_no;
  stringstream(line_no_str) >> line_no;
  
  for(const auto& f : files()) {
    string::size_type last_pos = f.first.rfind(filename);
    if(last_pos != string::npos && last_pos + filename.size() == f.first.size()) {
      if(f.second->has_line(line_no)) {
        return f.second->get_line(line_no);
      }
    }
  }
  
  return shared_ptr<line>();
}

shared_ptr<line> memory_map::find_line(uintptr_t addr) {
  auto iter = _ranges.find(addr);
  if(iter != _ranges.end()) {
    return iter->second;
  } else {
    return shared_ptr<line>();
  }
}

memory_map& memory_map::get_instance() {
  static char buf[sizeof(memory_map)];
  static memory_map* the_instance = new(buf) memory_map();
  return *the_instance;
}
