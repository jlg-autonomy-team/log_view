// Copyright 2020 Hatchbed L.L.C.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the copyright holder nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <log_view/journal_reader.h>

#include <systemd/sd-journal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <log_view/datatypes.h>

namespace log_view {

JournalReader::JournalReader(LogStorePtr& logs) :
  logs_(logs)
{}

JournalReader::~JournalReader() {
  stop();
}

std::vector<std::string> JournalReader::discoverContainers() {
  std::vector<std::string> containers;

  FILE* pipe = popen("podman ps --format '{{.Names}}'", "r");
  if (!pipe) {
    return containers;
  }

  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    std::string name(buffer);
    // Remove trailing newline
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) {
      name.pop_back();
    }
    if (!name.empty()) {
      containers.push_back(name);
    }
  }
  int status = pclose(pipe);
  if (status != 0) {
    // podman command failed; return whatever was collected (likely empty)
    containers.clear();
  }

  return containers;
}

uint8_t JournalReader::mapPriority(int priority) {
  switch (priority) {
    case 0:  // EMERG
    case 1:  // ALERT
    case 2:  // CRIT
      return LogLevel::FATAL;
    case 3:  // ERR
      return LogLevel::ERROR;
    case 4:  // WARNING
      return LogLevel::WARN;
    case 5:  // NOTICE
    case 6:  // INFO
      return LogLevel::INFO;
    case 7:  // DEBUG
      return LogLevel::DEBUG;
    default:
      return LogLevel::INFO;
  }
}

void JournalReader::run() {
  running_ = true;

  // Discover running podman containers
  std::vector<std::string> containers = discoverContainers();

  sd_journal* journal = nullptr;
  int r = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY);
  if (r < 0) {
    return;
  }

  // Add match filters for each container.
  // Podman logs to journalctl with CONTAINER_NAME=<name> field.
  if (!containers.empty()) {
    for (size_t i = 0; i < containers.size(); i++) {
      std::string match = "CONTAINER_NAME=" + containers[i];
      sd_journal_add_match(journal, match.c_str(), 0);
      // Add disjunction (OR) between container matches, but not after the last one
      if (i + 1 < containers.size()) {
        sd_journal_add_disjunction(journal);
      }
    }
  }

  // Seek to the beginning to read existing entries
  sd_journal_seek_head(journal);

  while (running_) {
    // Process all available entries
    while (sd_journal_next(journal) > 0) {
      if (!running_) {
        break;
      }

      // Get timestamp (microseconds since epoch)
      uint64_t usec = 0;
      sd_journal_get_realtime_usec(journal, &usec);
      double timestamp = static_cast<double>(usec) / 1000000.0;

      // Get priority
      const char* priority_data = nullptr;
      size_t priority_len = 0;
      int priority = 6;  // default to INFO
      if (sd_journal_get_data(journal, "PRIORITY", reinterpret_cast<const void**>(&priority_data),
          &priority_len) >= 0) {
        // Data format is "PRIORITY=N"
        const char* eq = static_cast<const char*>(memchr(priority_data, '=', priority_len));
        if (eq) {
          char* end = nullptr;
          long val = strtol(eq + 1, &end, 10);
          if (end != eq + 1 && val >= 0 && val <= 7) {
            priority = static_cast<int>(val);
          }
        }
      }

      // Get container name
      std::string container_name;
      const char* container_data = nullptr;
      size_t container_len = 0;
      if (sd_journal_get_data(journal, "CONTAINER_NAME",
          reinterpret_cast<const void**>(&container_data), &container_len) >= 0) {
        const char* eq = static_cast<const char*>(memchr(container_data, '=', container_len));
        if (eq) {
          container_name = std::string(eq + 1, container_data + container_len - (eq + 1));
        }
      }

      // Fall back to SYSLOG_IDENTIFIER if no container name
      if (container_name.empty()) {
        const char* syslog_data = nullptr;
        size_t syslog_len = 0;
        if (sd_journal_get_data(journal, "SYSLOG_IDENTIFIER",
            reinterpret_cast<const void**>(&syslog_data), &syslog_len) >= 0) {
          const char* eq = static_cast<const char*>(memchr(syslog_data, '=', syslog_len));
          if (eq) {
            container_name = std::string(eq + 1, syslog_data + syslog_len - (eq + 1));
          }
        }
      }

      // Get message
      std::string message;
      const char* msg_data = nullptr;
      size_t msg_len = 0;
      if (sd_journal_get_data(journal, "MESSAGE",
          reinterpret_cast<const void**>(&msg_data), &msg_len) >= 0) {
        const char* eq = static_cast<const char*>(memchr(msg_data, '=', msg_len));
        if (eq) {
          message = std::string(eq + 1, msg_data + msg_len - (eq + 1));
        }
      }

      if (!message.empty()) {
        uint8_t level = mapPriority(priority);
        logs_->addEntry(LogEntry(timestamp, level, container_name, message));
      }
    }

    if (!running_) {
      break;
    }

    // Wait for new entries before checking running_ flag again
    static const uint64_t kWaitTimeoutUsec = 500 * 1000;  // 500ms
    r = sd_journal_wait(journal, kWaitTimeoutUsec);
    if (r < 0) {
      break;
    }
  }

  sd_journal_close(journal);
}

void JournalReader::stop() {
  running_ = false;
}

}  // namespace log_view
