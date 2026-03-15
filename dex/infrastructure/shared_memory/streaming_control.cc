#include "dex/infrastructure/shared_memory/streaming_control.h"

#include <utility>

namespace dex::shared_memory {

StreamingControl::StreamingControl(Configuration configuration) : configuration_(std::move(configuration)) {
  running_.test_and_set();
  if (configuration_.handle_signals) {
    InstallSignalHandlers();
  }
}

StreamingControl::~StreamingControl() {
  if (configuration_.handle_signals) {
    RestoreSignalHandlers();
  }
}

void StreamingControl::Stop() {
  running_.clear(std::memory_order_relaxed);
  if (configuration_.handle_signals) {
    stop_requested_ = 1;
  }
}

void StreamingControl::Reset() {
  running_.test_and_set(std::memory_order_relaxed);  // Set running flag back to true
  stop_requested_ = 0;                               // Reset stop flag
}

void StreamingControl::ReconfigureAndReset(Configuration configuration) {
  // First restore old handlers if we were handling signals
  if (configuration_.handle_signals) {
    RestoreSignalHandlers();
  }

  // Update configuration
  configuration_ = std::move(configuration);

  running_.test_and_set(std::memory_order_relaxed);  // Set running flag back to true
  stop_requested_ = 0;                               // Reset stop flag

  // Install new signal handlers if needed
  if (configuration_.handle_signals) {
    InstallSignalHandlers();
  }
}

void StreamingControl::InstallSignalHandlers() {
  struct sigaction signal_action = {};
  signal_action.sa_handler = SignalHandler;
  signal_action.sa_flags = 0;
  sigemptyset(&signal_action.sa_mask);

  previous_handlers_.clear();
  for (const int sig : configuration_.signals) {
    struct sigaction previous = {};
    if (sigaction(sig, &signal_action, &previous) == 0) {
      previous_handlers_[sig] = previous;
    }
  }
}

void StreamingControl::RestoreSignalHandlers() {
  for (const auto& [sig, handler] : previous_handlers_) {
    sigaction(sig, &handler, nullptr);
  }
}

void StreamingControl::SignalHandler(int sig) {
  StreamingControl& instance = Instance();
  instance.stop_requested_ = 1;
  if (instance.configuration_.chain_handlers) {
    auto handler_it = instance.previous_handlers_.find(sig);
    if (handler_it != instance.previous_handlers_.end()) {
      const auto& previous = handler_it->second;
      if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(sig);
      }
    }
  }
}

}  // namespace dex::shared_memory

