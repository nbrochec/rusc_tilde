
#ifndef RUSC_TILDE_LEAKY_INTEGRATOR_H
#define RUSC_TILDE_LEAKY_INTEGRATOR_H

#include <chrono>
#include <vector>
#include <optional>

class LeakyIntegrator {
public:
    std::vector<float> process(const std::vector<float>& input) {
        auto current_time = std::chrono::steady_clock::now();

        if (!m_last_callback || m_tau < 1e-6 || m_previous_value.size() != input.size()) {
            m_last_callback = current_time;
            m_previous_value = input;
            return input;
        }

        auto elapsed = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(current_time - *m_last_callback).count());

        elapsed = std::min(m_tau, std::max(0.0, elapsed));

        auto output = integrate(input, elapsed);
        m_previous_value = output;
        m_last_callback = current_time;

        return output;
    }

    void set_tau(double tau) { m_tau = tau; }

private:
    std::vector<float> integrate(const std::vector<float>& current, double elapsed) {
        std::vector<float> result;
        result.reserve(current.size());
        auto dt = elapsed / m_tau;
        for (std::size_t i = 0; i < current.size(); i++) {
            result.emplace_back((1 - dt) * m_previous_value.at(i) + dt * current.at(i));
        }
        return result;
    }

    std::optional<std::chrono::time_point<std::chrono::steady_clock>> m_last_callback;
    std::vector<float> m_previous_value;
    double m_tau = 0.0;
};

#endif //RUSC_TILDE_LEAKY_INTEGRATOR_H
