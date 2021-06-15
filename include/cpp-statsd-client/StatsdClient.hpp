#ifndef STATSD_CLIENT_HPP
#define STATSD_CLIENT_HPP

#include <cpp-statsd-client/UDPSender.hpp>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>

#define STATSD_CLIENT_VERSION_MAJOR 2
#define STATSD_CLIENT_VERSION_MINOR 0
#define STATSD_CLIENT_VERSION_PATCH 0

namespace Statsd {

/*!
 *
 * Statsd client
 *
 * This is the Statsd client, exposing the classic methods
 * and relying on a UDP sender for the actual sending.
 *
 * The prefix for a stat is provided once at construction or
 * on reconfiguring the client. The separator character '.'
 * is automatically inserted between the prefix and the stats
 * key, therefore you should neither append one to the prefix
 * nor prepend one to the key
 *
 * The sampling frequency can be input, as well as the
 * batching size. The latter is the optional limit in
 * number of bytes a batch of stats can be before it is
 * new states are added to a fresh batch. A value of 0
 * means batching is disabled.
 *
 */
class StatsdClient {
public:
    //!@name Constructor and destructor
    //!@{

    //! Constructor
    StatsdClient(const std::string& host,
                 const uint16_t port,
                 const std::string& prefix,
                 const uint64_t batchsize = 0) noexcept;

    StatsdClient(const StatsdClient&) = delete;
    StatsdClient& operator=(const StatsdClient&) = delete;

    //!@}

    //!@name Methods
    //!@{

    //! Sets a configuration { host, port, prefix, batchsize }
    void setConfig(const std::string& host,
                   const uint16_t port,
                   const std::string& prefix,
                   const uint64_t batchsize = 0) noexcept;

    //! Returns the error message as an std::string
    const std::string& errorMessage() const noexcept;

    //! Increments the key, at a given frequency rate
    void increment(const std::string& key, float frequency = 1.0f) const noexcept;

    //! Increments the key, at a given frequency rate
    void decrement(const std::string& key, float frequency = 1.0f) const noexcept;

    //! Adjusts the specified key by a given delta, at a given frequency rate
    void count(const std::string& key, const int delta, float frequency = 1.0f) const noexcept;

    //! Records a gauge for the key, with a given value, at a given frequency rate
    void gauge(const std::string& key, const unsigned int value, float frequency = 1.0f) const noexcept;

    //! Records a timing for a key, at a given frequency
    void timing(const std::string& key, const unsigned int ms, float frequency = 1.0f) const noexcept;

    //! Send a value for a key, according to its type, at a given frequency
    void send(const std::string& key, const int value, const std::string& type, float frequency = 1.0f) const noexcept;

    //! Seed the RNG that controls sampling
    void seed(unsigned int seed = std::random_device()()) noexcept;

    //!@}

private:
    //! The prefix to be used for metrics
    std::string m_prefix;

    //! The UDP sender to be used for actual sending
    std::unique_ptr<UDPSender> m_sender;

    //! The random number generator for handling sampling
    mutable std::mt19937 m_randomEngine;
};

namespace detail {
std::string sanitizePrefix(std::string prefix) {
    // For convenience we provide the dot when generating the stat message
    if (!prefix.empty() && prefix.back() == '.') {
        prefix.pop_back();
    }
    return prefix;
}
}  // namespace detail

inline StatsdClient::StatsdClient(const std::string& host,
                                  const uint16_t port,
                                  const std::string& prefix,
                                  const uint64_t batchsize) noexcept
    : m_prefix(detail::sanitizePrefix(prefix)), m_sender(new UDPSender{host, port, batchsize}) {
    // Initialize the random generator to be used for sampling
    seed();
}

inline void StatsdClient::setConfig(const std::string& host,
                                    const uint16_t port,
                                    const std::string& prefix,
                                    const uint64_t batchsize) noexcept {
    m_prefix = detail::sanitizePrefix(prefix);
    m_sender.reset(new UDPSender(host, port, batchsize));
}

inline const std::string& StatsdClient::errorMessage() const noexcept {
    return m_sender->errorMessage();
}

inline void StatsdClient::decrement(const std::string& key, float frequency) const noexcept {
    return count(key, -1, frequency);
}

inline void StatsdClient::increment(const std::string& key, float frequency) const noexcept {
    return count(key, 1, frequency);
}

inline void StatsdClient::count(const std::string& key, const int delta, float frequency) const noexcept {
    return send(key, delta, "c", frequency);
}

inline void StatsdClient::gauge(const std::string& key,
                                const unsigned int value,
                                const float frequency) const noexcept {
    return send(key, value, "g", frequency);
}

inline void StatsdClient::timing(const std::string& key, const unsigned int ms, float frequency) const noexcept {
    return send(key, ms, "ms", frequency);
}

inline void StatsdClient::send(const std::string& key,
                               const int value,
                               const std::string& type,
                               float frequency) const noexcept {
    // Bail if we can't send anything anyway
    if (!m_sender->initialized()) {
        return;
    }

    // A valid frequency is: 0 <= f <= 1
    // At 0 you never emit the stat, at 1 you always emit the stat and with anything else you roll the dice
    frequency = std::max(std::min(frequency, 1.f), 0.f);
    constexpr float epsilon{0.0001f};
    const bool isFrequencyOne = std::fabs(frequency - 1.0f) < epsilon;
    const bool isFrequencyZero = std::fabs(frequency) < epsilon;
    if (isFrequencyZero ||
        (!isFrequencyOne && (frequency < std::uniform_real_distribution<float>(0.f, 1.f)(m_randomEngine)))) {
        return;
    }

    // Prepare the buffer and include the sampling rate if it's not 1.f
    const bool needs_dot = !m_prefix.empty() && !key.empty();
    std::string buffer(256, '\0');
    int string_len = -1;
    if (isFrequencyOne) {
        // Sampling rate is 1.0f, no need to specify it
        string_len = std::snprintf(&buffer.front(),
                                   buffer.size(),
                                   "%s%s%s:%d|%s",
                                   m_prefix.c_str(),
                                   needs_dot ? "." : "",
                                   key.c_str(),
                                   value,
                                   type.c_str());
    } else {
        // Sampling rate is different from 1.0f, hence specify it
        string_len = std::snprintf(&buffer.front(),
                                   buffer.size(),
                                   "%s%s%s:%d|%s|@%.2f",
                                   m_prefix.c_str(),
                                   needs_dot ? "." : "",
                                   key.c_str(),
                                   value,
                                   type.c_str(),
                                   frequency);
    }

    // Trim the trailing null chars
    // TODO: perf improvement: send the len along and keep the buffer around as a member variable
    buffer.resize(std::min(string_len, static_cast<int>(buffer.size())));

    // Send the message via the UDP sender
    m_sender->send(buffer);
}

inline void StatsdClient::seed(unsigned int seed) noexcept {
    m_randomEngine.seed(seed);
}

}  // namespace Statsd

#endif
