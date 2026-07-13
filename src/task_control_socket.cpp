#include "rsync_assistant/task_control_socket.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace rsync_assistant {
namespace {

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kCreateReadyTask = 1;
constexpr std::uint32_t kTaskResponse = 2;
constexpr std::uint32_t kListTasks = 3;
constexpr std::uint32_t kTaskListResponse = 4;
constexpr std::uint32_t kPreflight = 5;
constexpr std::uint32_t kExecute = 6;
constexpr std::uint32_t kPause = 7;
constexpr std::uint32_t kResume = 8;
constexpr std::uint32_t kStop = 9;
constexpr std::uint32_t kAwaitCompletion = 10;
constexpr std::uint32_t kExecutionLog = 11;
constexpr std::uint32_t kExecutionLogResponse = 12;
constexpr std::uint32_t kScpFallback = 13;
constexpr std::uint32_t kErrorResponse = 14;
constexpr std::uint32_t kRestart = 15;
constexpr std::size_t kMaximumPayloadBytes = 1024 * 1024;

struct FrameHeader {
  std::uint32_t version;
  std::uint32_t type;
  std::uint32_t payload_size;
};

void write_all(int descriptor, const void* data, std::size_t size) {
  auto* cursor = static_cast<const char*>(data);
  while (size > 0) {
    const auto written = write(descriptor, cursor, size);
    if (written <= 0) throw std::runtime_error("socket write failed");
    cursor += written;
    size -= static_cast<std::size_t>(written);
  }
}

void read_all(int descriptor, void* data, std::size_t size) {
  auto* cursor = static_cast<char*>(data);
  while (size > 0) {
    const auto read_size = read(descriptor, cursor, size);
    if (read_size <= 0) throw std::runtime_error("socket read failed");
    cursor += read_size;
    size -= static_cast<std::size_t>(read_size);
  }
}

void send_frame(int descriptor, std::uint32_t type, std::string_view payload) {
  FrameHeader header{htonl(kProtocolVersion), htonl(type),
                     htonl(static_cast<std::uint32_t>(payload.size()))};
  write_all(descriptor, &header, sizeof(header));
  write_all(descriptor, payload.data(), payload.size());
}

std::pair<std::uint32_t, std::string> receive_frame(int descriptor) {
  FrameHeader header{};
  read_all(descriptor, &header, sizeof(header));
  const auto version = ntohl(header.version);
  const auto type = ntohl(header.type);
  const auto payload_size = ntohl(header.payload_size);
  if (version != kProtocolVersion || payload_size > kMaximumPayloadBytes)
    throw std::runtime_error("unsupported socket frame");
  std::string payload(payload_size, '\0');
  read_all(descriptor, payload.data(), payload.size());
  return {type, std::move(payload)};
}

CreateReadyTask decode_request(const std::string& payload) {
  const auto first = payload.find('\0');
  const auto second = first == std::string::npos ? first : payload.find('\0', first + 1);
  const auto third = second == std::string::npos ? second : payload.find('\0', second + 1);
  const auto fourth = third == std::string::npos ? third : payload.find('\0', third + 1);
  if (first == std::string::npos || second == std::string::npos || third == std::string::npos || fourth == std::string::npos)
    throw std::runtime_error("invalid task request");
  const auto destination = payload.substr(first + 1,
                                          second - first - 1);
  return {payload.substr(0, first), destination,
          payload.substr(second + 1, third - second - 1) == "1",
          payload.substr(third + 1, fourth - third - 1) == "1",
          payload.substr(fourth + 1) == "1"};
}

std::string encode_task(const TransferTask& task) {
  const auto method = task.method == TransferMethod::rsync_daemon ? "rsync_daemon" :
                      task.method == TransferMethod::rsync_ssh ? "rsync_ssh" :
                      task.method == TransferMethod::scp ? "scp" : "local_rsync";
  const auto state = task.state == TaskState::ready ? "ready" :
                     task.state == TaskState::awaiting_execution_confirmation ? "awaiting_confirmation" :
                     task.state == TaskState::running ? "running" :
                     task.state == TaskState::paused ? "paused" :
                     task.state == TaskState::completed ? "completed" :
                     task.state == TaskState::cancelled ? "cancelled" :
                     task.state == TaskState::interrupted ? "interrupted" : "failed";
  return task.id + '\0' + task.source + '\0' + task.destination + '\0' + state + '\0' +
         (task.delete_extraneous ? "1" : "0") + '\0' +
         (task.compression ? "1" : "0") + '\0' +
         (task.dry_run ? "1" : "0") + '\0' + method;
}

TransferTask decode_task(const std::string& payload) {
  const auto first = payload.find('\0');
  const auto second = first == std::string::npos ? first : payload.find('\0', first + 1);
  const auto third = second == std::string::npos ? second : payload.find('\0', second + 1);
  const auto fourth = third == std::string::npos ? third : payload.find('\0', third + 1);
  const auto fifth = fourth == std::string::npos ? fourth : payload.find('\0', fourth + 1);
  const auto sixth = fifth == std::string::npos ? fifth : payload.find('\0', fifth + 1);
  const auto seventh = sixth == std::string::npos ? sixth : payload.find('\0', sixth + 1);
  if (first == std::string::npos || second == std::string::npos || third == std::string::npos || fourth == std::string::npos || fifth == std::string::npos || sixth == std::string::npos || seventh == std::string::npos)
    throw std::runtime_error("invalid task response");
  const auto state = payload.substr(third + 1, fourth - third - 1);
  return {payload.substr(0, first), payload.substr(first + 1, second - first - 1),
          payload.substr(second + 1, third - second - 1),
          state == "ready" ? TaskState::ready : state == "awaiting_confirmation" ? TaskState::awaiting_execution_confirmation : state == "running" ? TaskState::running : state == "paused" ? TaskState::paused : state == "completed" ? TaskState::completed : state == "cancelled" ? TaskState::cancelled : state == "interrupted" ? TaskState::interrupted : TaskState::failed, "", "", payload.substr(fourth + 1, fifth - fourth - 1) == "1", payload.substr(fifth + 1, sixth - fifth - 1) == "1", payload.substr(sixth + 1, seventh - sixth - 1) == "1", payload.substr(seventh + 1) == "scp" ? TransferMethod::scp : payload.substr(seventh + 1) == "rsync_daemon" ? TransferMethod::rsync_daemon : payload.substr(seventh + 1) == "rsync_ssh" ? TransferMethod::rsync_ssh : TransferMethod::local_rsync};
}

std::string encode_tasks(const std::vector<TransferTask>& tasks) {
  std::string payload;
  for (const auto& task : tasks) {
    payload += encode_task(task);
    payload += '\0';
  }
  return payload;
}

std::vector<TransferTask> decode_tasks(const std::string& payload) {
  std::vector<TransferTask> tasks;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    const auto first = payload.find('\0', offset);
    const auto second = first == std::string::npos ? first : payload.find('\0', first + 1);
    const auto third = second == std::string::npos ? second : payload.find('\0', second + 1);
    const auto fourth = third == std::string::npos ? third : payload.find('\0', third + 1);
    const auto fifth = fourth == std::string::npos ? fourth : payload.find('\0', fourth + 1);
    const auto sixth = fifth == std::string::npos ? fifth : payload.find('\0', fifth + 1);
    const auto seventh = sixth == std::string::npos ? sixth : payload.find('\0', sixth + 1);
    const auto eighth = seventh == std::string::npos ? seventh : payload.find('\0', seventh + 1);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos || fourth == std::string::npos || fifth == std::string::npos || sixth == std::string::npos || seventh == std::string::npos || eighth == std::string::npos)
      throw std::runtime_error("invalid task list response");
    const auto state = payload.substr(third + 1, fourth - third - 1);
    tasks.push_back({payload.substr(offset, first - offset),
                     payload.substr(first + 1, second - first - 1),
                     payload.substr(second + 1, third - second - 1),
                     state == "ready" ? TaskState::ready :
                     state == "awaiting_confirmation" ? TaskState::awaiting_execution_confirmation :
                     state == "running" ? TaskState::running : state == "paused" ? TaskState::paused : state == "completed" ? TaskState::completed : state == "cancelled" ? TaskState::cancelled : state == "interrupted" ? TaskState::interrupted : TaskState::failed,
                     "", "", payload.substr(fourth + 1, fifth - fourth - 1) == "1",
                     payload.substr(fifth + 1, sixth - fifth - 1) == "1",
                     payload.substr(sixth + 1, seventh - sixth - 1) == "1",
                     payload.substr(seventh + 1, eighth - seventh - 1) == "scp" ? TransferMethod::scp : payload.substr(seventh + 1, eighth - seventh - 1) == "rsync_daemon" ? TransferMethod::rsync_daemon : payload.substr(seventh + 1, eighth - seventh - 1) == "rsync_ssh" ? TransferMethod::rsync_ssh : TransferMethod::local_rsync});
    offset = eighth + 1;
  }
  return tasks;
}

sockaddr_un address_for(const std::filesystem::path& socket_path) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = socket_path.string();
  if (path.size() >= sizeof(address.sun_path)) throw std::runtime_error("socket path too long");
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  return address;
}

socklen_t address_length(const sockaddr_un& address) {
  return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                std::strlen(address.sun_path) + 1);
}

int connect_to(const std::filesystem::path& socket_path) {
  const auto address = address_for(socket_path);
  for (int attempt = 0; attempt < 100; ++attempt) {
    const int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor >= 0 &&
        connect(descriptor, reinterpret_cast<const sockaddr*>(&address), address_length(address)) == 0)
      return descriptor;
    if (descriptor >= 0) close(descriptor);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  throw std::runtime_error("connect to task daemon failed");
}

void require_response(std::uint32_t actual, std::uint32_t expected,
                      const std::string& payload, std::string_view operation) {
  if (actual == kErrorResponse) throw std::runtime_error(payload);
  if (actual != expected)
    throw std::runtime_error(std::string{"unexpected "} + std::string{operation} + " response");
}

}  // namespace

struct TaskControlSocketServer::Impl {
  TaskControlService& service;
  std::filesystem::path socket_path;
  std::atomic_int listener{-1};
};

TaskControlSocketServer::TaskControlSocketServer(TaskControlService& service,
                                                 std::filesystem::path socket_path)
    : impl_(std::make_unique<Impl>(service, std::move(socket_path))) {}

TaskControlSocketServer::~TaskControlSocketServer() { stop(); }

void TaskControlSocketServer::serve() {
  std::filesystem::remove(impl_->socket_path);
  const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0) throw std::runtime_error("create socket failed");
  const auto address = address_for(impl_->socket_path);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address), address_length(address)) != 0 ||
      listen(listener, 16) != 0) {
    close(listener);
    throw std::runtime_error(std::string{"bind or listen failed: "} +
                             std::strerror(errno));
  }
  impl_->listener.store(listener);
  while (impl_->listener.load() == listener) {
    const int client = accept(listener, nullptr, nullptr);
    if (client < 0) continue;
    try {
      const auto [type, payload] = receive_frame(client);
      if (type == kCreateReadyTask) {
        const auto request = decode_request(payload);
        send_frame(client, kTaskResponse,
                   encode_task(impl_->service.create_ready_task(request)));
      } else if (type == kListTasks) {
        impl_->service.reap_completed();
        send_frame(client, kTaskListResponse, encode_tasks(impl_->service.list_tasks()));
      } else if (type == kPreflight) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.preflight(payload)));
      } else if (type == kExecute) {
        const auto separator = payload.find('\0');
        const auto task_id = payload.substr(0, separator);
        const auto confirmed = separator != std::string::npos && payload.substr(separator + 1) == "DELETE";
        send_frame(client, kTaskResponse, encode_task(impl_->service.execute(task_id, confirmed)));
      } else if (type == kPause) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.pause(payload)));
      } else if (type == kResume) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.resume(payload)));
      } else if (type == kStop) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.stop(payload)));
      } else if (type == kRestart) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.restart(payload)));
      } else if (type == kAwaitCompletion) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.await_completion(payload)));
      } else if (type == kExecutionLog) {
        send_frame(client, kExecutionLogResponse, impl_->service.execution_log(payload));
      } else if (type == kScpFallback) {
        send_frame(client, kTaskResponse, encode_task(impl_->service.execute_scp_fallback(payload)));
      } else {
        throw std::runtime_error("unsupported request");
      }
    } catch (const std::exception& error) {
      send_frame(client, kErrorResponse, error.what());
    }
    close(client);
  }
  close(listener);
  std::filesystem::remove(impl_->socket_path);
}

TransferTask TaskControlSocketClient::preflight(const std::string& task_id) const {
  const int descriptor = connect_to(socket_path_);
  send_frame(descriptor, kPreflight, task_id);
  const auto [type, payload] = receive_frame(descriptor);
  close(descriptor);
  require_response(type, kTaskResponse, payload, "preflight");
  return decode_task(payload);
}

TransferTask TaskControlSocketClient::execute(const std::string& task_id,
                                              bool delete_confirmed) const {
  const int descriptor = connect_to(socket_path_);
    send_frame(descriptor, kExecute,
               task_id + '\0' + (delete_confirmed ? "DELETE" : ""));
  const auto [type, payload] = receive_frame(descriptor);
  close(descriptor);
  require_response(type, kTaskResponse, payload, "execute");
  return decode_task(payload);
}

namespace {
TransferTask request_task(const std::filesystem::path& socket_path, std::uint32_t type,
                          const std::string& task_id);
}

TransferTask TaskControlSocketClient::execute_scp_fallback(const std::string& task_id) const {
  return request_task(socket_path_, kScpFallback, task_id);
}

void TaskControlSocketServer::stop() {
  const int listener = impl_->listener.exchange(-1);
  if (listener >= 0) close(listener);
}

TaskControlSocketClient::TaskControlSocketClient(std::filesystem::path socket_path)
    : socket_path_(std::move(socket_path)) {}

TransferTask TaskControlSocketClient::create_ready_task(
    const CreateReadyTask& request) const {
  const int descriptor = connect_to(socket_path_);
  try {
    send_frame(descriptor, kCreateReadyTask,
               request.source + '\0' + request.destination + '\0' +
                   (request.delete_extraneous ? "1" : "0") + '\0' +
                   (request.compression ? "1" : "0") + '\0' +
                   (request.dry_run ? "1" : "0"));
    const auto [type, payload] = receive_frame(descriptor);
    close(descriptor);
    require_response(type, kTaskResponse, payload, "task");
    return decode_task(payload);
  } catch (...) {
    close(descriptor);
    throw;
  }
}

namespace {
TransferTask request_task(const std::filesystem::path& socket_path, std::uint32_t type,
                          const std::string& task_id) {
  const int descriptor = connect_to(socket_path);
  send_frame(descriptor, type, task_id);
  const auto [response_type, payload] = receive_frame(descriptor);
  close(descriptor);
  require_response(response_type, kTaskResponse, payload, "task");
  return decode_task(payload);
}
}  // namespace

TransferTask TaskControlSocketClient::pause(const std::string& task_id) const {
  return request_task(socket_path_, kPause, task_id);
}
TransferTask TaskControlSocketClient::resume(const std::string& task_id) const {
  return request_task(socket_path_, kResume, task_id);
}
TransferTask TaskControlSocketClient::stop(const std::string& task_id) const {
  return request_task(socket_path_, kStop, task_id);
}
TransferTask TaskControlSocketClient::restart(const std::string& task_id) const {
  return request_task(socket_path_, kRestart, task_id);
}
TransferTask TaskControlSocketClient::await_completion(const std::string& task_id) const {
  return request_task(socket_path_, kAwaitCompletion, task_id);
}

std::vector<TransferTask> TaskControlSocketClient::list_tasks() const {
  const int descriptor = connect_to(socket_path_);
  try {
    send_frame(descriptor, kListTasks, "");
    const auto [type, payload] = receive_frame(descriptor);
    close(descriptor);
    require_response(type, kTaskListResponse, payload, "task list");
    return decode_tasks(payload);
  } catch (...) {
    close(descriptor);
    throw;
  }
}

std::string TaskControlSocketClient::execution_log(const std::string& task_id) const {
  const int descriptor = connect_to(socket_path_);
  send_frame(descriptor, kExecutionLog, task_id);
  const auto [type, payload] = receive_frame(descriptor);
  close(descriptor);
  require_response(type, kExecutionLogResponse, payload, "execution log");
  return payload;
}

}  // namespace rsync_assistant
