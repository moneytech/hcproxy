#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>

#include "acceptor.h"
#include "connector.h"
#include "dns.h"
#include "error.h"
#include "forwarder.h"
#include "parser.h"

namespace hcproxy {
namespace {

struct Options : Acceptor::Options,
                 Parser::Options,
                 DnsResolver::Options,
                 Connector::Options,
                 Forwarder::Options {
  // Refuse to connect to any port other than these.
  std::unordered_set<std::string_view> allowed_ports = {"80", "443"};
  // If positive, set the maximum number of open file descriptors (NOFILE)
  // to this value on startup. The proxy uses 6 file descriptors per client
  // connection: 2 sockets + 2 pipes (each pipe is 2 file descriptors).
  // If the open file descriptor limit is reached, the proxy will crash.
  rlim_t max_num_open_files = 0;
};

void RunProxy(const Options& opt) {
  auto IsAllowedPort = [&](std::string_view host_port) {
    auto sep = host_port.find(':');
    return sep != std::string_view::npos && opt.allowed_ports.count(host_port.substr(sep + 1));
  };

  signal(SIGPIPE, SIG_IGN);

  if (opt.max_num_open_files > 0) {
    struct rlimit lim;
    HCP_CHECK(getrlimit(RLIMIT_NOFILE, &lim) == 0);
    lim.rlim_cur = opt.max_num_open_files;
    // It'll fail if opt.max_num_open_files is greater than lim.rlim_max.
    HCP_CHECK(setrlimit(RLIMIT_NOFILE, &lim) == 0);
  }

  auto& acceptor = *new Acceptor(opt);
  auto& parser = *new Parser(opt);
  auto& dns_resolver = *new DnsResolver(opt);
  auto& connector = *new Connector(opt);
  auto& forwarder = *new Forwarder(opt);

  while (true) {
    int client = acceptor.Accept();
    parser.ParseRequest(client, [&, client](std::string_view host_port) {
      if (host_port.empty() || !IsAllowedPort(host_port)) {
        HCP_CHECK(close(client) == 0);
        return;
      }
      dns_resolver.Resolve(host_port, [&, client](std::shared_ptr<const addrinfo> addr) {
        if (!addr) {
          HCP_CHECK(close(client) == 0);
          return;
        }
        connector.Connect(*addr, [&, client](int server) {
          if (server < 0) {
            HCP_CHECK(close(client) == 0);
            return;
          }
          forwarder.Forward(client, server);
        });
      });
    });
  }
}

}  // namespace
}  // namespace hcproxy

int main(int argc, char* argv[]) {
  if (argc != 1) {
    std::cerr << "Usage: " << argv[0] << "\n\n"
              << "To customize, modify `opt` in `main()` (defined in " << __FILE__ << ")"
              << " and recompile." << std::endl;
    return 1;
  }
  hcproxy::Options opt;
  hcproxy::RunProxy(opt);
  assert("!RunProxy is not supposed to return");
  return 1;
}