#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "robot_sdk/sdk_client.hpp"

#ifndef WEB_CONTROL_HTML_PATH
#define WEB_CONTROL_HTML_PATH "control.html"
#endif

#ifndef WEB_DASHBOARD_HTML_PATH
#define WEB_DASHBOARD_HTML_PATH "index.html"
#endif

namespace {

constexpr std::size_t kMaxHeaderSize = 8192;
constexpr std::size_t kMaxBodySize = 8192;
constexpr int kDefaultHttpPort = 8766;
constexpr int kDefaultDashboardPort = 8767;

std::atomic<bool> g_running{true};
std::string g_expected_host;
std::string g_expected_origin;

struct HttpRequest {
  std::string method;
  std::string target;
  std::string body;
  std::string host;
  std::string origin;
};

struct HttpError : std::runtime_error {
  HttpError(int status_code, const std::string& message)
      : std::runtime_error(message), status(status_code) {}
  int status;
};

void HandleSignal(int) {
  g_running.store(false);
}

std::string JsonEscape(const std::string& value) {
  std::ostringstream escaped;
  for (unsigned char character : value) {
    switch (character) {
      case '"':
        escaped << "\\\"";
        break;
      case '\\':
        escaped << "\\\\";
        break;
      case '\b':
        escaped << "\\b";
        break;
      case '\f':
        escaped << "\\f";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        if (character < 0x20) {
          escaped << "\\u00" << std::hex << std::setw(2)
                  << static_cast<int>(character) << std::dec;
        } else {
          escaped << character;
        }
    }
  }
  return escaped.str();
}

std::string DecodeQueryComponent(const std::string& value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (character == '+') {
      decoded.push_back(' ');
    } else if (character == '%') {
      if (index + 2 >= value.size()) {
        throw HttpError(400, "Malformed URL encoding");
      }
      const auto is_hex = [](char digit) {
        return std::isxdigit(static_cast<unsigned char>(digit)) != 0;
      };
      if (!is_hex(value[index + 1]) || !is_hex(value[index + 2])) {
        throw HttpError(400, "Malformed URL encoding");
      }
      const std::string hex = value.substr(index + 1, 2);
      decoded.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
      index += 2;
    } else {
      decoded.push_back(character);
    }
  }
  return decoded;
}

std::unordered_map<std::string, std::string> ParseQuery(
    const std::string& target) {
  std::unordered_map<std::string, std::string> parameters;
  const auto query_start = target.find('?');
  if (query_start == std::string::npos) return parameters;

  std::size_t position = query_start + 1;
  while (position <= target.size()) {
    const auto next = target.find('&', position);
    const std::string pair =
        target.substr(position, next == std::string::npos
                                    ? std::string::npos
                                    : next - position);
    if (!pair.empty()) {
      const auto equals = pair.find('=');
      const std::string key = DecodeQueryComponent(pair.substr(0, equals));
      const std::string value =
          equals == std::string::npos
              ? ""
              : DecodeQueryComponent(pair.substr(equals + 1));
      if (key.empty() || !parameters.emplace(key, value).second) {
        throw HttpError(400, "Invalid or duplicate query parameter");
      }
    }
    if (next == std::string::npos) break;
    position = next + 1;
  }
  return parameters;
}

std::string RequestPath(const std::string& target) {
  return target.substr(0, target.find('?'));
}

std::string RequiredParameter(
    const std::unordered_map<std::string, std::string>& parameters,
    const std::string& name) {
  const auto found = parameters.find(name);
  if (found == parameters.end() || found->second.empty()) {
    throw HttpError(400, "Missing required parameter: " + name);
  }
  return found->second;
}

HttpRequest ReadRequest(int client_socket) {
  std::string input;
  char buffer[2048];
  auto header_end = std::string::npos;
  while ((header_end = input.find("\r\n\r\n")) == std::string::npos) {
    if (input.size() >= kMaxHeaderSize) {
      throw HttpError(413, "Request headers are too large");
    }
    const ssize_t received = recv(client_socket, buffer, sizeof(buffer), 0);
    if (received <= 0) throw HttpError(400, "Could not read request");
    input.append(buffer, static_cast<std::size_t>(received));
  }

  if (header_end > kMaxHeaderSize) {
    throw HttpError(413, "Request headers are too large");
  }
  const std::string headers = input.substr(0, header_end);
  std::istringstream header_stream(headers);
  HttpRequest request;
  std::string version;
  if (!(header_stream >> request.method >> request.target >> version) ||
      version.rfind("HTTP/", 0) != 0) {
    throw HttpError(400, "Invalid HTTP request line");
  }

  std::size_t content_length = 0;
  std::string line;
  std::getline(header_stream, line);
  while (std::getline(header_stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (name == "transfer-encoding") {
      throw HttpError(400, "Chunked request bodies are not supported");
    }
    if (name == "host" || name == "origin") {
      std::string value = line.substr(colon + 1);
      const auto first = value.find_first_not_of(" \t");
      const auto last = value.find_last_not_of(" \t");
      value = first == std::string::npos
                  ? ""
                  : value.substr(first, last - first + 1);
      if (name == "host") request.host = value;
      if (name == "origin") request.origin = value;
    }
    if (name == "content-length") {
      const std::string length_text = line.substr(colon + 1);
      try {
        const auto parsed = std::stoull(length_text);
        if (parsed > kMaxBodySize) {
          throw HttpError(413, "Request body is too large");
        }
        content_length = static_cast<std::size_t>(parsed);
      } catch (const std::invalid_argument&) {
        throw HttpError(400, "Invalid Content-Length");
      } catch (const std::out_of_range&) {
        throw HttpError(413, "Request body is too large");
      }
    }
  }

  const std::size_t body_offset = header_end + 4;
  if (input.size() > body_offset) {
    request.body = input.substr(body_offset);
  }
  while (request.body.size() < content_length) {
    const std::size_t remaining = content_length - request.body.size();
    const ssize_t received =
        recv(client_socket, buffer, std::min(sizeof(buffer), remaining), 0);
    if (received <= 0) throw HttpError(400, "Incomplete request body");
    request.body.append(buffer, static_cast<std::size_t>(received));
  }
  if (request.body.size() > content_length) request.body.resize(content_length);
  return request;
}

void SendAll(int socket, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const ssize_t result =
        send(socket, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (result <= 0) return;
    sent += static_cast<std::size_t>(result);
  }
}

void SendResponse(
    int client_socket, int status, const std::string& content_type,
    const std::string& body) {
  const char* reason = status == 200   ? "OK"
                       : status == 400 ? "Bad Request"
                       : status == 403 ? "Forbidden"
                       : status == 404 ? "Not Found"
                       : status == 405 ? "Method Not Allowed"
                       : status == 409 ? "Conflict"
                       : status == 413 ? "Payload Too Large"
                       : status == 500 ? "Internal Server Error"
                                       : "Error";
  std::ostringstream response;
  response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "X-Content-Type-Options: nosniff\r\n"
           << "Connection: close\r\n\r\n"
           << body;
  SendAll(client_socket, response.str());
}

std::error_code RunCommand(robot_sdk::SDKClient& client,
                           const std::string& command) {
  using namespace robot_sdk;
  if (command == "forward") return client.Move(0.0F, 0.2F, 0.0F);
  if (command == "backward") return client.Move(0.0F, -0.2F, 0.0F);
  if (command == "left") return client.Move(0.5F, 0.0F, 0.0F);
  if (command == "right") return client.Move(-0.5F, 0.0F, 0.0F);
  if (command == "turn_left") return client.Move(0.0F, 0.0F, 0.5F);
  if (command == "turn_right") return client.Move(0.0F, 0.0F, -0.5F);
  if (command == "stop") {
    if (const auto error = client.Move(0.0F, 0.0F, 0.0F)) return error;
    if (const auto error = client.Turn(0)) return error;
    return client.ControlHead(0.0F, 0.0F);
  }
  if (command == "look_left") return client.ControlHead(0.5F, 0.0F);
  if (command == "look_up") return client.ControlHead(0.0F, 0.5F);
  if (command == "look_right") return client.ControlHead(-0.5F, 0.0F);
  if (command == "look_down") return client.ControlHead(0.0F, -0.5F);
  
  if (command == "stand") return client.StandUp();
  if (command == "lie_down") return client.LieDown();
  if (command == "crawl") return client.Crawl();
  if (command == "gait") return client.Gait();
  if (command == "climb") return client.Climb();
  if (command == "slim") return client.Slim();
  if (command == "mode_general") return client.SetMode(1);
  if (command == "mode_in_place") return client.SetMode(2);
  if (command == "mode_stair") return client.SetMode(3);
  if (command == "speed_low") return client.SetSpeed(1);
  if (command == "speed_medium") return client.SetSpeed(2);
  if (command == "speed_high") return client.SetSpeed(3);
  if (command == "take_control") return client.TakeControl();
  if (command == "release_control") return client.ReleaseControl();
  if (command == "estop_on") return client.SoftEmergencyStop(true);
  if (command == "estop_off") return client.SoftEmergencyStop(false);
  if (command == "lock") return client.Locked();
  if (command == "unlock") return client.StandUp();
  if (command == "reverse_head_tail") return client.ReverseHeadTail();
  if (command == "front_light_on") return client.FrontLight(true);
  if (command == "front_light_off") return client.FrontLight(false);
  if (command == "back_light_on") return client.BackLight(true);
  if (command == "back_light_off") return client.BackLight(false);
  if (command == "auto_light_on") return client.AutoModeLight(true);
  if (command == "auto_light_off") return client.AutoModeLight(false);
  throw HttpError(400, "Unknown command");
}

std::string ReadControlPage() {
  std::ifstream page(WEB_CONTROL_HTML_PATH, std::ios::binary);
  if (!page) {
    throw std::runtime_error(
        "Could not open control.html. Rebuild the example target.");
  }
  std::ostringstream content;
  content << page.rdbuf();
  return content.str();
}

std::string ReadDashboardPage() {
  std::ifstream page(WEB_DASHBOARD_HTML_PATH, std::ios::binary);
  if (!page) {
    throw std::runtime_error(
        "Could not open index.html. Rebuild the example target.");
  }
  std::ostringstream content;
  content << page.rdbuf();
  return content.str();
}

int WaitForClient(int server_socket) {
  pollfd descriptor{server_socket, POLLIN, 0};
  const int ready = poll(&descriptor, 1, 500);
  if (ready < 0) {
    if (errno == EINTR) return -1;
    throw std::runtime_error("HTTP server poll failed: " +
                             std::string(std::strerror(errno)));
  }
  if (ready == 0) return -1;
  if ((descriptor.revents & POLLIN) == 0) {
    throw std::runtime_error("HTTP server socket became unavailable");
  }
  const int client_socket = accept(server_socket, nullptr, nullptr);
  if (client_socket < 0 && errno != EINTR && errno != EAGAIN &&
      errno != EWOULDBLOCK) {
    std::cerr << "[ERROR] HTTP accept failed: " << std::strerror(errno)
              << std::endl;
  }
  return client_socket;
}

void RunDashboardServer(int server_socket, int port,
                        const std::string& dashboard_page) {
  const std::string expected_host = "127.0.0.1:" + std::to_string(port);
  while (g_running.load()) {
    int client_socket;
    try {
      client_socket = WaitForClient(server_socket);
    } catch (const std::exception& error) {
      std::cerr << "[ERROR] Dashboard server stopped: " << error.what()
                << std::endl;
      return;
    }
    if (client_socket < 0) continue;

    timeval timeout{5, 0};
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
               sizeof(timeout));
    try {
      const HttpRequest request = ReadRequest(client_socket);
      const std::string path = RequestPath(request.target);
      if (request.host != expected_host) {
        SendResponse(client_socket, 403, "text/plain; charset=utf-8",
                     "Requests must use the local dashboard URL.");
      } else if (request.method != "GET") {
        SendResponse(client_socket, 405, "text/plain; charset=utf-8",
                     "Method not allowed.");
      } else if (path != "/" && path != "/index.html") {
        SendResponse(client_socket, 404, "text/plain; charset=utf-8",
                     "Not found.");
      } else {
        SendResponse(client_socket, 200, "text/html; charset=utf-8",
                     dashboard_page);
      }
    } catch (const HttpError& error) {
      SendResponse(client_socket, error.status, "text/plain; charset=utf-8",
                   error.what());
    } catch (const std::exception& error) {
      std::cerr << "[ERROR] Dashboard request failed: " << error.what()
                << std::endl;
      SendResponse(client_socket, 500, "text/plain; charset=utf-8",
                   "Internal server error.");
    }
    close(client_socket);
  }
}

std::string HandleRequest(const HttpRequest& request,
                          robot_sdk::SDKClient& client,
                          std::string& content_type) {
  const std::string path = RequestPath(request.target);
  if (request.host != g_expected_host) {
    throw HttpError(403, "Requests must use the local control page");
  }
  if (request.method == "GET" &&
      (path == "/" || path == "/control.html")) {
    content_type = "text/html; charset=utf-8";
    return ReadControlPage();
  }

  content_type = "application/json; charset=utf-8";
  if (request.method == "POST" && request.origin != g_expected_origin) {
    throw HttpError(403, "Command requests must come from the local control page");
  }
  if (request.method == "GET" && path == "/api/status") {
    const bool connected = client.IsConnected();
    const std::string system_version =
        connected ? client.SystemVersion() : std::string();
    return std::string("{\"connected\":") + (connected ? "true" : "false") +
           ",\"systemVersion\":\"" + JsonEscape(system_version) + "\"}";
  }

  if (path == "/api/connect") {
    if (request.method != "POST") {
      throw HttpError(405, "Use POST to connect");
    }
    const auto parameters = ParseQuery(request.target);
    const std::string ip = RequiredParameter(parameters, "ip");
    const std::string port_text = RequiredParameter(parameters, "port");
    if (ip.size() > 253 ||
        !std::all_of(ip.begin(), ip.end(), [](unsigned char character) {
          return std::isalnum(character) || character == '.' ||
                 character == '-' || character == ':';
        })) {
      throw HttpError(400, "Enter a valid robot host or IP address");
    }
    unsigned int port = 0;
    try {
      std::size_t parsed_length = 0;
      port = std::stoul(port_text, &parsed_length);
      if (parsed_length != port_text.size() || port == 0 || port > 65535) {
        throw HttpError(400, "Robot port must be between 1 and 65535");
      }
    } catch (const std::invalid_argument&) {
      throw HttpError(400, "Robot port must be a number");
    } catch (const std::out_of_range&) {
      throw HttpError(400, "Robot port must be between 1 and 65535");
    }

    const std::error_code error =
        client.Connect(ip, std::to_string(port), true);
    if (error) throw HttpError(409, "Could not connect: " + error.message());
    if (!client.IsConnected()) {
      throw HttpError(409, "Robot connection did not reach connected state");
    }
    return "{\"ok\":true,\"message\":\"Connected to robot\"}";
  }

  if (path == "/api/disconnect") {
    if (request.method != "POST") {
      throw HttpError(405, "Use POST to disconnect");
    }
    const std::error_code error = client.Disconnect(true);
    if (error) throw HttpError(409, "Could not disconnect: " + error.message());
    return "{\"ok\":true,\"message\":\"Disconnected from robot\"}";
  }

  if (path == "/api/command") {
    if (request.method != "POST") {
      throw HttpError(405, "Use POST to send commands");
    }
    if (!client.IsConnected()) {
      throw HttpError(409, "Connect to a robot before sending commands");
    }
    const auto parameters = ParseQuery(request.target);
    const std::string command = RequiredParameter(parameters, "name");
    const std::error_code error = RunCommand(client, command);
    if (error) throw HttpError(409, "Command failed: " + error.message());
    return "{\"ok\":true,\"message\":\"Command sent\"}";
  }

  throw HttpError(404, "Not found");
}

void HandleClient(int client_socket, robot_sdk::SDKClient& client) {
  timeval timeout{5, 0};
  setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout,
             sizeof(timeout));
  try {
    const HttpRequest request = ReadRequest(client_socket);
    std::string content_type;
    try {
    const std::string body =
        HandleRequest(request, client, content_type);
    SendResponse(client_socket, 200, content_type, body);
    } catch (const HttpError& error) {
      const std::string body =
          "{\"ok\":false,\"message\":\"" + JsonEscape(error.what()) + "\"}";
      SendResponse(client_socket, error.status, "application/json; charset=utf-8",
                   body);
    } catch (const std::exception& error) {
      std::cerr << "[ERROR] Request failed: " << error.what() << std::endl;
      const std::string body =
          "{\"ok\":false,\"message\":\"Internal server error\"}";
      SendResponse(client_socket, 500, "application/json; charset=utf-8", body);
    }
  } catch (const HttpError& error) {
    const std::string body =
        "{\"ok\":false,\"message\":\"" + JsonEscape(error.what()) + "\"}";
    SendResponse(client_socket, error.status, "application/json; charset=utf-8",
                 body);
  }
}

int StartServer(int port) {
  const int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0) {
    throw std::runtime_error("Could not create HTTP socket");
  }
  int reuse_address = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
             sizeof(reuse_address));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
    close(server_socket);
    throw std::runtime_error("Could not configure localhost address");
  }
  if (bind(server_socket, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) < 0) {
    close(server_socket);
    throw std::runtime_error("Could not bind 127.0.0.1:" +
                             std::to_string(port) + ": " +
                             std::strerror(errno));
  }
  if (listen(server_socket, 8) < 0) {
    close(server_socket);
    throw std::runtime_error("Could not listen on HTTP socket");
  }
  return server_socket;
}

}  // namespace

int main(int argc, char* argv[]) {
  int http_port = kDefaultHttpPort;
  if (argc > 2) {
    std::cerr << "Usage: " << argv[0] << " [http-port]" << std::endl;
    return EXIT_FAILURE;
  }
  if (argc == 2) {
    try {
      std::size_t parsed_length = 0;
      const long parsed_port = std::stol(argv[1], &parsed_length);
      if (parsed_length != std::string(argv[1]).size() || parsed_port < 1 ||
          parsed_port > 65535) {
        throw std::invalid_argument("port out of range");
      }
      http_port = static_cast<int>(parsed_port);
    } catch (const std::exception&) {
      std::cerr << "HTTP port must be a number between 1 and 65535"
                << std::endl;
      return EXIT_FAILURE;
    }
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  int command_socket = -1;
  int dashboard_socket = -1;
  std::thread dashboard_thread;
  try {
    const int dashboard_port =
        http_port == kDefaultDashboardPort ? kDefaultDashboardPort + 1
                                           : kDefaultDashboardPort;
    command_socket = StartServer(http_port);
    dashboard_socket = StartServer(dashboard_port);
    g_expected_host = "127.0.0.1:" + std::to_string(http_port);
    g_expected_origin = "http://" + g_expected_host;
    robot_sdk::SDKClient client;
    const std::string dashboard_page = ReadDashboardPage();
    dashboard_thread = std::thread(RunDashboardServer, dashboard_socket,
                                   dashboard_port, dashboard_page);
    std::cout << "Robot web control is available at http://127.0.0.1:"
              << http_port << std::endl;
    std::cout << "Robot sensor dashboard is available at http://127.0.0.1:"
              << dashboard_port << std::endl;
    std::cout << "This server accepts connections from this computer only."
              << std::endl;
    while (g_running.load()) {
      const int client_socket = WaitForClient(command_socket);
      if (client_socket < 0) continue;
      HandleClient(client_socket, client);
      close(client_socket);
    }
  } catch (const std::exception& error) {
    g_running.store(false);
    if (dashboard_thread.joinable()) dashboard_thread.join();
    if (command_socket >= 0) close(command_socket);
    if (dashboard_socket >= 0) close(dashboard_socket);
    std::cerr << "[FATAL] " << error.what() << std::endl;
    return EXIT_FAILURE;
  }
  g_running.store(false);
  if (dashboard_thread.joinable()) dashboard_thread.join();
  if (command_socket >= 0) close(command_socket);
  if (dashboard_socket >= 0) close(dashboard_socket);
  return EXIT_SUCCESS;
}
