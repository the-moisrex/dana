#include "process.hpp"

#include "strings.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <future>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace dana::process {

    namespace {
        struct pump_entry {
            int                            fd       = -1;
            bool                           open     = true;
            std::string*                   data     = nullptr;
            command::chunk_callback const* callback = nullptr;
        };

        // Reads every stream until EOF, invoking the tee callback as chunks arrive.
        // The poll loop also enforces `timeout` (0 = wait forever); the caller kills
        // and reaps the child when timed_out is set.
        bool pump_streams(std::vector<pump_entry>& entries, std::chrono::milliseconds timeout) {
            std::vector<pollfd> fds;
            fds.reserve(entries.size());
            for (auto const& entry : entries) {
                fds.push_back(pollfd{.fd = entry.fd, .events = POLLIN, .revents = 0});
            }

            auto const deadline = std::chrono::steady_clock::now() + timeout;
            for (;;) {
                bool any_open = false;
                for (auto const& entry : entries) {
                    any_open = any_open || entry.open;
                }
                if (!any_open) {
                    return false;
                }

                int wait_ms = -1;
                if (timeout.count() > 0) {
                    auto const remaining =
                      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                        .count();
                    if (remaining <= 0) {
                        return true;
                    }
                    wait_ms = remaining > 2'147'483'647LL ? 2'147'483'647 : static_cast<int>(remaining);
                }

                int const ready = ::poll(fds.data(), fds.size(), wait_ms);
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return false;
                }
                if (ready == 0) {
                    return true;
                }

                for (std::size_t i = 0; i < entries.size(); ++i) {
                    auto& entry = entries.at(i);
                    if (!entry.open || fds.at(i).revents == 0) {
                        continue;
                    }
                    std::array<char, 8192> buffer{};
                    auto const             n = ::read(entry.fd, buffer.data(), buffer.size());
                    if (n > 0) {
                        entry.data->append(buffer.data(), static_cast<std::size_t>(n));
                        if (*entry.callback) {
                            (*entry.callback)(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
                        }
                    } else if (n == 0 || errno != EINTR) {
                        entry.open   = false;
                        fds.at(i).fd = -1; // poll ignores negative descriptors
                    }
                }
            }
        }

    } // namespace

    command& command::arg(std::string_view const str) {
        m_argv.emplace_back(str);
        return *this;
    }

    command& command::args(text::string_views as) {
        m_argv.insert(m_argv.end(), as.begin(), as.end());
        return *this;
    }

    command& command::stdin_data(std::string data) {
        m_stdin_data = std::move(data);
        return *this;
    }

    command& command::env(text::string_views extra) {
        m_env.insert(m_env.end(), extra.begin(), extra.end());
        return *this;
    }

    command& command::timeout(std::chrono::milliseconds const timeout) {
        m_timeout = timeout;
        return *this;
    }

    command& command::stderr(stderr_mode mode) {
        m_stderr_mode = mode;
        return *this;
    }

    command& command::capture_stderr() {
        m_stderr_mode = stderr_mode::capture;
        return *this;
    }

    command& command::stderr_to_stdout() {
        m_stderr_mode = stderr_mode::to_stdout;
        return *this;
    }

    command& command::discard_stderr() {
        m_stderr_mode = stderr_mode::discard;
        return *this;
    }

    command& command::discard_stdout() {
        m_discard_stdout = true;
        return *this;
    }

    command& command::inherit_stderr() {
        m_stderr_mode = stderr_mode::inherit;
        return *this;
    }

    command& command::on_stdout(chunk_callback callback) {
        m_stdout_callback = std::move(callback);
        return *this;
    }

    command& command::on_stderr(chunk_callback callback) {
        m_stderr_callback = std::move(callback);
        return *this;
    }

    command_result command::run() {
        command_result result;
        if (m_argv.empty()) {
            result.exit_code   = -1;
            result.stderr_data = "Empty command";
            return result;
        }

        std::array<int, 2> in_pipe{};  // parent -> child
        std::array<int, 2> out_pipe{}; // child -> parent
        std::array<int, 2> err_pipe{}; // child -> parent (capture mode only)
        if (::pipe(in_pipe.data()) != 0 || (!m_discard_stdout && ::pipe(out_pipe.data()) != 0)) {
            result.exit_code   = -1;
            result.stderr_data = "pipe failed";
            return result;
        }
        bool const capture_err = m_stderr_mode == stderr_mode::capture || static_cast<bool>(m_stderr_callback);
        if (capture_err && ::pipe(err_pipe.data()) != 0) {
            result.exit_code   = -1;
            result.stderr_data = "pipe failed";
            return result;
        }

        std::vector<char>  argv_buf; // todo: WTF we need array of chars instead of string?
        std::vector<char*> argv_c(m_argv.size() + 1);
        for (std::size_t i = 0; i < m_argv.size(); ++i) {
            // todo: why do we need const_caast?
            argv_c.at(i) = const_cast<char*>(m_argv.at(i).c_str());
        }
        argv_c[m_argv.size()] = nullptr;
        // environ is the process environment; passing it through execvpe keeps the
        // child's environment identical to ours (bash scripts rely on PATH etc.).
        std::vector<char*> envp;
        for (char** e = ::environ; *e != nullptr; ++e) {
            envp.push_back(*e);
        }
        for (auto const& kv : m_env) {
            envp.push_back(const_cast<char*>(kv.c_str()));
        }
        envp.push_back(nullptr);

        // NOTE: no threads exist yet — fork first, then feed the child's stdin
        // from a writer thread in the parent only. Forking while another thread
        // holds a libc lock deadlocks the child inside execvpe.
        auto const started = std::chrono::steady_clock::now();
        pid_t      pid     = ::fork();
        if (pid < 0) {
            result.exit_code   = -1;
            result.stderr_data = "fork failed";
            ::close(in_pipe.at(0));
            ::close(in_pipe.at(1));
            if (!m_discard_stdout) {
                ::close(out_pipe.at(1));
                ::close(out_pipe.at(0));
            }
            if (capture_err) {
                ::close(err_pipe.at(0));
                ::close(err_pipe.at(1));
            }
            return result;
        }

        if (pid == 0) {
            // Child: stdin/stdout are pipes; stderr is inherited (bash's `$(...)`),
            // captured, or merged onto stdout (2>&1).
            ::dup2(in_pipe.at(0), STDIN_FILENO);
            if (m_discard_stdout) {
                int null_fd = ::open("/dev/null", O_WRONLY);
                if (null_fd >= 0) {
                    ::dup2(null_fd, STDOUT_FILENO);
                    ::close(null_fd);
                }
            } else {
                ::dup2(out_pipe.at(1), STDOUT_FILENO);
            }
            if (capture_err) {
                ::dup2(err_pipe.at(1), STDERR_FILENO);
            } else if (m_stderr_mode == stderr_mode::to_stdout) {
                ::dup2(STDOUT_FILENO, STDERR_FILENO);
            } else if (m_stderr_mode == stderr_mode::discard) {
                int null_fd = ::open("/dev/null", O_WRONLY);
                if (null_fd >= 0) {
                    ::dup2(null_fd, STDERR_FILENO);
                    ::close(null_fd);
                }
            }
            ::close(in_pipe.at(1));
            if (!m_discard_stdout) {
                ::close(out_pipe.at(0));
            }
            if (capture_err) {
                ::close(err_pipe.at(0));
            }
            ::execvpe(argv_c.at(0), argv_c.data(), envp.data());
            _exit(127);
        }

        // The child has the read ends it needs; close the parent's copies and
        // hand the child its stdin from a writer thread (or close the pipe right
        // away when there is none). EOF on that write end is what lets a child
        // that reads stdin exit while the parent is still draining stdout.
        ::close(in_pipe.at(0));
        if (!m_discard_stdout) {
            ::close(out_pipe.at(1));
        }
        if (capture_err) {
            ::close(err_pipe.at(1));
        }
        std::string in_copy = m_stdin_data;
        std::thread writer;
        if (in_copy.empty()) {
            ::close(in_pipe.at(1));
            in_pipe.at(1) = -1;
        } else {
            writer = std::thread([&] {
                std::size_t off = 0;
                while (off < in_copy.size()) {
                    auto n = ::write(in_pipe.at(1), in_copy.data() + off, in_copy.size() - off);
                    if (n <= 0) {
                        if (n < 0 && errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    off += static_cast<std::size_t>(n);
                }
                ::close(in_pipe.at(1));
                in_pipe.at(1) = -1;
            });
        }
        std::vector<pump_entry> streams;
        if (!m_discard_stdout) {
            streams.push_back(pump_entry{.fd       = out_pipe.at(0),
                                         .open     = true,
                                         .data     = &result.stdout_data,
                                         .callback = &m_stdout_callback});
        }
        if (capture_err) {
            streams.push_back(pump_entry{.fd       = err_pipe.at(0),
                                         .open     = true,
                                         .data     = &result.stderr_data,
                                         .callback = &m_stderr_callback});
        }
        result.timed_out = pump_streams(streams, m_timeout);
        if (!m_discard_stdout) {
            ::close(out_pipe.at(0));
        }
        if (capture_err) {
            ::close(err_pipe.at(0));
        }
        // Kill before joining the stdin writer: on a timeout the writer may still
        // be blocked writing to a child that stopped reading.
        if (result.timed_out) {
            ::kill(pid, SIGKILL);
        }
        if (writer.joinable()) {
            writer.join();
        }

        int status = 0;
        if (result.timed_out) {
            ::waitpid(pid, &status, 0);
            status = -1;
        } else if (m_timeout.count() > 0) {
            // A discarded stdout gives pump_streams nothing to read, so the
            // deadline has to be enforced while reaping too.
            auto const elapsed =
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            auto const remaining = m_timeout > elapsed ? m_timeout - elapsed : std::chrono::milliseconds{0};
            auto const deadline  = std::chrono::steady_clock::now() + remaining;
            for (;;) {
                pid_t const reaped = ::waitpid(pid, &status, WNOHANG);
                if (reaped == pid) {
                    break;
                }
                if (reaped < 0) {
                    status = -1;
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    result.timed_out = true;
                    ::kill(pid, SIGKILL);
                    ::waitpid(pid, &status, 0);
                    status = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else {
            if (::waitpid(pid, &status, 0) < 0) {
                status = -1;
            }
        }

        if (result.timed_out) {
            result.exit_code = -1;
        } else if (status > 0) {
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return result;
    }

    command_result run_script(
      std::string_view          script,
      text::string_views        extras,
      std::string_view          stdin_data,
      std::chrono::milliseconds timeout) {
        // `bash -c <script> arg0 arg1 ...` passes the extras as $0 and $1.. ($0 is
        // the script name).
        std::array<std::string_view, 3> const cmdline{"bash", "-c", script};
        command                               cmd{cmdline};
        for (auto const& e : extras) {
            cmd.arg(e);
        }
        cmd.stdin_data(std::string(stdin_data));
        cmd.timeout(timeout);
        cmd.inherit_stderr();
        return cmd.run();
    }

    command_result run_command(
      text::string_views        argv,
      std::string_view          stdin_data,
      text::string_views        env,
      std::chrono::milliseconds timeout) {
        command cmd{argv};
        cmd.stdin_data(std::string(stdin_data));
        cmd.env(env);
        cmd.timeout(timeout);
        cmd.inherit_stderr();
        return cmd.run();
    }

    // command_result run_command(
    //   std::span<char const* const> argv,
    //   std::string_view             stdin_data,
    //   std::span<char const* const> env,
    //   std::chrono::milliseconds    timeout) {
    //     std::vector<std::string> args;
    //     for (auto const* p : argv) {
    //         if (p == nullptr) {
    //             break;
    //         }
    //         args.emplace_back(p);
    //     }
    //     std::vector<std::string> envs;
    //     for (auto const* e : env) {
    //         if (e == nullptr) {
    //             break;
    //         }
    //         envs.emplace_back(e);
    //     }
    //     return run_command(args, stdin_data, envs, timeout);
    // }

    // `$(...)` capture semantics: every trailing newline is dropped and the script
    // re-adds exactly one when it prints the value.
    std::string captured(std::string_view const text) {
        return text::strip_trailing_newlines(text);
    }

    snippet_result run_snippet(std::string_view const script, std::string_view const stdin_data) {
        std::string const script_storage(script);
        std::string       command = "python3 -c \"$0\" 2>&1";
        // run_command only redirects stdin when it has data to feed; an empty
        // payload must still read an empty stream, like `printf '%s' "" |` does.
        if (stdin_data.empty()) {
            command += " < /dev/null";
        }

        std::array<std::string, 4> const cmdline{"bash", "-c", command, script_storage};
        auto const                       result = run_command(cmdline, stdin_data);
        return {.output = captured(result.stdout_data), .exit_code = result.exit_code};
    }

    bool tool_exists(std::string_view const name) noexcept try {
        namespace fs = std::filesystem;

        if (name.empty() || name.contains('\0')) {
            return false;
        }

        auto const is_executable = [](fs::path const& path) {
            std::error_code ec;
            return fs::is_regular_file(path, ec) && ::access(path.c_str(), X_OK) == 0;
        };

        // A name containing '/' is a path, not a PATH lookup.
        if (name.contains('/')) {
            return is_executable(name);
        }

        char const* env = std::getenv("PATH");
        if (env == nullptr) {
            return false;
        }

        std::string_view dirs{env};
        for (;;) {
            auto const colon = dirs.find(':');
            // An empty PATH entry denotes the current directory.
            if (is_executable(fs::path{dirs.substr(0, colon)} / name)) {
                return true;
            }
            if (colon == std::string_view::npos) {
                return false;
            }
            dirs.remove_prefix(colon + 1);
        }
    } catch (...) {
        return false;
    }

    std::vector<std::string_view> xargs_tokens(std::string_view const text) {
        std::vector<std::string_view> tokens;
        std::size_t                   pos = 0;
        while (pos < text.size()) {
            auto const start = text.find_first_not_of(" \t\r\n", pos);
            if (start == std::string_view::npos) {
                break;
            }
            std::size_t end = text.find_first_of(" \t\r\n", start);
            if (end == std::string_view::npos) {
                end = text.size();
            }
            tokens.emplace_back(text.substr(start, end - start));
            pos = end;
        }
        return tokens;
    }


} // namespace dana::process
