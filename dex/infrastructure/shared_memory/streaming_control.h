#ifndef DEX_INFRASTRUCTURE_SHARED_MEMORY_STREAMING_CONTROL_H
#define DEX_INFRASTRUCTURE_SHARED_MEMORY_STREAMING_CONTROL_H

// System headers
#include <atomic>
#include <csignal>  // for signal, SIGINT, SIGTERM
#include <map>
#include <vector>

namespace dex::shared_memory {

/**
 * A singleton class that manages streaming control state and signal handling.
 *
 * This class provides centralized control for:
 * - Running state management
 * - Signal handling (optional)
 * - Graceful shutdown support
 *
 * The singleton instance can be configured either:
 * - Before first use via SetDefaultConfiguration()
 * - At runtime via Reset()
 */
class StreamingControl {
 public:
  /**
   * Configuration options for StreamingControl
   * @param handle_signals Whether to install signal handlers for graceful shutdown
   * @param chain_handlers Whether to chain to previous signal handlers if they exist
   * @param signals List of signals to handle (defaults to SIGINT and SIGTERM)
   */
  struct Configuration {
    bool handle_signals = false;                // Whether to install signal handlers
    bool chain_handlers = true;                 // Whether to chain to previous handlers
    std::vector<int> signals{SIGINT, SIGTERM};  // Signals to handle
  };

  // Delete copy/move operations
  StreamingControl(const StreamingControl&) = delete;
  StreamingControl& operator=(const StreamingControl&) = delete;
  StreamingControl(StreamingControl&&) = delete;
  StreamingControl& operator=(StreamingControl&&) = delete;

  /**
   * Sets the default configuration used when creating the singleton instance.
   * Must be called before the first call to Instance() to take effect.
   *
   * @param configuration The configuration to use as default
   */
  static void SetDefaultConfiguration(Configuration configuration) {
    GetDefaultConfiguration() = std::move(configuration);
  }

  /**
   * Returns the singleton instance, creating it with default configuration if needed.
   *
   * @return Reference to the singleton instance
   */
  static StreamingControl& Instance() {
    static auto instance = StreamingControl(GetDefaultConfiguration());
    return instance;
  }

  /**
   * Checks if streaming should continue.
   *
   * @return true if running and no stop has been requested, false otherwise
   */
  [[nodiscard]] bool IsRunning() const {
    return running_.test(std::memory_order_relaxed) && (configuration_.handle_signals ? (stop_requested_ == 0) : true);
  }

  /**
   * Signals that streaming should stop.
   * Sets the running flag to false and updates stop_requested_ if signal handling is enabled.
   */
  void Stop();

  /**
   * Resets only the running state, keeping current configuration.
   */
  void Reset();

  /**
   * Updates configuration and resets running state.
   * If signal handling is enabled/disabled, this will install/restore signal handlers.
   *
   * @param configuration The configuration to use
   */
  void ReconfigureAndReset(Configuration configuration);

 private:
  /**
   * Returns reference to the static default configuration.
   * Used internally to manage the default configuration state.
   */
  static Configuration& GetDefaultConfiguration() {
    static Configuration default_configuration = {};
    return default_configuration;
  }

  // Update constructor to take Configuration by value and move
  explicit StreamingControl(Configuration configuration);
  ~StreamingControl();

  // Installs the signal handlers for the signals specified in the config.
  void InstallSignalHandlers();
  // Restores the saved signal handlers.
  void RestoreSignalHandlers();
  // The signal handler installed by this class.
  static void SignalHandler(int sig);

  // Configuration
  Configuration configuration_;
  // Saved previous signal handlers (if any)
  std::map<int, struct sigaction> previous_handlers_;
  // Stop flag
  volatile sig_atomic_t stop_requested_ = 0;
  // Running state; initially false then set to true via test_and_set()
  std::atomic_flag running_{false};
};

}  // namespace dex::shared_memory
#endif  // DEX_INFRASTRUCTURE_SHARED_MEMORY_STREAMING_CONTROL_H

