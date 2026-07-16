#include "common/control/control_service.hpp"

#include "common/logging/logging.hpp"
#include "common/protocol/messages.hpp"

#include <limits>

namespace vkr::control {
namespace {
constexpr char kComponent[] = "control";
}

std::string serve_session(transport::MessageChannel& channel, protocol::IdAllocator& ids,
                          const std::vector<protocol::GpuDevice>& devices,
                          SessionLauncher* launcher, const std::string& worker_backend,
                          const protocol::HostDisplay& host_display,
                          DisplaySnapshotProvider* display_snapshots, bool devices_real) {
    display::DisplayLayoutDecodeResult advertised_layout;
    if (display_snapshots != nullptr) {
        advertised_layout = display_snapshots->capture_for_handshake();
    }
    protocol::HostDisplay effective_host_display = host_display;
    if (advertised_layout.status == display::LayoutDecodeStatus::Valid) {
        // The additive legacy fields must be projections of the SAME freshly captured snapshot,
        // never an independent SPI_GETWORKAREA geometry authority. New clients use virtual_bounds;
        // host_dpi remains the single-canvas guest font-DPI hint and therefore follows primary.
        effective_host_display.work_w = advertised_layout.layout.virtual_bounds.width;
        effective_host_display.work_h = advertised_layout.layout.virtual_bounds.height;
        const display::MonitorDesc* primary = display::find_monitor_by_id(
            advertised_layout.layout, advertised_layout.layout.primary_monitor_id);
        if (primary != nullptr &&
            primary->dpi_x <= static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            effective_host_display.dpi = static_cast<int>(primary->dpi_x);
        }
    }
    const transport::AcceptedSession session = transport::server_handshake(
        channel, ids, worker_backend, effective_host_display, advertised_layout);
    VKR_INFO(kComponent) << "session established"
                         << "; supervisor_session=" << session.server.supervisor_session_id
                         << " worker=" << session.server.worker_id
                         << " gpu=" << session.client.gpu_selector
                         << " display=" << session.client.display_backend;

    for (;;) {
        protocol::MessageType type = protocol::MessageType::Unknown;
        json::Value body;
        if (!channel.recv(type, body)) {
            VKR_INFO(kComponent) << "session closed; worker=" << session.server.worker_id;
            break;
        }

        switch (type) {
        case protocol::MessageType::GpuListRequest:
            channel.send(protocol::MessageType::GpuListResponse,
                         protocol::gpu_list_to_body(devices, devices_real));
            break;
        case protocol::MessageType::GpuSelectRequest: {
            const protocol::GpuSelectRequest req = protocol::GpuSelectRequest::from_body(body);
            protocol::GpuSelectResponse resp;
            const auto parsed = protocol::parse_selector(req.selector);
            if (!parsed.ok) {
                resp.ok = false;
                resp.reason = parsed.error;
            } else {
                std::string reason;
                const protocol::GpuDevice* chosen =
                    protocol::select_device(devices, parsed.selector, reason);
                resp.ok = (chosen != nullptr);
                resp.reason = reason;
                if (chosen != nullptr) {
                    resp.device = *chosen;
                }
            }
            channel.send(protocol::MessageType::GpuSelectResponse, resp.to_body());
            break;
        }
        case protocol::MessageType::Heartbeat: {
            const protocol::Heartbeat hb = protocol::Heartbeat::from_body(body);
            VKR_DEBUG(kComponent) << "heartbeat worker=" << hb.worker_id << " seq=" << hb.seq;
            break;
        }
        case protocol::MessageType::LaunchSession: {
            const protocol::LaunchSession req = protocol::LaunchSession::from_body(body);
            if (launcher == nullptr) {
                protocol::ErrorMsg err{"launch_unsupported",
                                       "this endpoint cannot launch worker sessions"};
                channel.send(protocol::MessageType::Error, err.to_body());
                break;
            }
            // The handshake identity is canonical. A launch_session that names a
            // different app_instance_id is rejected; an empty one inherits the
            // handshake identity. The launched session is bound to it.
            if (!req.app_instance_id.empty() &&
                req.app_instance_id != session.client.app_instance_id) {
                protocol::ErrorMsg err{
                    "app_identity_mismatch",
                    "launch_session app_instance_id does not match the handshake identity"};
                channel.send(protocol::MessageType::Error, err.to_body());
                break;
            }
            display::DisplayLayout pinned_layout;
            const display::DisplayLayout* pinned_layout_ptr = nullptr;
            if (display_snapshots != nullptr) {
                if (req.display_snapshot_id.empty()) {
                    protocol::ErrorMsg err{
                        "display_snapshot_required",
                        "launch_session must name the snapshot returned by the host-display query"};
                    channel.send(protocol::MessageType::Error, err.to_body());
                    break;
                }
                std::string reason;
                if (!display_snapshots->resolve_copy(req.display_snapshot_id, pinned_layout,
                                                     reason)) {
                    protocol::ErrorMsg err{"display_snapshot_not_found", reason};
                    channel.send(protocol::MessageType::Error, err.to_body());
                    break;
                }
                pinned_layout_ptr = &pinned_layout;
            }
            try {
                const SessionInfo info = launcher->launch_session(
                    session.client.app_instance_id, req.gpu_selector, req.display_backend,
                    req.graphics_frontend, req.profile_enabled, req.op_trace_enabled,
                    req.input_trace_enabled, pinned_layout_ptr);
                protocol::SessionStarted started;
                started.worker_id = info.worker_id;
                started.data_plane_host = info.data_plane_host;
                started.data_plane_port = info.data_plane_port;
                started.app_token = info.app_token;
                started.sidecar_plane_host = info.sidecar_plane_host;
                started.sidecar_plane_port = info.sidecar_plane_port;
                started.sidecar_token = info.sidecar_token;
                started.op_trace_path = info.op_trace_path;
                started.display_snapshot_id = info.display_snapshot_id;
                channel.send(protocol::MessageType::SessionStarted, started.to_body());
                VKR_INFO(kComponent)
                    << "launched session worker=" << info.worker_id
                    << " data_plane=" << info.data_plane_host << ":" << info.data_plane_port;
            } catch (const std::exception& e) {
                protocol::ErrorMsg err{"launch_failed", e.what()};
                channel.send(protocol::MessageType::Error, err.to_body());
            }
            break;
        }
        case protocol::MessageType::CloseSession: {
            const protocol::CloseSession req = protocol::CloseSession::from_body(body);
            protocol::SessionClosed closed;
            closed.worker_id = req.worker_id;
            if (launcher == nullptr) {
                closed.reason = "this endpoint cannot close worker sessions";
            } else if (req.worker_id.empty() || req.app_token.empty()) {
                closed.reason = "close_session requires worker_id and app_token";
            } else {
                closed.accepted = launcher->close_session(
                    session.client.app_instance_id, req.worker_id, req.app_token, closed.reason);
            }
            channel.send(protocol::MessageType::SessionClosed, closed.to_body());
            VKR_INFO(kComponent) << "close session worker=" << req.worker_id
                                 << " accepted=" << (closed.accepted ? 1 : 0)
                                 << " reason=" << closed.reason;
            break;
        }
        default: {
            protocol::ErrorMsg err{"unsupported_request", std::string("unsupported message: ") +
                                                              protocol::to_string(type)};
            channel.send(protocol::MessageType::Error, err.to_body());
            break;
        }
        }
    }
    return session.server.worker_id;
}

} // namespace vkr::control
