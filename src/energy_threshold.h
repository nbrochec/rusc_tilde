
#ifndef RUSC_TILDE_ENERGY_THRESHOLD_H
#define RUSC_TILDE_ENERGY_THRESHOLD_H

#include <vector>
#include <cmath>

class EnergyThreshold {
public:
    static constexpr double MINIMUM_THRESHOLD = -80.0;

    explicit EnergyThreshold(double threshold_db) : m_threshold_db(threshold_db) {}

    bool is_above_threshold(const std::vector<double>& v) const {
        if (m_threshold_db <= MINIMUM_THRESHOLD) {
            return true;
        }
        return atodb(rms(v)) >= m_threshold_db;
    }

    void set_threshold_db(double threshold_db) {
        m_threshold_db = threshold_db;
    }

    static double atodb(double a) {
        if (a <= 0) return -120.0;
        return 20.0 * std::log10(a);
    }

    static double rms(const std::vector<double>& v) {
        double sum = 0.0;
        for (auto& x : v) sum += x * x;
        return std::sqrt(sum / static_cast<double>(v.size()));
    }

private:
    double m_threshold_db;
};

#endif //RUSC_TILDE_ENERGY_THRESHOLD_H
