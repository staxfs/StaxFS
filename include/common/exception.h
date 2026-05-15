#pragma once

#include <cstring>
#include <exception>

namespace dfs {

class LinuxSyscallException : public std::exception {
  int errcode_;

public:
  explicit LinuxSyscallException(int errcode) : errcode_(errcode) {}

  auto what() const noexcept -> const char * override {
    return ::strerror(errcode_);
  }
};

} // namespace dfs