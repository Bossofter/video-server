#include <exception>
#include <iostream>

#include <spdlog/spdlog.h>

#include "../src/testing/soak_test_framework.h"
#include "../src/webrtc/logging_utils.h"

int main(int argc, char **argv)
{
    video_server::ensure_default_logging_config();

    std::string parse_error;
    const auto options = video_server::soak::parse_runner_options(argc, argv, &parse_error);
    if (!options.has_value())
    {
        if (!parse_error.empty())
        {
            std::cerr << parse_error << '\n';
        }
        return parse_error.rfind("Usage:", 0) == 0 ? 0 : 1;
    }

    try
    {
        video_server::soak::SoakRunner runner(*options);
        const auto summary = runner.run();
        std::cout << "[soak] completed duration=" << summary.total_duration_seconds
                  << "s success=" << (summary.success ? "true" : "false")
                  << " failures=" << summary.failures.size() << '\n';
        std::cout << "[soak] scenario_coverage reconnect="
                  << (summary.all_streams_saw_reconnect_churn ? "all-streams" : "partial")
                  << " config="
                  << (summary.all_streams_saw_config_churn ? "all-streams" : "partial");
        if (!summary.streams_missing_reconnect_churn.empty())
        {
            std::cout << " missing_reconnect=";
            for (size_t i = 0; i < summary.streams_missing_reconnect_churn.size(); ++i)
            {
                if (i > 0)
                {
                    std::cout << ',';
                }
                std::cout << summary.streams_missing_reconnect_churn[i];
            }
        }
        if (!summary.streams_missing_config_churn.empty())
        {
            std::cout << " missing_config=";
            for (size_t i = 0; i < summary.streams_missing_config_churn.size(); ++i)
            {
                if (i > 0)
                {
                    std::cout << ',';
                }
                std::cout << summary.streams_missing_config_churn[i];
            }
        }
        std::cout << '\n';
        for (const auto &stream : summary.streams)
        {
            std::cout << "[soak] stream=" << stream.stream_id
                      << " samples=" << stream.samples
                      << " reconnects=" << stream.reconnect_count
                      << " config_updates=" << stream.config_updates
                      << " reconnect_churn=" << (stream.reconnect_churn_observed ? "yes" : "no")
                      << " config_churn=" << (stream.config_churn_observed ? "yes" : "no")
                      << " final_session_generation=" << stream.final_session_generation
                      << " final_config_generation=" << stream.final_config_generation
                      << " disconnects=" << stream.final_disconnect_count
                      << " packets_sent=" << stream.final_packets_sent
                      << " attempted=" << stream.final_packets_attempted
                      << " send_failures=" << stream.final_send_failures
                      << " packetization_failures=" << stream.final_packetization_failures
                      << " dropped_frames=" << stream.final_total_frames_dropped << '\n';
        }
        for (const auto &failure : summary.failures)
        {
            std::cout << "[soak][failure] t=" << failure.elapsed_seconds
                      << "s stream=" << failure.stream_id
                      << " code=" << failure.code
                      << " message=" << failure.message << '\n';
        }
        return summary.success ? 0 : 2;
    }
    catch (const std::exception &ex)
    {
        spdlog::error("[soak] fatal error: {}", ex.what());
        return 1;
    }
}
