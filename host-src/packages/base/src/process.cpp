#include "goober/base/process.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>

extern char** environ;

namespace goober::base {

namespace {
void close_both(int p[2]) {
  if (p[0] >= 0) close(p[0]);
  if (p[1] >= 0) close(p[1]);
}
}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv_in, const std::string& input, size_t max_out,
                          int64_t timeout_ms) {
  ProcessResult r;
  if (argv_in.empty()) return r;
  int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
  if (pipe2(in, O_CLOEXEC) != 0 || pipe2(out, O_CLOEXEC) != 0 || pipe2(err, O_CLOEXEC) != 0) {
    close_both(in);
    close_both(out);
    close_both(err);
    return r;
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  std::vector<std::string> args = argv_in;
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(in[0]);
  close(out[1]);
  close(err[1]);
  if (rc != 0) {
    close(in[1]);
    close(out[0]);
    close(err[0]);
    return r;
  }
  r.started = true;
  // Feed stdin and drain stdout and stderr together, so neither side blocks the other.
  signal(SIGPIPE, SIG_IGN);
  fcntl(in[1], F_SETFL, O_NONBLOCK);
  size_t written = 0;
  if (input.empty()) {
    close(in[1]);
    in[1] = -1;
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool out_open = true, err_open = true;
  char buf[65536];
  while (out_open || err_open) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      r.timed_out = true;
      break;
    }
    pollfd fds[3];
    int n = 0;
    int out_i = -1, err_i = -1, in_i = -1;
    if (out_open) fds[out_i = n++] = {out[0], POLLIN, 0};
    if (err_open) fds[err_i = n++] = {err[0], POLLIN, 0};
    if (in[1] >= 0) fds[in_i = n++] = {in[1], POLLOUT, 0};
    if (poll(fds, nfds_t(n), int(std::min<int64_t>(left, 1000))) < 0) break;
    if (in_i >= 0 && (fds[in_i].revents & (POLLOUT | POLLERR | POLLHUP))) {
      ssize_t w = write(in[1], input.data() + written, input.size() - written);
      if (w > 0) written += size_t(w);
      if (w < 0 || written == input.size()) {
        close(in[1]);
        in[1] = -1;
      }
    }
    if (out_i >= 0 && (fds[out_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t got = read(out[0], buf, sizeof buf);
      if (got <= 0) {
        out_open = false;
      } else if (r.out.size() + size_t(got) > max_out) {
        r.truncated = true;
        break;
      } else {
        r.out.append(buf, size_t(got));
      }
    }
    if (err_i >= 0 && (fds[err_i].revents & (POLLIN | POLLHUP | POLLERR))) {
      ssize_t got = read(err[0], buf, sizeof buf);
      if (got <= 0) {
        err_open = false;
      } else if (r.err.size() < 4096) {
        r.err.append(buf, size_t(std::min<ssize_t>(got, ssize_t(4096 - r.err.size()))));
      }
    }
  }
  if (in[1] >= 0) close(in[1]);
  close(out[0]);
  close(err[0]);
  if (r.truncated || r.timed_out) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
  return r;
}

}  // namespace goober::base
