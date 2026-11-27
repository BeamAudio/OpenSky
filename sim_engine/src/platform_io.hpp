#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nsim {

using FileHandle = intptr_t;
inline constexpr FileHandle kInvalidFileHandle = -1;

FileHandle open_read_only(const std::string &path);
void close_file(FileHandle &handle);
bool read_at_offset(FileHandle handle, void *dst, size_t size, uint64_t offset);
bool get_file_size(FileHandle handle, uint64_t &out_size);
bool file_larger_than(const std::string &path, uint64_t min_bytes);
bool read_entire_file_checked(const std::string &path, uint64_t min_bytes, std::vector<uint8_t> &out);

std::string create_unique_temp_file(const std::filesystem::path &base_path);
std::filesystem::path create_unique_temp_dir(const char *prefix);
bool atomic_replace_file(const std::filesystem::path &tmp_path, const std::filesystem::path &dest_path);

bool http_get_to_string(const std::string &url, const char *user_agent,
                        int connect_timeout_ms, int total_timeout_ms,
                        std::string &out_body);
bool http_download_to_file(const std::string &url, const char *user_agent,
                           int connect_timeout_ms, int total_timeout_ms,
                           const std::string &dest_path);

void launch_world_editor();
void resolve_windows_working_root();
void show_fatal_error(const std::string &title, const std::string &message);

} // namespace nsim
