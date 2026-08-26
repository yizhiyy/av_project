#include "modules/rendering/opengl/OpenGlVideoRendererHud.h"

#include <cstring>

#include "imgui/imgui.h"

namespace sclient {

namespace {

// HUD 内部保留最近 120 个采样，用于绘制简短的延迟趋势图。
constexpr std::size_t kLatencyHistorySize = 120;

struct LatencyHistory {
    float values[kLatencyHistorySize] = {};
    std::size_t index = 0;
    std::size_t count = 0;

    void Push(float value) {
        // 环形队列写满后覆盖最旧样本，避免 HUD 运行时间越长占用越多内存。
        values[index] = value;
        index = (index + 1) % kLatencyHistorySize;
        if (count < kLatencyHistorySize) {
            ++count;
        }
    }
};

LatencyHistory g_capture_to_render_history;
LatencyHistory g_receive_to_render_history;
LatencyHistory g_decode_time_history;

bool HasSamples(const LatencySummary &summary) {
    return summary.count > 0;
}

ImVec4 LatencyColor(double ms) {
    // 以约一帧（16ms）和较高延迟（50ms）作为直观的绿/黄/红阈值。
    if (ms < 16.0) {
        return ImVec4(0.40f, 0.85f, 0.45f, 1.0f);  // green
    }
    if (ms < 50.0) {
        return ImVec4(0.95f, 0.80f, 0.25f, 1.0f);  // yellow
    }
    return ImVec4(0.95f, 0.30f, 0.30f, 1.0f);  // red
}

ImVec4 LossColor(double percent) {
    // 丢包率采用更宽松的 1%/5% 阈值。
    if (percent < 1.0) {
        return ImVec4(0.40f, 0.85f, 0.45f, 1.0f);
    }
    if (percent < 5.0) {
        return ImVec4(0.95f, 0.80f, 0.25f, 1.0f);
    }
    return ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
}

void LatencyLine(const char *label, double last_ms, double avg_ms, double p95_ms, double p99_ms) {
    ImGui::Text("%s", label);
    ImGui::SameLine(160.0f);
    ImGui::TextColored(LatencyColor(last_ms), "%.1f", last_ms);
    ImGui::SameLine(220.0f);
    ImGui::TextColored(LatencyColor(avg_ms), "%.1f", avg_ms);
    ImGui::SameLine(280.0f);
    ImGui::TextColored(LatencyColor(p95_ms), "%.1f", p95_ms);
    ImGui::SameLine(340.0f);
    ImGui::TextColored(LatencyColor(p99_ms), "%.1f", p99_ms);
}

void LatencyLine3(const char *label, double avg_ms, double p95_ms, double max_ms) {
    ImGui::Text("%s", label);
    ImGui::SameLine(160.0f);
    ImGui::TextColored(LatencyColor(avg_ms), "%.1f", avg_ms);
    ImGui::SameLine(220.0f);
    ImGui::TextColored(LatencyColor(p95_ms), "%.1f", p95_ms);
    ImGui::SameLine(280.0f);
    ImGui::TextColored(LatencyColor(max_ms), "%.1f", max_ms);
}

void DrawLatencyPlot(const char *label, LatencyHistory &history, float max_range) {
    if (history.count < 2) {
        return;
    }
    // ImGui::PlotLines 直接读取环形数组，并从 index 指示的位置继续显示最新值。
    ImGui::PlotLines(label, history.values, static_cast<int>(history.count),
                     static_cast<int>(history.index), nullptr, 0.0f, max_range,
                     ImVec2(0, 40));
}

void RenderHeaderRow() {
    ImGui::Text("         ");
    ImGui::SameLine(160.0f);
    ImGui::Text("last");
    ImGui::SameLine(220.0f);
    ImGui::Text("avg");
    ImGui::SameLine(280.0f);
    ImGui::Text("p95");
    ImGui::SameLine(340.0f);
    ImGui::Text("p99");
}

void RenderHeaderRow3() {
    ImGui::Text("         ");
    ImGui::SameLine(160.0f);
    ImGui::Text("avg");
    ImGui::SameLine(220.0f);
    ImGui::Text("p95");
    ImGui::SameLine(280.0f);
    ImGui::Text("max");
}

}  // namespace

void RenderOpenGlHudPanel(const RenderFrameInfo &frame_info, double fps, bool *hud_visible) {
    if (hud_visible == nullptr || !(*hud_visible)) {
        return;
    }

    // HUD 固定在窗口左上角，不参与视频四边形的缩放。
    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Always);

    const ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("PlaybackHud", hud_visible, window_flags)) {
        ImGui::End();
        return;
    }

    ImGui::Text("sclient player");
    ImGui::Separator();

    // 连接状态和当前视频基本信息。
    if (!frame_info.connected) {
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "DISCONNECTED");
    } else if (frame_info.waiting_for_first_frame) {
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.25f, 1.0f), "Waiting for video stream...");
    } else {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "Connected");
    }

    ImGui::Text("Transport: %s", frame_info.transport.c_str());
    ImGui::Text("Resolution: %d x %d", frame_info.frame_width, frame_info.frame_height);
    ImGui::TextColored(ImVec4(0.5f, 0.9f, 1.0f, 1.0f), "FPS: %.1f", fps);

    // 接收队列/解码队列深度能帮助判断是网络还是解码环节积压。
    ImGui::Text("Queue recv: %zu  decode: %zu", frame_info.receive_queue_depth, frame_info.decode_queue_depth);

    // UDP 分片丢包率只在 UDP 且存在丢包时显示。
    if (frame_info.transport == "udp" && frame_info.fragment_loss_percent > 0.0) {
        ImGui::Text("Fragment loss: ");
        ImGui::SameLine();
        ImGui::TextColored(LossColor(frame_info.fragment_loss_percent), "%.2f%%", frame_info.fragment_loss_percent);
    }

    // 端到端延迟需要发送端元数据；没有有效时间戳时不显示误导性数值。
    if (frame_info.metadata_expected) {
        ImGui::Separator();
        ImGui::Text("Sender metadata: %s", frame_info.sender_metadata_available ? "available" : "unavailable");
        if (frame_info.sender_metadata_available && HasSamples(frame_info.capture_to_render)) {
            if (ImGui::CollapsingHeader("End-to-End Latency", ImGuiTreeNodeFlags_DefaultOpen)) {
                RenderHeaderRow();
                LatencyLine("capture->render",
                            frame_info.capture_to_render.last_ms,
                            frame_info.capture_to_render.avg_ms,
                            frame_info.capture_to_render.p95_ms,
                            frame_info.capture_to_render.p99_ms);
                LatencyLine("network->recv",
                            frame_info.network_to_receive.last_ms,
                            frame_info.network_to_receive.avg_ms,
                            frame_info.network_to_receive.p95_ms,
                            frame_info.network_to_receive.p99_ms);
                g_capture_to_render_history.Push(static_cast<float>(frame_info.capture_to_render.last_ms));
                DrawLatencyPlot("e2e##plot", g_capture_to_render_history, 100.0f);
            }
        }
    }

    // 本地链路延迟不依赖发送端时间戳，包括接收、解码和渲染各阶段。
    if (HasSamples(frame_info.receive_to_render)) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Local Latency", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderHeaderRow();
            LatencyLine("recv->render",
                        frame_info.receive_to_render.last_ms,
                        frame_info.receive_to_render.avg_ms,
                        frame_info.receive_to_render.p95_ms,
                        frame_info.receive_to_render.p99_ms);
            g_receive_to_render_history.Push(static_cast<float>(frame_info.receive_to_render.last_ms));
            DrawLatencyPlot("local##plot", g_receive_to_render_history, 50.0f);

            ImGui::Spacing();
            RenderHeaderRow3();
            LatencyLine3("recv->decode",
                         frame_info.receive_to_decode.avg_ms,
                         frame_info.receive_to_decode.p95_ms,
                         frame_info.receive_to_decode.max_ms);
            LatencyLine3("decode time",
                         frame_info.decode_time.avg_ms,
                         frame_info.decode_time.p95_ms,
                         frame_info.decode_time.max_ms);
            g_decode_time_history.Push(static_cast<float>(frame_info.decode_time.last_ms));
            LatencyLine3("decode->render",
                         frame_info.decode_to_render.avg_ms,
                         frame_info.decode_to_render.p95_ms,
                         frame_info.decode_to_render.max_ms);
        }
    }

    // UDP jitter buffer 和 NACK/FEC 运行状态。
    if (frame_info.transport == "udp" && frame_info.udp_jitter_buffer_enabled) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Jitter Buffer")) {
            ImGui::Text("Strategy: %s", frame_info.udp_jitter_buffer_strategy.c_str());
            if (!frame_info.udp_jitter_buffer_active_mode.empty()) {
                ImVec4 mode_color = ImVec4(0.40f, 0.85f, 0.45f, 1.0f);
                if (frame_info.udp_jitter_buffer_active_mode == "low_latency") {
                    mode_color = ImVec4(0.95f, 0.80f, 0.25f, 1.0f);
                } else if (frame_info.udp_jitter_buffer_active_mode == "smooth") {
                    mode_color = ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
                }
                ImGui::SameLine();
                ImGui::TextColored(mode_color, "[%s]", frame_info.udp_jitter_buffer_active_mode.c_str());
            }
            if (!frame_info.udp_jitter_buffer_quality.empty()) {
                ImVec4 q_color = ImVec4(0.40f, 0.85f, 0.45f, 1.0f);
                if (frame_info.udp_jitter_buffer_quality == "fair") {
                    q_color = ImVec4(0.95f, 0.80f, 0.25f, 1.0f);
                } else if (frame_info.udp_jitter_buffer_quality == "poor") {
                    q_color = ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
                } else if (frame_info.udp_jitter_buffer_quality == "good") {
                    q_color = ImVec4(0.70f, 0.85f, 0.40f, 1.0f);
                }
                ImGui::SameLine();
                ImGui::TextColored(q_color, "(%s)", frame_info.udp_jitter_buffer_quality.c_str());
            }
            ImGui::Text("Depth: %zu / %zu", frame_info.udp_jitter_buffer_current_depth, frame_info.udp_jitter_buffer_max_depth);
            ImGui::Text("Target: %.1fms", frame_info.udp_jitter_buffer_target_delay_ms);
            if (frame_info.udp_jitter_p50_ms > 0.0 || frame_info.udp_jitter_p95_ms > 0.0) {
                ImGui::Text("Jitter p50: %.2fms  p95: %.2fms", frame_info.udp_jitter_p50_ms, frame_info.udp_jitter_p95_ms);
            }
            ImGui::Text("Wait avg: %.1fms  max: %.1fms",
                        frame_info.udp_jitter_buffer_wait_avg_ms,
                        frame_info.udp_jitter_buffer_wait_max_ms);
            ImGui::Text("Skipped: %llu  Dropped: %llu",
                        static_cast<unsigned long long>(frame_info.udp_jitter_buffer_skipped_frames),
                        static_cast<unsigned long long>(frame_info.udp_jitter_buffer_dropped_frames));
            if (frame_info.udp_nack_enabled || frame_info.udp_fec_enabled) {
                ImGui::Text("NACK: %s",
                            frame_info.udp_nack_enabled ? "on" : "off");
                if (frame_info.udp_nack_enabled && frame_info.udp_nack_requests_sent > 0) {
                    ImGui::SameLine();
                    ImGui::Text("(%llu req)",
                                static_cast<unsigned long long>(frame_info.udp_nack_requests_sent));
                }
                ImGui::Text("FEC: %s",
                            frame_info.udp_fec_enabled ? "on" : "off");
                if (frame_info.udp_fec_enabled && frame_info.udp_fec_recovered_frames > 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f), "(%llu recovered)",
                                static_cast<unsigned long long>(frame_info.udp_fec_recovered_frames));
                }
            }
        }
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Q/Esc:quit  H:HUD  Space:pause");
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "F:fullscreen  S:screenshot  R:reset");

    ImGui::End();
}

}  // namespace sclient
