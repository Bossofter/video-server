#include "signaling_server.h"

#include <algorithm>
#include <utility>

#include <spdlog/spdlog.h>

#include "logging_utils.h"

namespace video_server
{
    namespace
    {
        std::string make_session_id(uint64_t session_generation)
        {
            return "session-" + std::to_string(session_generation);
        }
    } // namespace

    void SignalingServer::prune_inactive_recipients_locked(StreamSessionSlot &slot)
    {
        for (auto it = slot.sessions.begin(); it != slot.sessions.end();)
        {
            if (!it->second.session || !it->second.session->is_active())
            {
                it = slot.sessions.erase(it);
                continue;
            }
            ++it;
        }
    }

    SignalingServer::SignalingServer(StreamExistsFn stream_exists, MaxSubscribersGetterFn max_subscribers_getter,
                                     LatestFrameGetterFn latest_frame_getter,
                                     LatestEncodedUnitGetterFn latest_encoded_unit_getter,
                                     size_t max_pending_candidates_per_stream)
        : stream_exists_(std::move(stream_exists)),
          max_subscribers_getter_(std::move(max_subscribers_getter)),
          latest_frame_getter_(std::move(latest_frame_getter)),
          latest_encoded_unit_getter_(std::move(latest_encoded_unit_getter)),
          max_pending_candidates_per_stream_(max_pending_candidates_per_stream)
    {
        ensure_default_logging_config();
    }

    SignalingServer::~SignalingServer() { stop(); }

    bool SignalingServer::set_offer(const std::string &stream_id, const std::string &offer_sdp,
                                    std::string *error_message, std::string *session_id_out)
    {
        if (!stream_exists_(stream_id))
        {
            if (error_message != nullptr)
            {
                *error_message = "stream not found";
            }
            return false;
        }

        const auto max_subscribers = max_subscribers_getter_(stream_id).value_or(1u);
        uint64_t session_generation = 0;
        std::string session_id;
        std::vector<std::string> pending_remote_candidates;
        const bool replace_single_subscriber_session = max_subscribers == 1u;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &slot = sessions_[stream_id];
            prune_inactive_recipients_locked(slot);
            if (!replace_single_subscriber_session && slot.sessions.size() >= max_subscribers)
            {
                if (error_message != nullptr)
                {
                    *error_message = "max subscribers reached";
                }
                spdlog::warn("[signaling] rejected offer stream={} active_subscribers={} max_subscribers={}", stream_id,
                             slot.sessions.size(), max_subscribers);
                return false;
            }

            session_generation = ++slot.next_session_generation;
            session_id = make_session_id(session_generation);
            pending_remote_candidates = slot.pending_remote_candidates;
            slot.pending_remote_candidates.clear();
        }

        auto session = std::make_shared<WebRtcStreamSession>(stream_id, latest_frame_getter_, latest_encoded_unit_getter_);
        spdlog::info("[signaling] offer received stream={} session_id={} size={}", stream_id, session_id, offer_sdp.size());
        if (!session->apply_offer(offer_sdp, error_message))
        {
            session->stop();
            return false;
        }

        std::vector<std::shared_ptr<WebRtcStreamSession>> previous_sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto &slot = sessions_[stream_id];
            prune_inactive_recipients_locked(slot);
            if (replace_single_subscriber_session)
            {
                previous_sessions.reserve(slot.sessions.size());
                for (const auto &[_, recipient] : slot.sessions)
                {
                    if (recipient.session)
                    {
                        previous_sessions.push_back(recipient.session);
                    }
                }
                slot.sessions.clear();
            }
            slot.sessions[session_id] = StreamSessionSlot::RecipientSessionSlot{session_generation, session};
        }

        for (const auto &previous : previous_sessions)
        {
            previous->stop();
        }

        for (const auto &candidate : pending_remote_candidates)
        {
            std::string candidate_error;
            if (session->add_remote_candidate(candidate, &candidate_error))
            {
                spdlog::debug("[signaling] applied queued remote candidate stream={} session_id={} size={}", stream_id,
                              session_id, candidate.size());
                continue;
            }
            spdlog::warn("[signaling] failed to apply queued remote candidate stream={} session_id={} error={}",
                         stream_id, session_id, candidate_error);
        }

        if (session_id_out != nullptr)
        {
            *session_id_out = session_id;
        }
        return true;
    }

    bool SignalingServer::set_answer(const std::string &stream_id, const std::string &answer_sdp,
                                     std::string *error_message, std::optional<std::string> session_id)
    {
        std::shared_ptr<WebRtcStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(stream_id);
            if (it == sessions_.end())
            {
                if (error_message != nullptr)
                {
                    *error_message = "session not found";
                }
                return false;
            }

            prune_inactive_recipients_locked(it->second);
            if (it->second.sessions.empty())
            {
                if (error_message != nullptr)
                {
                    *error_message = "session not found";
                }
                return false;
            }

            if (session_id.has_value())
            {
                const auto recipient_it = it->second.sessions.find(*session_id);
                if (recipient_it == it->second.sessions.end())
                {
                    if (error_message != nullptr)
                    {
                        *error_message = "session not found";
                    }
                    return false;
                }
                session = recipient_it->second.session;
            }
            else
            {
                const auto newest = std::max_element(
                    it->second.sessions.begin(), it->second.sessions.end(),
                    [](const auto &lhs, const auto &rhs)
                    { return lhs.second.session_generation < rhs.second.session_generation; });
                session = newest->second.session;
            }
        }
        return session->apply_answer(answer_sdp, error_message);
    }

    bool SignalingServer::add_ice_candidate(const std::string &stream_id, const std::string &candidate,
                                            std::string *error_message, std::optional<std::string> session_id)
    {
        std::shared_ptr<WebRtcStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(stream_id);
            if (it == sessions_.end())
            {
                if (!stream_exists_(stream_id))
                {
                    if (error_message != nullptr)
                    {
                        *error_message = "stream not found";
                    }
                    return false;
                }
                if (session_id.has_value())
                {
                    if (error_message != nullptr)
                    {
                        *error_message = "session not found";
                    }
                    return false;
                }
                auto &slot = sessions_[stream_id];
                if (slot.pending_remote_candidates.size() >= max_pending_candidates_per_stream_)
                {
                    if (error_message != nullptr)
                    {
                        *error_message = "too many pending candidates";
                    }
                    return false;
                }
                slot.pending_remote_candidates.push_back(candidate);
                spdlog::debug("[signaling] queued remote candidate before offer stream={} size={}", stream_id,
                              candidate.size());
                return true;
            }

            prune_inactive_recipients_locked(it->second);
            if (session_id.has_value())
            {
                const auto recipient_it = it->second.sessions.find(*session_id);
                if (recipient_it == it->second.sessions.end())
                {
                    if (error_message != nullptr)
                    {
                        *error_message = "session not found";
                    }
                    return false;
                }
                session = recipient_it->second.session;
            }
            else if (!it->second.sessions.empty())
            {
                const auto newest = std::max_element(
                    it->second.sessions.begin(), it->second.sessions.end(),
                    [](const auto &lhs, const auto &rhs)
                    { return lhs.second.session_generation < rhs.second.session_generation; });
                session = newest->second.session;
            }
            else
            {
                if (it->second.pending_remote_candidates.size() >= max_pending_candidates_per_stream_)
                {
                    if (error_message != nullptr)
                    {
                        *error_message = "too many pending candidates";
                    }
                    return false;
                }
                it->second.pending_remote_candidates.push_back(candidate);
                spdlog::debug("[signaling] queued remote candidate awaiting replacement session stream={} size={}",
                              stream_id, candidate.size());
                return true;
            }
        }

        spdlog::debug("[signaling] candidate received stream={} session_id={} size={}", stream_id,
                      session_id.value_or("latest"), candidate.size());
        if (!session->add_remote_candidate(candidate, error_message))
        {
            spdlog::warn("[signaling] candidate rejected stream={} session_id={} error={}", stream_id,
                         session_id.value_or("latest"),
                         (error_message != nullptr ? *error_message : std::string("unknown")));
            return false;
        }
        spdlog::debug("[signaling] candidate applied stream={} session_id={} size={}", stream_id,
                      session_id.value_or("latest"), candidate.size());
        return true;
    }

    void SignalingServer::on_latest_frame(const std::string &stream_id, std::shared_ptr<const LatestFrame> latest_frame)
    {
        std::vector<std::shared_ptr<WebRtcStreamSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(stream_id);
            if (it == sessions_.end())
            {
                return;
            }
            prune_inactive_recipients_locked(it->second);
            sessions.reserve(it->second.sessions.size());
            for (const auto &[_, recipient] : it->second.sessions)
            {
                sessions.push_back(recipient.session);
            }
        }
        for (const auto &session : sessions)
        {
            session->on_latest_frame(latest_frame);
        }
    }

    void SignalingServer::on_encoded_access_unit(const std::string &stream_id,
                                                 std::shared_ptr<const LatestEncodedUnit> latest_encoded_unit)
    {
        std::vector<std::shared_ptr<WebRtcStreamSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(stream_id);
            if (it == sessions_.end())
            {
                return;
            }
            prune_inactive_recipients_locked(it->second);
            sessions.reserve(it->second.sessions.size());
            for (const auto &[_, recipient] : it->second.sessions)
            {
                sessions.push_back(recipient.session);
            }
        }
        for (const auto &session : sessions)
        {
            session->on_encoded_access_unit(latest_encoded_unit);
        }
    }

    std::optional<SignalingSession> SignalingServer::get_session(const std::string &stream_id,
                                                                 std::optional<std::string> session_id) const
    {
        std::string resolved_session_id;
        uint64_t session_generation = 0;
        std::shared_ptr<WebRtcStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(stream_id);
            if (it == sessions_.end())
            {
                return std::nullopt;
            }

            prune_inactive_recipients_locked(it->second);
            if (it->second.sessions.empty())
            {
                return std::nullopt;
            }

            if (session_id.has_value())
            {
                const auto recipient_it = it->second.sessions.find(*session_id);
                if (recipient_it == it->second.sessions.end())
                {
                    return std::nullopt;
                }
                resolved_session_id = recipient_it->first;
                session_generation = recipient_it->second.session_generation;
                session = recipient_it->second.session;
            }
            else
            {
                const auto newest = std::max_element(
                    it->second.sessions.begin(), it->second.sessions.end(),
                    [](const auto &lhs, const auto &rhs)
                    { return lhs.second.session_generation < rhs.second.session_generation; });
                resolved_session_id = newest->first;
                session_generation = newest->second.session_generation;
                session = newest->second.session;
            }
        }

        const auto snapshot = session->snapshot();
        return SignalingSession{resolved_session_id,
                                session_generation,
                                snapshot.stream_id,
                                snapshot.offer_sdp,
                                snapshot.answer_sdp,
                                snapshot.last_remote_candidate,
                                snapshot.last_local_candidate,
                                snapshot.peer_state,
                                snapshot.active,
                                snapshot.sending_active,
                                snapshot.teardown_reason,
                                snapshot.last_transition_reason,
                                snapshot.disconnect_count,
                                snapshot.media_source};
    }

    std::vector<SignalingSession> SignalingServer::list_sessions() const
    {
        struct SessionRef
        {
            std::string session_id;
            uint64_t generation{0};
            std::shared_ptr<WebRtcStreamSession> session;
        };

        std::vector<SessionRef> sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto &[_, slot] : sessions_)
            {
                prune_inactive_recipients_locked(slot);
                for (const auto &[session_id, recipient] : slot.sessions)
                {
                    if (!recipient.session)
                    {
                        continue;
                    }
                    sessions.push_back(SessionRef{session_id, recipient.session_generation, recipient.session});
                }
            }
        }

        std::vector<SignalingSession> output;
        output.reserve(sessions.size());
        for (const auto &entry : sessions)
        {
            const auto snapshot = entry.session->snapshot();
            output.push_back(SignalingSession{entry.session_id,
                                              entry.generation,
                                              snapshot.stream_id,
                                              snapshot.offer_sdp,
                                              snapshot.answer_sdp,
                                              snapshot.last_remote_candidate,
                                              snapshot.last_local_candidate,
                                              snapshot.peer_state,
                                              snapshot.active,
                                              snapshot.sending_active,
                                              snapshot.teardown_reason,
                                              snapshot.last_transition_reason,
                                              snapshot.disconnect_count,
                                              snapshot.media_source});
        }
        return output;
    }

    void SignalingServer::remove_stream(const std::string &stream_id)
    {
        StreamSessionSlot slot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(stream_id);
            if (it == sessions_.end())
            {
                return;
            }
            slot = std::move(it->second);
            sessions_.erase(it);
        }

        for (auto &[_, recipient] : slot.sessions)
        {
            if (recipient.session)
            {
                recipient.session->stop();
            }
        }
    }

    void SignalingServer::stop()
    {
        std::unordered_map<std::string, StreamSessionSlot> sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions.swap(sessions_);
        }

        for (auto &[_, slot] : sessions)
        {
            for (auto &[__, recipient] : slot.sessions)
            {
                if (recipient.session)
                {
                    recipient.session->stop();
                }
            }
        }
    }

} // namespace video_server
