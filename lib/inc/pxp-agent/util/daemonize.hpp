#ifndef SRC_AGENT_UTIL_DAEMONIZE_HPP_
#define SRC_AGENT_UTIL_DAEMONIZE_HPP_

#ifndef _WIN32
#include <pxp-agent/util/posix/pid_file.hpp>
#endif
#include <memory>

namespace PXPAgent {
namespace Util {

#ifdef _WIN32
void daemonize();
void cleanup();
#else
std::unique_ptr<PIDFile> daemonize();
#endif

}  // namespace Util
}  // namespace PXPAgent

#endif  // SRC_AGENT_UTIL_DAEMONIZE_HPP_
