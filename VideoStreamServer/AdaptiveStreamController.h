#pragma once
#include <string>
#include <mutex>

struct StreamStrategy
{
    double multiplier;
    int fps_limit;
};

class AdaptiveStreamController
{
public:
    AdaptiveStreamController();
    StreamStrategy get_current_strategy();
    void update_strategy(double loss_rate);

private:
    std::string m_current_strategy_name;
    StreamStrategy m_good_strategy;
    StreamStrategy m_medium_strategy;
    StreamStrategy m_poor_strategy;
    std::mutex m_mutex;
};