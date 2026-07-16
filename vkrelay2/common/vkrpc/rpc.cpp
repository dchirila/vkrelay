#include "common/vkrpc/rpc.hpp"

#include "common/protocol/wire.hpp"

namespace vkr::vkrpc {

const char* to_string(RpcStatus status) {
    switch (status) {
    case RpcStatus::Ok:
        return "ok";
    case RpcStatus::UnknownOp:
        return "unknown_op";
    case RpcStatus::BadRequest:
        return "bad_request";
    case RpcStatus::Internal:
        return "internal";
    }
    return "unknown";
}

std::string encode_rpc(const RpcMessage& msg) {
    std::string out(kRpcHeaderBytes, '\0');
    auto* p = reinterpret_cast<unsigned char*>(&out[0]);
    protocol::store_le32(msg.op, p + 0);
    protocol::store_le32(msg.request_id, p + 4);
    protocol::store_le32(msg.status, p + 8);
    out.append(msg.body);
    return out;
}

bool decode_rpc(const std::string& payload, RpcMessage& msg, std::string& error) {
    if (payload.size() < kRpcHeaderBytes) {
        error = "rpc payload shorter than header";
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(payload.data());
    msg.op = protocol::load_le32(p + 0);
    msg.request_id = protocol::load_le32(p + 4);
    msg.status = protocol::load_le32(p + 8);
    msg.body.assign(payload, kRpcHeaderBytes, std::string::npos);
    return true;
}

void RpcChannel::send(const RpcMessage& msg) {
    const std::string frame = protocol::encode_frame(encode_rpc(msg));
    conn_.write_all(frame.data(), frame.size());
}

bool RpcChannel::recv(RpcMessage& msg) {
    for (;;) {
        std::size_t consumed = 0;
        std::string payload;
        std::string error;
        const protocol::FrameStatus status =
            protocol::try_decode_frame(buffer_, consumed, payload, error);
        if (status == protocol::FrameStatus::Ok) {
            buffer_.erase(0, consumed);
            if (!decode_rpc(payload, msg, error)) {
                throw transport::TransportError("malformed rpc: " + error);
            }
            return true;
        }
        if (status == protocol::FrameStatus::Error) {
            throw transport::TransportError("rpc framing error: " + error);
        }
        // NeedMore: pull more bytes.
        char chunk[4096];
        const std::size_t n = conn_.read_some(chunk, sizeof(chunk));
        if (n == 0) {
            if (buffer_.empty()) {
                return false; // clean EOF on a frame boundary
            }
            throw transport::TransportError("connection closed mid-rpc-frame");
        }
        buffer_.append(chunk, n);
    }
}

} // namespace vkr::vkrpc
