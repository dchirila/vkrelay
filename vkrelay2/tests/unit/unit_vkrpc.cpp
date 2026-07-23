// Unit tests for the Vulkan RPC envelope + capability negotiation. Pure, no network: encode/decode
// totality, JSON body round-trips,
// and the mock backend's negotiation logic.
#include "common/protocol/gpu.hpp"
#include "common/protocol/wire.hpp"
#include "common/vkrpc/device_loss_policy.h"
#include "common/vkrpc/indirect_draw_validation.h"
#include "common/vkrpc/rpc.hpp"
#include "common/vkrpc/rpc_profile.h"
#include "common/vkrpc/vulkan_session.hpp"
#include "tests/test_assert.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace vkr;

namespace {

class NoIoConnection final : public transport::Connection {
  public:
    std::size_t read_some(void*, std::size_t) override {
        throw transport::TransportError("unexpected read");
    }
    void write_all(const void*, std::size_t) override {
        ++writes;
        throw transport::TransportError("unexpected write");
    }
    void close() override {}
    void cancel() override {}

    int writes = 0;
};

void test_device_loss_policy() {
    VKR_CHECK(!vkrpc::present_fence_retire_requested(nullptr));
    VKR_CHECK(!vkrpc::present_fence_retire_requested(""));
    VKR_CHECK(!vkrpc::present_fence_retire_requested("0"));
    VKR_CHECK(!vkrpc::present_fence_retire_requested("true"));
    VKR_CHECK(!vkrpc::present_fence_retire_requested("01"));
    VKR_CHECK(vkrpc::present_fence_retire_requested("1"));
}

void test_rpc_roundtrip() {
    vkrpc::RpcMessage m;
    m.op = 7;
    m.request_id = 0xDEADBEEF;
    m.status = 2;
    m.body = std::string("he\0llo", 6); // embedded NUL must survive

    const std::string wire = vkrpc::encode_rpc(m);
    VKR_CHECK_EQ(wire.size(), vkrpc::kRpcHeaderBytes + m.body.size());

    vkrpc::RpcMessage out;
    std::string err;
    VKR_CHECK(vkrpc::decode_rpc(wire, out, err));
    VKR_CHECK_EQ(out.op, m.op);
    VKR_CHECK_EQ(out.request_id, m.request_id);
    VKR_CHECK_EQ(out.status, m.status);
    VKR_CHECK(out.body == m.body);
}

void test_decode_totality() {
    vkrpc::RpcMessage out;
    std::string err;
    // Anything shorter than the header is rejected without UB.
    for (std::size_t n = 0; n < vkrpc::kRpcHeaderBytes; ++n) {
        VKR_CHECK(!vkrpc::decode_rpc(std::string(n, '\0'), out, err));
    }
    // Exactly the header => empty body, ok.
    VKR_CHECK(vkrpc::decode_rpc(std::string(vkrpc::kRpcHeaderBytes, '\0'), out, err));
    VKR_CHECK(out.body.empty());
}

void test_body_roundtrip() {
    vkrpc::CapabilitiesRequest req;
    req.requested_api_major = 1;
    req.requested_api_minor = 2;
    const vkrpc::CapabilitiesRequest req2 = vkrpc::CapabilitiesRequest::from_body(req.to_body());
    VKR_CHECK_EQ(req2.requested_api_major, 1);
    VKR_CHECK_EQ(req2.requested_api_minor, 2);

    vkrpc::CapabilitiesResponse resp;
    resp.ok = true;
    resp.reason = "ok";
    resp.negotiated_api_major = 1;
    resp.negotiated_api_minor = 3;
    resp.device.device_name = "Test GPU";
    resp.device.vendor_id = 0x10DE;
    resp.device.device_id = 0x1234;
    resp.device.device_type = "discrete";
    const vkrpc::CapabilitiesResponse r2 = vkrpc::CapabilitiesResponse::from_body(resp.to_body());
    VKR_CHECK(r2.ok);
    VKR_CHECK_EQ(r2.negotiated_api_major, 1);
    VKR_CHECK_EQ(r2.negotiated_api_minor, 3);
    VKR_CHECK_EQ(r2.device.device_name, std::string("Test GPU"));
    VKR_CHECK_EQ(r2.device.vendor_id, static_cast<std::uint32_t>(0x10DE));
    VKR_CHECK_EQ(r2.device.device_id, static_cast<std::uint32_t>(0x1234));
    VKR_CHECK_EQ(r2.device.device_type, std::string("discrete"));
}

void test_negotiation() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    VKR_CHECK(!devices.empty());
    const std::string known = devices.front().name;

    vkrpc::MockVulkanBackend backend(known);

    // Requesting the worker-supported level returns it exactly, with the device.
    vkrpc::CapabilitiesRequest at_max;
    at_max.requested_api_major = vkrpc::kSupportedApiMajor;
    at_max.requested_api_minor = vkrpc::kSupportedApiMinor;
    const vkrpc::CapabilitiesResponse r_max = backend.negotiate(at_max);
    VKR_CHECK(r_max.ok);
    VKR_CHECK_EQ(r_max.negotiated_api_major, vkrpc::kSupportedApiMajor);
    VKR_CHECK_EQ(r_max.negotiated_api_minor, vkrpc::kSupportedApiMinor);
    VKR_CHECK_EQ(r_max.device.device_name, known);

    // Requesting above the supported level caps to it.
    vkrpc::CapabilitiesRequest too_high;
    too_high.requested_api_major = vkrpc::kSupportedApiMajor;
    too_high.requested_api_minor = vkrpc::kSupportedApiMinor + 1;
    const vkrpc::CapabilitiesResponse r_high = backend.negotiate(too_high);
    VKR_CHECK(r_high.ok);
    VKR_CHECK_EQ(r_high.negotiated_api_minor, vkrpc::kSupportedApiMinor);

    // Requesting below the supported level keeps the lower request.
    vkrpc::CapabilitiesRequest lower;
    lower.requested_api_major = 1;
    lower.requested_api_minor = 1;
    const vkrpc::CapabilitiesResponse r_low = backend.negotiate(lower);
    VKR_CHECK(r_low.ok);
    VKR_CHECK_EQ(r_low.negotiated_api_major, 1);
    VKR_CHECK_EQ(r_low.negotiated_api_minor, 1);

    // Below the Vulkan 1.0 floor is rejected.
    vkrpc::CapabilitiesRequest too_low;
    too_low.requested_api_major = 0;
    too_low.requested_api_minor = 9;
    VKR_CHECK(!backend.negotiate(too_low).ok);

    // A negative minor version is invalid and rejected.
    vkrpc::CapabilitiesRequest neg_minor;
    neg_minor.requested_api_major = 1;
    neg_minor.requested_api_minor = -1;
    VKR_CHECK(!backend.negotiate(neg_minor).ok);

    // An unknown device name yields no usable device.
    vkrpc::MockVulkanBackend missing("no-such-adapter-xyz");
    VKR_CHECK(!missing.negotiate(at_max).ok);
}

void test_lifecycle() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    VKR_CHECK(!devices.empty());
    const std::string known = devices.front().name;
    vkrpc::MockVulkanBackend backend(known);

    // create_instance -> a handle.
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    VKR_CHECK(ci.ok);
    VKR_CHECK(ci.instance != 0);

    // create_device before enumerate is rejected even if the client guesses the
    // physical-device handle value (the handle is only minted by enumeration).
    vkrpc::CreateDeviceRequest pre;
    pre.instance = ci.instance;
    pre.physical_device = ci.instance + 1;
    VKR_CHECK(!backend.create_device(pre).ok);

    // enumerate -> exactly the selected device, with a stable handle.
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    VKR_CHECK(en.ok);
    VKR_CHECK_EQ(en.devices.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(en.devices.front().caps.device_name, known);
    const std::uint64_t phys = en.devices.front().handle;
    VKR_CHECK(phys != 0);

    // Enumeration is stable: a second call returns the same physical handle.
    VKR_CHECK_EQ(backend.enumerate_physical_devices(er).devices.front().handle, phys);

    // enumerate on an unknown instance is rejected.
    vkrpc::EnumeratePhysicalDevicesRequest bad_er;
    bad_er.instance = ci.instance + 99999;
    VKR_CHECK(!backend.enumerate_physical_devices(bad_er).ok);

    // create_device on the enumerated (selected) physical device succeeds.
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    VKR_CHECK(cd.device != 0);

    // Selection is enforced: a physical device not enumerated from this instance
    // is rejected, as is an unknown instance.
    vkrpc::CreateDeviceRequest wrong_phys;
    wrong_phys.instance = ci.instance;
    wrong_phys.physical_device = phys + 12345;
    VKR_CHECK(!backend.create_device(wrong_phys).ok);
    vkrpc::CreateDeviceRequest wrong_inst;
    wrong_inst.instance = ci.instance + 7;
    wrong_inst.physical_device = phys;
    VKR_CHECK(!backend.create_device(wrong_inst).ok);

    // Ordering: an instance with a live device cannot be destroyed.
    vkrpc::HandleRequest di;
    di.handle = ci.instance;
    VKR_CHECK(!backend.destroy_instance(di).ok);

    // Destroy the device (twice -> second is a no-op error), then the instance.
    vkrpc::HandleRequest dd;
    dd.handle = cd.device;
    VKR_CHECK(backend.destroy_device(dd).ok);
    VKR_CHECK(!backend.destroy_device(dd).ok);
    VKR_CHECK(backend.destroy_instance(di).ok);
    VKR_CHECK(!backend.destroy_instance(di).ok);
}

void test_lifecycle_body_roundtrip() {
    vkrpc::CreateInstanceResponse ci;
    ci.ok = true;
    ci.reason = "ok";
    ci.instance = 0x1122334455667788ull;
    const vkrpc::CreateInstanceResponse ci2 =
        vkrpc::CreateInstanceResponse::from_body(ci.to_body());
    VKR_CHECK(ci2.ok);
    VKR_CHECK_EQ(ci2.instance, ci.instance);

    vkrpc::EnumeratePhysicalDevicesResponse en;
    en.ok = true;
    vkrpc::PhysicalDeviceEntry e;
    e.handle = 42;
    e.caps.device_name = "GPU";
    e.caps.vendor_id = 0x10DE;
    // the host device-extension list rides the entry (the ICD intersects its allowlist
    // with it); absent in an OLD body -> empty = the policy-only fallback.
    e.device_extensions = {"VK_KHR_swapchain", "VK_EXT_transform_feedback"};
    en.devices.push_back(e);
    const vkrpc::EnumeratePhysicalDevicesResponse en2 =
        vkrpc::EnumeratePhysicalDevicesResponse::from_body(en.to_body());
    VKR_CHECK(en2.ok);
    VKR_CHECK_EQ(en2.devices.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(en2.devices.front().handle, static_cast<std::uint64_t>(42));
    VKR_CHECK_EQ(en2.devices.front().caps.device_name, std::string("GPU"));
    VKR_CHECK_EQ(en2.devices.front().device_extensions.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(en2.devices.front().device_extensions[1],
                 std::string("VK_EXT_transform_feedback"));
    {
        json::Value old_body = e.to_body();
        old_body.set("device_extensions", json::Value(0)); // not an array -> decodes to empty
        VKR_CHECK(vkrpc::PhysicalDeviceEntry::from_body(old_body).device_extensions.empty());
    }
}

void test_handle_parsing() {
    // Handles are unsigned decimal strings; signed/whitespace/non-digit/overflow
    // forms map to 0 (null), and valid values round-trip (incl. the full u64 max).
    auto parse = [](const std::string& s) -> std::uint64_t {
        json::Value b = json::Value::make_object();
        b.set("handle", json::Value(s));
        return vkrpc::HandleRequest::from_body(b).handle;
    };
    VKR_CHECK_EQ(parse("0"), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse("42"), static_cast<std::uint64_t>(42));
    VKR_CHECK_EQ(parse("18446744073709551615"),
                 static_cast<std::uint64_t>(18446744073709551615ull));
    VKR_CHECK_EQ(parse("-1"), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse(" 1"), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse("1 "), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse("+1"), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse("0x10"), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse("abc"), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse(""), static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(parse("18446744073709551616"), static_cast<std::uint64_t>(0)); // overflow
}

// Brings a backend to a live device, returning the full create_device response
// (device == 0 on failure) so callers can read the reported queue family/count.
vkrpc::CreateDeviceResponse make_device_resp(vkrpc::MockVulkanBackend& backend) {
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    if (!en.ok || en.devices.empty()) {
        return {};
    }
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    return backend.create_device(cdr);
}

// Convenience: just the device handle (0 on failure).
std::uint64_t make_device(vkrpc::MockVulkanBackend& backend) {
    return make_device_resp(backend).device;
}

void test_command_objects() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateDeviceResponse cd = make_device_resp(backend);
    const std::uint64_t device = cd.device;
    VKR_CHECK(device != 0);

    // create_device reports the queue family/count it created so the app knows
    // which (family, index) get_device_queue may be called with.
    VKR_CHECK(cd.queue_family_index >= 0);
    VKR_CHECK(cd.queue_count >= 1);

    // get_device_queue: the reported (family, index) is retrievable and stable;
    // a wrong family or out-of-range index is rejected (only what create_device
    // created is valid -- the real driver makes any other family/index UB); an
    // unknown device is rejected.
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q0 = backend.get_device_queue(gq);
    VKR_CHECK(q0.ok);
    VKR_CHECK(q0.queue != 0);
    VKR_CHECK_EQ(backend.get_device_queue(gq).queue, q0.queue);
    vkrpc::GetDeviceQueueRequest gq_badfamily = gq;
    gq_badfamily.queue_family_index = cd.queue_family_index + 1000;
    VKR_CHECK(!backend.get_device_queue(gq_badfamily).ok);
    vkrpc::GetDeviceQueueRequest gq_badindex = gq;
    gq_badindex.queue_index = cd.queue_count;
    VKR_CHECK(!backend.get_device_queue(gq_badindex).ok);
    vkrpc::GetDeviceQueueRequest gq_bad;
    gq_bad.device = device + 999;
    VKR_CHECK(!backend.get_device_queue(gq_bad).ok);

    // command pool: device child; targets the family create_device reported; an
    // unknown device and a wrong family are rejected.
    vkrpc::CreateCommandPoolRequest cp;
    cp.device = device;
    cp.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cp);
    VKR_CHECK(pool.ok);
    VKR_CHECK(pool.command_pool != 0);
    vkrpc::CreateCommandPoolRequest cp_bad;
    cp_bad.device = device + 999;
    VKR_CHECK(!backend.create_command_pool(cp_bad).ok);
    vkrpc::CreateCommandPoolRequest cp_badfamily;
    cp_badfamily.device = device;
    cp_badfamily.queue_family_index = cd.queue_family_index + 1000;
    VKR_CHECK(!backend.create_command_pool(cp_badfamily).ok);

    // allocate command buffers: count must be positive; unknown pool rejected.
    vkrpc::AllocateCommandBuffersRequest ab;
    ab.command_pool = pool.command_pool;
    ab.count = 3;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(ab);
    VKR_CHECK(bufs.ok);
    VKR_CHECK_EQ(bufs.command_buffers.size(), static_cast<std::size_t>(3));
    vkrpc::AllocateCommandBuffersRequest ab0 = ab;
    ab0.count = 0;
    VKR_CHECK(!backend.allocate_command_buffers(ab0).ok);
    vkrpc::AllocateCommandBuffersRequest ab_bad;
    ab_bad.command_pool = pool.command_pool + 999;
    ab_bad.count = 1;
    VKR_CHECK(!backend.allocate_command_buffers(ab_bad).ok);

    // A second pool, to prove a buffer can't be freed via the wrong pool.
    const vkrpc::CreateCommandPoolResponse pool2 = backend.create_command_pool(cp);
    vkrpc::AllocateCommandBuffersRequest ab2;
    ab2.command_pool = pool2.command_pool;
    ab2.count = 1;
    const std::uint64_t other = backend.allocate_command_buffers(ab2).command_buffers.front();

    // free a subset; double-free rejected; cross-pool free rejected.
    vkrpc::FreeCommandBuffersRequest fb;
    fb.command_pool = pool.command_pool;
    fb.command_buffers = {bufs.command_buffers[0]};
    VKR_CHECK(backend.free_command_buffers(fb).ok);
    VKR_CHECK(!backend.free_command_buffers(fb).ok);
    vkrpc::FreeCommandBuffersRequest f_cross;
    f_cross.command_pool = pool.command_pool;
    f_cross.command_buffers = {other};
    VKR_CHECK(!backend.free_command_buffers(f_cross).ok);

    // A count outside the cap is rejected at full width -- a value that would wrap
    // into [1,4096] if narrowed to int must still be rejected.
    vkrpc::AllocateCommandBuffersRequest ab_huge;
    ab_huge.command_pool = pool.command_pool;
    ab_huge.count = (1LL << 32) + 1; // narrows to 1; must NOT allocate
    VKR_CHECK(!backend.allocate_command_buffers(ab_huge).ok);

    // A batch repeating a live handle is rejected atomically (nothing freed), so a
    // subsequent single free of that handle still succeeds.
    vkrpc::FreeCommandBuffersRequest f_dup;
    f_dup.command_pool = pool.command_pool;
    f_dup.command_buffers = {bufs.command_buffers[1], bufs.command_buffers[1]};
    VKR_CHECK(!backend.free_command_buffers(f_dup).ok);
    vkrpc::FreeCommandBuffersRequest f_one;
    f_one.command_pool = pool.command_pool;
    f_one.command_buffers = {bufs.command_buffers[1]};
    VKR_CHECK(backend.free_command_buffers(f_one).ok);

    // An empty free list is rejected (Vulkan requires count > 0; also closes the
    // malformed-request no-op hole).
    vkrpc::FreeCommandBuffersRequest f_empty;
    f_empty.command_pool = pool.command_pool;
    VKR_CHECK(!backend.free_command_buffers(f_empty).ok);

    // A device with live pools cannot be destroyed.
    vkrpc::HandleRequest dd;
    dd.handle = device;
    VKR_CHECK(!backend.destroy_device(dd).ok);

    // Destroy both pools (the first cascades its 1 remaining buffer), then the
    // device. A second pool destroy is a no-op error.
    vkrpc::HandleRequest dp;
    dp.handle = pool.command_pool;
    VKR_CHECK(backend.destroy_command_pool(dp).ok);
    VKR_CHECK(!backend.destroy_command_pool(dp).ok);
    vkrpc::HandleRequest dp2;
    dp2.handle = pool2.command_pool;
    VKR_CHECK(backend.destroy_command_pool(dp2).ok);
    VKR_CHECK(backend.destroy_device(dd).ok);
}

void test_handle_array_decode() {
    // A handle array decodes one entry per element, preserving length: a malformed
    // element (here a JSON number, not a decimal string) becomes handle 0 rather
    // than being dropped, so a malformed request fails semantically instead of
    // silently shrinking to an empty, "successful" no-op.
    json::Value b = json::Value::make_object();
    b.set("command_pool", json::Value(std::string("1")));
    json::Array arr;
    arr.emplace_back(json::Value(123LL)); // number, not a decimal string
    arr.emplace_back(json::Value(std::string("7")));
    b.set("command_buffers", json::Value(std::move(arr)));

    const vkrpc::FreeCommandBuffersRequest req = vkrpc::FreeCommandBuffersRequest::from_body(b);
    VKR_CHECK_EQ(req.command_pool, static_cast<std::uint64_t>(1));
    VKR_CHECK_EQ(req.command_buffers.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(req.command_buffers[0], static_cast<std::uint64_t>(0)); // malformed -> 0
    VKR_CHECK_EQ(req.command_buffers[1], static_cast<std::uint64_t>(7));

    // A missing or wrong-typed command_buffers field decodes to an empty vector
    // (the backend then rejects an empty free as a malformed no-op).
    json::Value missing = json::Value::make_object();
    missing.set("command_pool", json::Value(std::string("1")));
    VKR_CHECK(vkrpc::FreeCommandBuffersRequest::from_body(missing).command_buffers.empty());
    json::Value wrong = json::Value::make_object();
    wrong.set("command_pool", json::Value(std::string("1")));
    wrong.set("command_buffers", json::Value(std::string("oops")));
    VKR_CHECK(vkrpc::FreeCommandBuffersRequest::from_body(wrong).command_buffers.empty());
}

void test_sync_memory_objects() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const std::uint64_t device = make_device(backend);
    VKR_CHECK(device != 0);

    // fence + semaphore: device children; unknown device rejected.
    vkrpc::CreateFenceRequest cf;
    cf.device = device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(cf);
    VKR_CHECK(fence.ok);
    VKR_CHECK(fence.fence != 0);
    vkrpc::CreateFenceRequest cf_bad;
    cf_bad.device = device + 999;
    VKR_CHECK(!backend.create_fence(cf_bad).ok);

    vkrpc::CreateSemaphoreRequest cs;
    cs.device = device;
    const vkrpc::CreateSemaphoreResponse sem = backend.create_semaphore(cs);
    VKR_CHECK(sem.ok);
    VKR_CHECK(sem.semaphore != 0);

    // memory: allocation_size must be > 0; unknown device rejected.
    vkrpc::AllocateMemoryRequest am;
    am.device = device;
    am.allocation_size = 4096;
    const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(am);
    VKR_CHECK(mem.ok);
    VKR_CHECK(mem.memory != 0);
    vkrpc::AllocateMemoryRequest am0 = am;
    am0.allocation_size = 0;
    VKR_CHECK(!backend.allocate_memory(am0).ok);
    vkrpc::AllocateMemoryRequest am_bad;
    am_bad.device = device + 999;
    am_bad.allocation_size = 16;
    VKR_CHECK(!backend.allocate_memory(am_bad).ok);
    // memory_type_index is an unsigned Vulkan index: negative and out-of-uint32
    // values are rejected (carried wide so a narrowing decode can't slip one in).
    vkrpc::AllocateMemoryRequest am_neg = am;
    am_neg.memory_type_index = -1;
    VKR_CHECK(!backend.allocate_memory(am_neg).ok);
    vkrpc::AllocateMemoryRequest am_huge = am;
    am_huge.memory_type_index = (1LL << 32);
    VKR_CHECK(!backend.allocate_memory(am_huge).ok);
    // Option 1 Tier 1: the mock table's index 2 is a propertyFlags==0 device-accessible type (the
    // NVIDIA-shaped non-mappable class). The mock backend's memory-class reassertion must ADMIT it
    // (it previously fail-closed with "neither HOST_VISIBLE|HOST_COHERENT nor DEVICE_LOCAL-only"),
    // proving mock parity with the ICD's relaxed icd_subset::memory_class_ok.
    vkrpc::AllocateMemoryRequest am_flags0 = am;
    am_flags0.memory_type_index = 2;
    const vkrpc::AllocateMemoryResponse mem_flags0 = backend.allocate_memory(am_flags0);
    VKR_CHECK(mem_flags0.ok);
    // Free it right away so the device-teardown leaf accounting below is unchanged.
    vkrpc::HandleRequest h_flags0;
    h_flags0.handle = mem_flags0.memory;
    VKR_CHECK(backend.free_memory(h_flags0).ok);

    // Typed destroy: a handle of the wrong kind is rejected (a fence handle must
    // not be destroyable as a semaphore or freed as memory).
    vkrpc::HandleRequest h_fence;
    h_fence.handle = fence.fence;
    VKR_CHECK(!backend.destroy_semaphore(h_fence).ok);
    VKR_CHECK(!backend.free_memory(h_fence).ok);

    // A device with live leaves cannot be destroyed.
    vkrpc::HandleRequest dd;
    dd.handle = device;
    VKR_CHECK(!backend.destroy_device(dd).ok);

    // Destroy each leaf (double-destroy rejected), then the device.
    VKR_CHECK(backend.destroy_fence(h_fence).ok);
    VKR_CHECK(!backend.destroy_fence(h_fence).ok);
    vkrpc::HandleRequest h_sem;
    h_sem.handle = sem.semaphore;
    VKR_CHECK(backend.destroy_semaphore(h_sem).ok);
    vkrpc::HandleRequest h_mem;
    h_mem.handle = mem.memory;
    VKR_CHECK(backend.free_memory(h_mem).ok);
    VKR_CHECK(backend.destroy_device(dd).ok);
}

// (core-1.0 sync honesty): vkGetFenceStatus + the VkEvent object model (create/destroy,
// host status/set/reset) + the sync1 command events. Wire-and-mock round-trip; wrong-kind rejects;
// the wait_events length re-derivation accepts a well-formed payload and rejects a malformed one.
void test_sync_events_and_fence_status_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gq);
    VKR_CHECK(q.ok);

    // --- vkGetFenceStatus: signaled create -> VK_SUCCESS; unsignaled -> VK_NOT_READY; a submit
    // signals; reset returns to unsignaled. ---
    vkrpc::CreateFenceRequest cf_sig;
    cf_sig.device = cd.device;
    cf_sig.signaled = true;
    const vkrpc::CreateFenceResponse fence_sig = backend.create_fence(cf_sig);
    VKR_CHECK(fence_sig.ok);
    vkrpc::HandleRequest h_fsig;
    h_fsig.handle = fence_sig.fence;
    vkrpc::GetFenceStatusResponse gfs = backend.get_fence_status(h_fsig);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, vkrpc::kVkSuccess);

    vkrpc::CreateFenceRequest cf;
    cf.device = cd.device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(cf);
    VKR_CHECK(fence.ok);
    vkrpc::HandleRequest h_fence;
    h_fence.handle = fence.fence;
    gfs = backend.get_fence_status(h_fence);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, vkrpc::kVkNotReady);

    // A submit carrying the fence signals it (the mock executes synchronously).
    vkrpc::QueueSubmitRequest sub_fence;
    sub_fence.queue = q.queue;
    sub_fence.fence = fence.fence;
    VKR_CHECK(backend.queue_submit(sub_fence).ok);
    gfs = backend.get_fence_status(h_fence);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, vkrpc::kVkSuccess);

    // Reset returns it to unsignaled.
    vkrpc::ResetFencesRequest rf;
    rf.fences = {fence.fence};
    VKR_CHECK(backend.reset_fences(rf).ok);
    gfs = backend.get_fence_status(h_fence);
    VKR_CHECK(gfs.ok);
    VKR_CHECK_EQ(gfs.result, vkrpc::kVkNotReady);

    // --- VkEvent object: create -> RESET; set -> SET; reset -> RESET; wrong-kind rejects. ---
    vkrpc::CreateEventRequest ce;
    ce.device = cd.device;
    const vkrpc::CreateEventResponse event = backend.create_event(ce);
    VKR_CHECK(event.ok);
    VKR_CHECK(event.event != 0);
    vkrpc::CreateEventRequest ce_bad;
    ce_bad.device = cd.device + 999;
    VKR_CHECK(!backend.create_event(ce_bad).ok); // unknown device

    vkrpc::HandleRequest h_event;
    h_event.handle = event.event;
    vkrpc::GetEventStatusResponse ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, vkrpc::kVkEventReset);
    VKR_CHECK(backend.set_event(h_event).ok);
    ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, vkrpc::kVkEventSet);
    VKR_CHECK(backend.reset_event(h_event).ok);
    ges = backend.get_event_status(h_event);
    VKR_CHECK(ges.ok);
    VKR_CHECK_EQ(ges.result, vkrpc::kVkEventReset);

    // Wrong-kind rejects: a fence handle is not an event, and vice versa (no typed op frees or
    // reads another object's handle).
    VKR_CHECK(!backend.get_event_status(h_fence).ok);
    VKR_CHECK(!backend.set_event(h_fence).ok);
    VKR_CHECK(!backend.get_fence_status(h_event).ok);
    VKR_CHECK(!backend.destroy_fence(h_event).ok);

    // --- sync1 command events: record set_event + reset_event + a wait_events over the event; the
    // stream validates and submits. ---
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    VKR_CHECK(pool.ok);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs.ok && bufs.command_buffers.size() == 1);
    const std::uint64_t cmd = bufs.command_buffers[0];

    constexpr long long kStageTopOfPipe = 0x00000001;
    constexpr long long kStageTransfer = 0x00001000;

    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    rec.one_time_submit = true;
    vkrpc::RecordedCommand set_ev;
    set_ev.kind = "set_event";
    set_ev.src_stage = kStageTopOfPipe;
    set_ev.args_u64 = {event.event};
    rec.commands.push_back(set_ev);
    vkrpc::RecordedCommand reset_ev;
    reset_ev.kind = "reset_event";
    reset_ev.src_stage = kStageTopOfPipe;
    reset_ev.args_u64 = {event.event};
    rec.commands.push_back(reset_ev);
    // wait_events: 1 event, 1 global memory barrier, 0 buffer, 0 image. args_u64 = header[4] +
    // event + memory[srcAccess, dstAccess]; length = 4 + 1 + 2 = 7.
    vkrpc::RecordedCommand wait_ev;
    wait_ev.kind = "wait_events";
    wait_ev.src_stage = kStageTopOfPipe;
    wait_ev.dst_stage = kStageTransfer;
    wait_ev.args_u64 = {1, 1, 0, 0, event.event, /*srcAccess*/ 0, /*dstAccess*/ 0x1000};
    rec.commands.push_back(wait_ev);
    VKR_CHECK(backend.record_command_buffer(rec).ok);
    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    sub.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(sub).ok);

    // A malformed wait_events (payload length disagrees with the header counts) is rejected, never
    // decoded past its end.
    const vkrpc::AllocateCommandBuffersResponse bufs2 = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs2.ok);
    vkrpc::RecordCommandBufferRequest rec_bad;
    rec_bad.command_buffer = bufs2.command_buffers[0];
    rec_bad.one_time_submit = true;
    vkrpc::RecordedCommand wait_bad;
    wait_bad.kind = "wait_events";
    wait_bad.src_stage = kStageTopOfPipe;
    wait_bad.dst_stage = kStageTransfer;
    // header claims 1 memory barrier (needs 2 more slots) but only the event follows -> too short.
    wait_bad.args_u64 = {1, 1, 0, 0, event.event};
    rec_bad.commands.push_back(wait_bad);
    VKR_CHECK(!backend.record_command_buffer(rec_bad).ok);

    // An event with a zero stage mask on set_event is rejected (sync1 stageMask must be non-zero).
    const vkrpc::AllocateCommandBuffersResponse bufs3 = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs3.ok);
    vkrpc::RecordCommandBufferRequest rec_zero;
    rec_zero.command_buffer = bufs3.command_buffers[0];
    rec_zero.one_time_submit = true;
    vkrpc::RecordedCommand set_zero;
    set_zero.kind = "set_event";
    set_zero.src_stage = 0; // invalid
    set_zero.args_u64 = {event.event};
    rec_zero.commands.push_back(set_zero);
    VKR_CHECK(!backend.record_command_buffer(rec_zero).ok);

    // Destroy the event (double-destroy rejected).
    VKR_CHECK(backend.destroy_event(h_event).ok);
    VKR_CHECK(!backend.destroy_event(h_event).ok);
}

// (VK_KHR_synchronization2): the typed DependencyInfo2 codec round-trip (sparse + JSON),
// the shared structural validator, and the mock command/submit gates. The real 64-bit-mask replay +
// the compute-RAW-across-barrier2 proof live in the sync2 canary; here we pin the wire + mock
// semantics.
void test_sync2_mock() {
    // --- The typed DependencyInfo2 sparse-wire + JSON codec round-trips byte-exactly. ---
    vkrpc::RecordedCommand c;
    c.kind = "pipeline_barrier2";
    vkrpc::DependencyInfo2 d;
    d.dependency_flags = 1; // BY_REGION
    d.memory.push_back({0x1ULL << 40, 0x2ULL << 40, 0x3ULL << 40, 0x4ULL << 40});
    vkrpc::BufferMemoryBarrier2 bb;
    bb.src_stage = 0x5ULL << 40;
    bb.dst_stage = 0x6ULL << 40;
    bb.src_access = 7;
    bb.dst_access = 8;
    bb.src_queue_family = vkrpc::kVkQueueFamilyIgnored;
    bb.dst_queue_family = vkrpc::kVkQueueFamilyIgnored;
    bb.buffer = 0xABCD;
    bb.offset = 16;
    bb.size = ~0ULL;
    d.buffer.push_back(bb);
    vkrpc::ImageMemoryBarrier2 im;
    im.src_stage = 0x7ULL << 40;
    im.dst_stage = 0x8ULL << 40;
    im.src_access = 9;
    im.dst_access = 10;
    im.old_layout = 1;
    im.new_layout = 7;
    im.src_queue_family = vkrpc::kVkQueueFamilyIgnored;
    im.dst_queue_family = vkrpc::kVkQueueFamilyIgnored;
    im.image = 0x1234;
    im.aspect = 1;
    im.base_mip = 0;
    im.level_count = 1;
    im.base_layer = 0;
    im.layer_count = 1;
    d.image.push_back(im);
    c.deps2.push_back(d);
    vkrpc::RecordCommandBufferRequest rc;
    rc.command_buffer = 42;
    rc.commands.push_back(c);

    const auto check_dep_eq = [](const vkrpc::DependencyInfo2& a, const vkrpc::DependencyInfo2& b) {
        VKR_CHECK_EQ(a.dependency_flags, b.dependency_flags);
        VKR_CHECK(a.memory.size() == b.memory.size() && a.buffer.size() == b.buffer.size() &&
                  a.image.size() == b.image.size());
        if (!a.memory.empty()) {
            VKR_CHECK(a.memory[0].src_stage == b.memory[0].src_stage &&
                      a.memory[0].dst_access == b.memory[0].dst_access);
        }
        if (!a.buffer.empty()) {
            VKR_CHECK(a.buffer[0].src_stage == b.buffer[0].src_stage &&
                      a.buffer[0].buffer == b.buffer[0].buffer &&
                      a.buffer[0].size == b.buffer[0].size &&
                      a.buffer[0].src_queue_family == b.buffer[0].src_queue_family);
        }
        if (!a.image.empty()) {
            VKR_CHECK(a.image[0].src_stage == b.image[0].src_stage &&
                      a.image[0].old_layout == b.image[0].old_layout &&
                      a.image[0].new_layout == b.image[0].new_layout &&
                      a.image[0].image == b.image[0].image &&
                      a.image[0].aspect == b.image[0].aspect);
        }
    };
    // Sparse binary (the perf/native path).
    std::string werr;
    const vkrpc::RecordCommandBufferRequest w =
        vkrpc::RecordCommandBufferRequest::from_wire(rc.to_wire(), werr);
    VKR_CHECK(werr.empty() && w.commands.size() == 1 && w.commands[0].deps2.size() == 1);
    check_dep_eq(rc.commands[0].deps2[0], w.commands[0].deps2[0]);
    // JSON fallback path.
    const vkrpc::RecordCommandBufferRequest j =
        vkrpc::RecordCommandBufferRequest::from_body(rc.to_body());
    VKR_CHECK(j.commands.size() == 1 && j.commands[0].deps2.size() == 1);
    check_dep_eq(rc.commands[0].deps2[0], j.commands[0].deps2[0]);

    // --- The shared structural validator: QFI policy + caps. ---
    std::string why;
    VKR_CHECK(vkrpc::validate_dependency_info2(d, why)); // both IGNORED -> ok
    vkrpc::DependencyInfo2 bad_qfi = d;
    bad_qfi.buffer[0].src_queue_family = 0;
    bad_qfi.buffer[0].dst_queue_family = 1; // a real ownership transfer -> reject
    VKR_CHECK(!vkrpc::validate_dependency_info2(bad_qfi, why));
    vkrpc::DependencyInfo2 same_fam = d;
    same_fam.buffer[0].src_queue_family = 0;
    same_fam.buffer[0].dst_queue_family = 0; // both the single family 0 -> ok
    VKR_CHECK(vkrpc::validate_dependency_info2(same_fam, why));

    // --- The mock command + submit gates (feature-gated; mock == real). ---
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    // The sync2 gate is ext AND feature (mock == real), so the positive device enables BOTH.
    cdr.enabled_extensions = {vkrpc::kSync2ExtensionName};
    cdr.synchronization2_feature_enabled = 1;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.ok);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gq);
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    const std::uint64_t cmd = bufs.command_buffers[0];
    vkrpc::CreateEventRequest ce;
    ce.device = cd.device;
    const vkrpc::CreateEventResponse event = backend.create_event(ce);
    VKR_CHECK(event.ok);

    // A stream: pipeline_barrier2 (one global memory barrier) + set_event2 + reset_event2 +
    // wait_events2 (empty dependency). All handle-free, so no buffer/image setup needed.
    vkrpc::DependencyInfo2 mem_only;
    mem_only.memory.push_back({1, 2, 0x1000, 0x800});
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    rec.one_time_submit = true;
    vkrpc::RecordedCommand b2;
    b2.kind = "pipeline_barrier2";
    b2.deps2.push_back(mem_only);
    rec.commands.push_back(b2);
    vkrpc::RecordedCommand se2;
    se2.kind = "set_event2";
    se2.args_u64 = {event.event};
    se2.deps2.push_back(vkrpc::DependencyInfo2{});
    rec.commands.push_back(se2);
    vkrpc::RecordedCommand re2;
    re2.kind = "reset_event2";
    re2.args_u64 = {event.event, 0x1000};
    rec.commands.push_back(re2);
    vkrpc::RecordedCommand we2;
    we2.kind = "wait_events2";
    we2.args_u64 = {event.event};
    we2.deps2.push_back(vkrpc::DependencyInfo2{});
    rec.commands.push_back(we2);
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    // Submit via queue_submit2 (one VkSubmitInfo2 carrying the recorded CB).
    vkrpc::QueueSubmit2Request s2;
    s2.queue = q.queue;
    vkrpc::SubmitInfo2 si;
    si.command_buffers = {cmd};
    s2.submits.push_back(si);
    VKR_CHECK(backend.queue_submit2(s2).ok);

    // Reject: wait_events2 with a deps/event count mismatch.
    const vkrpc::AllocateCommandBuffersResponse bufs2 = backend.allocate_command_buffers(abr);
    vkrpc::RecordCommandBufferRequest rec_bad;
    rec_bad.command_buffer = bufs2.command_buffers[0];
    vkrpc::RecordedCommand we_bad;
    we_bad.kind = "wait_events2";
    we_bad.args_u64 = {event.event}; // 1 event
    // ... but 0 dependencies (deps2 empty) -> mismatch.
    rec_bad.commands.push_back(we_bad);
    VKR_CHECK(!backend.record_command_buffer(rec_bad).ok);

    // Reject: a device with the FEATURE but WITHOUT the extension cannot record barrier2 or submit2
    // -- the gate is ext AND feature, so feature-only fails (mock == real).
    vkrpc::CreateDeviceRequest cdr0;
    cdr0.instance = ci.instance;
    cdr0.physical_device = en.devices.front().handle;
    cdr0.synchronization2_feature_enabled = 1; // feature ON, extension NOT enabled -> still rejects
    const vkrpc::CreateDeviceResponse cd0 = backend.create_device(cdr0);
    vkrpc::GetDeviceQueueRequest gq0;
    gq0.device = cd0.device;
    gq0.queue_family_index = cd0.queue_family_index;
    gq0.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q0 = backend.get_device_queue(gq0);
    vkrpc::CreateCommandPoolRequest cpr0;
    cpr0.device = cd0.device;
    cpr0.queue_family_index = cd0.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool0 = backend.create_command_pool(cpr0);
    vkrpc::AllocateCommandBuffersRequest abr0;
    abr0.command_pool = pool0.command_pool;
    abr0.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs0 = backend.allocate_command_buffers(abr0);
    vkrpc::RecordCommandBufferRequest rec0;
    rec0.command_buffer = bufs0.command_buffers[0];
    vkrpc::RecordedCommand b2_0;
    b2_0.kind = "pipeline_barrier2";
    b2_0.deps2.push_back(mem_only);
    rec0.commands.push_back(b2_0);
    VKR_CHECK(!backend.record_command_buffer(rec0).ok); // feature without ext -> reject
    vkrpc::QueueSubmit2Request s2_0;
    s2_0.queue = q0.queue;
    s2_0.submits.push_back(vkrpc::SubmitInfo2{});
    VKR_CHECK(!backend.queue_submit2(s2_0).ok); // feature without ext -> reject

    // A fence-only vkQueueSubmit2 (empty submits + a fence) on the sync2 device is valid and
    // signals the fence -- the corner the ICD's fence-only readback proof mirrors.
    vkrpc::CreateFenceRequest cf2;
    cf2.device = cd.device;
    const vkrpc::CreateFenceResponse fence2 = backend.create_fence(cf2);
    VKR_CHECK(fence2.ok);
    vkrpc::QueueSubmit2Request s2_fence;
    s2_fence.queue = q.queue;
    s2_fence.fence = fence2.fence; // no submits
    VKR_CHECK(backend.queue_submit2(s2_fence).ok);
    vkrpc::HandleRequest h_f2;
    h_f2.handle = fence2.fence;
    const vkrpc::GetFenceStatusResponse gfs2 = backend.get_fence_status(h_f2);
    VKR_CHECK(gfs2.ok);
    VKR_CHECK_EQ(gfs2.result, vkrpc::kVkSuccess); // the fence-only submit2 signaled it
}

void test_bda_mock() {
    // --- Wire round-trips: the additive allocate_flags + feature bit + the new op pair. ---
    {
        vkrpc::AllocateMemoryRequest am;
        am.device = 7;
        am.allocation_size = 4096;
        am.memory_type_index = 1;
        am.allocate_flags = vkrpc::kMemoryAllocateDeviceAddressBit;
        const vkrpc::AllocateMemoryRequest rt =
            vkrpc::AllocateMemoryRequest::from_body(am.to_body());
        VKR_CHECK_EQ(rt.allocate_flags, vkrpc::kMemoryAllocateDeviceAddressBit);
        // A legacy body without the field decodes to 0 (additive).
        json::Value legacy = json::Value::make_object();
        legacy.set("device", json::Value(std::string("1")));
        legacy.set("allocation_size", json::Value(std::string("16")));
        legacy.set("memory_type_index", json::Value(std::string("0")));
        VKR_CHECK_EQ(vkrpc::AllocateMemoryRequest::from_body(legacy).allocate_flags, 0ull);
    }
    {
        vkrpc::CreateDeviceRequest cdr;
        cdr.buffer_device_address_feature_enabled = 1;
        const vkrpc::CreateDeviceRequest rt = vkrpc::CreateDeviceRequest::from_body(cdr.to_body());
        VKR_CHECK_EQ(rt.buffer_device_address_feature_enabled, 1);
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(json::Value::make_object())
                         .buffer_device_address_feature_enabled,
                     0);
    }
    {
        // vertex-attr-divisor: the two feature-enable scalars ride the wire; a legacy body decodes
        // both to 0 (additive).
        vkrpc::CreateDeviceRequest cdr;
        cdr.vertex_attr_divisor_feature_enabled = 1;
        cdr.vertex_attr_zero_divisor_feature_enabled = 1;
        const vkrpc::CreateDeviceRequest rt = vkrpc::CreateDeviceRequest::from_body(cdr.to_body());
        VKR_CHECK_EQ(rt.vertex_attr_divisor_feature_enabled, 1);
        VKR_CHECK_EQ(rt.vertex_attr_zero_divisor_feature_enabled, 1);
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(json::Value::make_object())
                         .vertex_attr_divisor_feature_enabled,
                     0);
    }
    {
        vkrpc::GetBufferDeviceAddressRequest gr;
        gr.device = 11;
        gr.buffer = 22;
        const auto rt = vkrpc::GetBufferDeviceAddressRequest::from_body(gr.to_body());
        VKR_CHECK_EQ(rt.device, 11ull);
        VKR_CHECK_EQ(rt.buffer, 22ull);
        vkrpc::GetBufferDeviceAddressResponse resp;
        resp.ok = true;
        resp.reason = "ok";
        resp.device_address = 0xB0DA000000012345ull; // full 64-bit width must survive the wire
        const auto rrt = vkrpc::GetBufferDeviceAddressResponse::from_body(resp.to_body());
        VKR_CHECK(rrt.ok);
        VKR_CHECK_EQ(rrt.device_address, 0xB0DA000000012345ull);
    }

    // --- The mock gates (mock == real): feature-gated allocate flags + address queries. ---
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    // Device A: WITHOUT the feature. Device B: WITH it.
    vkrpc::CreateDeviceRequest cda;
    cda.instance = ci.instance;
    cda.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse dev_a = backend.create_device(cda);
    VKR_CHECK(dev_a.ok);
    vkrpc::CreateDeviceRequest cdb = cda;
    cdb.buffer_device_address_feature_enabled = 1;
    const vkrpc::CreateDeviceResponse dev_b = backend.create_device(cdb);
    VKR_CHECK(dev_b.ok);

    auto make_buffer = [&](std::uint64_t device, std::uint64_t usage) {
        vkrpc::CreateBufferRequest cbr;
        cbr.device = device;
        cbr.size = 4096;
        cbr.usage = usage;
        cbr.sharing_mode = 0; // EXCLUSIVE
        const vkrpc::CreateBufferResponse b = backend.create_buffer(cbr);
        VKR_CHECK(b.ok);
        return b.buffer;
    };
    auto make_memory = [&](std::uint64_t device, std::uint64_t flags, bool expect_ok) {
        vkrpc::AllocateMemoryRequest am;
        am.device = device;
        am.allocation_size = 8192;
        am.memory_type_index = 0;
        am.allocate_flags = flags;
        const vkrpc::AllocateMemoryResponse m = backend.allocate_memory(am);
        VKR_CHECK_EQ(m.ok, expect_ok);
        return m.memory;
    };

    // Feature gates on the allocate flag: A rejects, B accepts; non-DEVICE_ADDRESS bits reject
    // even on B (deviceMask 0x1, captureReplay 0x4 -- by name).
    make_memory(dev_a.device, vkrpc::kMemoryAllocateDeviceAddressBit, false);
    make_memory(dev_b.device, 0x1, false);
    make_memory(dev_b.device, 0x4, false);
    const std::uint64_t mem_bda =
        make_memory(dev_b.device, vkrpc::kMemoryAllocateDeviceAddressBit, true);
    const std::uint64_t mem_plain = make_memory(dev_b.device, 0, true);

    // VUID 03339 at bind: a SHADER_DEVICE_ADDRESS buffer refuses non-DEVICE_ADDRESS memory.
    const std::uint64_t buf_bda = make_buffer(
        dev_b.device, vkrpc::kBufferUsageStorageBuffer | vkrpc::kBufferUsageShaderDeviceAddress);
    {
        vkrpc::BindBufferMemoryRequest bb;
        bb.buffer = buf_bda;
        bb.memory = mem_plain;
        bb.memory_offset = 0;
        VKR_CHECK(!backend.bind_buffer_memory(bb).ok); // VUID 03339
        bb.memory = mem_bda;
        VKR_CHECK(backend.bind_buffer_memory(bb).ok);
    }

    // The address: unbound/usage-less/feature-less all reject; bound-on-B is non-zero, STABLE
    // across calls, and DISTINCT per buffer.
    vkrpc::GetBufferDeviceAddressRequest gar;
    gar.device = dev_b.device;
    gar.buffer = buf_bda;
    const auto a1 = backend.get_buffer_device_address(gar);
    VKR_CHECK(a1.ok && a1.device_address != 0);
    const auto a2 = backend.get_buffer_device_address(gar);
    VKR_CHECK(a2.ok);
    VKR_CHECK_EQ(a2.device_address, a1.device_address); // stable
    {
        // A second BDA buffer: distinct address; and UNBOUND -> reject until bound.
        const std::uint64_t buf2 =
            make_buffer(dev_b.device,
                        vkrpc::kBufferUsageStorageBuffer | vkrpc::kBufferUsageShaderDeviceAddress);
        vkrpc::GetBufferDeviceAddressRequest g2;
        g2.device = dev_b.device;
        g2.buffer = buf2;
        VKR_CHECK(!backend.get_buffer_device_address(g2).ok); // unbound
        const std::uint64_t mem2 =
            make_memory(dev_b.device, vkrpc::kMemoryAllocateDeviceAddressBit, true);
        vkrpc::BindBufferMemoryRequest bb;
        bb.buffer = buf2;
        bb.memory = mem2;
        bb.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bb).ok);
        const auto ad2 = backend.get_buffer_device_address(g2);
        VKR_CHECK(ad2.ok && ad2.device_address != 0);
        VKR_CHECK(ad2.device_address != a1.device_address); // distinct per buffer
    }
    {
        // No SHADER_DEVICE_ADDRESS usage -> reject even when bound on the BDA device.
        const std::uint64_t plain_buf = make_buffer(dev_b.device, vkrpc::kBufferUsageStorageBuffer);
        const std::uint64_t pm = make_memory(dev_b.device, 0, true);
        vkrpc::BindBufferMemoryRequest bb;
        bb.buffer = plain_buf;
        bb.memory = pm;
        bb.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bb).ok);
        vkrpc::GetBufferDeviceAddressRequest g;
        g.device = dev_b.device;
        g.buffer = plain_buf;
        VKR_CHECK(!backend.get_buffer_device_address(g).ok);
    }
    {
        // Device A (feature off): rejected regardless of the buffer; unknown buffer rejected too.
        vkrpc::GetBufferDeviceAddressRequest g;
        g.device = dev_a.device;
        g.buffer = buf_bda; // wrong device anyway -- the feature gate fires first
        VKR_CHECK(!backend.get_buffer_device_address(g).ok);
        g.device = dev_b.device;
        g.buffer = 0xDEAD;
        VKR_CHECK(!backend.get_buffer_device_address(g).ok);
    }
}

void test_descriptor_indexing_mock() {
    const char* why = "";
    auto mkb = [](int binding, int type, int count, long long flags) {
        vkrpc::DescriptorSetLayoutBindingDesc d;
        d.binding = binding;
        d.descriptor_type = type;
        d.descriptor_count = count;
        d.stage_flags = 0x20; // COMPUTE
        d.binding_flags = flags;
        return d;
    };
    const long long kUabPool = vkrpc::kDescriptorSetLayoutCreateUpdateAfterBindPool;

    // --- 1. The shared per-binding-flag admission (the pure helper both backends + the ICD
    // call). The EXPLICIT per-type UAB map: each admissible type keyed to its
    // feature bit -- present -> ok, absent (all OTHER bits set) -> reject; INPUT_ATTACHMENT, the
    // DYNAMIC buffer types, and an unwired type reject outright.
    {
        const struct {
            int type;
            std::uint64_t bit;
        } map_ok[] = {
            {vkrpc::kDescriptorTypeUniformBuffer, vkrpc::kDIFeatureUpdateAfterBindUniformBuffer},
            {vkrpc::kDescriptorTypeStorageBuffer, vkrpc::kDIFeatureUpdateAfterBindStorageBuffer},
            {vkrpc::kDescriptorTypeSampler, vkrpc::kDIFeatureUpdateAfterBindSampledImage},
            {vkrpc::kDescriptorTypeCombinedImageSampler,
             vkrpc::kDIFeatureUpdateAfterBindSampledImage},
            {vkrpc::kDescriptorTypeSampledImage, vkrpc::kDIFeatureUpdateAfterBindSampledImage},
            {vkrpc::kDescriptorTypeStorageImage, vkrpc::kDIFeatureUpdateAfterBindStorageImage},
            {vkrpc::kDescriptorTypeUniformTexelBuffer,
             vkrpc::kDIFeatureUpdateAfterBindUniformTexelBuffer},
            {vkrpc::kDescriptorTypeStorageTexelBuffer,
             vkrpc::kDIFeatureUpdateAfterBindStorageTexelBuffer},
        };
        for (const auto& m : map_ok) {
            std::vector<vkrpc::DescriptorSetLayoutBindingDesc> bs = {
                mkb(0, m.type, 4, vkrpc::kDescriptorBindingUpdateAfterBind)};
            VKR_CHECK(vkrpc::descriptor_indexing_layout_ok(kUabPool, bs, m.bit, &why));
            VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(
                kUabPool, bs, vkrpc::kDIFeatureAllBits & ~m.bit, &why)); // the per-type bit rules
            VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(0, bs, m.bit,
                                                            &why)); // needs the layout UAB flag
        }
        const int reject_types[] = {vkrpc::kDescriptorTypeInputAttachment,
                                    vkrpc::kDescriptorTypeUniformBufferDynamic,
                                    vkrpc::kDescriptorTypeStorageBufferDynamic,
                                    1000138000 /* inline uniform block: unwired */};
        for (const int t : reject_types) {
            std::vector<vkrpc::DescriptorSetLayoutBindingDesc> bs = {
                mkb(0, t, 1, vkrpc::kDescriptorBindingUpdateAfterBind)};
            VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(kUabPool, bs, vkrpc::kDIFeatureAllBits,
                                                            &why));
        }
        // UPDATE_UNUSED_WHILE_PENDING / PARTIALLY_BOUND: their own bits, no layout flag needed.
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> uuwp = {
            mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1,
                vkrpc::kDescriptorBindingUpdateUnusedWhilePending)};
        VKR_CHECK(vkrpc::descriptor_indexing_layout_ok(
            0, uuwp, vkrpc::kDIFeatureUpdateUnusedWhilePending, &why));
        VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(0, uuwp, 0, &why));
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> pb = {mkb(
            0, vkrpc::kDescriptorTypeStorageBuffer, 1, vkrpc::kDescriptorBindingPartiallyBound)};
        VKR_CHECK(
            vkrpc::descriptor_indexing_layout_ok(0, pb, vkrpc::kDIFeaturePartiallyBound, &why));
        VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(0, pb, 0, &why));
        // VARIABLE_DESCRIPTOR_COUNT: its bit, only the highest-numbered binding, never DYNAMIC.
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> vdc = {
            mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1, 0),
            mkb(1, vkrpc::kDescriptorTypeStorageBuffer, 64,
                vkrpc::kDescriptorBindingVariableDescriptorCount)};
        VKR_CHECK(vkrpc::descriptor_indexing_layout_ok(
            0, vdc, vkrpc::kDIFeatureVariableDescriptorCount, &why));
        VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(0, vdc, 0, &why));
        VKR_CHECK_EQ(vkrpc::di_variable_binding(vdc), 1);
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> vdc_not_last = {
            mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 64,
                vkrpc::kDescriptorBindingVariableDescriptorCount),
            mkb(1, vkrpc::kDescriptorTypeStorageBuffer, 1, 0)};
        VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(
            0, vdc_not_last, vkrpc::kDIFeatureVariableDescriptorCount, &why));
        VKR_CHECK_EQ(vkrpc::di_variable_binding(vdc_not_last), -1);
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> vdc_dyn = {
            mkb(0, vkrpc::kDescriptorTypeUniformBufferDynamic, 4,
                vkrpc::kDescriptorBindingVariableDescriptorCount)};
        VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(
            0, vdc_dyn, vkrpc::kDIFeatureVariableDescriptorCount, &why));
        // An unknown flag bit rejects by name.
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> unknown = {
            mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1, 0x10)};
        VKR_CHECK(
            !vkrpc::descriptor_indexing_layout_ok(0, unknown, vkrpc::kDIFeatureAllBits, &why));
        // The layout UPDATE_AFTER_BIND_POOL flag is a CONTAINER flag: valid
        // with no UAB binding and NO feature at all -- INCLUDING with flagless DYNAMIC bindings
        // (the dynamic ban is VUID 03001, conditioned on an ACTUAL
        // UPDATE_AFTER_BIND binding, not the container flag). Once any binding carries
        // UPDATE_AFTER_BIND, a dynamic SIBLING -- flagged or not -- rejects.
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> none = {
            mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1, 0)};
        VKR_CHECK(vkrpc::descriptor_indexing_layout_ok(kUabPool, none, 0, &why));
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> uab_pool_dyn = {
            mkb(0, vkrpc::kDescriptorTypeUniformBufferDynamic, 1, 0)};
        VKR_CHECK(vkrpc::descriptor_indexing_layout_ok(kUabPool, uab_pool_dyn, 0, &why));
        std::vector<vkrpc::DescriptorSetLayoutBindingDesc> uab_dyn_sibling = {
            mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1,
                vkrpc::kDescriptorBindingUpdateAfterBind),
            mkb(1, vkrpc::kDescriptorTypeUniformBufferDynamic, 1, 0)};
        VKR_CHECK(!vkrpc::descriptor_indexing_layout_ok(kUabPool, uab_dyn_sibling,
                                                        vkrpc::kDIFeatureAllBits, &why));
    }

    // --- 2. Wire round-trips (additive; legacy decodes empty/0). ---
    {
        vkrpc::CreateDeviceRequest cdr;
        cdr.descriptor_indexing_feature_bits = vkrpc::kDIFeatureAllBits;
        VKR_CHECK_EQ(
            vkrpc::CreateDeviceRequest::from_body(cdr.to_body()).descriptor_indexing_feature_bits,
            vkrpc::kDIFeatureAllBits);
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(json::Value::make_object())
                         .descriptor_indexing_feature_bits,
                     0ull);
        vkrpc::AllocateDescriptorSetsRequest adr;
        adr.variable_counts = {7, 16};
        const auto adr_rt = vkrpc::AllocateDescriptorSetsRequest::from_body(adr.to_body());
        VKR_CHECK_EQ(adr_rt.variable_counts.size(), std::size_t{2});
        VKR_CHECK_EQ(adr_rt.variable_counts[1], 16ull);
        VKR_CHECK(vkrpc::AllocateDescriptorSetsRequest::from_body(json::Value::make_object())
                      .variable_counts.empty());
        vkrpc::GetDescriptorSetLayoutSupportResponse sup;
        sup.ok = true;
        sup.supported = 1;
        sup.max_variable_descriptor_count = 12345;
        const auto sup_rt = vkrpc::GetDescriptorSetLayoutSupportResponse::from_body(sup.to_body());
        VKR_CHECK(sup_rt.ok);
        VKR_CHECK_EQ(sup_rt.supported, 1);
        VKR_CHECK_EQ(sup_rt.max_variable_descriptor_count, 12345ull);
    }

    // --- 3. The mock end-to-end (mock == real gates). ---
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const auto en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cda;
    cda.instance = ci.instance;
    cda.physical_device = en.devices.front().handle;
    const auto dev_a = backend.create_device(cda); // WITHOUT any DI feature
    VKR_CHECK(dev_a.ok);
    vkrpc::CreateDeviceRequest cdb = cda;
    cdb.descriptor_indexing_feature_bits = vkrpc::kDIFeatureServedBits;
    const auto dev_b = backend.create_device(cdb); // WITH the full SERVED set
    VKR_CHECK(dev_b.ok);
    {
        vkrpc::CreateDeviceRequest bad = cda;
        bad.descriptor_indexing_feature_bits = 1ull << 40; // unknown bit -> reject
        VKR_CHECK(!backend.create_device(bad).ok);
        // A DEFERRED-but-known bit (an image/texel UAB class) rejects exactly like
        // an unknown one -- CreateDevice policy is the SERVED set, not the known universe.
        bad.descriptor_indexing_feature_bits = vkrpc::kDIFeatureUpdateAfterBindSampledImage;
        VKR_CHECK(!backend.create_device(bad).ok);
        bad.descriptor_indexing_feature_bits = vkrpc::kDIFeatureAllBits; // contains deferred bits
        VKR_CHECK(!backend.create_device(bad).ok);
    }
    // The served set plus the two BUFFER shader non-uniform-indexing wire bits.
    {
        // The served subset is a strict subset of the known universe, carries the two shader
        // bits, and excludes every deferred image/texel UAB class.
        VKR_CHECK_EQ(vkrpc::kDIFeatureServedBits & ~vkrpc::kDIFeatureAllBits, 0ull);
        VKR_CHECK((vkrpc::kDIFeatureServedBits &
                   vkrpc::kDIFeatureShaderUniformBufferArrayNonUniformIndexing) != 0);
        VKR_CHECK((vkrpc::kDIFeatureServedBits &
                   vkrpc::kDIFeatureShaderStorageBufferArrayNonUniformIndexing) != 0);
        VKR_CHECK_EQ(vkrpc::kDIFeatureServedBits &
                         (vkrpc::kDIFeatureUpdateAfterBindSampledImage |
                          vkrpc::kDIFeatureUpdateAfterBindStorageImage |
                          vkrpc::kDIFeatureUpdateAfterBindUniformTexelBuffer |
                          vkrpc::kDIFeatureUpdateAfterBindStorageTexelBuffer),
                     0ull);
        // The wire scalar round-trips the served set, and the mock accepts exactly it (the
        // shader bits gate nothing at record time -- they are SPIR-V capability gates the driver
        // enforces at pipeline creation).
        vkrpc::CreateDeviceRequest served = cda;
        served.descriptor_indexing_feature_bits = vkrpc::kDIFeatureServedBits;
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(served.to_body())
                         .descriptor_indexing_feature_bits,
                     vkrpc::kDIFeatureServedBits);
        VKR_CHECK(backend.create_device(served).ok);
        // The first still-unknown bit past the widened universe rejects.
        vkrpc::CreateDeviceRequest past = cda;
        past.descriptor_indexing_feature_bits = 1ull << 12;
        VKR_CHECK(!backend.create_device(past).ok);
    }
    auto make_layout = [&](std::uint64_t device,
                           std::vector<vkrpc::DescriptorSetLayoutBindingDesc> bs,
                           long long layout_flags) {
        vkrpc::CreateDescriptorSetLayoutRequest r;
        r.device = device;
        r.layout_flags = layout_flags;
        r.bindings = std::move(bs);
        return backend.create_descriptor_set_layout(r);
    };
    // Feature-off device: UAB/PB BINDING flags reject (the feature gates); the layout + pool
    // UPDATE_AFTER_BIND flags are CONTAINER flags -- the whole
    // container-only path (UAB_POOL layout with flagless bindings, a UAB pool, and the
    // allocation) works with NO feature at all, while a plain pool still refuses the UAB layout.
    VKR_CHECK(!make_layout(dev_a.device,
                           {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 4,
                                vkrpc::kDescriptorBindingUpdateAfterBind)},
                           kUabPool)
                   .ok);
    VKR_CHECK(!make_layout(dev_a.device,
                           {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 4,
                                vkrpc::kDescriptorBindingPartiallyBound)},
                           0)
                   .ok);
    {
        const auto container_layout = make_layout(
            dev_a.device, {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 4, 0)}, kUabPool);
        VKR_CHECK(container_layout.ok); // the container flag alone needs no feature
        VKR_CHECK(make_layout(dev_a.device,
                              {mkb(0, vkrpc::kDescriptorTypeUniformBufferDynamic, 1, 0)},
                              kUabPool)
                      .ok); // flagless DYNAMIC in a UAB_POOL layout is VALID
        vkrpc::CreateDescriptorPoolRequest r;
        r.device = dev_a.device;
        r.max_sets = 2;
        r.flags = vkrpc::kDescriptorPoolCreateUpdateAfterBind;
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeStorageBuffer, 8});
        const auto uab_pool_a = backend.create_descriptor_pool(r);
        VKR_CHECK(uab_pool_a.ok); // container flag, featureless -> fine
        r.flags = 0;
        const auto plain_pool_a = backend.create_descriptor_pool(r);
        VKR_CHECK(plain_pool_a.ok);
        vkrpc::AllocateDescriptorSetsRequest a;
        a.device = dev_a.device;
        a.pool = plain_pool_a.pool;
        a.set_layouts = {container_layout.set_layout};
        VKR_CHECK(!backend.allocate_descriptor_sets(a).ok); // UAB layout needs a UAB pool
        a.pool = uab_pool_a.pool;
        VKR_CHECK(backend.allocate_descriptor_sets(a).ok);
    }
    // The layout-support query mirrors the admission: unsupported on A, supported on B.
    {
        vkrpc::CreateDescriptorSetLayoutRequest r;
        r.device = dev_a.device;
        r.layout_flags = kUabPool;
        r.bindings = {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 4,
                          vkrpc::kDescriptorBindingUpdateAfterBind)};
        const auto sup_a = backend.get_descriptor_set_layout_support(r);
        VKR_CHECK(sup_a.ok);
        VKR_CHECK_EQ(sup_a.supported, 0);
        r.device = dev_b.device;
        const auto sup_b = backend.get_descriptor_set_layout_support(r);
        VKR_CHECK(sup_b.ok);
        VKR_CHECK_EQ(sup_b.supported, 1);
        VKR_CHECK_EQ(sup_b.max_variable_descriptor_count, 0ull); // no variable binding
        r.bindings = {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 64,
                          vkrpc::kDescriptorBindingVariableDescriptorCount)};
        r.layout_flags = 0;
        const auto sup_v = backend.get_descriptor_set_layout_support(r);
        VKR_CHECK_EQ(sup_v.supported, 1);
        VKR_CHECK(sup_v.max_variable_descriptor_count > 0);
    }

    // Layouts on B: a CLASSIC one, a PARTIALLY_BOUND one, a UAB one, and a variable-count one.
    const std::uint64_t dsl_classic =
        make_layout(dev_b.device, {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1, 0)}, 0)
            .set_layout;
    const std::uint64_t dsl_pb = make_layout(dev_b.device,
                                             {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 4,
                                                  vkrpc::kDescriptorBindingPartiallyBound)},
                                             0)
                                     .set_layout;
    const std::uint64_t dsl_uab = make_layout(dev_b.device,
                                              {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 2,
                                                   vkrpc::kDescriptorBindingUpdateAfterBind)},
                                              kUabPool)
                                      .set_layout;
    const std::uint64_t dsl_var =
        make_layout(dev_b.device,
                    {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1, 0),
                     mkb(1, vkrpc::kDescriptorTypeStorageBuffer, 64,
                         vkrpc::kDescriptorBindingVariableDescriptorCount |
                             vkrpc::kDescriptorBindingPartiallyBound)},
                    0)
            .set_layout;
    VKR_CHECK(dsl_classic != 0 && dsl_pb != 0 && dsl_uab != 0 && dsl_var != 0);
    // (VUID 03001): once ANY binding carries UPDATE_AFTER_BIND, a DYNAMIC sibling
    // rejects even on a fully DI-enabled device.
    VKR_CHECK(!make_layout(dev_b.device,
                           {mkb(0, vkrpc::kDescriptorTypeStorageBuffer, 1,
                                vkrpc::kDescriptorBindingUpdateAfterBind),
                            mkb(1, vkrpc::kDescriptorTypeUniformBufferDynamic, 1, 0)},
                           kUabPool)
                   .ok);

    // Pools: a plain one and a UAB one. A UAB-pool layout refuses the plain pool.
    auto make_pool = [&](long long flags, int max_sets) {
        vkrpc::CreateDescriptorPoolRequest r;
        r.device = dev_b.device;
        r.max_sets = max_sets;
        r.flags = flags;
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeStorageBuffer, 256});
        return backend.create_descriptor_pool(r).pool;
    };
    const std::uint64_t pool_plain = make_pool(0, 16);
    const std::uint64_t pool_uab = make_pool(vkrpc::kDescriptorPoolCreateUpdateAfterBind, 16);
    VKR_CHECK(pool_plain != 0 && pool_uab != 0);
    auto alloc_sets = [&](std::uint64_t pool, std::vector<std::uint64_t> layouts,
                          std::vector<std::uint64_t> counts) {
        vkrpc::AllocateDescriptorSetsRequest r;
        r.device = dev_b.device;
        r.pool = pool;
        r.set_layouts = std::move(layouts);
        r.variable_counts = std::move(counts);
        return backend.allocate_descriptor_sets(r);
    };
    VKR_CHECK(!alloc_sets(pool_plain, {dsl_uab}, {}).ok); // UAB layout needs a UAB pool
    const auto uab_alloc = alloc_sets(pool_uab, {dsl_uab}, {});
    VKR_CHECK(uab_alloc.ok);
    const std::uint64_t set_uab = uab_alloc.descriptor_sets.front();

    // Variable-count semantics: parallel-or-absent; ignored for non-variable
    // layouts; <= the declared max for variable ones; the vector is sized to the ALLOCATED count.
    VKR_CHECK(!alloc_sets(pool_plain, {dsl_var}, {128}).ok);  // > declared max 64
    VKR_CHECK(!alloc_sets(pool_plain, {dsl_var}, {8, 8}).ok); // not parallel
    const auto mixed = alloc_sets(pool_plain, {dsl_classic, dsl_var}, {7, 16});
    VKR_CHECK(mixed.ok); // 7 is IGNORED for the classic layout; 16 sizes the variable binding
    const std::uint64_t set_var = mixed.descriptor_sets[1];
    const auto var_zero_alloc = alloc_sets(pool_plain, {dsl_var}, {});
    VKR_CHECK(var_zero_alloc.ok); // absent counts -> the variable binding is sized 0
    const std::uint64_t set_var_zero = var_zero_alloc.descriptor_sets.front();

    // Buffers to write into slots.
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = en.devices.front().handle;
    const auto mp = backend.get_physical_device_memory_properties(mpr);
    int coherent_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(coherent_type >= 0);
    vkrpc::AllocateMemoryRequest am;
    am.device = dev_b.device;
    am.allocation_size = 64 * 1024;
    am.memory_type_index = coherent_type;
    const std::uint64_t mem = backend.allocate_memory(am).memory;
    std::uint64_t next_off = 0;
    auto make_buf = [&]() {
        vkrpc::CreateBufferRequest r;
        r.device = dev_b.device;
        r.size = 1024;
        r.usage = vkrpc::kBufferUsageStorageBuffer;
        r.sharing_mode = 0;
        const std::uint64_t b = backend.create_buffer(r).buffer;
        vkrpc::BindBufferMemoryRequest bb;
        bb.buffer = b;
        bb.memory = mem;
        bb.memory_offset = next_off;
        next_off += 1024;
        VKR_CHECK(backend.bind_buffer_memory(bb).ok);
        return b;
    };
    auto write_slot = [&](std::uint64_t set, int binding, int element, std::uint64_t buffer) {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = dev_b.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = set;
        w.dst_binding = binding;
        w.dst_array_element = element;
        w.descriptor_type = vkrpc::kDescriptorTypeStorageBuffer;
        w.descriptor_count = 1;
        w.buffer_infos.push_back({buffer, 0, 1024});
        r.writes.push_back(w);
        return backend.update_descriptor_sets(r);
    };
    // The allocated count bounds writes: element 15 lands, element 16 rejects (the layout max 64
    // would have admitted it); the zero-sized variable binding admits nothing.
    VKR_CHECK(write_slot(set_var, 1, 15, make_buf()).ok);
    VKR_CHECK(!write_slot(set_var, 1, 16, make_buf()).ok);
    VKR_CHECK(!write_slot(set_var_zero, 1, 0, make_buf()).ok);

    // --- The readiness matrix, through the RECORD gate (bind + dispatch). ---
    vkrpc::CreateShaderModuleRequest smr;
    smr.device = dev_b.device;
    smr.code = std::string(8, 'a');
    smr.code_size = 8;
    const std::uint64_t cs = backend.create_shader_module(smr).shader_module;
    VKR_CHECK(cs != 0);
    auto make_pipeline = [&](std::uint64_t dsl_for_layout, std::uint64_t& out_pl) {
        vkrpc::CreatePipelineLayoutRequest plr;
        plr.device = dev_b.device;
        plr.set_layout_count = 1;
        plr.push_constant_range_count = 0;
        plr.set_layouts = {dsl_for_layout};
        out_pl = backend.create_pipeline_layout(plr).pipeline_layout;
        vkrpc::CreateComputePipelinesRequest r;
        r.device = dev_b.device;
        r.pipeline_cache = 0;
        r.layout = out_pl;
        r.shader_module = cs;
        r.entry_point = "main";
        return backend.create_compute_pipelines(r).pipeline;
    };
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = dev_b.device;
    cpr.queue_family_index = dev_b.queue_family_index;
    const std::uint64_t cpool = backend.create_command_pool(cpr).command_pool;
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = cpool;
    acb.count = 4;
    const auto cbs = backend.allocate_command_buffers(acb);
    VKR_CHECK(cbs.ok);
    vkrpc::GetDeviceQueueRequest gqr;
    gqr.device = dev_b.device;
    gqr.queue_family_index = dev_b.queue_family_index;
    gqr.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(gqr).queue;
    auto record_dispatch = [&](std::uint64_t cmd, std::uint64_t pipeline, std::uint64_t pl,
                               std::uint64_t set) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        vkrpc::RecordedCommand bp;
        bp.kind = "bind_pipeline";
        bp.pipeline = pipeline;
        bp.args_i64 = {1};
        vkrpc::RecordedCommand bs;
        bs.kind = "bind_descriptor_sets";
        bs.desc_layout = pl;
        bs.first_set = 0;
        bs.descriptor_sets = {set};
        bs.args_i64 = {1};
        vkrpc::RecordedCommand d;
        d.kind = "dispatch";
        d.args_u64 = {1, 1, 1};
        r.commands = {bp, bs, d};
        return backend.record_command_buffer(r);
    };
    auto submit = [&](std::uint64_t cmd) {
        vkrpc::QueueSubmitRequest s;
        s.queue = queue;
        s.command_buffers = {cmd};
        return backend.queue_submit(s);
    };
    // CLASSIC: an unwritten set still refuses at record (the fail-closed baseline holds).
    std::uint64_t pl_classic = 0;
    const std::uint64_t cp_classic = make_pipeline(dsl_classic, pl_classic);
    const auto classic_alloc = alloc_sets(pool_plain, {dsl_classic}, {});
    VKR_CHECK(classic_alloc.ok);
    const std::uint64_t set_classic = classic_alloc.descriptor_sets.front();
    VKR_CHECK(!record_dispatch(cbs.command_buffers[0], cp_classic, pl_classic, set_classic).ok);
    // PARTIALLY_BOUND: an unwritten set records fine (host-owned completeness).
    std::uint64_t pl_pb = 0;
    const std::uint64_t cp_pb = make_pipeline(dsl_pb, pl_pb);
    const auto pb_alloc = alloc_sets(pool_plain, {dsl_pb}, {});
    VKR_CHECK(pb_alloc.ok);
    const std::uint64_t set_pb = pb_alloc.descriptor_sets.front();
    VKR_CHECK(record_dispatch(cbs.command_buffers[1], cp_pb, pl_pb, set_pb).ok);
    // UPDATE_AFTER_BIND: an unwritten set records fine; a SUCCESSFUL update after the record does
    // NOT invalidate (the record-then-update-then-submit ordering the feature exists for).
    std::uint64_t pl_uab = 0;
    const std::uint64_t cp_uab = make_pipeline(dsl_uab, pl_uab);
    VKR_CHECK(record_dispatch(cbs.command_buffers[2], cp_uab, pl_uab, set_uab).ok);
    VKR_CHECK(write_slot(set_uab, 0, 0, make_buf()).ok);
    VKR_CHECK(write_slot(set_uab, 0, 1, make_buf()).ok);
    VKR_CHECK(submit(cbs.command_buffers[2]).ok);
    // The dangle exemption: destroying a PARTIALLY_BOUND slot's referent does
    // NOT invalidate the recorded CB (its slot is cleared, readiness skips it, the submit lands);
    // a CLASSIC set's referent destroy still invalidates.
    const std::uint64_t pb_buf = make_buf();
    VKR_CHECK(write_slot(set_pb, 0, 0, pb_buf).ok);
    VKR_CHECK(record_dispatch(cbs.command_buffers[1], cp_pb, pl_pb, set_pb).ok);
    {
        vkrpc::HandleRequest db;
        db.handle = pb_buf;
        VKR_CHECK(backend.destroy_buffer(db).ok);
    }
    VKR_CHECK(submit(cbs.command_buffers[1]).ok); // NOT invalidated (PARTIALLY_BOUND)
    const std::uint64_t classic_buf = make_buf();
    VKR_CHECK(write_slot(set_classic, 0, 0, classic_buf).ok);
    VKR_CHECK(record_dispatch(cbs.command_buffers[0], cp_classic, pl_classic, set_classic).ok);
    {
        vkrpc::HandleRequest db;
        db.handle = classic_buf;
        VKR_CHECK(backend.destroy_buffer(db).ok);
    }
    VKR_CHECK(!submit(cbs.command_buffers[0]).ok); // classic: invalidated (fail-closed baseline)
}

void test_inline_uniform_block_mock() {
    // Vulkan 1.3 support (inlineUniformBlock): the IUB descriptor plumbing -- feature-gated
    // byte-sized layouts, the pool pNext pairing, byte-offset/byte-count writes carrying raw
    // inline_data, and the wire round-trip (mock == real gates).
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const auto en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest base;
    base.instance = ci.instance;
    base.physical_device = en.devices.front().handle;
    const auto dev_off = backend.create_device(base); // WITHOUT the inlineUniformBlock bit
    VKR_CHECK(dev_off.ok);
    vkrpc::CreateDeviceRequest on = base;
    on.vk13_feature_bits = vkrpc::kVk13FeatureServedBits;
    on.vk13_device_enabled = 1;
    const auto dev_on = backend.create_device(on); // WITH the served vk13 set (IUB included)
    // Required-feature audit (mock == real): while the relay-served gate is OPEN
    // (multiview / hostQueryReset not both served) the native lane is 1.2, so the vk13 DEVICE --
    // and with it every 1.3-only feature like inlineUniformBlock -- is fail-closed. The honest
    // oracle REJECTS it, so the IUB exercise below is genuinely unreachable until the gate closes.
    // Assert the rejection and return; the full exercise
    // (and the real GPU's Vulkan 1.3 canary) run once the device is actually 1.3.
    if (!vkrpc::kRelayServesFullVk13RequiredMatrix) {
        VKR_CHECK(!dev_on.ok);
        return;
    }
    VKR_CHECK(dev_on.ok);
    // maintenance4 create-info query and create must agree for the guarded usage shape. The mock
    // has no private host guard, but this pins the invariant at the backend contract boundary.
    {
        vkrpc::CreateBufferRequest r;
        r.device = dev_on.device;
        r.size = 1920;
        r.usage = vkrpc::kBufferUsageVertexBuffer;
        r.sharing_mode = 0;
        const auto queried = backend.get_device_buffer_memory_requirements(r);
        const auto created = backend.create_buffer(r);
        VKR_CHECK(queried.ok && created.ok);
        VKR_CHECK_EQ(queried.mem_size, created.mem_size);
        VKR_CHECK_EQ(queried.mem_alignment, created.mem_alignment);
        VKR_CHECK_EQ(queried.mem_type_bits, created.mem_type_bits);
    }
    auto mkb = [](int binding, int type, int count) {
        vkrpc::DescriptorSetLayoutBindingDesc d;
        d.binding = binding;
        d.descriptor_type = type;
        d.descriptor_count = count;
        d.stage_flags = 0x20; // COMPUTE
        d.binding_flags = 0;
        return d;
    };
    auto make_layout = [&](std::uint64_t device, int count) {
        vkrpc::CreateDescriptorSetLayoutRequest r;
        r.device = device;
        r.bindings = {mkb(0, vkrpc::kDescriptorTypeInlineUniformBlock, count)};
        return backend.create_descriptor_set_layout(r);
    };
    // Layout admission: feature-gated; descriptorCount is a BYTE size (positive multiple of 4,
    // bounded by kMaxInlineUniformBlockBytes).
    VKR_CHECK(!make_layout(dev_off.device, 16).ok); // feature off -> reject
    VKR_CHECK(!make_layout(dev_on.device, 15).ok);  // not a multiple of 4
    VKR_CHECK(!make_layout(dev_on.device, 0).ok);   // empty byte size
    VKR_CHECK(!make_layout(dev_on.device, vkrpc::kMaxInlineUniformBlockBytes + 4).ok);
    const auto dsl = make_layout(dev_on.device, 16);
    VKR_CHECK(dsl.ok);
    // The layout-support query mirrors the admission (never "supported" then rejected).
    {
        vkrpc::CreateDescriptorSetLayoutRequest r;
        r.device = dev_off.device;
        r.bindings = {mkb(0, vkrpc::kDescriptorTypeInlineUniformBlock, 16)};
        const auto sup = backend.get_descriptor_set_layout_support(r);
        VKR_CHECK(sup.ok);
        VKR_CHECK_EQ(sup.supported, 0);
    }
    // Pool admission: an IUB pool size (a BYTE budget, %4) requires the
    // VkDescriptorPoolInlineUniformBlockCreateInfo pairing + the feature.
    auto make_pool = [&](std::uint64_t device, int iub_bytes, int max_iub_bindings) {
        vkrpc::CreateDescriptorPoolRequest r;
        r.device = device;
        r.max_sets = 4;
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeInlineUniformBlock, iub_bytes});
        r.max_inline_uniform_block_bindings = max_iub_bindings;
        return backend.create_descriptor_pool(r);
    };
    VKR_CHECK(!make_pool(dev_on.device, 64, 0).ok);  // IUB pool size without the pNext
    VKR_CHECK(!make_pool(dev_on.device, 63, 1).ok);  // byte budget not a multiple of 4
    VKR_CHECK(!make_pool(dev_off.device, 64, 1).ok); // feature off -> reject
    const auto pool = make_pool(dev_on.device, 64, 1);
    VKR_CHECK(pool.ok);
    // Allocate a set: the IUB binding's slots are its 16 BYTES.
    vkrpc::AllocateDescriptorSetsRequest ar;
    ar.device = dev_on.device;
    ar.pool = pool.pool;
    ar.set_layouts = {dsl.set_layout};
    const auto alloc = backend.allocate_descriptor_sets(ar);
    VKR_CHECK(alloc.ok);
    const std::uint64_t set = alloc.descriptor_sets.front();
    auto write = [&](int offset, int count, std::string bytes) {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = dev_on.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = set;
        w.dst_binding = 0;
        w.dst_array_element = offset;
        w.descriptor_type = vkrpc::kDescriptorTypeInlineUniformBlock;
        w.descriptor_count = count;
        w.inline_data = std::move(bytes);
        r.writes.push_back(std::move(w));
        return backend.update_descriptor_sets(r);
    };
    VKR_CHECK(write(4, 8, std::string(8, 'x')).ok);   // 8 bytes at byte offset 4
    VKR_CHECK(!write(2, 8, std::string(8, 'x')).ok);  // offset not a multiple of 4
    VKR_CHECK(!write(0, 8, std::string(7, 'x')).ok);  // inline_data.size() != descriptorCount
    VKR_CHECK(!write(12, 8, std::string(8, 'x')).ok); // 12 + 8 > the 16-byte binding
    // An IUB write must not carry structured infos alongside its bytes.
    {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = dev_on.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = set;
        w.dst_binding = 0;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeInlineUniformBlock;
        w.descriptor_count = 4;
        w.inline_data = std::string(4, 'x');
        w.buffer_infos.push_back({0, 0, 4});
        r.writes.push_back(std::move(w));
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
    }
    // A non-IUB write aimed at the IUB binding rejects (the type agreement is bidirectional --
    // pre-gate, an image-class write with null handles would have marked byte-slots written).
    {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = dev_on.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = set;
        w.dst_binding = 0;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeSampler;
        w.descriptor_count = 1;
        w.image_infos.push_back({0, 0, 0});
        r.writes.push_back(std::move(w));
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
    }
    // Wire round-trip: raw inline bytes (incl. NUL + 0xFF) survive to_body/from_body (hex).
    {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = 7;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = 9;
        w.dst_binding = 0;
        w.dst_array_element = 4;
        w.descriptor_type = vkrpc::kDescriptorTypeInlineUniformBlock;
        w.descriptor_count = 3;
        w.inline_data = std::string("\x00\x01\xff", 3);
        r.writes.push_back(w);
        const auto rt = vkrpc::UpdateDescriptorSetsRequest::from_body(r.to_body());
        VKR_CHECK_EQ(rt.writes.size(), std::size_t{1});
        VKR_CHECK_EQ(rt.writes[0].inline_data, std::string("\x00\x01\xff", 3));
        VKR_CHECK_EQ(rt.writes[0].descriptor_count, 3);
        VKR_CHECK_EQ(rt.writes[0].dst_array_element, 4);
        VKR_CHECK_EQ(rt.writes[0].descriptor_type, vkrpc::kDescriptorTypeInlineUniformBlock);
    }
}

void test_allocate_memory_type_index_required() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    // A missing or wrong-typed memory_type_index decodes to a negative sentinel
    // (not 0), so allocate_memory rejects it rather than silently using type 0.
    // (The index check precedes the device lookup, so no real device is needed.)
    json::Value missing_body = json::Value::make_object();
    missing_body.set("device", json::Value(std::string("1")));
    missing_body.set("allocation_size", json::Value(std::string("16")));
    const vkrpc::AllocateMemoryRequest missing =
        vkrpc::AllocateMemoryRequest::from_body(missing_body);
    VKR_CHECK(missing.memory_type_index < 0);
    VKR_CHECK(!backend.allocate_memory(missing).ok);

    json::Value wrong_body = missing_body;
    wrong_body.set("memory_type_index", json::Value(std::string("oops")));
    const vkrpc::AllocateMemoryRequest wrong = vkrpc::AllocateMemoryRequest::from_body(wrong_body);
    VKR_CHECK(wrong.memory_type_index < 0);
    VKR_CHECK(!backend.allocate_memory(wrong).ok);

    // An explicitly-sent index 0 is a valid unsigned index and is accepted.
    const std::uint64_t device = make_device(backend);
    VKR_CHECK(device != 0);
    vkrpc::AllocateMemoryRequest ok_req;
    ok_req.device = device;
    ok_req.allocation_size = 64;
    ok_req.memory_type_index = 0;
    VKR_CHECK(backend.allocate_memory(ok_req).ok);
}

void test_get_device_queue_family_required() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    // A missing / wrong-typed family or index decodes to a negative sentinel (not
    // 0), so get_device_queue rejects it rather than treating it as the created
    // (family 0, index 0): vkGetDeviceQueue on an unrequested family/index is
    // undefined behavior on the real backend.
    json::Value missing = json::Value::make_object();
    missing.set("device", json::Value(std::string("1")));
    const vkrpc::GetDeviceQueueRequest md = vkrpc::GetDeviceQueueRequest::from_body(missing);
    VKR_CHECK(md.queue_family_index < 0);
    VKR_CHECK(md.queue_index < 0);

    // An out-of-int-range wire value is rejected to the sentinel, never truncated
    // into a small value that could masquerade as a created (family, index).
    json::Value huge = missing;
    huge.set("queue_family_index", json::Value(static_cast<long long>(1LL << 40)));
    huge.set("queue_index", json::Value(static_cast<long long>(1LL << 40)));
    const vkrpc::GetDeviceQueueRequest hd = vkrpc::GetDeviceQueueRequest::from_body(huge);
    VKR_CHECK(hd.queue_family_index < 0);
    VKR_CHECK(hd.queue_index < 0);

    // On a live device, that smuggled request is rejected (not retrieved as 0,0).
    const vkrpc::CreateDeviceResponse cd = make_device_resp(backend);
    VKR_CHECK(cd.device != 0);
    vkrpc::GetDeviceQueueRequest smuggled = hd;
    smuggled.device = cd.device;
    VKR_CHECK(!backend.get_device_queue(smuggled).ok);
}

void test_presentation_spine() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    VKR_CHECK(ci.ok);
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    VKR_CHECK(en.ok && !en.devices.empty());
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    VKR_CHECK(cd.device != 0);

    // surface: instance child; an unknown instance is rejected.
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);
    VKR_CHECK(surf.surface != 0);
    vkrpc::CreateSurfaceRequest sr_bad;
    sr_bad.instance = ci.instance + 999;
    VKR_CHECK(!backend.create_surface(sr_bad).ok);

    // swapchain: device child targeting a surface; unknown device/surface rejected.
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.width = 1280;
    scr.height = 720;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    VKR_CHECK(sc.ok);
    VKR_CHECK(sc.swapchain != 0);
    vkrpc::CreateSwapchainRequest scr_baddev = scr;
    scr_baddev.device = cd.device + 999;
    VKR_CHECK(!backend.create_swapchain(scr_baddev).ok);
    vkrpc::CreateSwapchainRequest scr_badsurf = scr;
    scr_badsurf.surface = surf.surface + 999;
    VKR_CHECK(!backend.create_swapchain(scr_badsurf).ok);

    // Malformed params (decoder -1 sentinels / count < 1 / zero usage) are rejected, not
    // silently substituted -- consistent with the real backend's stricter caps validation.
    vkrpc::CreateSwapchainRequest scr_sentinel = scr;
    scr_sentinel.present_mode = -1;
    VKR_CHECK(!backend.create_swapchain(scr_sentinel).ok);
    vkrpc::CreateSwapchainRequest scr_badcount = scr;
    scr_badcount.min_image_count = -1;
    VKR_CHECK(!backend.create_swapchain(scr_badcount).ok);
    vkrpc::CreateSwapchainRequest scr_nousage = scr;
    scr_nousage.image_usage = 0; // the app must request at least one usage bit
    VKR_CHECK(!backend.create_swapchain(scr_nousage).ok);

    // A surface from a different instance than the device is rejected: build a
    // second instance + device, and try to swap its device against this surface.
    const vkrpc::CreateInstanceResponse ci2 = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er2;
    er2.instance = ci2.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en2 = backend.enumerate_physical_devices(er2);
    VKR_CHECK(en2.ok && !en2.devices.empty());
    vkrpc::CreateDeviceRequest cdr2;
    cdr2.instance = ci2.instance;
    cdr2.physical_device = en2.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd2 = backend.create_device(cdr2);
    VKR_CHECK(cd2.device != 0);
    vkrpc::CreateSwapchainRequest scr_xinst = scr;
    scr_xinst.device = cd2.device; // device from ci2, surface from ci
    VKR_CHECK(!backend.create_swapchain(scr_xinst).ok);

    // get_swapchain_images: count derives from min_image_count (>=2), handles are
    // non-null, stable/idempotent across calls; unknown swapchain rejected; the
    // response body round-trips (images array).
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse gi = backend.get_swapchain_images(gir);
    VKR_CHECK(gi.ok);
    VKR_CHECK_EQ(static_cast<int>(gi.images.size()), 2);
    for (const std::uint64_t img : gi.images) {
        VKR_CHECK(img != 0);
    }
    const vkrpc::GetSwapchainImagesResponse gi2 = backend.get_swapchain_images(gir);
    VKR_CHECK(gi2.images == gi.images); // stable
    const vkrpc::GetSwapchainImagesResponse gi_rt =
        vkrpc::GetSwapchainImagesResponse::from_body(gi.to_body());
    VKR_CHECK(gi_rt.images == gi.images);
    vkrpc::GetSwapchainImagesRequest gir_bad;
    gir_bad.swapchain = sc.swapchain + 999;
    VKR_CHECK(!backend.get_swapchain_images(gir_bad).ok);

    // Ordering: a surface/device/instance with a live child can't be destroyed.
    vkrpc::HandleRequest h_surf;
    h_surf.handle = surf.surface;
    VKR_CHECK(!backend.destroy_surface(h_surf).ok); // live swapchain targets it
    vkrpc::HandleRequest h_dev;
    h_dev.handle = cd.device;
    VKR_CHECK(!backend.destroy_device(h_dev).ok); // live swapchain
    vkrpc::HandleRequest h_inst;
    h_inst.handle = ci.instance;
    VKR_CHECK(!backend.destroy_instance(h_inst).ok); // live device + surface

    // Ordered teardown: swapchain -> surface -> device -> instance.
    vkrpc::HandleRequest h_swc;
    h_swc.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h_swc).ok);
    VKR_CHECK(!backend.destroy_instance(h_inst).ok); // still a live device + surface
    VKR_CHECK(backend.destroy_surface(h_surf).ok);
    VKR_CHECK(backend.destroy_device(h_dev).ok);
    VKR_CHECK(backend.destroy_instance(h_inst).ok);

    // Unknown / already-freed handles are rejected.
    VKR_CHECK(!backend.destroy_swapchain(h_swc).ok);
    VKR_CHECK(!backend.destroy_surface(h_surf).ok);
}

void test_swapchain_params_decode() {
    // The swapchain extents/counts/enums decode wide: a missing or out-of-int-range
    // wire value becomes a -1 sentinel, never a truncated plausible small int, so
    // the real backend can reject it against surface caps later.
    json::Value missing = json::Value::make_object();
    missing.set("device", json::Value(std::string("1")));
    missing.set("surface", json::Value(std::string("2")));
    const vkrpc::CreateSwapchainRequest md = vkrpc::CreateSwapchainRequest::from_body(missing);
    VKR_CHECK(md.width < 0);
    VKR_CHECK(md.height < 0);
    VKR_CHECK(md.min_image_count < 0);
    VKR_CHECK(md.image_format < 0);
    VKR_CHECK(md.present_mode < 0);
    VKR_CHECK(md.image_usage < 0); // missing usage -> sentinel (rejected, not defaulted)

    json::Value huge = missing;
    huge.set("width", json::Value(static_cast<long long>(1LL << 40)));
    huge.set("height", json::Value(static_cast<long long>(1LL << 40)));
    huge.set("min_image_count", json::Value(static_cast<long long>(1LL << 40)));
    const vkrpc::CreateSwapchainRequest hd = vkrpc::CreateSwapchainRequest::from_body(huge);
    VKR_CHECK(hd.width < 0); // not truncated into a small valid-looking extent
    VKR_CHECK(hd.height < 0);
    VKR_CHECK(hd.min_image_count < 0);

    // An explicitly-sent in-range value is preserved.
    json::Value ok = missing;
    ok.set("width", json::Value(1920));
    ok.set("height", json::Value(1080));
    const vkrpc::CreateSwapchainRequest okd = vkrpc::CreateSwapchainRequest::from_body(ok);
    VKR_CHECK_EQ(okd.width, 1920);
    VKR_CHECK_EQ(okd.height, 1080);
}

// Acquire + present over the mock, and the dirty-latch state machine (the real
// backend sets the latch from a Win32 WM_SIZE, which can't build on WSL; this proves
// the SET -> acquire/present OUT_OF_DATE -> create_swapchain CLEARS state machine on
// both platforms via the debug_mark_surface_dirty seam).
void test_acquire_present_latch() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gq);
    VKR_CHECK(q.ok && q.queue != 0);
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = 44;
    scr.color_space = 0;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 3;
    scr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    VKR_CHECK(sc.ok);
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok);
    const int n = static_cast<int>(imgs.images.size());
    VKR_CHECK(n >= 2);

    // Clean acquire: SUCCESS, in-range image index, rotates across calls.
    vkrpc::AcquireNextImageRequest air;
    air.swapchain = sc.swapchain;
    const vkrpc::AcquireNextImageResponse a0 = backend.acquire_next_image(air);
    VKR_CHECK(a0.ok);
    VKR_CHECK_EQ(a0.result, vkrpc::kVkSuccess);
    VKR_CHECK(a0.image_index >= 0 && a0.image_index < n);
    const vkrpc::AcquireNextImageResponse a1 = backend.acquire_next_image(air);
    VKR_CHECK_EQ(a1.result, vkrpc::kVkSuccess);
    VKR_CHECK_EQ(a1.image_index, (a0.image_index + 1) % n);

    // present: SUCCESS, per-target results parallel the request.
    vkrpc::QueuePresentRequest pr;
    pr.queue = q.queue;
    pr.presents.push_back({sc.swapchain, a0.image_index});
    const vkrpc::QueuePresentResponse p0 = backend.queue_present(pr);
    VKR_CHECK(p0.ok);
    VKR_CHECK_EQ(p0.result, vkrpc::kVkSuccess);
    VKR_CHECK_EQ(static_cast<int>(p0.results.size()), 1);
    VKR_CHECK_EQ(p0.results[0], vkrpc::kVkSuccess);

    // Wait semaphores are validated against the present queue's device (mirrors the real
    // backend): a same-device semaphore is accepted; one from another device is rejected.
    vkrpc::CreateSemaphoreRequest csA;
    csA.device = cd.device;
    const vkrpc::CreateSemaphoreResponse semA = backend.create_semaphore(csA);
    VKR_CHECK(semA.ok);
    vkrpc::QueuePresentRequest pr_sem = pr;
    pr_sem.wait_semaphores = {semA.semaphore};
    VKR_CHECK(backend.queue_present(pr_sem).ok);
    vkrpc::CreateDeviceRequest cdrB;
    cdrB.instance = ci.instance;
    cdrB.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cdB = backend.create_device(cdrB);
    VKR_CHECK(cdB.ok);
    vkrpc::CreateSemaphoreRequest csB;
    csB.device = cdB.device;
    const vkrpc::CreateSemaphoreResponse semB = backend.create_semaphore(csB);
    VKR_CHECK(semB.ok);
    vkrpc::QueuePresentRequest pr_xsem = pr;
    pr_xsem.wait_semaphores = {semB.semaphore}; // semaphore on device B, queue on device A
    VKR_CHECK(!backend.queue_present(pr_xsem).ok);

    // RPC/handle faults are ok=false (the op did not run), distinct from a VkResult.
    vkrpc::AcquireNextImageRequest air_bad = air;
    air_bad.swapchain = sc.swapchain + 999;
    VKR_CHECK(!backend.acquire_next_image(air_bad).ok);
    vkrpc::QueuePresentRequest pr_badq = pr;
    pr_badq.queue = q.queue + 999;
    VKR_CHECK(!backend.queue_present(pr_badq).ok);
    vkrpc::QueuePresentRequest pr_badidx = pr;
    pr_badidx.presents[0].image_index = n; // out of range
    VKR_CHECK(!backend.queue_present(pr_badidx).ok);
    vkrpc::QueuePresentRequest pr_empty;
    pr_empty.queue = q.queue; // no targets
    VKR_CHECK(!backend.queue_present(pr_empty).ok);

    // mark dirty -> acquire/present return ok=true, OUT_OF_DATE, no driver, no
    // rotation advance; a recreate (create_swapchain) clears the latch.
    backend.debug_mark_surface_dirty(surf.surface);
    const vkrpc::AcquireNextImageResponse a_dirty = backend.acquire_next_image(air);
    VKR_CHECK(a_dirty.ok);
    VKR_CHECK_EQ(a_dirty.result, vkrpc::kVkErrorOutOfDateKhr);
    const vkrpc::QueuePresentResponse p_dirty = backend.queue_present(pr);
    VKR_CHECK(p_dirty.ok);
    VKR_CHECK_EQ(p_dirty.result, vkrpc::kVkErrorOutOfDateKhr);
    VKR_CHECK_EQ(p_dirty.results[0], vkrpc::kVkErrorOutOfDateKhr);

    vkrpc::HandleRequest h_swc;
    h_swc.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h_swc).ok);
    const vkrpc::CreateSwapchainResponse sc2 = backend.create_swapchain(scr); // clears the latch
    VKR_CHECK(sc2.ok);
    vkrpc::GetSwapchainImagesRequest gir2;
    gir2.swapchain = sc2.swapchain;
    VKR_CHECK(backend.get_swapchain_images(gir2).ok);
    vkrpc::AcquireNextImageRequest air2;
    air2.swapchain = sc2.swapchain;
    const vkrpc::AcquireNextImageResponse a_clean = backend.acquire_next_image(air2);
    VKR_CHECK(a_clean.ok);
    VKR_CHECK_EQ(a_clean.result, vkrpc::kVkSuccess);

    // Wire round-trips for the new messages (incl. the signed VkResult + per-target array).
    const vkrpc::AcquireNextImageRequest air_rt =
        vkrpc::AcquireNextImageRequest::from_body(air.to_body());
    VKR_CHECK(air_rt.swapchain == air.swapchain);
    vkrpc::QueuePresentResponse pr_resp;
    pr_resp.ok = true;
    pr_resp.result = vkrpc::kVkSuboptimalKhr;
    pr_resp.results = {vkrpc::kVkSuccess, vkrpc::kVkErrorOutOfDateKhr};
    const vkrpc::QueuePresentResponse pr_resp_rt =
        vkrpc::QueuePresentResponse::from_body(pr_resp.to_body());
    VKR_CHECK_EQ(pr_resp_rt.result, vkrpc::kVkSuboptimalKhr);
    VKR_CHECK_EQ(static_cast<int>(pr_resp_rt.results.size()), 2);
    VKR_CHECK_EQ(pr_resp_rt.results[1], vkrpc::kVkErrorOutOfDateKhr);

    // CreateSwapchainResponse carries a signed VkResult: a
    // non-converging surface comes back ok=true + result=OUT_OF_DATE. The field round-trips, and an
    // absent "result" decodes to VK_SUCCESS (back-compat with an old/silent worker).
    vkrpc::CreateSwapchainResponse sc_ood;
    sc_ood.ok = true;
    sc_ood.swapchain = 0;
    sc_ood.result = vkrpc::kVkErrorOutOfDateKhr;
    const vkrpc::CreateSwapchainResponse sc_ood_rt =
        vkrpc::CreateSwapchainResponse::from_body(sc_ood.to_body());
    VKR_CHECK(sc_ood_rt.ok);
    VKR_CHECK_EQ(sc_ood_rt.result, vkrpc::kVkErrorOutOfDateKhr);
    json::Value legacy = json::Value::make_object(); // no "result" key -> VK_SUCCESS
    legacy.set("ok", json::Value(true));
    legacy.set("swapchain", json::Value(std::string("7")));
    VKR_CHECK_EQ(vkrpc::CreateSwapchainResponse::from_body(legacy).result, vkrpc::kVkSuccess);
}

// The present image index decodes wide: a missing / out-of-range value becomes a -1
// sentinel (rejected against image_count), never a truncated plausible index.
void test_present_index_decode() {
    json::Value entry = json::Value::make_object();
    entry.set("swapchain", json::Value(std::string("7")));
    json::Array arr;
    arr.emplace_back(entry);
    json::Value body = json::Value::make_object();
    body.set("queue", json::Value(std::string("3")));
    body.set("presents", json::Value(std::move(arr)));
    const vkrpc::QueuePresentRequest md = vkrpc::QueuePresentRequest::from_body(body);
    VKR_CHECK_EQ(static_cast<int>(md.presents.size()), 1);
    VKR_CHECK(md.presents[0].image_index < 0); // missing -> sentinel

    json::Value huge_entry = json::Value::make_object();
    huge_entry.set("swapchain", json::Value(std::string("7")));
    huge_entry.set("image_index", json::Value(static_cast<long long>(1LL << 40)));
    json::Array huge_arr;
    huge_arr.emplace_back(huge_entry);
    json::Value huge = json::Value::make_object();
    huge.set("queue", json::Value(std::string("3")));
    huge.set("presents", json::Value(std::move(huge_arr)));
    const vkrpc::QueuePresentRequest hd = vkrpc::QueuePresentRequest::from_body(huge);
    VKR_CHECK(hd.presents[0].image_index < 0); // not truncated into a small valid index
}

// Command recording + queue submit over the mock: the record/submit state
// machine (validate-then-record, recorded-before-submit), device-ownership checks, the
// boring sync cases (empty-CB fence-only submit, sync1 stage masks), and reset/wait fence
// validation -- all dual-platform (no GPU).
void test_command_record_submit() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gq);
    VKR_CHECK(q.ok);

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = 44;
    scr.color_space = 0;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    VKR_CHECK(sc.ok);
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());

    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    VKR_CHECK(pool.ok);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs.ok && bufs.command_buffers.size() == 1);
    const std::uint64_t cmd = bufs.command_buffers[0];

    // VkPipelineStage / layout values the recording uses (named here so the intent is
    // clear without pulling in <vulkan/vulkan.h>, which the dual-platform test can't).
    constexpr long long kStageTopOfPipe = 0x00000001;
    constexpr long long kStageTransfer = 0x00001000;
    constexpr long long kAccessTransferWrite = 0x00001000;
    constexpr int kLayoutUndefined = 0;
    constexpr int kLayoutTransferDst = 7;
    constexpr int kAspectColor = 1;

    // A barrier + clear recording over the swapchain image: ok, flips `recorded`.
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    rec.one_time_submit = true;
    vkrpc::RecordedCommand barrier;
    barrier.kind = "pipeline_barrier";
    barrier.image = imgs.images[0];
    barrier.old_layout = kLayoutUndefined;
    barrier.new_layout = kLayoutTransferDst;
    barrier.src_stage = kStageTopOfPipe;
    barrier.dst_stage = kStageTransfer;
    barrier.src_access = 0;
    barrier.dst_access = kAccessTransferWrite;
    barrier.aspect = kAspectColor;
    rec.commands.push_back(barrier);
    vkrpc::RecordedCommand clear;
    clear.kind = "clear_color_image";
    clear.image = imgs.images[0];
    clear.layout = kLayoutTransferDst;
    clear.r = 0.1;
    clear.g = 0.5;
    clear.b = 1.0;
    clear.a = 1.0;
    rec.commands.push_back(clear);
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    // Submit the recorded buffer: ok, honest VkResult.
    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    sub.command_buffers = {cmd};
    const vkrpc::QueueSubmitResponse s0 = backend.queue_submit(sub);
    VKR_CHECK(s0.ok);
    VKR_CHECK_EQ(s0.result, vkrpc::kVkSuccess);

    // A never-recorded buffer is rejected by submit.
    const vkrpc::AllocateCommandBuffersResponse bufs2 = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs2.ok);
    vkrpc::QueueSubmitRequest sub_unrec;
    sub_unrec.queue = q.queue;
    sub_unrec.command_buffers = {bufs2.command_buffers[0]};
    VKR_CHECK(!backend.queue_submit(sub_unrec).ok);

    // Boring sync: an empty-CB, fence-only submit is valid.
    vkrpc::CreateFenceRequest cfr;
    cfr.device = cd.device;
    const vkrpc::CreateFenceResponse fence = backend.create_fence(cfr);
    VKR_CHECK(fence.ok);
    vkrpc::QueueSubmitRequest sub_fenceonly;
    sub_fenceonly.queue = q.queue;
    sub_fenceonly.fence = fence.fence;
    VKR_CHECK(backend.queue_submit(sub_fenceonly).ok);

    // sync1: a wait semaphore with a zero stage mask is rejected; a non-zero one is fine.
    vkrpc::CreateSemaphoreRequest csr;
    csr.device = cd.device;
    const vkrpc::CreateSemaphoreResponse sem = backend.create_semaphore(csr);
    VKR_CHECK(sem.ok);
    vkrpc::QueueSubmitRequest sub_badstage;
    sub_badstage.queue = q.queue;
    sub_badstage.waits.push_back({sem.semaphore, 0});
    VKR_CHECK(!backend.queue_submit(sub_badstage).ok);
    vkrpc::QueueSubmitRequest sub_goodstage;
    sub_goodstage.queue = q.queue;
    sub_goodstage.waits.push_back({sem.semaphore, kStageTransfer});
    sub_goodstage.signal_semaphores = {sem.semaphore};
    VKR_CHECK(backend.queue_submit(sub_goodstage).ok);

    // A cross-device sync object is rejected (mirrors present's check).
    vkrpc::CreateDeviceRequest cdrB;
    cdrB.instance = ci.instance;
    cdrB.physical_device = en.devices.front().handle;
    const vkrpc::CreateDeviceResponse cdB = backend.create_device(cdrB);
    VKR_CHECK(cdB.ok);
    vkrpc::CreateSemaphoreRequest csB;
    csB.device = cdB.device;
    const vkrpc::CreateSemaphoreResponse semB = backend.create_semaphore(csB);
    vkrpc::QueueSubmitRequest sub_xdev;
    sub_xdev.queue = q.queue;
    sub_xdev.waits.push_back({semB.semaphore, kStageTransfer});
    VKR_CHECK(!backend.queue_submit(sub_xdev).ok);

    // RPC faults: unknown queue / unknown command buffer.
    vkrpc::QueueSubmitRequest sub_badq = sub;
    sub_badq.queue = q.queue + 999;
    VKR_CHECK(!backend.queue_submit(sub_badq).ok);
    vkrpc::QueueSubmitRequest sub_badcb;
    sub_badcb.queue = q.queue;
    sub_badcb.command_buffers = {cmd + 99999};
    VKR_CHECK(!backend.queue_submit(sub_badcb).ok);

    // A clear on an image without TRANSFER_DST usage is rejected at record time: build a
    // COLOR-only swapchain and try to clear its image.
    vkrpc::CreateSwapchainRequest scr_color = scr;
    scr_color.image_usage = vkrpc::kImageUsageColorAttachment;
    const vkrpc::CreateSwapchainResponse sc_color = backend.create_swapchain(scr_color);
    VKR_CHECK(sc_color.ok);
    vkrpc::GetSwapchainImagesRequest gir_c;
    gir_c.swapchain = sc_color.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs_c = backend.get_swapchain_images(gir_c);
    VKR_CHECK(imgs_c.ok && !imgs_c.images.empty());
    vkrpc::RecordCommandBufferRequest rec_noclear;
    rec_noclear.command_buffer = cmd;
    vkrpc::RecordedCommand clear_noxd;
    clear_noxd.kind = "clear_color_image";
    clear_noxd.image = imgs_c.images[0];
    clear_noxd.layout = kLayoutTransferDst;
    rec_noclear.commands.push_back(clear_noxd);
    VKR_CHECK(!backend.record_command_buffer(rec_noclear).ok);

    // record faults: unknown command buffer, and an unknown referenced image.
    vkrpc::RecordCommandBufferRequest rec_badcb = rec;
    rec_badcb.command_buffer = cmd + 99999;
    VKR_CHECK(!backend.record_command_buffer(rec_badcb).ok);
    vkrpc::RecordCommandBufferRequest rec_badimg;
    rec_badimg.command_buffer = cmd;
    vkrpc::RecordedCommand clear_badimg;
    clear_badimg.kind = "clear_color_image";
    clear_badimg.image = 0; // unknown image
    clear_badimg.layout = kLayoutTransferDst;
    rec_badimg.commands.push_back(clear_badimg);
    VKR_CHECK(!backend.record_command_buffer(rec_badimg).ok);

    // Oversized stage/access masks are rejected (they would otherwise truncate into 32-bit
    // VkFlags on the real path), not just zero/missing ones -- in record AND submit.
    constexpr long long kOversizedMask = 1LL << 40;
    vkrpc::RecordCommandBufferRequest rec_bigstage;
    rec_bigstage.command_buffer = cmd;
    vkrpc::RecordedCommand barrier_bigstage = barrier; // otherwise-valid barrier...
    barrier_bigstage.src_stage = kOversizedMask;       // ...but an out-of-range stage mask
    rec_bigstage.commands.push_back(barrier_bigstage);
    VKR_CHECK(!backend.record_command_buffer(rec_bigstage).ok);
    vkrpc::RecordCommandBufferRequest rec_bigaccess;
    rec_bigaccess.command_buffer = cmd;
    vkrpc::RecordedCommand barrier_bigaccess = barrier;
    barrier_bigaccess.dst_access = kOversizedMask;
    rec_bigaccess.commands.push_back(barrier_bigaccess);
    VKR_CHECK(!backend.record_command_buffer(rec_bigaccess).ok);
    vkrpc::QueueSubmitRequest sub_bigstage;
    sub_bigstage.queue = q.queue;
    sub_bigstage.waits.push_back({sem.semaphore, kOversizedMask});
    VKR_CHECK(!backend.queue_submit(sub_bigstage).ok);

    // reset_fences / wait_for_fences: empty rejected; valid ok; cross-device rejected.
    vkrpc::ResetFencesRequest rf_empty;
    VKR_CHECK(!backend.reset_fences(rf_empty).ok);
    vkrpc::ResetFencesRequest rf;
    rf.fences = {fence.fence};
    VKR_CHECK(backend.reset_fences(rf).ok);
    vkrpc::WaitForFencesRequest wf;
    wf.fences = {fence.fence};
    wf.wait_all = true;
    wf.timeout = 0;
    const vkrpc::WaitForFencesResponse w0 = backend.wait_for_fences(wf);
    VKR_CHECK(w0.ok);
    VKR_CHECK_EQ(w0.result, vkrpc::kVkSuccess);
    vkrpc::CreateFenceRequest cfrB;
    cfrB.device = cdB.device;
    const vkrpc::CreateFenceResponse fenceB = backend.create_fence(cfrB);
    VKR_CHECK(fenceB.ok);
    vkrpc::WaitForFencesRequest wf_xdev;
    wf_xdev.fences = {fence.fence, fenceB.fence}; // span two devices
    VKR_CHECK(!backend.wait_for_fences(wf_xdev).ok);

    // Wire round-trips for the new messages (command list, submit waits, fence timeout).
    const vkrpc::RecordCommandBufferRequest rec_rt =
        vkrpc::RecordCommandBufferRequest::from_body(rec.to_body());
    VKR_CHECK_EQ(rec_rt.command_buffer, rec.command_buffer);
    VKR_CHECK_EQ(static_cast<int>(rec_rt.commands.size()), static_cast<int>(rec.commands.size()));
    VKR_CHECK(rec_rt.commands[0].kind == "pipeline_barrier");
    VKR_CHECK(rec_rt.commands[1].kind == "clear_color_image");
    VKR_CHECK_EQ(rec_rt.commands[0].dst_stage, barrier.dst_stage);
    const vkrpc::QueueSubmitRequest sub_rt =
        vkrpc::QueueSubmitRequest::from_body(sub_goodstage.to_body());
    VKR_CHECK_EQ(static_cast<int>(sub_rt.waits.size()), 1);
    VKR_CHECK_EQ(sub_rt.waits[0].stage, sub_goodstage.waits[0].stage);
    vkrpc::WaitForFencesResponse wresp;
    wresp.ok = true;
    wresp.result = vkrpc::kVkTimeout;
    const vkrpc::WaitForFencesResponse wresp_rt =
        vkrpc::WaitForFencesResponse::from_body(wresp.to_body());
    VKR_CHECK_EQ(wresp_rt.result, vkrpc::kVkTimeout);
}

// Destroying a swapchain invalidates a command buffer recorded against its images: the
// baked commands reference images that die with the swapchain, so a later queue_submit must
// be refused (the recorded latch is cleared) rather than handing the driver stale work.
void test_record_invalidated_by_destroy_swapchain() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = en.devices.front().handle;
    cdr.enabled_extensions = {vkrpc::kSync2ExtensionName};
    cdr.synchronization2_feature_enabled = 1;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gq);
    VKR_CHECK(q.ok);
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = 44;
    scr.color_space = 0;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment | vkrpc::kImageUsageTransferDst;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    VKR_CHECK(sc.ok);
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const vkrpc::GetSwapchainImagesResponse imgs = backend.get_swapchain_images(gir);
    VKR_CHECK(imgs.ok && !imgs.images.empty());
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse pool = backend.create_command_pool(cpr);
    vkrpc::AllocateCommandBuffersRequest abr;
    abr.command_pool = pool.command_pool;
    abr.count = 1;
    const vkrpc::AllocateCommandBuffersResponse bufs = backend.allocate_command_buffers(abr);
    VKR_CHECK(bufs.ok && bufs.command_buffers.size() == 1);
    const std::uint64_t cmd = bufs.command_buffers[0];

    const auto sync2_record = [&](std::uint64_t image) {
        vkrpc::ImageMemoryBarrier2 barrier;
        barrier.src_stage = 0x1;    // TOP_OF_PIPE
        barrier.dst_stage = 0x1000; // TRANSFER
        barrier.old_layout = 0;     // UNDEFINED
        barrier.new_layout = 7;     // TRANSFER_DST_OPTIMAL
        barrier.src_queue_family = vkrpc::kVkQueueFamilyIgnored;
        barrier.dst_queue_family = vkrpc::kVkQueueFamilyIgnored;
        barrier.image = image;
        barrier.aspect = vkrpc::kImageAspectColor;
        barrier.level_count = 1;
        barrier.layer_count = 1;
        vkrpc::DependencyInfo2 dependency;
        dependency.image.push_back(barrier);
        vkrpc::RecordedCommand command;
        command.kind = "pipeline_barrier2";
        command.deps2.push_back(dependency);
        vkrpc::RecordCommandBufferRequest request;
        request.command_buffer = cmd;
        request.commands.push_back(command);
        return request;
    };

    // Split the two pre-existing failure classes: a never-seen handle is distinct from a live
    // image owned by another device. The real backend mirrors these exact reason shapes.
    const std::uint64_t unknown_image = 0xDEADBEEF;
    const vkrpc::StatusResponse unknown =
        backend.record_command_buffer(sync2_record(unknown_image));
    VKR_CHECK(!unknown.ok);
    VKR_CHECK_EQ(unknown.reason, "sync2 image barrier references unknown image handle " +
                                     std::to_string(unknown_image) +
                                     " (never seen or tombstone expired)");

    vkrpc::CreateDeviceRequest other_cdr = cdr;
    const vkrpc::CreateDeviceResponse other_device = backend.create_device(other_cdr);
    VKR_CHECK(other_device.ok);
    vkrpc::CreateImageRequest other_ir;
    other_ir.device = other_device.device;
    other_ir.image_type = vkrpc::kImageType2D;
    other_ir.format = vkrpc::kFormatR8G8B8A8Unorm;
    other_ir.width = 1;
    other_ir.height = 1;
    other_ir.depth = 1;
    other_ir.mip_levels = 1;
    other_ir.array_layers = 1;
    other_ir.samples = 1;
    other_ir.tiling = vkrpc::kImageTilingOptimal;
    other_ir.usage = vkrpc::kImageUsageTransferDst;
    other_ir.sharing_mode = 0;
    other_ir.initial_layout = 0;
    const vkrpc::CreateImageResponse other_image = backend.create_image(other_ir);
    VKR_CHECK(other_image.ok);
    const vkrpc::StatusResponse wrong_device =
        backend.record_command_buffer(sync2_record(other_image.image));
    VKR_CHECK(!wrong_device.ok);
    VKR_CHECK_EQ(wrong_device.reason,
                 "sync2 image barrier references image " + std::to_string(other_image.image) +
                     " on device " + std::to_string(other_device.device) +
                     ", command buffer device is " + std::to_string(cd.device));

    // Capture a sync2 barrier while the image is live, but defer sending the request until after
    // swapchain destruction. This is the worker-side shape of the ICD's vkCmd* -> destroy ->
    // vkEndCommandBuffer batching window; it remains deliberately rejected.
    const vkrpc::RecordCommandBufferRequest stale_sync2 = sync2_record(imgs.images[0]);

    // Record a clear against the swapchain image, then submit OK.
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    vkrpc::RecordedCommand clear;
    clear.kind = "clear_color_image";
    clear.image = imgs.images[0];
    clear.layout = 7; // TRANSFER_DST_OPTIMAL
    rec.commands.push_back(clear);
    VKR_CHECK(backend.record_command_buffer(rec).ok);
    vkrpc::QueueSubmitRequest sub;
    sub.queue = q.queue;
    sub.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(sub).ok);

    // Destroy the swapchain -> the recorded buffer is invalidated -> a later submit refuses.
    vkrpc::HandleRequest h;
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok);
    VKR_CHECK(!backend.queue_submit(sub).ok);
    const vkrpc::StatusResponse stale = backend.record_command_buffer(stale_sync2);
    VKR_CHECK(!stale.ok);
    VKR_CHECK_EQ(stale.reason, "sync2 image barrier references destroyed swapchain image " +
                                   std::to_string(imgs.images[0]) + " (swapchain " +
                                   std::to_string(sc.swapchain) + ", destroy_swapchain)");
}

// record/submit command fields decode wide: a missing image/stage becomes a sentinel the
// backend rejects, never a truncated plausible value.
void test_command_decode() {
    json::Value cmd = json::Value::make_object();
    cmd.set("kind", json::Value(std::string("pipeline_barrier")));
    // no image, no stage masks
    json::Array arr;
    arr.emplace_back(cmd);
    json::Value body = json::Value::make_object();
    body.set("command_buffer", json::Value(std::string("5")));
    body.set("commands", json::Value(std::move(arr)));
    const vkrpc::RecordCommandBufferRequest md = vkrpc::RecordCommandBufferRequest::from_body(body);
    VKR_CHECK_EQ(static_cast<int>(md.commands.size()), 1);
    VKR_CHECK(md.commands[0].image == 0);    // missing handle -> 0 (rejected downstream)
    VKR_CHECK(md.commands[0].src_stage < 0); // missing stage -> sentinel (not zero)
    VKR_CHECK(md.commands[0].dst_stage < 0);

    json::Value wait = json::Value::make_object();
    wait.set("semaphore", json::Value(std::string("9")));
    // no stage
    json::Array warr;
    warr.emplace_back(wait);
    json::Value sbody = json::Value::make_object();
    sbody.set("queue", json::Value(std::string("3")));
    sbody.set("waits", json::Value(std::move(warr)));
    const vkrpc::QueueSubmitRequest sd = vkrpc::QueueSubmitRequest::from_body(sbody);
    VKR_CHECK_EQ(static_cast<int>(sd.waits.size()), 1);
    VKR_CHECK(sd.waits[0].stage < 0); // missing -> sentinel
}

// App-facing WSI capability queries over the mock: the {physical_device, surface}
// validation, the canned caps (dynamic-extent sentinel + honest TRANSFER_DST), formats,
// present modes, support, and the RPC faults -- all dual-platform.
void test_surface_queries() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);

    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    VKR_CHECK(en.ok && !en.devices.empty());
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok);

    // Capabilities: currentExtent is the dynamic-extent sentinel; supported usage
    // advertises TRANSFER_DST so a clear canary can request it.
    vkrpc::GetSurfaceCapabilitiesRequest cap_req;
    cap_req.physical_device = phys;
    cap_req.surface = surf.surface;
    const vkrpc::GetSurfaceCapabilitiesResponse caps = backend.get_surface_capabilities(cap_req);
    VKR_CHECK(caps.ok);
    VKR_CHECK_EQ(caps.current_extent_width, vkrpc::kDynamicExtentSentinel);
    VKR_CHECK_EQ(caps.current_extent_height, vkrpc::kDynamicExtentSentinel);
    VKR_CHECK(caps.min_image_count >= 1);
    VKR_CHECK((caps.supported_usage_flags &
               static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst)) != 0);
    VKR_CHECK((caps.supported_usage_flags &
               static_cast<std::uint64_t>(vkrpc::kImageUsageColorAttachment)) != 0);

    // Formats / present modes: at least one each.
    vkrpc::GetSurfaceFormatsRequest fmt_req;
    fmt_req.physical_device = phys;
    fmt_req.surface = surf.surface;
    const vkrpc::GetSurfaceFormatsResponse fmts = backend.get_surface_formats(fmt_req);
    VKR_CHECK(fmts.ok && !fmts.formats.empty());
    vkrpc::GetSurfacePresentModesRequest pm_req;
    pm_req.physical_device = phys;
    pm_req.surface = surf.surface;
    const vkrpc::GetSurfacePresentModesResponse pms = backend.get_surface_present_modes(pm_req);
    VKR_CHECK(pms.ok && !pms.present_modes.empty());

    // Support: family 0 supported; any other family rejected.
    vkrpc::GetSurfaceSupportRequest sup_req;
    sup_req.physical_device = phys;
    sup_req.queue_family_index = 0;
    sup_req.surface = surf.surface;
    const vkrpc::GetSurfaceSupportResponse sup = backend.get_surface_support(sup_req);
    VKR_CHECK(sup.ok && sup.supported);
    vkrpc::GetSurfaceSupportRequest sup_badfam = sup_req;
    sup_badfam.queue_family_index = 1;
    VKR_CHECK(!backend.get_surface_support(sup_badfam).ok);

    // RPC faults: unknown surface, and a physical device not from the surface's instance.
    vkrpc::GetSurfaceCapabilitiesRequest cap_badsurf = cap_req;
    cap_badsurf.surface = surf.surface + 999;
    VKR_CHECK(!backend.get_surface_capabilities(cap_badsurf).ok);
    vkrpc::GetSurfaceCapabilitiesRequest cap_badphys = cap_req;
    cap_badphys.physical_device = phys + 999;
    VKR_CHECK(!backend.get_surface_capabilities(cap_badphys).ok);

    // Wire round-trips (incl. the wide sentinel extent + the formats array).
    const vkrpc::GetSurfaceCapabilitiesResponse caps_rt =
        vkrpc::GetSurfaceCapabilitiesResponse::from_body(caps.to_body());
    VKR_CHECK_EQ(caps_rt.current_extent_width, vkrpc::kDynamicExtentSentinel);
    VKR_CHECK_EQ(caps_rt.supported_usage_flags, caps.supported_usage_flags);
    const vkrpc::GetSurfaceFormatsResponse fmts_rt =
        vkrpc::GetSurfaceFormatsResponse::from_body(fmts.to_body());
    VKR_CHECK_EQ(static_cast<int>(fmts_rt.formats.size()), static_cast<int>(fmts.formats.size()));
    VKR_CHECK_EQ(fmts_rt.formats[0].format, fmts.formats[0].format);
    const vkrpc::GetSurfaceSupportRequest sup_rt =
        vkrpc::GetSurfaceSupportRequest::from_body(sup_req.to_body());
    VKR_CHECK_EQ(sup_rt.queue_family_index, 0);
}

// draw surface: wire round-trips for the new create structs, the binary-tail SPIR-V
// framing (exact length, rejection edges), and the draw RecordedCommand kinds.
void test_draw_surface_wire() {
    // ImageView request/response round-trip (the bufferless triangle's swapchain view).
    vkrpc::CreateImageViewRequest iv;
    iv.image = 0x100000001ull;
    iv.view_type = 1;                                              // 2D
    iv.format = 44;                                                // some VkFormat
    iv.swizzle_r = iv.swizzle_g = iv.swizzle_b = iv.swizzle_a = 0; // IDENTITY
    iv.aspect = 1;                                                 // COLOR
    iv.base_mip_level = 0;
    iv.level_count = 1;
    iv.base_array_layer = 0;
    iv.layer_count = 1;
    iv.view_usage = 0x4; // VkImageViewUsageCreateInfo usage (SAMPLED)
    const vkrpc::CreateImageViewRequest iv2 =
        vkrpc::CreateImageViewRequest::from_body(iv.to_body());
    VKR_CHECK_EQ(iv2.image, iv.image);
    VKR_CHECK_EQ(iv2.view_type, 1);
    VKR_CHECK_EQ(iv2.format, 44);
    VKR_CHECK_EQ(iv2.aspect, 1);
    VKR_CHECK_EQ(iv2.level_count, 1);
    VKR_CHECK_EQ(iv2.view_usage, 0x4ull);
    // an OLD body (no view_usage key) decodes to 0 = absent.
    {
        json::Value old_body = iv.to_body();
        old_body.set("view_usage", json::Value(0)); // not a handle string -> decodes to 0
        VKR_CHECK_EQ(vkrpc::CreateImageViewRequest::from_body(old_body).view_usage, 0ull);
    }
    vkrpc::CreateImageViewResponse ivr;
    ivr.ok = true;
    ivr.image_view = 0x55ull;
    const vkrpc::CreateImageViewResponse ivr2 =
        vkrpc::CreateImageViewResponse::from_body(ivr.to_body());
    VKR_CHECK(ivr2.ok);
    VKR_CHECK_EQ(ivr2.image_view, 0x55ull);

    // Shader module: the first BINARY-bodied op. Exact tail round-trip.
    vkrpc::CreateShaderModuleRequest sm;
    sm.device = 7;
    sm.code = std::string("\x03\x02\x23\x07", 4); // 4 bytes of "SPIR-V" (embedded high byte)
    sm.code_size = sm.code.size();
    const std::string wire = sm.to_wire();
    std::string smerr;
    const vkrpc::CreateShaderModuleRequest sm2 =
        vkrpc::CreateShaderModuleRequest::from_wire(wire, smerr);
    VKR_CHECK(smerr.empty());
    VKR_CHECK_EQ(sm2.device, 7ull);
    VKR_CHECK_EQ(sm2.code_size, static_cast<std::uint64_t>(4));
    VKR_CHECK(sm2.code == sm.code);
    // Rejection: trailing garbage (tail_len > code_size) -> exact length required.
    {
        std::string trailer = wire + std::string("X");
        std::string e;
        (void) vkrpc::CreateShaderModuleRequest::from_wire(trailer, e);
        VKR_CHECK(!e.empty());
    }
    // Rejection: truncated tail (tail_len < code_size).
    {
        std::string truncated = wire.substr(0, wire.size() - 1);
        std::string e;
        (void) vkrpc::CreateShaderModuleRequest::from_wire(truncated, e);
        VKR_CHECK(!e.empty());
    }
    // Rejection: body shorter than the 4-byte length prefix.
    {
        std::string e;
        (void) vkrpc::CreateShaderModuleRequest::from_wire(std::string("ab"), e);
        VKR_CHECK(!e.empty());
    }
    // Rejection: json_len claims more than the body holds (no OOB read).
    {
        std::string bad(4, '\0');
        protocol::store_le32(1000u, reinterpret_cast<unsigned char*>(&bad[0]));
        bad += "{}";
        std::string e;
        (void) vkrpc::CreateShaderModuleRequest::from_wire(bad, e);
        VKR_CHECK(!e.empty());
    }
    // Rejection: oversize json header.
    {
        std::string bad(4, '\0');
        protocol::store_le32(static_cast<std::uint32_t>(vkrpc::kMaxBinaryJsonHeaderBytes + 1),
                             reinterpret_cast<unsigned char*>(&bad[0]));
        std::string e;
        (void) vkrpc::CreateShaderModuleRequest::from_wire(bad, e);
        VKR_CHECK(!e.empty());
    }

    // Render pass: 1 color attachment + 1 dependency round-trip.
    vkrpc::CreateRenderPassRequest rp;
    rp.device = 7;
    vkrpc::AttachmentDesc a;
    a.format = 44;
    a.samples = 1;
    a.load_op = 1;  // CLEAR
    a.store_op = 0; // STORE
    a.stencil_load_op = 2;
    a.stencil_store_op = 1;
    a.initial_layout = 0;        // UNDEFINED
    a.final_layout = 1000001002; // PRESENT_SRC_KHR
    rp.attachments.push_back(a);
    rp.color_attachment = 0;
    rp.color_layout = 2; // COLOR_ATTACHMENT_OPTIMAL
    vkrpc::SubpassDependencyDesc d;
    d.src_subpass = 0xFFFFFFFFLL; // VK_SUBPASS_EXTERNAL (exceeds INT_MAX -> wide)
    d.dst_subpass = 0;
    d.src_stage = 0x400;
    d.dst_stage = 0x400;
    d.src_access = 0;
    d.dst_access = 0x100;
    d.dependency_flags = 1;
    rp.dependencies.push_back(d);
    const vkrpc::CreateRenderPassRequest rp2 =
        vkrpc::CreateRenderPassRequest::from_body(rp.to_body());
    VKR_CHECK_EQ(static_cast<int>(rp2.attachments.size()), 1);
    VKR_CHECK_EQ(rp2.attachments[0].final_layout, 1000001002);
    VKR_CHECK_EQ(static_cast<int>(rp2.dependencies.size()), 1);
    VKR_CHECK_EQ(rp2.dependencies[0].src_subpass,
                 0xFFFFFFFFLL); // VK_SUBPASS_EXTERNAL survives wide

    // Framebuffer + pipeline layout + graphics pipeline round-trips.
    vkrpc::CreateFramebufferRequest fb;
    fb.device = 7;
    fb.render_pass = 0x10;
    fb.image_view = 0x20;
    fb.width = 1280;
    fb.height = 720;
    fb.layers = 1;
    const vkrpc::CreateFramebufferRequest fb2 =
        vkrpc::CreateFramebufferRequest::from_body(fb.to_body());
    VKR_CHECK_EQ(fb2.width, 1280);
    VKR_CHECK_EQ(fb2.image_view, 0x20ull);

    vkrpc::CreatePipelineLayoutRequest pl;
    pl.device = 7;
    pl.set_layout_count = 0;
    pl.push_constant_range_count = 0;
    const vkrpc::CreatePipelineLayoutRequest pl2 =
        vkrpc::CreatePipelineLayoutRequest::from_body(pl.to_body());
    VKR_CHECK_EQ(pl2.set_layout_count, 0);
    VKR_CHECK_EQ(pl2.push_constant_range_count, 0);

    vkrpc::CreateGraphicsPipelinesRequest gp;
    gp.device = 7;
    gp.pipeline_cache = 0;
    vkrpc::ShaderStageDesc vs;
    vs.stage = 1; // VERTEX
    vs.module = 0x30;
    vs.entry = "main";
    vkrpc::ShaderStageDesc fs;
    fs.stage = 16; // FRAGMENT
    fs.module = 0x31;
    fs.entry = "main";
    gp.stages = {vs, fs};
    gp.topology = 3; // TRIANGLE_LIST
    gp.vertex_binding_count = 0;
    gp.vertex_attribute_count = 0;
    gp.cull_mode = 2; // BACK
    gp.front_face = 1;
    gp.dynamic_states = {0, 1}; // VIEWPORT, SCISSOR
    gp.layout = 0x40;
    gp.render_pass = 0x10;
    gp.subpass = 0;
    // vertex-attr-divisor: the divisor pNext fields ride the wire (present flag + the {binding,
    // divisor} array). Carried wide so a >u32 divisor survives the round-trip for the validator.
    gp.vertex_divisor_present = 1;
    gp.vertex_binding_divisors = {{0, 1}, {2, 4}};
    const vkrpc::CreateGraphicsPipelinesRequest gp2 =
        vkrpc::CreateGraphicsPipelinesRequest::from_body(gp.to_body());
    VKR_CHECK_EQ(static_cast<int>(gp2.stages.size()), 2);
    VKR_CHECK_EQ(gp2.stages[1].stage, 16);
    VKR_CHECK_EQ(gp2.stages[0].module, 0x30ull);
    VKR_CHECK_EQ(static_cast<int>(gp2.dynamic_states.size()), 2);
    VKR_CHECK_EQ(gp2.vertex_binding_count, 0);
    VKR_CHECK_EQ(gp2.vertex_divisor_present, 1);
    VKR_CHECK_EQ(static_cast<int>(gp2.vertex_binding_divisors.size()), 2);
    VKR_CHECK_EQ(gp2.vertex_binding_divisors[1].binding, 2);
    VKR_CHECK_EQ(gp2.vertex_binding_divisors[1].divisor, 4);

    // Draw RecordedCommands round-trip through record_command_buffer.
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = 0x80;
    vkrpc::RecordedCommand begin;
    begin.kind = "begin_render_pass";
    begin.render_pass = 0x10;
    begin.framebuffer = 0x11;
    begin.render_area_w = 1280;
    begin.render_area_h = 720;
    begin.r = 0.1;
    begin.g = 0.2;
    begin.b = 0.3;
    begin.a = 1.0;
    vkrpc::RecordedCommand bind;
    bind.kind = "bind_pipeline";
    bind.pipeline = 0x40;
    vkrpc::RecordedCommand vp;
    vp.kind = "set_viewport";
    vp.vp_w = 1280.0;
    vp.vp_h = 720.0;
    vp.vp_max_depth = 1.0;
    vkrpc::RecordedCommand sc;
    sc.kind = "set_scissor";
    sc.sc_w = 1280;
    sc.sc_h = 720;
    vkrpc::RecordedCommand draw;
    draw.kind = "draw";
    draw.vertex_count = 3;
    draw.instance_count = 1;
    draw.first_vertex = 0;
    draw.first_instance = 0;
    vkrpc::RecordedCommand end;
    end.kind = "end_render_pass";
    rec.commands = {begin, bind, vp, sc, draw, end};
    const vkrpc::RecordCommandBufferRequest rec2 =
        vkrpc::RecordCommandBufferRequest::from_body(rec.to_body());
    VKR_CHECK_EQ(static_cast<int>(rec2.commands.size()), 6);
    VKR_CHECK_EQ(rec2.commands[0].kind, std::string("begin_render_pass"));
    VKR_CHECK_EQ(rec2.commands[0].framebuffer, 0x11ull);
    VKR_CHECK_EQ(rec2.commands[0].render_area_w, 1280);
    VKR_CHECK_EQ(rec2.commands[1].pipeline, 0x40ull);
    VKR_CHECK_EQ(rec2.commands[4].vertex_count, 3LL);
    VKR_CHECK_EQ(rec2.commands[5].kind, std::string("end_render_pass"));
    // The viewport floats survive the round-trip.
    VKR_CHECK(rec2.commands[2].vp_w > 1279.0 && rec2.commands[2].vp_w < 1281.0);
}

struct MockDrawFixture {
    vkrpc::MockVulkanBackend& backend;
    int extent = 0;
    int format = 44;
    vkrpc::CreateInstanceResponse ci;
    vkrpc::EnumeratePhysicalDevicesResponse devices;
    std::uint64_t physical_device = 0;
    vkrpc::CreateDeviceResponse device;
    vkrpc::GetDeviceQueueResponse queue;
    vkrpc::CreateCommandPoolResponse command_pool;
    std::uint64_t command_buffer = 0;
    vkrpc::CreateSurfaceResponse surface;
    vkrpc::CreateSwapchainResponse swapchain;
    vkrpc::GetSwapchainImagesResponse images;
    vkrpc::CreateImageViewResponse image_view;
    vkrpc::CreateRenderPassResponse render_pass;
    vkrpc::CreateFramebufferResponse framebuffer;
    vkrpc::CreateShaderModuleResponse vertex_shader;
    vkrpc::CreateShaderModuleResponse fragment_shader;
    vkrpc::CreatePipelineLayoutResponse pipeline_layout;
    vkrpc::CreateGraphicsPipelinesResponse pipeline;

    MockDrawFixture(vkrpc::MockVulkanBackend& backend_in, int extent_in,
                    std::uint64_t feature_bits = 0, std::vector<std::string> extensions = {},
                    bool draw_indirect_count_enabled = false)
        : backend(backend_in), extent(extent_in) {
        ci = backend.create_instance({});
        vkrpc::EnumeratePhysicalDevicesRequest enumerate;
        enumerate.instance = ci.instance;
        devices = backend.enumerate_physical_devices(enumerate);
        VKR_CHECK(ci.ok);
        VKR_CHECK(devices.ok);
        VKR_CHECK(!devices.devices.empty());
        physical_device = devices.devices.front().handle;

        vkrpc::CreateDeviceRequest create_device;
        create_device.instance = ci.instance;
        create_device.physical_device = physical_device;
        create_device.enabled_extensions = std::move(extensions);
        create_device.enabled_feature_bits = feature_bits;
        create_device.draw_indirect_count_enabled = draw_indirect_count_enabled ? 1 : 0;
        device = backend.create_device(create_device);
        VKR_CHECK(device.ok);

        vkrpc::GetDeviceQueueRequest get_queue;
        get_queue.device = device.device;
        get_queue.queue_family_index = device.queue_family_index;
        queue = backend.get_device_queue(get_queue);
        VKR_CHECK(queue.ok);

        vkrpc::CreateCommandPoolRequest create_pool;
        create_pool.device = device.device;
        create_pool.queue_family_index = device.queue_family_index;
        command_pool = backend.create_command_pool(create_pool);
        vkrpc::AllocateCommandBuffersRequest allocate;
        allocate.command_pool = command_pool.command_pool;
        allocate.count = 1;
        const auto command_buffers = backend.allocate_command_buffers(allocate);
        VKR_CHECK(command_pool.ok);
        VKR_CHECK(command_buffers.ok);
        VKR_CHECK_EQ(command_buffers.command_buffers.size(), static_cast<std::size_t>(1));
        command_buffer = command_buffers.command_buffers.front();

        vkrpc::CreateSurfaceRequest create_surface;
        create_surface.instance = ci.instance;
        surface = backend.create_surface(create_surface);
        vkrpc::CreateSwapchainRequest create_swapchain;
        create_swapchain.device = device.device;
        create_swapchain.surface = surface.surface;
        create_swapchain.image_format = format;
        create_swapchain.color_space = 0;
        create_swapchain.present_mode = 2;
        create_swapchain.width = extent;
        create_swapchain.height = extent;
        create_swapchain.min_image_count = 2;
        create_swapchain.image_usage = vkrpc::kImageUsageColorAttachment;
        swapchain = backend.create_swapchain(create_swapchain);
        vkrpc::GetSwapchainImagesRequest get_images;
        get_images.swapchain = swapchain.swapchain;
        images = backend.get_swapchain_images(get_images);
        VKR_CHECK(surface.ok);
        VKR_CHECK(swapchain.ok);
        VKR_CHECK(images.ok);
        VKR_CHECK(images.images.size() >= 2);

        image_view = backend.create_image_view(make_image_view(images.images.front(), format));
        render_pass = backend.create_render_pass(make_render_pass());
        framebuffer = backend.create_framebuffer(
            make_framebuffer(render_pass.render_pass, image_view.image_view));
        vertex_shader = backend.create_shader_module(make_shader_module(8));
        fragment_shader = backend.create_shader_module(make_shader_module(8));
        vkrpc::CreatePipelineLayoutRequest create_layout;
        create_layout.device = device.device;
        create_layout.set_layout_count = 0;
        create_layout.push_constant_range_count = 0;
        pipeline_layout = backend.create_pipeline_layout(create_layout);
        pipeline = backend.create_graphics_pipelines(make_pipeline());
        VKR_CHECK(image_view.ok);
        VKR_CHECK(render_pass.ok);
        VKR_CHECK(framebuffer.ok);
        VKR_CHECK(vertex_shader.ok);
        VKR_CHECK(fragment_shader.ok);
        VKR_CHECK(pipeline_layout.ok);
        VKR_CHECK(pipeline.ok);
    }

    vkrpc::CreateImageViewRequest make_image_view(std::uint64_t image, int image_format) const {
        vkrpc::CreateImageViewRequest request;
        request.image = image;
        request.view_type = 1;
        request.format = image_format;
        request.aspect = 1;
        request.level_count = 1;
        request.layer_count = 1;
        return request;
    }

    vkrpc::CreateShaderModuleRequest make_shader_module(std::size_t bytes) const {
        vkrpc::CreateShaderModuleRequest request;
        request.device = device.device;
        request.code.assign(bytes, '\0');
        request.code_size = bytes;
        return request;
    }

    vkrpc::CreateRenderPassRequest make_render_pass() const {
        vkrpc::CreateRenderPassRequest request;
        request.device = device.device;
        vkrpc::AttachmentDesc attachment;
        attachment.format = format;
        attachment.samples = 1;
        attachment.load_op = 1;
        attachment.store_op = 0;
        attachment.stencil_load_op = 2;
        attachment.stencil_store_op = 1;
        attachment.initial_layout = 0;
        attachment.final_layout = 1000001002;
        request.attachments = {attachment};
        request.color_attachment = 0;
        request.color_layout = 2;
        return request;
    }

    vkrpc::CreateFramebufferRequest make_framebuffer(std::uint64_t pass, std::uint64_t view) const {
        vkrpc::CreateFramebufferRequest request;
        request.device = device.device;
        request.render_pass = pass;
        request.image_view = view;
        request.width = extent;
        request.height = extent;
        request.layers = 1;
        return request;
    }

    vkrpc::CreateGraphicsPipelinesRequest make_pipeline() const {
        vkrpc::CreateGraphicsPipelinesRequest request;
        request.device = device.device;
        vkrpc::ShaderStageDesc vertex;
        vertex.stage = 1;
        vertex.module = vertex_shader.shader_module;
        vertex.entry = "main";
        vkrpc::ShaderStageDesc fragment;
        fragment.stage = 16;
        fragment.module = fragment_shader.shader_module;
        fragment.entry = "main";
        request.stages = {vertex, fragment};
        request.topology = 3;
        request.vertex_binding_count = 0;
        request.vertex_attribute_count = 0;
        request.cull_mode = 2;
        request.front_face = 1;
        request.dynamic_states = {0, 1};
        request.layout = pipeline_layout.pipeline_layout;
        request.render_pass = render_pass.render_pass;
        request.subpass = 0;
        return request;
    }
};

struct MockTestBuffer {
    std::uint64_t buffer = 0;
    std::uint64_t memory = 0;
};

MockTestBuffer make_mock_test_buffer(vkrpc::MockVulkanBackend& backend, std::uint64_t device,
                                     const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse& props,
                                     std::uint64_t usage, bool bind, std::uint64_t size = 64) {
    vkrpc::CreateBufferRequest request;
    request.device = device;
    request.size = size;
    request.usage = usage;
    request.sharing_mode = 0;
    const auto created = backend.create_buffer(request);
    VKR_CHECK(created.ok);
    MockTestBuffer result{created.buffer, 0};
    if (!bind) {
        return result;
    }
    int type = -1;
    for (std::size_t i = 0; i < props.types.size() && i < 32; ++i) {
        if ((created.mem_type_bits & (std::uint64_t{1} << i)) != 0) {
            type = static_cast<int>(i);
            break;
        }
    }
    VKR_CHECK(type >= 0);
    vkrpc::AllocateMemoryRequest allocate;
    allocate.device = device;
    allocate.allocation_size = created.mem_size;
    allocate.memory_type_index = type;
    const auto memory = backend.allocate_memory(allocate);
    VKR_CHECK(memory.ok);
    vkrpc::BindBufferMemoryRequest bind_request;
    bind_request.buffer = created.buffer;
    bind_request.memory = memory.memory;
    VKR_CHECK(backend.bind_buffer_memory(bind_request).ok);
    result.memory = memory.memory;
    return result;
}

void destroy_mock_test_buffer(vkrpc::MockVulkanBackend& backend, const MockTestBuffer& buffer,
                              bool already_destroyed = false) {
    if (buffer.buffer != 0 && !already_destroyed) {
        VKR_CHECK(backend.destroy_buffer({buffer.buffer}).ok);
    }
    if (buffer.memory != 0) {
        VKR_CHECK(backend.free_memory({buffer.memory}).ok);
    }
}

vkrpc::StatusResponse record_mock_draw(vkrpc::MockVulkanBackend& backend,
                                       const MockDrawFixture& fixture, vkrpc::RecordedCommand draw,
                                       std::uint64_t index_buffer = 0) {
    vkrpc::RecordedCommand begin;
    begin.kind = "begin_render_pass";
    begin.render_pass = fixture.render_pass.render_pass;
    begin.framebuffer = fixture.framebuffer.framebuffer;
    begin.render_area_w = fixture.extent;
    begin.render_area_h = fixture.extent;
    vkrpc::RecordedCommand bind;
    bind.kind = "bind_pipeline";
    bind.pipeline = fixture.pipeline.pipeline;
    vkrpc::RecordedCommand viewport;
    viewport.kind = "set_viewport";
    viewport.vp_w = fixture.extent;
    viewport.vp_h = fixture.extent;
    viewport.vp_max_depth = 1;
    vkrpc::RecordedCommand scissor;
    scissor.kind = "set_scissor";
    scissor.sc_w = fixture.extent;
    scissor.sc_h = fixture.extent;
    vkrpc::RecordedCommand end;
    end.kind = "end_render_pass";
    vkrpc::RecordCommandBufferRequest request;
    request.command_buffer = fixture.command_buffer;
    request.commands = {begin, bind, viewport, scissor};
    if (index_buffer != 0) {
        vkrpc::RecordedCommand bind_index;
        bind_index.kind = "bind_index_buffer";
        bind_index.args_u64 = {index_buffer, 0};
        bind_index.args_i64 = {0};
        request.commands.push_back(std::move(bind_index));
    }
    request.commands.push_back(std::move(draw));
    request.commands.push_back(std::move(end));
    return backend.record_command_buffer(request);
}

// Core indirect draws need full draw readiness in addition to their buffer/stride/range rules.
// Build the smallest real mock object graph twice so both feature states are exercised at the
// backend boundary (the pure ICD/shared predicate has its own exhaustive pins).
void test_core_indirect_draw_mock_case(bool multi_draw_indirect) {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const std::uint64_t feature_bits = multi_draw_indirect ? vkrpc::kFeatureMultiDrawIndirect : 0;
    MockDrawFixture fixture(backend, 64, feature_bits);
    const auto& ci = fixture.ci;
    const std::uint64_t phys = fixture.physical_device;
    const auto& cd = fixture.device;
    const auto& queue = fixture.queue;
    const auto& surface = fixture.surface;
    const auto& swapchain = fixture.swapchain;
    const auto& view = fixture.image_view;
    const auto& render_pass = fixture.render_pass;
    const auto& framebuffer = fixture.framebuffer;
    const auto& vs = fixture.vertex_shader;
    const auto& fs = fixture.fragment_shader;
    const auto& layout = fixture.pipeline_layout;
    const auto& pipeline = fixture.pipeline;
    const auto& pool = fixture.command_pool;
    const std::uint64_t command_buffer = fixture.command_buffer;

    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = phys;
    const auto memory_props = backend.get_physical_device_memory_properties(mpr);
    VKR_CHECK(memory_props.ok);
    auto make_buffer = [&](std::uint64_t usage, bool bind) {
        return make_mock_test_buffer(backend, cd.device, memory_props, usage, bind);
    };
    const MockTestBuffer indirect = make_buffer(vkrpc::kBufferUsageIndirectBuffer, true);
    const MockTestBuffer wrong_usage = make_buffer(vkrpc::kBufferUsageVertexBuffer, true);
    const MockTestBuffer index = make_buffer(vkrpc::kBufferUsageIndexBuffer, true);
    const MockTestBuffer unbound = make_buffer(vkrpc::kBufferUsageIndirectBuffer, false);

    auto indirect_command = [&](bool indexed, std::uint64_t buffer, std::uint64_t offset,
                                long long count, long long stride) {
        vkrpc::RecordedCommand c;
        c.kind = indexed ? "draw_indexed_indirect" : "draw_indirect";
        c.src_buffer = buffer;
        c.indirect_offset = offset;
        c.indirect_draw_count = count;
        c.indirect_stride = stride;
        return c;
    };
    auto record = [&](vkrpc::RecordedCommand draw, bool bind_index) {
        return record_mock_draw(backend, fixture, std::move(draw), bind_index ? index.buffer : 0);
    };

    VKR_CHECK(record(indirect_command(false, indirect.buffer, 0, 1, 16), false).ok);
    VKR_CHECK(record(indirect_command(true, indirect.buffer, 0, 1, 20), true).ok);
    vkrpc::RecordedCommand legacy = indirect_command(false, indirect.buffer, 0, 1, 16);
    legacy.indirect_draw_count = -1;
    legacy.indirect_stride = -1;
    legacy.args_u64 = {0};
    legacy.args_i64 = {1, 16};
    VKR_CHECK(record(legacy, false).ok);
    legacy.indirect_draw_count = 1; // mixed old/new spelling fails closed
    VKR_CHECK(!record(legacy, false).ok);
    VKR_CHECK(!record(indirect_command(true, indirect.buffer, 0, 1, 20), false).ok);
    VKR_CHECK(!record(indirect_command(false, wrong_usage.buffer, 0, 1, 16), false).ok);
    VKR_CHECK(!record(indirect_command(false, unbound.buffer, 0, 1, 16), false).ok);
    VKR_CHECK(!record(indirect_command(false, 0xDEAD, 0, 1, 16), false).ok);
    VKR_CHECK(!record(indirect_command(false, indirect.buffer, 2, 1, 16), false).ok);
    VKR_CHECK(!record(indirect_command(true, indirect.buffer, 48, 1, 20), true).ok);
    if (multi_draw_indirect) {
        VKR_CHECK(record(indirect_command(false, indirect.buffer, 0, 2, 16), false).ok);
        VKR_CHECK(record(indirect_command(true, indirect.buffer, 0, 2, 20), true).ok);
        VKR_CHECK(!record(indirect_command(false, indirect.buffer, 0, 2, 15), false).ok);
        VKR_CHECK(!record(indirect_command(true, indirect.buffer, 0, 2, 16), true).ok);
        VKR_CHECK(!record(indirect_command(true, indirect.buffer,
                                           std::numeric_limits<std::uint64_t>::max() - 3, 2, 20),
                          true)
                       .ok);
    } else {
        VKR_CHECK(!record(indirect_command(false, indirect.buffer, 0, 2, 16), false).ok);
    }

    // The indirect and index buffers become command-buffer dependencies just like pipelines.
    VKR_CHECK(record(indirect_command(false, indirect.buffer, 0, 1, 16), false).ok);
    VKR_CHECK(backend.destroy_buffer({indirect.buffer}).ok);
    // This record RPC is the worker boundary reached by guest vkEndCommandBuffer: the destroyed
    // referent is rejected there rather than becoming a stale host VkBuffer.
    VKR_CHECK(!record(indirect_command(false, indirect.buffer, 0, 1, 16), false).ok);
    vkrpc::QueueSubmitRequest submit;
    submit.queue = queue.queue;
    submit.command_buffers = {command_buffer};
    VKR_CHECK(!backend.queue_submit(submit).ok);

    destroy_mock_test_buffer(backend, indirect, /*already_destroyed=*/true);
    destroy_mock_test_buffer(backend, wrong_usage);
    destroy_mock_test_buffer(backend, index);
    destroy_mock_test_buffer(backend, unbound);
    VKR_CHECK(backend.destroy_pipeline({pipeline.pipeline}).ok);
    VKR_CHECK(backend.destroy_pipeline_layout({layout.pipeline_layout}).ok);
    VKR_CHECK(backend.destroy_framebuffer({framebuffer.framebuffer}).ok);
    VKR_CHECK(backend.destroy_render_pass({render_pass.render_pass}).ok);
    VKR_CHECK(backend.destroy_shader_module({vs.shader_module}).ok);
    VKR_CHECK(backend.destroy_shader_module({fs.shader_module}).ok);
    VKR_CHECK(backend.destroy_image_view({view.image_view}).ok);
    VKR_CHECK(backend.destroy_swapchain({swapchain.swapchain}).ok);
    VKR_CHECK(backend.destroy_command_pool({pool.command_pool}).ok);
    VKR_CHECK(backend.destroy_surface({surface.surface}).ok);
    VKR_CHECK(backend.destroy_device({cd.device}).ok);
    VKR_CHECK(backend.destroy_instance({ci.instance}).ok);
}

void test_core_indirect_draw_mock() {
    test_core_indirect_draw_mock_case(false);
    test_core_indirect_draw_mock_case(true);
}

void test_core_indirect_count_validation() {
    bool scalar_enabled = false;
    std::string scalar_reason;
    VKR_CHECK(vkrpc::decode_three_state_scalar(vkrpc::kThreeStateScalarOmitted, true, "field",
                                               scalar_enabled, scalar_reason) &&
              scalar_enabled);
    VKR_CHECK(vkrpc::decode_three_state_scalar(0, true, "field", scalar_enabled, scalar_reason) &&
              !scalar_enabled);
    VKR_CHECK(vkrpc::decode_three_state_scalar(1, false, "field", scalar_enabled, scalar_reason) &&
              scalar_enabled);
    VKR_CHECK(!vkrpc::decode_three_state_scalar(vkrpc::kThreeStateScalarInvalid, false, "field",
                                                scalar_enabled, scalar_reason));
    VKR_CHECK(scalar_reason.find("field must be 0 or 1") != std::string::npos);

    const vkrpc::IndirectBufferState good{true, true, true, 64};
    const char* why = "";
    const auto ok = [&](bool enabled, vkrpc::IndirectBufferState main,
                        vkrpc::IndirectBufferState count, std::uint64_t offset,
                        std::uint64_t count_offset, long long max_count, long long stride,
                        std::uint64_t command_size) {
        return vkrpc::core_indirect_count_draw_ok(enabled, main, count, offset, count_offset,
                                                  max_count, stride, command_size, &why);
    };
    VKR_CHECK(ok(true, good, good, 0, 0, 2, 16, 16));
    VKR_CHECK(ok(true, good, good, 0, 60, 0, 16, 16)); // zero max still reads count slot
    VKR_CHECK(!ok(false, good, good, 0, 0, 1, 16, 16));
    auto bad = good;
    bad.live = false;
    VKR_CHECK(!ok(true, bad, good, 0, 0, 1, 16, 16));
    bad = good;
    bad.bound = false;
    VKR_CHECK(!ok(true, bad, good, 0, 0, 1, 16, 16));
    bad = good;
    bad.has_indirect_usage = false;
    VKR_CHECK(!ok(true, bad, good, 0, 0, 1, 16, 16));
    bad = good;
    bad.live = false;
    VKR_CHECK(!ok(true, good, bad, 0, 0, 1, 16, 16));
    bad = good;
    bad.bound = false;
    VKR_CHECK(!ok(true, good, bad, 0, 0, 1, 16, 16));
    bad = good;
    bad.has_indirect_usage = false;
    VKR_CHECK(!ok(true, good, bad, 0, 0, 1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 2, 0, 1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 0, 2, 1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 0, 64, 1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 0, std::numeric_limits<std::uint64_t>::max() - 3, 1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 0, 0, -1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 0, 0,
                  static_cast<long long>(std::numeric_limits<std::uint32_t>::max()) + 1, 16, 16));
    VKR_CHECK(!ok(true, good, good, 0, 0, 1, -1, 16));
    VKR_CHECK(!ok(true, good, good, 0, 0, 1,
                  static_cast<long long>(std::numeric_limits<std::uint32_t>::max()) + 1, 16));
    // Count variants require a valid stride unconditionally, even at maxDrawCount 0/1.
    VKR_CHECK(!ok(true, good, good, 0, 0, 0, 15, 16));
    VKR_CHECK(!ok(true, good, good, 0, 0, 1, 12, 16));
    VKR_CHECK(!ok(true, good, good, 0, 0, 1, 18, 16));
    VKR_CHECK(!ok(true, good, good, 48, 0, 2, 16, 16));
    VKR_CHECK(!ok(true, good, good, std::numeric_limits<std::uint64_t>::max() - 3, 0, 2, 16, 16));
    // maxDrawCount > 1 has no multiDrawIndirect dependency, and buffer aliasing is legal.
    VKR_CHECK(ok(true, good, good, 0, 60, 2, 16, 16));
}

void test_core_indirect_count_draw_mock_case(bool enabled) {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    MockDrawFixture fixture(backend, 64, 0, {}, enabled);
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpr;
    mpr.physical_device = fixture.physical_device;
    const auto memory_props = backend.get_physical_device_memory_properties(mpr);
    VKR_CHECK(memory_props.ok);
    auto make_buffer = [&](std::uint64_t usage, bool bind) {
        return make_mock_test_buffer(backend, fixture.device.device, memory_props, usage, bind);
    };
    const MockTestBuffer main0 = make_buffer(vkrpc::kBufferUsageIndirectBuffer, true);
    const MockTestBuffer count0 = make_buffer(vkrpc::kBufferUsageIndirectBuffer, true);
    const MockTestBuffer main1 = make_buffer(vkrpc::kBufferUsageIndirectBuffer, true);
    const MockTestBuffer count1 = make_buffer(vkrpc::kBufferUsageIndirectBuffer, true);
    const MockTestBuffer wrong = make_buffer(vkrpc::kBufferUsageVertexBuffer, true);
    const MockTestBuffer unbound = make_buffer(vkrpc::kBufferUsageIndirectBuffer, false);
    const MockTestBuffer index = make_buffer(vkrpc::kBufferUsageIndexBuffer, true);

    auto draw = [&](bool indexed, std::uint64_t main, std::uint64_t count,
                    std::uint64_t count_offset = 0) {
        vkrpc::RecordedCommand c = vkrpc::make_core_indirect_count_draw_command(
            main, 0, count, count_offset, 2, indexed ? 20u : 16u, indexed);
        c.kind = std::string(vkrpc::recorded_command_kind_name(c));
        return c;
    };
    auto record = [&](vkrpc::RecordedCommand d, bool bind_index) {
        return record_mock_draw(backend, fixture, std::move(d), bind_index ? index.buffer : 0);
    };

    if (!enabled) {
        VKR_CHECK(!record(draw(false, main0.buffer, count0.buffer), false).ok);
    } else {
        VKR_CHECK(record(draw(false, main0.buffer, count0.buffer), false).ok);
        VKR_CHECK(record(draw(true, main0.buffer, count0.buffer), true).ok);
        VKR_CHECK(!record(draw(true, main0.buffer, count0.buffer), false).ok);
        VKR_CHECK(record(draw(false, main0.buffer, main0.buffer, 60), false).ok);
        VKR_CHECK(!record(draw(false, wrong.buffer, count0.buffer), false).ok);
        VKR_CHECK(!record(draw(false, main0.buffer, wrong.buffer), false).ok);
        VKR_CHECK(!record(draw(false, main0.buffer, unbound.buffer), false).ok);
        VKR_CHECK(!record(draw(false, main0.buffer, 0xDEAD), false).ok);
        auto mixed = draw(false, main0.buffer, count0.buffer);
        mixed.args_u64 = {0};
        VKR_CHECK(!record(mixed, false).ok);

        // Both handles are baked dependencies. Destroying either invalidates the last successful
        // recording; aliasing above did not create a special case.
        VKR_CHECK(record(draw(false, main0.buffer, count0.buffer), false).ok);
        VKR_CHECK(backend.destroy_buffer({count0.buffer}).ok);
        vkrpc::QueueSubmitRequest submit;
        submit.queue = fixture.queue.queue;
        submit.command_buffers = {fixture.command_buffer};
        VKR_CHECK(!backend.queue_submit(submit).ok);
        VKR_CHECK(record(draw(false, main1.buffer, count1.buffer), false).ok);
        VKR_CHECK(backend.destroy_buffer({main1.buffer}).ok);
        VKR_CHECK(!backend.queue_submit(submit).ok);
    }

    destroy_mock_test_buffer(backend, main0);
    destroy_mock_test_buffer(backend, count0, enabled);
    destroy_mock_test_buffer(backend, main1, enabled);
    destroy_mock_test_buffer(backend, count1);
    destroy_mock_test_buffer(backend, wrong);
    destroy_mock_test_buffer(backend, unbound);
    destroy_mock_test_buffer(backend, index);
    VKR_CHECK(backend.destroy_pipeline({fixture.pipeline.pipeline}).ok);
    VKR_CHECK(backend.destroy_pipeline_layout({fixture.pipeline_layout.pipeline_layout}).ok);
    VKR_CHECK(backend.destroy_framebuffer({fixture.framebuffer.framebuffer}).ok);
    VKR_CHECK(backend.destroy_render_pass({fixture.render_pass.render_pass}).ok);
    VKR_CHECK(backend.destroy_shader_module({fixture.vertex_shader.shader_module}).ok);
    VKR_CHECK(backend.destroy_shader_module({fixture.fragment_shader.shader_module}).ok);
    VKR_CHECK(backend.destroy_image_view({fixture.image_view.image_view}).ok);
    VKR_CHECK(backend.destroy_swapchain({fixture.swapchain.swapchain}).ok);
    VKR_CHECK(backend.destroy_command_pool({fixture.command_pool.command_pool}).ok);
    VKR_CHECK(backend.destroy_surface({fixture.surface.surface}).ok);
    VKR_CHECK(backend.destroy_device({fixture.device.device}).ok);
    VKR_CHECK(backend.destroy_instance({fixture.ci.instance}).ok);
}

void test_core_indirect_count_draw_mock() {
    test_core_indirect_count_validation();
    {
        const auto devices = protocol::probe_mocked();
        vkrpc::MockVulkanBackend backend(devices.front().name);
        const auto instance = backend.create_instance({});
        vkrpc::EnumeratePhysicalDevicesRequest er;
        er.instance = instance.instance;
        const auto enumerated = backend.enumerate_physical_devices(er);
        vkrpc::CreateDeviceRequest old_icd;
        old_icd.instance = instance.instance;
        old_icd.physical_device = enumerated.devices.front().handle;
        old_icd.enabled_extensions = {vkrpc::kDrawIndirectCountExtensionName};
        old_icd.draw_indirect_count_enabled = vkrpc::kDrawIndirectCountScalarOmitted;
        const auto derived = backend.create_device(old_icd);
        VKR_CHECK(derived.ok); // absent scalar derives the extension path
        VKR_CHECK(backend.destroy_device({derived.device}).ok);
        auto invalid = old_icd;
        invalid.draw_indirect_count_enabled = vkrpc::kDrawIndirectCountScalarInvalid;
        VKR_CHECK(!backend.create_device(invalid).ok);
        VKR_CHECK(backend.destroy_instance({instance.instance}).ok);
    }
    test_core_indirect_count_draw_mock_case(false);
    test_core_indirect_count_draw_mock_case(true);
}

// draw surface on the mock backend: the full bufferless-triangle object graph + the
// create -> record(draw) -> submit -> present chain, the bounded-subset rejections, and destroy
// ordering -- all headless, so it runs on both platforms (the real backend's parallel coverage is
// the Windows integration test).
void test_draw_surface_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    // (GL/zink): this device enables the wired extension commands (transform feedback +
    // conditional rendering) so the block below exercises accept paths; the bare second
    // device there exercises the not-enabled rejections.
    MockDrawFixture fixture(backend, 256, 0,
                            {"VK_EXT_transform_feedback", "VK_EXT_conditional_rendering"});
    const auto& ci = fixture.ci;
    const auto& en = fixture.devices;
    const auto& cd = fixture.device;
    const auto& q = fixture.queue;
    const auto& cp = fixture.command_pool;
    const std::uint64_t cmd = fixture.command_buffer;
    const auto& surf = fixture.surface;
    const int kFormat = fixture.format;
    const auto& sc = fixture.swapchain;
    const auto& imgs = fixture.images;
    const std::uint64_t img0 = imgs.images.front();

    // Image view (2D / matching format / identity / COLOR / single mip+layer) + rejections.
    auto make_iv = [&](std::uint64_t image, int format) {
        return fixture.make_image_view(image, format);
    };
    const auto& iv = fixture.image_view;
    // (GL/zink): a mismatched format / non-2D viewType / non-identity swizzle are now
    // ACCEPTED (faithfully forwarded; the host driver gates them). An unknown image handle is still
    // rejected.
    VKR_CHECK(!backend.create_image_view(make_iv(0xDEAD, kFormat)).ok); // unknown image

    // Shader modules (vert + frag) + rejections.
    auto make_sm = [&](std::size_t bytes) { return fixture.make_shader_module(bytes); };
    const auto& vs = fixture.vertex_shader;
    const auto& fs = fixture.fragment_shader;
    VKR_CHECK(!backend.create_shader_module(make_sm(7)).ok); // not a multiple of 4
    {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code = "abcd";
        r.code_size = 8; // payload/size mismatch
        VKR_CHECK(!backend.create_shader_module(r).ok);
    }

    // Render pass (1 color attachment, UNDEFINED -> PRESENT_SRC_KHR) + rejections.
    auto make_rp = [&]() { return fixture.make_render_pass(); };
    const auto& rp = fixture.render_pass;
    {
        auto r = make_rp();
        r.attachments.push_back(r.attachments[0]); // 2 attachments
        VKR_CHECK(!backend.create_render_pass(r).ok);
    }
    {
        auto r = make_rp();
        r.attachments[0].final_layout = 2; // not PRESENT_SRC_KHR
        VKR_CHECK(!backend.create_render_pass(r).ok);
    }

    // Framebuffer + rejection (layers != 1).
    auto make_fb = [&](std::uint64_t renderpass, std::uint64_t view) {
        return fixture.make_framebuffer(renderpass, view);
    };
    const auto& fb = fixture.framebuffer;
    auto imageless_req = make_fb(rp.render_pass, 0);
    imageless_req.imageless = true;
    imageless_req.attachment_count = 1;
    vkrpc::FramebufferAttachmentInfoDesc imageless_info;
    imageless_info.usage = 0x10; // VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
    imageless_info.width = 256;
    imageless_info.height = 256;
    imageless_info.layer_count = 1;
    imageless_info.view_formats = {kFormat};
    imageless_req.attachment_infos = {imageless_info};
    const vkrpc::CreateFramebufferResponse imageless_fb = backend.create_framebuffer(imageless_req);
    VKR_CHECK(imageless_fb.ok && imageless_fb.framebuffer != 0);
    {
        auto r = imageless_req;
        r.attachment_count = 2;
        VKR_CHECK(!backend.create_framebuffer(r).ok);
    }
    {
        auto r = make_fb(rp.render_pass, iv.image_view);
        r.layers = 2;
        VKR_CHECK(!backend.create_framebuffer(r).ok);
    }
    {
        auto r = make_fb(rp.render_pass, iv.image_view);
        r.width = 0; // zero extent
        VKR_CHECK(!backend.create_framebuffer(r).ok);
    }
    {
        auto r = make_fb(rp.render_pass, iv.image_view);
        r.width = 128; // != the 256x256 swapchain extent
        VKR_CHECK(!backend.create_framebuffer(r).ok);
    }

    // Pipeline layout (empty) + rejection (non-zero set count).
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 0;
    const auto& pl = fixture.pipeline_layout;
    {
        auto r = plr;
        r.set_layout_count = 1;
        VKR_CHECK(!backend.create_pipeline_layout(r).ok);
    }

    // Graphics pipeline (bufferless) + rejections.
    auto make_gp = [&]() { return fixture.make_pipeline(); };
    const auto& gp = fixture.pipeline;
    {
        // Every ACCEPTED pipeline in this block is destroyed at the end so the teardown's
        // destroy_device (no-live-children) assertions stay exact.
        std::vector<std::uint64_t> made;
        // Tess/geom breadth: a VERTEX-only pipeline is now ACCEPTED (faithful; a fragmentless
        // pipeline is legal Vulkan) -- the old exactly-two-stages gate was mock != real.
        auto r = make_gp();
        r.stages.pop_back();
        {
            const auto p = backend.create_graphics_pipelines(r);
            VKR_CHECK(p.ok);
            made.push_back(p.pipeline);
        }
        // But a pipeline WITHOUT a vertex stage stays structurally rejected.
        r = make_gp();
        r.stages.erase(r.stages.begin());
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        // Duplicate stages / a non-graphics stage bit / an out-of-range topology reject.
        r = make_gp();
        r.stages.push_back(r.stages[1]); // FRAGMENT twice
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        r = make_gp();
        r.stages[1].stage = 32; // COMPUTE in a graphics pipeline
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        r = make_gp();
        r.topology = 11;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        // Tessellation coupling (mock == real structural rules): V+TCS+TES+F with PATCH_LIST +
        // patchControlPoints accepted; every half-shape rejects.
        auto tess = make_gp();
        vkrpc::ShaderStageDesc tcs;
        tcs.stage = 2;
        tcs.module = tess.stages[0].module;
        tcs.entry = "main";
        vkrpc::ShaderStageDesc tes;
        tes.stage = 4;
        tes.module = tess.stages[0].module;
        tes.entry = "main";
        tess.stages = {tess.stages[0], tcs, tes, tess.stages[1]};
        tess.topology = 10; // PATCH_LIST
        tess.patch_control_points = 3;
        {
            const auto p = backend.create_graphics_pipelines(tess);
            VKR_CHECK(p.ok);
            made.push_back(p.pipeline);
        }
        auto half = tess;
        half.stages = {tess.stages[0], tcs, tess.stages[3]}; // TCS without TES
        VKR_CHECK(!backend.create_graphics_pipelines(half).ok);
        half = tess;
        half.topology = 3; // tess stages without PATCH_LIST
        VKR_CHECK(!backend.create_graphics_pipelines(half).ok);
        half = make_gp();
        half.topology = 10; // PATCH_LIST without tess stages
        half.patch_control_points = 3;
        VKR_CHECK(!backend.create_graphics_pipelines(half).ok);
        half = tess;
        half.patch_control_points = 0; // PATCH_LIST needs pcp >= 1
        VKR_CHECK(!backend.create_graphics_pipelines(half).ok);
        half = tess;
        half.patch_control_points = 33; // > the spec-minimum ceiling
        VKR_CHECK(!backend.create_graphics_pipelines(half).ok);
        // Geometry: V+G+F accepted on a plain topology.
        auto geom = make_gp();
        vkrpc::ShaderStageDesc gs;
        gs.stage = 8;
        gs.module = geom.stages[0].module;
        gs.entry = "main";
        geom.stages = {geom.stages[0], gs, geom.stages[1]};
        {
            const auto p = backend.create_graphics_pipelines(geom);
            VKR_CHECK(p.ok);
            made.push_back(p.pipeline);
        }
        for (const std::uint64_t p : made) {
            VKR_CHECK(backend.destroy_pipeline({p}).ok);
        }
    }
    {
        auto r = make_gp();
        r.vertex_binding_count = 1; // not bufferless
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        auto r = make_gp();
        r.dynamic_states = {0}; // missing SCISSOR
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        auto r = make_gp();
        r.pipeline_cache = 99; // must be VK_NULL_HANDLE
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        auto r = make_gp();
        r.cull_mode = 3; // VK_CULL_MODE_FRONT_AND_BACK, outside the {NONE,FRONT,BACK} subset
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }

    // Record the bufferless triangle, then submit + present it.
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = cmd;
    {
        vkrpc::RecordedCommand b;
        b.kind = "begin_render_pass";
        b.render_pass = rp.render_pass;
        b.framebuffer = fb.framebuffer;
        b.render_area_w = 256;
        b.render_area_h = 256;
        vkrpc::RecordedCommand bp;
        bp.kind = "bind_pipeline";
        bp.pipeline = gp.pipeline;
        vkrpc::RecordedCommand vp;
        vp.kind = "set_viewport";
        vp.vp_w = 256.0;
        vp.vp_h = 256.0;
        vp.vp_max_depth = 1.0;
        vkrpc::RecordedCommand scc;
        scc.kind = "set_scissor";
        scc.sc_w = 256;
        scc.sc_h = 256;
        vkrpc::RecordedCommand d;
        d.kind = "draw";
        d.vertex_count = 3;
        d.instance_count = 1;
        d.first_vertex = 0;
        d.first_instance = 0;
        vkrpc::RecordedCommand e;
        e.kind = "end_render_pass";
        rec.commands = {b, bp, vp, scc, d, e};
    }
    VKR_CHECK(backend.record_command_buffer(rec).ok);
    // Rejection: a draw outside an active render pass.
    {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        vkrpc::RecordedCommand d;
        d.kind = "draw";
        d.vertex_count = 3;
        d.instance_count = 1;
        d.first_vertex = 0;
        d.first_instance = 0;
        r.commands = {d};
        VKR_CHECK(!backend.record_command_buffer(r).ok);
    }
    // Rejection: a render pass left open (begin without end).
    {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        vkrpc::RecordedCommand b;
        b.kind = "begin_render_pass";
        b.render_pass = rp.render_pass;
        b.framebuffer = fb.framebuffer;
        b.render_area_w = 256;
        b.render_area_h = 256;
        r.commands = {b};
        VKR_CHECK(!backend.record_command_buffer(r).ok);
    }

    // clear_attachments: the faithful in-render-pass scissored clear zink emits for a
    // partial glClear (the first frame after a window resize). Payload contract:
    // args_i64=[attachmentCount, rectCount, per-attachment (aspect, colorAttachment), per-rect
    // (x, y, w, h, baseArrayLayer, layerCount)]; args_u64 = 4 raw VkClearValue words/attachment.
    {
        auto make_ca = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "clear_attachments";
            c.args_i64 = {1,
                          1,
                          /*aspect=COLOR*/ 1,
                          /*colorAttachment*/ 0,
                          /*rect x,y,w,h,layer,count*/ 0,
                          0,
                          64,
                          64,
                          0,
                          1};
            // Raw float bits for (1.0, 0.0, 0.0, 1.0) -- the union words ride bit-faithfully.
            c.args_u64 = {0x3f800000ull, 0ull, 0ull, 0x3f800000ull};
            return c;
        };
        auto make_begin = [&]() {
            vkrpc::RecordedCommand b;
            b.kind = "begin_render_pass";
            b.render_pass = rp.render_pass;
            b.framebuffer = fb.framebuffer;
            b.render_area_w = 256;
            b.render_area_h = 256;
            return b;
        };
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        // Accepted INSIDE an active render pass.
        vkrpc::RecordCommandBufferRequest ok_rec;
        ok_rec.command_buffer = cmd;
        ok_rec.commands = {make_begin(), make_ca(), end};
        VKR_CHECK(backend.record_command_buffer(ok_rec).ok);
        // Rejected OUTSIDE a render pass (spec: requires an active render pass).
        vkrpc::RecordCommandBufferRequest out_rec;
        out_rec.command_buffer = cmd;
        out_rec.commands = {make_ca()};
        VKR_CHECK(!backend.record_command_buffer(out_rec).ok);
        // Rejected on a malformed payload (args_i64 shorter than the declared counts).
        vkrpc::RecordedCommand bad = make_ca();
        bad.args_i64.pop_back();
        vkrpc::RecordCommandBufferRequest bad_rec;
        bad_rec.command_buffer = cmd;
        bad_rec.commands = {make_begin(), bad, end};
        VKR_CHECK(!backend.record_command_buffer(bad_rec).ok);
        // Rejected when the clear-value words do not match the attachment count.
        vkrpc::RecordedCommand badu = make_ca();
        badu.args_u64.pop_back();
        vkrpc::RecordCommandBufferRequest badu_rec;
        badu_rec.command_buffer = cmd;
        badu_rec.commands = {make_begin(), badu, end};
        VKR_CHECK(!backend.record_command_buffer(badu_rec).ok);
    }

    // The draw command state machine rejects streams a validation-clean backend would.
    auto begin_cmd = [&](std::uint64_t renderpass, std::uint64_t framebuffer, int w, int h) {
        vkrpc::RecordedCommand c;
        c.kind = "begin_render_pass";
        c.render_pass = renderpass;
        c.framebuffer = framebuffer;
        c.render_area_w = w;
        c.render_area_h = h;
        return c;
    };
    auto bind_cmd = [&](std::uint64_t pipeline) {
        vkrpc::RecordedCommand c;
        c.kind = "bind_pipeline";
        c.pipeline = pipeline;
        return c;
    };
    auto vp_cmd = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_viewport";
        c.vp_w = 256.0;
        c.vp_h = 256.0;
        c.vp_max_depth = 1.0;
        return c;
    };
    auto sc_cmd = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_scissor";
        c.sc_w = 256;
        c.sc_h = 256;
        return c;
    };
    auto draw_cmd = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "draw";
        c.vertex_count = 3;
        c.instance_count = 1;
        c.first_vertex = 0;
        c.first_instance = 0;
        return c;
    };
    auto end_cmd = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "end_render_pass";
        return c;
    };
    auto rec_ok = [&](std::vector<vkrpc::RecordedCommand> cmds) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        r.commands = std::move(cmds);
        return backend.record_command_buffer(r).ok;
    };
    // Draw with no bound pipeline.
    VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), vp_cmd(), sc_cmd(),
                       draw_cmd(), end_cmd()}));
    // Draw without the required dynamic viewport/scissor.
    VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), bind_cmd(gp.pipeline),
                       draw_cmd(), end_cmd()}));
    // Render area larger than the framebuffer.
    VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 512, 512), bind_cmd(gp.pipeline),
                       vp_cmd(), sc_cmd(), draw_cmd(), end_cmd()}));
    // A second render pass + pipeline with a DIFFERENT color format, for the compatibility checks.
    vkrpc::CreateRenderPassRequest rpb_req = make_rp();
    rpb_req.attachments[0].format = kFormat + 1;
    const vkrpc::CreateRenderPassResponse rpb = backend.create_render_pass(rpb_req);
    VKR_CHECK(rpb.ok);
    vkrpc::CreateGraphicsPipelinesRequest gpb_req = make_gp();
    gpb_req.render_pass = rpb.render_pass;
    const vkrpc::CreateGraphicsPipelinesResponse gpb = backend.create_graphics_pipelines(gpb_req);
    VKR_CHECK(gpb.ok);
    // begin with a framebuffer built for an incompatible render pass.
    VKR_CHECK(!rec_ok({begin_cmd(rpb.render_pass, fb.framebuffer, 256, 256), bind_cmd(gpb.pipeline),
                       vp_cmd(), sc_cmd(), draw_cmd(), end_cmd()}));
    // Bound pipeline incompatible with the active render pass.
    VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), bind_cmd(gpb.pipeline),
                       vp_cmd(), sc_cmd(), draw_cmd(), end_cmd()}));
    {
        vkrpc::HandleRequest hh;
        hh.handle = gpb.pipeline;
        VKR_CHECK(backend.destroy_pipeline(hh).ok);
        hh.handle = rpb.render_pass;
        VKR_CHECK(backend.destroy_render_pass(hh).ok);
    }

    // (GL/zink): the wired VK_EXT_transform_feedback + VK_EXT_conditional_rendering command
    // surfaces + blit_image. Payload contracts live at the ICD recorders; validation here is the
    // mock's (== the real backend's).
    {
        const std::uint64_t img1 = imgs.images[1];
        // A live + bound buffer for the XFB / conditional-rendering cases.
        vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
        mpq.physical_device = en.devices.front().handle;
        const auto mp = backend.get_physical_device_memory_properties(mpq);
        VKR_CHECK(mp.ok);
        int coherent_type = -1;
        for (std::size_t i = 0; i < mp.types.size(); ++i) {
            const std::uint64_t want =
                vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
            if ((mp.types[i].property_flags & want) == want) {
                coherent_type = static_cast<int>(i);
            }
        }
        VKR_CHECK(coherent_type >= 0);
        vkrpc::CreateBufferRequest cbq;
        cbq.device = cd.device;
        cbq.size = 1024;
        cbq.usage = vkrpc::kBufferUsageTransformFeedback |
                    vkrpc::kBufferUsageTransformFeedbackCounter |
                    vkrpc::kBufferUsageConditionalRendering;
        cbq.sharing_mode = 0;
        const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbq);
        VKR_CHECK(buf.ok && buf.buffer != 0); // the EXT usage bits are in the subset
        vkrpc::AllocateMemoryRequest am;
        am.device = cd.device;
        am.allocation_size = 4096;
        am.memory_type_index = coherent_type;
        const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(am);
        VKR_CHECK(mem.ok);
        vkrpc::BindBufferMemoryRequest bbm;
        bbm.buffer = buf.buffer;
        bbm.memory = mem.memory;
        bbm.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bbm).ok);
        // A created-but-never-bound buffer: commands referencing it must be rejected.
        const vkrpc::CreateBufferResponse unbound = backend.create_buffer(cbq);
        VKR_CHECK(unbound.ok);

        // blit_image: outside a render pass, between two live swapchain images.
        auto blit_cmd = [&](std::uint64_t src, std::uint64_t dst) {
            vkrpc::RecordedCommand c;
            c.kind = "blit_image";
            c.args_u64 = {src, dst};
            c.args_i64 = {/*srcLayout TRANSFER_SRC*/ 6,
                          /*dstLayout TRANSFER_DST*/ 7,
                          /*filter LINEAR*/ 1,
                          /*regionCount*/ 1,
                          /*srcSub aspect,mip,layer,count*/ 1,
                          0,
                          0,
                          1,
                          /*srcOfs0*/ 0,
                          0,
                          0,
                          /*srcOfs1*/ 256,
                          256,
                          1,
                          /*dstSub*/ 1,
                          0,
                          0,
                          1,
                          /*dstOfs0*/ 0,
                          0,
                          0,
                          /*dstOfs1*/ 128,
                          128,
                          1};
            return c;
        };
        VKR_CHECK(rec_ok({blit_cmd(img0, img1)}));
        // Rejected inside a render pass / malformed region payload / unknown image.
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                           blit_cmd(img0, img1), end_cmd()}));
        {
            auto bad = blit_cmd(img0, img1);
            bad.args_i64.pop_back(); // 19 values for the declared region
            VKR_CHECK(!rec_ok({bad}));
        }
        VKR_CHECK(!rec_ok({blit_cmd(img0, 0xDEAD)}));

        // copy_image: the unscaled sibling (glCopyTexSubImage2D-class) -- same gates.
        auto copy_img_cmd = [&](std::uint64_t src, std::uint64_t dst) {
            vkrpc::RecordedCommand c;
            c.kind = "copy_image";
            c.args_u64 = {src, dst};
            c.args_i64 = {/*srcLayout TRANSFER_SRC*/ 6,
                          /*dstLayout TRANSFER_DST*/ 7,
                          /*regionCount*/ 1,
                          /*srcSub aspect,mip,layer,count*/ 1,
                          0,
                          0,
                          1,
                          /*srcOffset*/ 0,
                          0,
                          0,
                          /*dstSub*/ 1,
                          0,
                          0,
                          1,
                          /*dstOffset*/ 0,
                          0,
                          0,
                          /*extent*/ 128,
                          128,
                          1};
            return c;
        };
        VKR_CHECK(rec_ok({copy_img_cmd(img0, img1)}));
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                           copy_img_cmd(img0, img1), end_cmd()}));
        {
            auto bad = copy_img_cmd(img0, img1);
            bad.args_i64.pop_back(); // 16 values for the declared region
            VKR_CHECK(!rec_ok({bad}));
        }
        VKR_CHECK(!rec_ok({copy_img_cmd(0xDEAD, img1)}));

        // copy_image_to_buffer (glReadPixels readback for offscreen PNG export) -- a transfer
        // command outside any render pass; the source may be a swapchain image (default-framebuffer
        // readback, no app-bound memory), the dest buffer must be live + bound. args_u64=[srcImage,
        // dstBuffer]; args_i64=[srcLayout, regionCount, 13 per region].
        auto ci2b_cmd = [&](std::uint64_t src, std::uint64_t dst) {
            vkrpc::RecordedCommand c;
            c.kind = "copy_image_to_buffer";
            c.args_u64 = {src, dst};
            c.args_i64 = {/*srcLayout TRANSFER_SRC*/ 6,
                          /*regionCount*/ 1,
                          /*bufferOffset*/ 0,
                          /*bufferRowLength*/ 0,
                          /*bufferImageHeight*/ 0,
                          /*aspect COLOR*/ 1,
                          /*mipLevel*/ 0,
                          /*baseArrayLayer*/ 0,
                          /*layerCount*/ 1,
                          /*imageOffset*/ 0,
                          0,
                          0,
                          /*imageExtent*/ 64,
                          64,
                          1};
            return c;
        };
        VKR_CHECK(rec_ok({ci2b_cmd(img0, buf.buffer)})); // swapchain src + bound dest: accepted
        // Rejected: inside a render pass / unknown image / unbound dest buffer.
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                           ci2b_cmd(img0, buf.buffer), end_cmd()}));
        VKR_CHECK(!rec_ok({ci2b_cmd(0xDEAD, buf.buffer)}));
        VKR_CHECK(!rec_ok({ci2b_cmd(img0, unbound.buffer)}));
        // An app-created image that was never memory-bound is not a valid readback source.
        {
            vkrpc::CreateImageRequest uir;
            uir.device = cd.device;
            uir.image_type = vkrpc::kImageType2D;
            uir.format = vkrpc::kFormatR8G8B8A8Unorm;
            uir.width = 16;
            uir.height = 16;
            uir.depth = 1;
            uir.mip_levels = 1;
            uir.array_layers = 1;
            uir.samples = 1;
            uir.tiling = vkrpc::kImageTilingOptimal;
            uir.usage = vkrpc::kImageUsageSampled; // in the mock's create_image allowlist
            uir.sharing_mode = 0;
            uir.initial_layout = 0;
            const vkrpc::CreateImageResponse uimg = backend.create_image(uir);
            VKR_CHECK(uimg.ok);
            VKR_CHECK(!rec_ok({ci2b_cmd(uimg.image, buf.buffer)}));
            vkrpc::HandleRequest dh;
            dh.handle = uimg.image;
            VKR_CHECK(backend.destroy_image(dh).ok); // the device destroy below blocks on children
        }
        // Malformed region payloads: a short region, and a negative regionCount that must not
        // wrap the overflow-safe shape check (the query-range lesson).
        {
            auto bad = ci2b_cmd(img0, buf.buffer);
            bad.args_i64.pop_back(); // 12 values for the declared region
            VKR_CHECK(!rec_ok({bad}));
        }
        {
            auto bad = ci2b_cmd(img0, buf.buffer);
            bad.args_i64[1] = -1; // huge as size_t; the division-form check must reject
            VKR_CHECK(!rec_ok({bad}));
        }

        // Transform feedback: bind buffers, then a begin/end scope inside the render pass.
        auto bind_tf = [&](std::uint64_t b, bool with_size) {
            vkrpc::RecordedCommand c;
            c.kind = "bind_transform_feedback_buffers";
            c.args_i64 = {0, 1, with_size ? 1 : 0};
            c.args_u64 = {b, 0};
            if (with_size) {
                c.args_u64.push_back(1024);
            }
            return c;
        };
        auto begin_tf = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "begin_transform_feedback";
            c.args_i64 = {0, 0, 0}; // no counter buffers
            return c;
        };
        auto end_tf = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "end_transform_feedback";
            c.args_i64 = {0, 0, 0};
            return c;
        };
        VKR_CHECK(
            rec_ok({bind_tf(buf.buffer, false), begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                    begin_tf(), end_tf(), end_cmd()}));
        VKR_CHECK(
            rec_ok({bind_tf(buf.buffer, true), begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                    begin_tf(), end_tf(), end_cmd()}));
        // A NULL (0) counter-buffer entry is legal; a real one must be live + bound.
        {
            vkrpc::RecordedCommand c = begin_tf();
            c.args_i64 = {0, 1, 1};
            c.args_u64 = {0, 0};
            VKR_CHECK(rec_ok(
                {begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), c, end_tf(), end_cmd()}));
            c.args_u64 = {unbound.buffer, 0};
            VKR_CHECK(!rec_ok(
                {begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), c, end_tf(), end_cmd()}));
        }
        // Rejections: scope outside a render pass; unbalanced begin/end; the render pass must not
        // end mid-XFB; bind while active; an unbound data buffer; a malformed payload.
        VKR_CHECK(!rec_ok({begin_tf()}));
        VKR_CHECK(
            !rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), end_tf(), end_cmd()}));
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), begin_tf(),
                           begin_tf(), end_tf(), end_cmd()}));
        VKR_CHECK(
            !rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), begin_tf(), end_cmd()}));
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), begin_tf(),
                           bind_tf(buf.buffer, false), end_tf(), end_cmd()}));
        VKR_CHECK(!rec_ok({bind_tf(unbound.buffer, false)}));
        {
            auto bad = bind_tf(buf.buffer, true);
            bad.args_u64.pop_back(); // declared sizes missing
            VKR_CHECK(!rec_ok({bad}));
        }

        // draw_indirect_byte_count: full draw-readiness + a live counter buffer.
        auto didbc = [&](std::uint64_t counter) {
            vkrpc::RecordedCommand c;
            c.kind = "draw_indirect_byte_count";
            c.args_u64 = {counter};
            c.args_i64 = {/*instanceCount*/ 1, /*firstInstance*/ 0, /*counterBufferOffset*/ 0,
                          /*counterOffset*/ 0, /*vertexStride*/ 16};
            return c;
        };
        VKR_CHECK(
            rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), bind_cmd(gp.pipeline),
                    vp_cmd(), sc_cmd(), didbc(buf.buffer), end_cmd()}));
        // Rejected without a bound pipeline / with a zero stride / an unknown counter buffer.
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), vp_cmd(), sc_cmd(),
                           didbc(buf.buffer), end_cmd()}));
        {
            auto bad = didbc(buf.buffer);
            bad.args_i64[4] = 0;
            VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                               bind_cmd(gp.pipeline), vp_cmd(), sc_cmd(), bad, end_cmd()}));
        }
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                           bind_cmd(gp.pipeline), vp_cmd(), sc_cmd(), didbc(0xDEAD), end_cmd()}));

        // Conditional rendering: balanced, no nesting, scope symmetry (begun outside ends
        // outside; begun inside ends inside the same pass; a begun-outside scope may SPAN a
        // whole render pass).
        auto begin_cr = [&](long long offset) {
            vkrpc::RecordedCommand c;
            c.kind = "begin_conditional_rendering";
            c.args_u64 = {buf.buffer};
            c.args_i64 = {offset, 0};
            return c;
        };
        auto end_cr = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "end_conditional_rendering";
            return c;
        };
        VKR_CHECK(rec_ok({begin_cr(0), end_cr()}));
        VKR_CHECK(rec_ok({begin_cr(4), begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                          end_cmd(), end_cr()})); // spans the whole pass
        VKR_CHECK(rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), begin_cr(0),
                          end_cr(), end_cmd()}));       // begun + ended inside
        VKR_CHECK(!rec_ok({begin_cr(2), end_cr()}));    // offset not 4-byte aligned
        VKR_CHECK(!rec_ok({begin_cr(0), begin_cr(0)})); // nesting
        VKR_CHECK(!rec_ok({end_cr()}));                 // end without begin
        VKR_CHECK(!rec_ok({begin_cr(0), begin_cmd(rp.render_pass, fb.framebuffer, 256, 256),
                           end_cr(), end_cmd()})); // begun outside, ended inside
        VKR_CHECK(!rec_ok({begin_cmd(rp.render_pass, fb.framebuffer, 256, 256), begin_cr(0),
                           end_cmd(), end_cr()})); // the pass ends over an inside-begun scope
        VKR_CHECK(!rec_ok({begin_cr(0)}));         // survives the stream
        {
            vkrpc::RecordedCommand c = begin_cr(0);
            c.args_u64 = {unbound.buffer};
            VKR_CHECK(!rec_ok({c, end_cr()}));
        }

        // A device WITHOUT the extensions rejects the commands (mock == real fail-closed gate).
        vkrpc::CreateDeviceRequest bare_cdr;
        bare_cdr.instance = ci.instance;
        bare_cdr.physical_device = en.devices.front().handle;
        const vkrpc::CreateDeviceResponse bare = backend.create_device(bare_cdr);
        VKR_CHECK(bare.ok);
        vkrpc::CreateCommandPoolRequest bare_cpr;
        bare_cpr.device = bare.device;
        bare_cpr.queue_family_index = bare.queue_family_index;
        const vkrpc::CreateCommandPoolResponse bare_cp = backend.create_command_pool(bare_cpr);
        vkrpc::AllocateCommandBuffersRequest bare_acb;
        bare_acb.command_pool = bare_cp.command_pool;
        bare_acb.count = 1;
        const std::uint64_t bare_cmd =
            backend.allocate_command_buffers(bare_acb).command_buffers.front();
        {
            vkrpc::RecordCommandBufferRequest r;
            r.command_buffer = bare_cmd;
            r.commands = {bind_tf(buf.buffer, false)};
            VKR_CHECK(!backend.record_command_buffer(r).ok);
            r.commands = {end_cr()};
            VKR_CHECK(!backend.record_command_buffer(r).ok);
        }
        vkrpc::HandleRequest hgl;
        hgl.handle = bare_cp.command_pool;
        VKR_CHECK(backend.destroy_command_pool(hgl).ok);
        hgl.handle = bare.device;
        VKR_CHECK(backend.destroy_device(hgl).ok);
        // Tear the objects down so the device teardown below stays clean. NOTE: the buffer
        // was baked into `cmd` recordings above; destroying it invalidates them -- the good
        // triangle is re-recorded right below, before the submit.
        hgl.handle = buf.buffer;
        VKR_CHECK(backend.destroy_buffer(hgl).ok);
        hgl.handle = unbound.buffer;
        VKR_CHECK(backend.destroy_buffer(hgl).ok);
        hgl.handle = mem.memory;
        VKR_CHECK(backend.free_memory(hgl).ok);
    }

    // Re-record the good triangle so the command buffer is recorded for the submit below.
    VKR_CHECK(backend.record_command_buffer(rec).ok);

    vkrpc::AcquireNextImageRequest air;
    air.swapchain = sc.swapchain;
    air.timeout = 0;
    const vkrpc::AcquireNextImageResponse acq = backend.acquire_next_image(air);
    VKR_CHECK(acq.ok);
    vkrpc::QueueSubmitRequest qs;
    qs.queue = q.queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok);
    vkrpc::QueuePresentRequest qp;
    qp.queue = q.queue;
    vkrpc::PresentEntry pe;
    pe.swapchain = sc.swapchain;
    pe.image_index = acq.image_index;
    qp.presents = {pe};
    VKR_CHECK(backend.queue_present(qp).ok);

    // idle waits are REAL RPCs (the ICD's local success stubs are gone) --
    // ok + VK_SUCCESS on live handles (the mock executes synchronously, so it is always idle);
    // unknown handles are rejected.
    {
        vkrpc::HandleRequest wi;
        wi.handle = q.queue;
        const vkrpc::WaitIdleResponse qr = backend.queue_wait_idle(wi);
        VKR_CHECK(qr.ok && qr.result == 0);
        wi.handle = cd.device;
        const vkrpc::WaitIdleResponse dr = backend.device_wait_idle(wi);
        VKR_CHECK(dr.ok && dr.result == 0);
        wi.handle = 0xDEAD;
        VKR_CHECK(!backend.queue_wait_idle(wi).ok);
        VKR_CHECK(!backend.device_wait_idle(wi).ok);
    }

    // Destroy ordering: the device blocks on live draw objects; the swapchain blocks on its view.
    vkrpc::HandleRequest h;
    h.handle = cd.device;
    VKR_CHECK(!backend.destroy_device(h).ok);
    h.handle = sc.swapchain;
    VKR_CHECK(!backend.destroy_swapchain(h).ok);
    // Tear the draw objects down, then the swapchain, then the rest.
    h.handle = gp.pipeline;
    VKR_CHECK(backend.destroy_pipeline(h).ok);
    // Destroying a baked draw object (the pipeline) invalidated the recorded command
    // buffer, so a submit of it now fails instead of replaying a freed handle.
    {
        vkrpc::QueueSubmitRequest qs2;
        qs2.queue = q.queue;
        qs2.command_buffers = {cmd};
        VKR_CHECK(!backend.queue_submit(qs2).ok);
    }
    h.handle = pl.pipeline_layout;
    VKR_CHECK(backend.destroy_pipeline_layout(h).ok);
    h.handle = fb.framebuffer;
    VKR_CHECK(backend.destroy_framebuffer(h).ok);
    h.handle = imageless_fb.framebuffer;
    VKR_CHECK(backend.destroy_framebuffer(h).ok);
    h.handle = rp.render_pass;
    VKR_CHECK(backend.destroy_render_pass(h).ok);
    h.handle = vs.shader_module;
    VKR_CHECK(backend.destroy_shader_module(h).ok);
    h.handle = fs.shader_module;
    VKR_CHECK(backend.destroy_shader_module(h).ok);
    h.handle = iv.image_view;
    VKR_CHECK(backend.destroy_image_view(h).ok);
    h.handle = sc.swapchain;
    VKR_CHECK(backend.destroy_swapchain(h).ok); // no more views -> ok now
    h.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(h).ok);
    h.handle = cp.command_pool;
    VKR_CHECK(backend.destroy_command_pool(h).ok);
    h.handle = cd.device;
    VKR_CHECK(backend.destroy_device(h).ok);
    h.handle = ci.instance;
    VKR_CHECK(backend.destroy_instance(h).ok);
}

// Builds a valid WriteMemoryRanges request whose payload length matches the summed range sizes.
vkrpc::WriteMemoryRangesRequest make_write(const std::vector<vkrpc::MemoryUpload>& uploads) {
    vkrpc::WriteMemoryRangesRequest w;
    w.uploads = uploads;
    std::uint64_t total = 0;
    for (const auto& u : uploads) {
        for (const auto& r : u.ranges) {
            total += r.size;
        }
    }
    w.payload.assign(static_cast<std::size_t>(total), '\xAB');
    return w;
}

void test_memory_buffer_wire() {
    // Memory properties: honest host table round-trip (wide flags + 64-bit heap size).
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
    mpq.physical_device = 0x100000001ull;
    VKR_CHECK_EQ(
        vkrpc::GetPhysicalDeviceMemoryPropertiesRequest::from_body(mpq.to_body()).physical_device,
        mpq.physical_device);
    vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mpr;
    mpr.ok = true;
    mpr.types.push_back({vkrpc::kMemoryPropertyDeviceLocal, 0});
    mpr.types.push_back(
        {vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent, 1});
    mpr.heaps.push_back({8ull * 1024 * 1024 * 1024, vkrpc::kMemoryPropertyDeviceLocal}); // 8 GiB
    mpr.heaps.push_back({16ull * 1024 * 1024 * 1024, 0});
    const auto mpr2 = vkrpc::GetPhysicalDeviceMemoryPropertiesResponse::from_body(mpr.to_body());
    VKR_CHECK(mpr2.ok);
    VKR_CHECK_EQ(static_cast<int>(mpr2.types.size()), 2);
    VKR_CHECK_EQ(mpr2.types[1].property_flags,
                 vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent);
    VKR_CHECK_EQ(mpr2.types[1].heap_index, static_cast<std::uint64_t>(1));
    VKR_CHECK_EQ(static_cast<int>(mpr2.heaps.size()), 2);
    VKR_CHECK_EQ(mpr2.heaps[0].size, 8ull * 1024 * 1024 * 1024); // 64-bit survives wide

    // Buffer create: request + response (carrying the real VkMemoryRequirements).
    vkrpc::CreateBufferRequest cb;
    cb.device = 7;
    cb.size = 5ull * 1024 * 1024 * 1024; // > 4 GiB, exercises wide
    cb.usage = vkrpc::kBufferUsageVertexBuffer;
    cb.sharing_mode = 0; // EXCLUSIVE
    const auto cb2 = vkrpc::CreateBufferRequest::from_body(cb.to_body());
    VKR_CHECK_EQ(cb2.size, cb.size);
    VKR_CHECK_EQ(cb2.usage, vkrpc::kBufferUsageVertexBuffer);
    VKR_CHECK_EQ(cb2.sharing_mode, 0);
    vkrpc::CreateBufferResponse cbr;
    cbr.ok = true;
    cbr.buffer = 0x99ull;
    cbr.mem_size = 4096;
    cbr.mem_alignment = 256;
    cbr.mem_type_bits = 0x1AB;
    const auto cbr2 = vkrpc::CreateBufferResponse::from_body(cbr.to_body());
    VKR_CHECK_EQ(cbr2.buffer, 0x99ull);
    VKR_CHECK_EQ(cbr2.mem_size, static_cast<std::uint64_t>(4096));
    VKR_CHECK_EQ(cbr2.mem_alignment, static_cast<std::uint64_t>(256));
    VKR_CHECK_EQ(cbr2.mem_type_bits, static_cast<std::uint64_t>(0x1AB));

    // Bind: 64-bit offset survives wide.
    vkrpc::BindBufferMemoryRequest bb;
    bb.buffer = 0x99;
    bb.memory = 0x55;
    bb.memory_offset = 6ull * 1024 * 1024 * 1024;
    const auto bb2 = vkrpc::BindBufferMemoryRequest::from_body(bb.to_body());
    VKR_CHECK_EQ(bb2.buffer, 0x99ull);
    VKR_CHECK_EQ(bb2.memory, 0x55ull);
    VKR_CHECK_EQ(bb2.memory_offset, 6ull * 1024 * 1024 * 1024);

    // write_memory_ranges: exact round-trip (2 uploads; first has 2 disjoint sorted ranges).
    vkrpc::MemoryUpload u1;
    u1.memory = 0x55;
    u1.ranges.push_back({0, 16});
    u1.ranges.push_back({4096, 32});
    vkrpc::MemoryUpload u2;
    u2.memory = 0x56;
    u2.ranges.push_back({0, 8});
    const vkrpc::WriteMemoryRangesRequest w = make_write({u1, u2});
    const std::string wire = w.to_wire();
    std::string werr;
    const vkrpc::WriteMemoryRangesRequest w2 =
        vkrpc::WriteMemoryRangesRequest::from_wire(wire, werr);
    VKR_CHECK(werr.empty());
    VKR_CHECK_EQ(static_cast<int>(w2.uploads.size()), 2);
    VKR_CHECK_EQ(w2.uploads[0].memory, 0x55ull);
    VKR_CHECK_EQ(static_cast<int>(w2.uploads[0].ranges.size()), 2);
    VKR_CHECK_EQ(w2.uploads[0].ranges[1].offset, static_cast<std::uint64_t>(4096));
    VKR_CHECK_EQ(w2.uploads[1].memory, 0x56ull);
    VKR_CHECK_EQ(w2.payload.size(), static_cast<std::size_t>(56)); // 16 + 32 + 8
    VKR_CHECK(w2.payload == w.payload);

    auto rejected = [](const vkrpc::WriteMemoryRangesRequest& bad) {
        std::string e;
        (void) vkrpc::WriteMemoryRangesRequest::from_wire(bad.to_wire(), e);
        return !e.empty();
    };
    // Duplicate memory handle.
    {
        vkrpc::MemoryUpload a;
        a.memory = 0x55;
        a.ranges.push_back({0, 8});
        vkrpc::MemoryUpload b;
        b.memory = 0x55; // duplicate
        b.ranges.push_back({16, 8});
        VKR_CHECK(rejected(make_write({a, b})));
    }
    // Unsorted / overlapping ranges within one allocation.
    {
        vkrpc::MemoryUpload a;
        a.memory = 0x55;
        a.ranges.push_back({100, 50}); // [100,150)
        a.ranges.push_back({120, 50}); // overlaps
        VKR_CHECK(rejected(make_write({a})));
    }
    // Zero-size range.
    {
        vkrpc::MemoryUpload a;
        a.memory = 0x55;
        a.ranges.push_back({0, 0});
        vkrpc::WriteMemoryRangesRequest bad;
        bad.uploads.push_back(a); // payload empty; decoder rejects the zero-size range first
        VKR_CHECK(rejected(bad));
    }
    // offset + size overflow.
    {
        vkrpc::MemoryUpload a;
        a.memory = 0x55;
        a.ranges.push_back({~0ull - 10, 100});
        vkrpc::WriteMemoryRangesRequest bad;
        bad.uploads.push_back(a);
        std::string e;
        (void) vkrpc::WriteMemoryRangesRequest::from_wire(bad.to_wire(), e);
        VKR_CHECK(!e.empty());
    }
    // Payload length mismatch: trailing byte, and truncation.
    {
        std::string e;
        (void) vkrpc::WriteMemoryRangesRequest::from_wire(wire + std::string("X"), e);
        VKR_CHECK(!e.empty());
        std::string e2;
        (void) vkrpc::WriteMemoryRangesRequest::from_wire(wire.substr(0, wire.size() - 1), e2);
        VKR_CHECK(!e2.empty());
    }
    // Body shorter than the 4-byte length prefix; oversize json header.
    {
        std::string e;
        (void) vkrpc::WriteMemoryRangesRequest::from_wire(std::string("ab"), e);
        VKR_CHECK(!e.empty());
        std::string bad(4, '\0');
        protocol::store_le32(static_cast<std::uint32_t>(vkrpc::kMaxBinaryJsonHeaderBytes + 1),
                             reinterpret_cast<unsigned char*>(&bad[0]));
        std::string e2;
        (void) vkrpc::WriteMemoryRangesRequest::from_wire(bad, e2);
        VKR_CHECK(!e2.empty());
    }
    // Per-call payload cap: a well-framed body whose tail exceeds kMaxMemoryUploadBytes is a
    // decoder fault. (The cap sits below the transport frame cap with headroom, so a cap-passing
    // body always fits the frame -- the full-frame check is belt-and-suspenders.)
    {
        vkrpc::MemoryUpload a;
        a.memory = 0x55;
        a.ranges.push_back({0, vkrpc::kMaxMemoryUploadBytes + 1});
        VKR_CHECK(rejected(make_write({a})));
    }
}

// End-to-end mock of the spine: memory properties -> allocate (coherent) -> create/bind a
// vertex buffer -> write_memory_ranges -> a VBO graphics pipeline -> record(bind_vertex_buffers +
// draw) -> submit, plus the negatives a validation-clean backend would reject.
void test_memory_buffer_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    // vertex-attr-divisor: enable the ext + feature on this device so the divisor content tests
    // below run; a separate no-ext device (cd_noext) proves the ext gate.
    // geometry-stream: likewise enable transform feedback + geometryStreams so the stream gate
    // tests run against the mock's modeled properties (4 streams, StreamSelect true).
    cdr.enabled_extensions = {vkrpc::kVertexAttributeDivisorExtensionName,
                              vkrpc::kTransformFeedbackExtensionName};
    cdr.vertex_attr_divisor_feature_enabled = 1;
    cdr.geometry_streams_feature_enabled = 1;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    vkrpc::CreateDeviceRequest cdr_noext;
    cdr_noext.instance = ci.instance;
    cdr_noext.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd_noext = backend.create_device(cdr_noext);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const vkrpc::GetDeviceQueueResponse q = backend.get_device_queue(gq);
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const vkrpc::CreateCommandPoolResponse cp = backend.create_command_pool(cpr);
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = cp.command_pool;
    acb.count = 1;
    const std::uint64_t cmd = backend.allocate_command_buffers(acb).command_buffers.front();

    // Memory properties: honest table; find the HOST_VISIBLE|HOST_COHERENT type.
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
    mpq.physical_device = phys;
    const vkrpc::GetPhysicalDeviceMemoryPropertiesResponse mp =
        backend.get_physical_device_memory_properties(mpq);
    VKR_CHECK(mp.ok && mp.types.size() >= 2);
    int coherent_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(coherent_type >= 0);
    VKR_CHECK(!backend.get_physical_device_memory_properties({0}).ok); // invalid physical device
    VKR_CHECK(!backend.get_physical_device_memory_properties({phys + 999}).ok); // unknown handle
    int device_local_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) != want) {
            device_local_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(device_local_type >= 0);

    // Buffer (VERTEX) -> honest memreqs; usage/sharing rejections.
    vkrpc::CreateBufferRequest cbq;
    cbq.device = cd.device;
    cbq.size = 1024;
    cbq.usage = vkrpc::kBufferUsageVertexBuffer;
    cbq.sharing_mode = 0;
    const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbq);
    VKR_CHECK(buf.ok && buf.buffer != 0 && buf.mem_size >= 1024 && buf.mem_type_bits != 0);
    {
        auto r = cbq;
        r.usage = vkrpc::kBufferUsageVertexBuffer | 0x40; // INDEX_BUFFER is now in the subset
        const vkrpc::CreateBufferResponse idx = backend.create_buffer(r);
        VKR_CHECK(idx.ok);
        vkrpc::HandleRequest didx;
        didx.handle = idx.buffer;
        VKR_CHECK(backend.destroy_buffer(didx).ok); // don't leave it live for the device destroy
        r = cbq;
        r.usage = vkrpc::kBufferUsageVertexBuffer | 0x10000000; // a high unused bit -- rejected
        VKR_CHECK(!backend.create_buffer(r).ok);
        r = cbq;
        r.sharing_mode = 1; // CONCURRENT
        VKR_CHECK(!backend.create_buffer(r).ok);
        r = cbq;
        r.size = 0;
        VKR_CHECK(!backend.create_buffer(r).ok);
    }

    // Allocate coherent memory; bind; reject out-of-range bind + out-of-range memory type.
    vkrpc::AllocateMemoryRequest am;
    am.device = cd.device;
    am.allocation_size = 4096;
    am.memory_type_index = coherent_type;
    const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(am);
    VKR_CHECK(mem.ok && mem.memory != 0);
    {
        auto r = am;
        r.memory_type_index = static_cast<long long>(mp.types.size()); // >= memoryTypeCount
        VKR_CHECK(!backend.allocate_memory(r).ok);
    }
    vkrpc::BindBufferMemoryRequest bbm;
    bbm.buffer = buf.buffer;
    bbm.memory = mem.memory;
    bbm.memory_offset = 0;
    VKR_CHECK(backend.bind_buffer_memory(bbm).ok);
    // Logical-range policy for texel-buffer views (the real worker may privately over-allocate a
    // vertex buffer, but that storage must never become guest-visible).
    {
        vkrpc::CreateBufferViewRequest vr;
        vr.buffer = buf.buffer;
        vr.format = 100; // format semantics remain the host driver's authority
        vr.offset = 16;
        vr.range = vkrpc::kVkWholeSize;
        const auto whole = backend.create_buffer_view(vr);
        VKR_CHECK(whole.ok);
        VKR_CHECK(backend.destroy_buffer_view({whole.buffer_view}).ok);
        vr.range = 1008; // exactly reaches the logical end
        const auto exact = backend.create_buffer_view(vr);
        VKR_CHECK(exact.ok);
        VKR_CHECK(backend.destroy_buffer_view({exact.buffer_view}).ok);
        vr.offset = 1024;
        vr.range = vkrpc::kVkWholeSize;
        VKR_CHECK(!backend.create_buffer_view(vr).ok);
        vr.offset = 16;
        vr.range = 1009;
        VKR_CHECK(!backend.create_buffer_view(vr).ok);
    }
    {
        auto r = bbm;
        r.memory_offset = 4096; // offset + buffer size past the allocation
        VKR_CHECK(!backend.bind_buffer_memory(r).ok);
        r = bbm;
        r.memory = 0xDEAD;
        VKR_CHECK(!backend.bind_buffer_memory(r).ok);
    }
    // A misaligned bind offset is rejected against the advertised alignment.
    {
        vkrpc::CreateBufferRequest cbq2 = cbq;
        cbq2.size = 64;
        const vkrpc::CreateBufferResponse buf2 = backend.create_buffer(cbq2);
        VKR_CHECK(buf2.ok && buf2.mem_alignment == 16);
        vkrpc::BindBufferMemoryRequest bad;
        bad.buffer = buf2.buffer;
        bad.memory = mem.memory;
        bad.memory_offset = 1; // not a multiple of 16
        VKR_CHECK(!backend.bind_buffer_memory(bad).ok);
        bad.memory_offset = 16; // aligned -> ok
        VKR_CHECK(backend.bind_buffer_memory(bad).ok);
        vkrpc::HandleRequest hb2;
        hb2.handle = buf2.buffer;
        VKR_CHECK(backend.destroy_buffer(hb2).ok);
    }

    // write_memory_ranges: ok into the bound allocation; reject unknown memory + out-of-range
    // range.
    vkrpc::MemoryUpload up;
    up.memory = mem.memory;
    up.ranges.push_back({0, 64});
    VKR_CHECK(backend.write_memory_ranges(make_write({up})).ok);
    {
        vkrpc::MemoryUpload bad;
        bad.memory = 0xDEAD;
        bad.ranges.push_back({0, 8});
        VKR_CHECK(!backend.write_memory_ranges(make_write({bad})).ok);
        vkrpc::MemoryUpload oob;
        oob.memory = mem.memory;
        oob.ranges.push_back({4096, 16}); // past the 4096-byte allocation
        VKR_CHECK(!backend.write_memory_ranges(make_write({oob})).ok);
    }
    // A write to non-coherent (device-local) memory is rejected even though it exists (the
    // backend stays a defense if the ICD tracker is bypassed).
    {
        vkrpc::AllocateMemoryRequest dam = am;
        dam.memory_type_index = device_local_type;
        const vkrpc::AllocateMemoryResponse dmem = backend.allocate_memory(dam);
        VKR_CHECK(dmem.ok);
        vkrpc::MemoryUpload du;
        du.memory = dmem.memory;
        du.ranges.push_back({0, 16});
        VKR_CHECK(!backend.write_memory_ranges(make_write({du})).ok);
    }

    // Build the rest of the draw graph (surface/swapchain/view/render
    // pass/framebuffer/shaders/layout).
    const int kFormat = 44;
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf.surface;
    scr.image_format = kFormat;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const vkrpc::CreateSwapchainResponse sc = backend.create_swapchain(scr);
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc.swapchain;
    const std::uint64_t img0 = backend.get_swapchain_images(gir).images.front();
    vkrpc::CreateImageViewRequest ivr;
    ivr.image = img0;
    ivr.view_type = 1;
    ivr.format = kFormat;
    ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = 0;
    ivr.aspect = 1;
    ivr.base_mip_level = 0;
    ivr.level_count = 1;
    ivr.base_array_layer = 0;
    ivr.layer_count = 1;
    const vkrpc::CreateImageViewResponse iv = backend.create_image_view(ivr);
    auto make_sm = [&](std::size_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code = std::string(bytes, '\0');
        r.code_size = bytes;
        return r;
    };
    const std::uint64_t vs = backend.create_shader_module(make_sm(8)).shader_module;
    const std::uint64_t fs = backend.create_shader_module(make_sm(8)).shader_module;
    vkrpc::CreateRenderPassRequest rpr;
    rpr.device = cd.device;
    vkrpc::AttachmentDesc att;
    att.format = kFormat;
    att.samples = 1;
    att.load_op = 1;
    att.store_op = 0;
    att.stencil_load_op = 2;
    att.stencil_store_op = 1;
    att.initial_layout = 0;
    att.final_layout = 1000001002;
    rpr.attachments.push_back(att);
    rpr.color_attachment = 0;
    rpr.color_layout = 2;
    const std::uint64_t rp = backend.create_render_pass(rpr).render_pass;
    vkrpc::CreateFramebufferRequest fbr;
    fbr.device = cd.device;
    fbr.render_pass = rp;
    fbr.image_view = iv.image_view;
    fbr.width = 256;
    fbr.height = 256;
    fbr.layers = 1;
    const std::uint64_t fb = backend.create_framebuffer(fbr).framebuffer;
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 0;
    const std::uint64_t pl = backend.create_pipeline_layout(plr).pipeline_layout;

    // A VBO graphics pipeline: one VERTEX-rate binding 0 (stride 20) + 2 attributes (pos float2 at
    // 0, color float3 at 8). Validate the subset rejections too.
    auto make_vbo_gp = [&]() {
        vkrpc::CreateGraphicsPipelinesRequest r;
        r.device = cd.device;
        vkrpc::ShaderStageDesc s0;
        s0.stage = 1;
        s0.module = vs;
        s0.entry = "main";
        vkrpc::ShaderStageDesc s1;
        s1.stage = 16;
        s1.module = fs;
        s1.entry = "main";
        r.stages = {s0, s1};
        r.topology = 3;
        vkrpc::VertexBindingDesc vb;
        vb.binding = 0;
        vb.stride = 20;
        vb.input_rate = 0; // VERTEX
        r.vertex_bindings = {vb};
        vkrpc::VertexAttributeDesc a0;
        a0.location = 0;
        a0.binding = 0;
        a0.format = 103; // R32G32_SFLOAT
        a0.offset = 0;
        vkrpc::VertexAttributeDesc a1;
        a1.location = 1;
        a1.binding = 0;
        a1.format = 106; // R32G32B32_SFLOAT
        a1.offset = 8;
        r.vertex_attributes = {a0, a1};
        r.vertex_binding_count = 1;
        r.vertex_attribute_count = 2;
        r.cull_mode = 0; // NONE
        r.front_face = 1;
        r.dynamic_states = {0, 1};
        r.layout = pl;
        r.render_pass = rp;
        r.subpass = 0;
        return r;
    };
    const vkrpc::CreateGraphicsPipelinesResponse gp =
        backend.create_graphics_pipelines(make_vbo_gp());
    VKR_CHECK(gp.ok && gp.pipeline != 0);
    {
        auto r = make_vbo_gp();
        r.vertex_attribute_count = 1; // count disagrees with the array
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        // vertex-attr-divisor: the mock now MATCHES the real backend, which forwards vertex input
        // to the host and validates only stride/offset <= u32. INSTANCE-rate, a host-gated format,
        // and stride 0 are all ACCEPTED (Vulkan-validity is the host's, mock == real). The narrow
        // mock used to reject these.
        r = make_vbo_gp();
        r.vertex_bindings[0].input_rate = 1; // INSTANCE now accepted
        VKR_CHECK(backend.create_graphics_pipelines(r).ok);
        r = make_vbo_gp();
        r.vertex_attributes[0].format = 999; // host-gated format now accepted by the mock
        VKR_CHECK(backend.create_graphics_pipelines(r).ok);
        r = make_vbo_gp();
        r.vertex_bindings[0].stride = 0; // stride 0 now accepted
        VKR_CHECK(backend.create_graphics_pipelines(r).ok);
        r = make_vbo_gp();
        r.vertex_bindings[0].stride = (1LL << 33); // ... but a stride beyond u32 STILL rejects
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        // vertex-attr-divisor CONTENT gates through the mock (shared vertex_binding_divisors_ok).
        // cd enabled the ext + feature, so divisor 1 and 2 are accepted; a missing binding rejects.
        auto with_divisor = [&](int binding, long long divisor) {
            auto rr = make_vbo_gp();
            rr.vertex_divisor_present = 1;
            vkrpc::VertexBindingDivisorDesc d;
            d.binding = binding;
            d.divisor = divisor;
            rr.vertex_binding_divisors = {d};
            return rr;
        };
        VKR_CHECK(backend.create_graphics_pipelines(with_divisor(0, 1)).ok); // divisor 1
        VKR_CHECK(
            backend.create_graphics_pipelines(with_divisor(0, 2)).ok); // divisor 2 (feature on)
        VKR_CHECK(
            !backend.create_graphics_pipelines(with_divisor(5, 1)).ok); // names a missing binding
        r = make_vbo_gp();
        r.vertex_divisor_present = 1; // present flag but empty array
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        r = make_vbo_gp();
        r.vertex_divisor_present = 0; // array carried without the present flag
        vkrpc::VertexBindingDivisorDesc dd;
        dd.binding = 0;
        dd.divisor = 1;
        r.vertex_binding_divisors = {dd};
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        // ext gate: the SAME divisor-1 pipeline on a device that did NOT enable
        // VK_EXT_vertex_attribute_divisor rejects (the backend re-asserts the ext, independently of
        // the ICD). The divisor validation runs before the layout lookup, so the shared cd layout
        // is fine here.
        r = with_divisor(0, 1);
        r.device = cd_noext.device;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);

        // geometry-stream VALUE gates through the mock (shared rasterization_stream_ok against the
        // modeled 4-stream/StreamSelect device): stream 0 + in-range nonzero accept; == max, out
        // of u32, stray-value-without-flag, and the no-ext device all reject.
        auto with_stream = [&](long long stream) {
            auto rr = make_vbo_gp();
            rr.stream_state_present = 1;
            rr.rasterization_stream = stream;
            return rr;
        };
        VKR_CHECK(backend.create_graphics_pipelines(with_stream(0)).ok);  // explicit stream zero
        VKR_CHECK(backend.create_graphics_pipelines(with_stream(2)).ok);  // nonzero (select true)
        VKR_CHECK(!backend.create_graphics_pipelines(with_stream(4)).ok); // == modeled max
        VKR_CHECK(!backend.create_graphics_pipelines(with_stream(1LL << 33)).ok); // out of u32
        r = make_vbo_gp();
        r.rasterization_stream = 1; // stray value without the present flag
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        r = with_stream(0); // ext/feature gate: the no-ext device rejects even stream zero
        r.device = cd_noext.device;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);

        // The mock advertises the stream capability like the real worker...
        VKR_CHECK_EQ(static_cast<int>(en.devices.front().caps.rasterization_stream_state), 1);
        // ...mirrors the structural extension invariant (a true scalar without the TF extension
        // is self-contradictory -- the real worker's exact named reason)...
        {
            vkrpc::CreateDeviceRequest bad;
            bad.instance = ci.instance;
            bad.physical_device = phys;
            bad.geometry_streams_feature_enabled = 1; // ...but no VK_EXT_transform_feedback
            const auto badr = backend.create_device(bad);
            VKR_CHECK(!badr.ok);
            VKR_CHECK(badr.reason.find("without VK_EXT_transform_feedback") != std::string::npos);
        }
        // ...and treats the -1 OMITTED sentinel (an older ICD's payload) as DISABLED: the
        // device creates, but a stream pNext on it rejects (the mock is a scalar-only oracle;
        // the real worker is the derive-from-chain site -- integration_real_draw).
        {
            vkrpc::CreateDeviceRequest omitted;
            omitted.instance = ci.instance;
            omitted.physical_device = phys;
            omitted.enabled_extensions = {vkrpc::kTransformFeedbackExtensionName};
            omitted.geometry_streams_feature_enabled = vkrpc::kGeometryStreamsScalarOmitted;
            const auto omr = backend.create_device(omitted);
            VKR_CHECK(omr.ok);
            // Same-device resources (the mock's ownership checks run before the stream gate, so
            // borrowing cd.device's modules would reject for the WRONG reason).
            auto om_sm = make_sm(8);
            om_sm.device = omr.device;
            const std::uint64_t om_vs = backend.create_shader_module(om_sm).shader_module;
            const std::uint64_t om_fs = backend.create_shader_module(om_sm).shader_module;
            vkrpc::CreateRenderPassRequest om_rpr = rpr;
            om_rpr.device = omr.device;
            const std::uint64_t om_rp = backend.create_render_pass(om_rpr).render_pass;
            vkrpc::CreatePipelineLayoutRequest om_plr = plr;
            om_plr.device = omr.device;
            const std::uint64_t om_pl = backend.create_pipeline_layout(om_plr).pipeline_layout;
            r = with_stream(0);
            r.device = omr.device;
            r.stages[0].module = om_vs;
            r.stages[1].module = om_fs;
            r.render_pass = om_rp;
            r.layout = om_pl;
            const auto omp = backend.create_graphics_pipelines(r);
            VKR_CHECK(!omp.ok);
            // Pin the REASON: the feature gate fired, not a cross-device handle lookup.
            VKR_CHECK(omp.reason.find("geometryStreams feature was not") != std::string::npos);
        }
        // An OUT-OF-DOMAIN scalar -- the INVALID (-2) a forged/transmitted -1
        // decodes to, and any other stray value -- rejects by name (mock == real), so legacy
        // status cannot be claimed past the normalization boundary.
        for (const int forged : {static_cast<int>(vkrpc::kGeometryStreamsScalarInvalid), 2, -5}) {
            vkrpc::CreateDeviceRequest inv;
            inv.instance = ci.instance;
            inv.physical_device = phys;
            inv.enabled_extensions = {vkrpc::kTransformFeedbackExtensionName};
            inv.geometry_streams_feature_enabled = forged;
            const auto invr = backend.create_device(inv);
            VKR_CHECK(!invr.ok);
            VKR_CHECK(invr.reason.find("must be 0 or 1 when present") != std::string::npos);
        }
    }

    auto begin = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "begin_render_pass";
        c.render_pass = rp;
        c.framebuffer = fb;
        c.render_area_w = 256;
        c.render_area_h = 256;
        return c;
    };
    auto bind_pipe = [&](std::uint64_t p) {
        vkrpc::RecordedCommand c;
        c.kind = "bind_pipeline";
        c.pipeline = p;
        return c;
    };
    auto vp = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_viewport";
        c.vp_w = 256.0;
        c.vp_h = 256.0;
        c.vp_max_depth = 1.0;
        return c;
    };
    auto scis = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_scissor";
        c.sc_w = 256;
        c.sc_h = 256;
        return c;
    };
    auto bind_vb = [&](std::uint64_t b) {
        vkrpc::RecordedCommand c;
        c.kind = "bind_vertex_buffers";
        c.first_binding = 0;
        c.vertex_buffers = {b};
        c.vertex_buffer_offsets = {0};
        return c;
    };
    auto draw = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "draw";
        c.vertex_count = 3;
        c.instance_count = 1;
        c.first_vertex = 0;
        c.first_instance = 0;
        return c;
    };
    auto end = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "end_render_pass";
        return c;
    };
    auto rec = [&](std::vector<vkrpc::RecordedCommand> cmds) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        r.commands = std::move(cmds);
        return backend.record_command_buffer(r).ok;
    };
    // Valid: bind the vertex buffer before the draw.
    VKR_CHECK(
        rec({begin(), bind_pipe(gp.pipeline), vp(), scis(), bind_vb(buf.buffer), draw(), end()}));
    // Rejection: the VBO pipeline needs a vertex buffer, but none was bound.
    VKR_CHECK(!rec({begin(), bind_pipe(gp.pipeline), vp(), scis(), draw(), end()}));
    // Rejection: bind_vertex_buffers with mismatched buffers/offsets, and a non-VERTEX buffer
    // handle.
    {
        vkrpc::RecordedCommand bad = bind_vb(buf.buffer);
        bad.vertex_buffer_offsets = {}; // length mismatch
        VKR_CHECK(!rec({begin(), bind_pipe(gp.pipeline), vp(), scis(), bad, draw(), end()}));
        vkrpc::RecordedCommand unknown = bind_vb(0xDEAD);
        VKR_CHECK(!rec({begin(), bind_pipe(gp.pipeline), vp(), scis(), unknown, draw(), end()}));
    }

    // Submit the good recording, then prove buffer-destroy invalidates it.
    VKR_CHECK(
        rec({begin(), bind_pipe(gp.pipeline), vp(), scis(), bind_vb(buf.buffer), draw(), end()}));
    vkrpc::QueueSubmitRequest qs;
    qs.queue = q.queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok);
    // The device still has the live buffer -> destroy_device is blocked.
    vkrpc::HandleRequest hdev;
    hdev.handle = cd.device;
    VKR_CHECK(!backend.destroy_device(hdev).ok);
    vkrpc::HandleRequest hbuf;
    hbuf.handle = buf.buffer;
    VKR_CHECK(backend.destroy_buffer(hbuf).ok);
    VKR_CHECK(!backend.destroy_buffer(hbuf).ok); // double destroy
    VKR_CHECK(!backend.queue_submit(qs).ok);     // recording invalidated by the buffer destroy
}

// Descriptor surface + per-frame UBO: wire round-trips, incl. the
// VK_WHOLE_SIZE = UINT64_MAX survival and the bind_descriptor_sets recorded command.
void test_descriptor_surface_wire() {
    // Set layout: one UNIFORM_BUFFER binding, VERTEX stage.
    vkrpc::CreateDescriptorSetLayoutRequest dsl;
    dsl.device = 7;
    vkrpc::DescriptorSetLayoutBindingDesc b;
    b.binding = 0;
    b.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
    b.descriptor_count = 1;
    b.stage_flags = 1; // VERTEX
    dsl.bindings.push_back(b);
    const auto dsl2 = vkrpc::CreateDescriptorSetLayoutRequest::from_body(dsl.to_body());
    VKR_CHECK_EQ(dsl2.device, static_cast<std::uint64_t>(7));
    VKR_CHECK_EQ(static_cast<int>(dsl2.bindings.size()), 1);
    VKR_CHECK_EQ(dsl2.bindings[0].descriptor_type, vkrpc::kDescriptorTypeUniformBuffer);
    VKR_CHECK_EQ(dsl2.bindings[0].stage_flags, static_cast<long long>(1));

    // Pool: one UNIFORM_BUFFER size.
    vkrpc::CreateDescriptorPoolRequest dp;
    dp.device = 7;
    dp.max_sets = 4;
    dp.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 8});
    const auto dp2 = vkrpc::CreateDescriptorPoolRequest::from_body(dp.to_body());
    VKR_CHECK_EQ(dp2.max_sets, 4);
    VKR_CHECK_EQ(static_cast<int>(dp2.pool_sizes.size()), 1);
    VKR_CHECK_EQ(dp2.pool_sizes[0].descriptor_count, 8);

    // Allocate: request + response (handle arrays).
    vkrpc::AllocateDescriptorSetsRequest ads;
    ads.device = 7;
    ads.pool = 0x55;
    ads.set_layouts = {0x100000001ull, 0x2};
    const auto ads2 = vkrpc::AllocateDescriptorSetsRequest::from_body(ads.to_body());
    VKR_CHECK_EQ(ads2.set_layouts.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(ads2.set_layouts[0], 0x100000001ull);
    vkrpc::AllocateDescriptorSetsResponse adr;
    adr.ok = true;
    adr.descriptor_sets = {0x9, 0xA};
    const auto adr2 = vkrpc::AllocateDescriptorSetsResponse::from_body(adr.to_body());
    VKR_CHECK(adr2.ok);
    VKR_CHECK_EQ(adr2.descriptor_sets.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(adr2.descriptor_sets[1], static_cast<std::uint64_t>(0xA));

    // Update: VK_WHOLE_SIZE (UINT64_MAX) and a 64-bit offset must round-trip exactly
    // (decimal-string u64, never signed get_i64 collapsing to 0).
    vkrpc::UpdateDescriptorSetsRequest ud;
    ud.device = 7;
    vkrpc::WriteDescriptorSetDesc w;
    w.dst_set = 0x9;
    w.dst_binding = 0;
    w.dst_array_element = 0;
    w.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
    w.descriptor_count = 1;
    vkrpc::DescriptorBufferInfoDesc bi;
    bi.buffer = 0x77;
    bi.offset = 0;
    bi.range = vkrpc::kVkWholeSize;
    w.buffer_infos.push_back(bi);
    ud.writes.push_back(w);
    const auto ud2 = vkrpc::UpdateDescriptorSetsRequest::from_body(ud.to_body());
    VKR_CHECK_EQ(static_cast<int>(ud2.writes.size()), 1);
    VKR_CHECK_EQ(ud2.writes[0].buffer_infos.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(ud2.writes[0].buffer_infos[0].range, vkrpc::kVkWholeSize); // UINT64_MAX survives
    VKR_CHECK_EQ(ud2.writes[0].buffer_infos[0].buffer, static_cast<std::uint64_t>(0x77));

    // Pipeline layout now carries a set-layout list.
    vkrpc::CreatePipelineLayoutRequest pl;
    pl.device = 7;
    pl.set_layout_count = 1;
    pl.push_constant_range_count = 0;
    pl.set_layouts = {0x100000001ull};
    const auto pl2 = vkrpc::CreatePipelineLayoutRequest::from_body(pl.to_body());
    VKR_CHECK_EQ(pl2.set_layout_count, 1);
    VKR_CHECK_EQ(pl2.set_layouts.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(pl2.set_layouts[0], 0x100000001ull);

    // bind_descriptor_sets recorded command round-trip.
    vkrpc::RecordCommandBufferRequest rc;
    rc.command_buffer = 3;
    vkrpc::RecordedCommand c;
    c.kind = "bind_descriptor_sets";
    c.desc_layout = 0x42;
    c.first_set = 0;
    c.descriptor_sets = {0x9, 0xA};
    rc.commands.push_back(c);
    const auto rc2 = vkrpc::RecordCommandBufferRequest::from_body(rc.to_body());
    VKR_CHECK_EQ(static_cast<int>(rc2.commands.size()), 1);
    VKR_CHECK_EQ(rc2.commands[0].desc_layout, static_cast<std::uint64_t>(0x42));
    VKR_CHECK_EQ(rc2.commands[0].first_set, 0);
    VKR_CHECK_EQ(rc2.commands[0].descriptor_sets.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(rc2.commands[0].descriptor_sets[1], static_cast<std::uint64_t>(0xA));
}

// Descriptor surface + per-frame UBO mock state machine (the dual-platform oracle): object
// lifecycle, update validation, bind/draw exactness, the destroy/UAF edges.
void test_descriptor_surface_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = cd.device;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(gq).queue;
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const std::uint64_t pool_cmd = backend.create_command_pool(cpr).command_pool;
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = pool_cmd;
    acb.count = 1;
    const std::uint64_t cmd = backend.allocate_command_buffers(acb).command_buffers.front();

    // Coherent memory + a UNIFORM buffer bound at offset 0.
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
    mpq.physical_device = phys;
    const auto mp = backend.get_physical_device_memory_properties(mpq);
    int coherent_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(coherent_type >= 0);
    vkrpc::AllocateMemoryRequest am;
    am.device = cd.device;
    am.allocation_size = 64 * 1024;
    am.memory_type_index = coherent_type;
    const std::uint64_t mem = backend.allocate_memory(am).memory;
    auto make_ubo = [&](std::uint64_t size, std::uint64_t offset) {
        vkrpc::CreateBufferRequest cbq;
        cbq.device = cd.device;
        cbq.size = size;
        cbq.usage = vkrpc::kBufferUsageUniformBuffer;
        cbq.sharing_mode = 0;
        const std::uint64_t b = backend.create_buffer(cbq).buffer;
        vkrpc::BindBufferMemoryRequest bbm;
        bbm.buffer = b;
        bbm.memory = mem;
        bbm.memory_offset = offset;
        VKR_CHECK(backend.bind_buffer_memory(bbm).ok);
        return b;
    };
    const std::uint64_t ubo_small = make_ubo(256, 0);
    const std::uint64_t ubo_big = make_ubo(32768, 4096); // > kUniformBufferRangeCap (16384)
    // A UNIFORM buffer that is created but NOT bound.
    vkrpc::CreateBufferRequest cbq_unbound;
    cbq_unbound.device = cd.device;
    cbq_unbound.size = 256;
    cbq_unbound.usage = vkrpc::kBufferUsageUniformBuffer;
    cbq_unbound.sharing_mode = 0;
    const std::uint64_t ubo_unbound = backend.create_buffer(cbq_unbound).buffer;
    // A VERTEX-only buffer to prove descriptor writes reject a non-UNIFORM buffer.
    vkrpc::CreateBufferRequest cbq_vbo;
    cbq_vbo.device = cd.device;
    cbq_vbo.size = 256;
    cbq_vbo.usage = vkrpc::kBufferUsageVertexBuffer;
    cbq_vbo.sharing_mode = 0;
    const std::uint64_t vbo_only = backend.create_buffer(cbq_vbo).buffer;
    {
        vkrpc::BindBufferMemoryRequest bbm;
        bbm.buffer = vbo_only;
        bbm.memory = mem;
        bbm.memory_offset = 48 * 1024;
        VKR_CHECK(backend.bind_buffer_memory(bbm).ok);
    }

    // Set layout (binding 0, UNIFORM_BUFFER, count 1, VERTEX) + subset rejections.
    auto make_dsl = [&]() {
        vkrpc::CreateDescriptorSetLayoutRequest r;
        r.device = cd.device;
        vkrpc::DescriptorSetLayoutBindingDesc b;
        b.binding = 0;
        b.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        b.descriptor_count = 1;
        b.stage_flags = 1; // VERTEX
        r.bindings.push_back(b);
        return r;
    };
    const std::uint64_t set_layout = backend.create_descriptor_set_layout(make_dsl()).set_layout;
    VKR_CHECK(set_layout != 0);
    {
        // (GL/zink): previously out-of-subset layouts are now ACCEPTED (faithfully
        // forwarded) -- a STORAGE_BUFFER type, a GEOMETRY stage, and an empty layout (zink mints
        // these). A negative descriptor count is still rejected.
        auto r = make_dsl();
        r.bindings[0].descriptor_type = 7; // STORAGE_BUFFER
        VKR_CHECK(backend.create_descriptor_set_layout(r).ok);
        r = make_dsl();
        r.bindings[0].stage_flags = 8; // GEOMETRY
        VKR_CHECK(backend.create_descriptor_set_layout(r).ok);
        r = make_dsl();
        r.bindings.clear(); // empty layout
        VKR_CHECK(backend.create_descriptor_set_layout(r).ok);
        r = make_dsl();
        r.bindings[0].descriptor_count = -1; // genuinely invalid
        VKR_CHECK(!backend.create_descriptor_set_layout(r).ok);
    }
    // A second, distinct set layout (proves the bind layout-exactness check).
    const std::uint64_t set_layout_b = backend.create_descriptor_set_layout(make_dsl()).set_layout;

    // Pool: one UNIFORM_BUFFER size, maxSets 4 + accounting/subset rejections.
    auto make_pool = [&]() {
        vkrpc::CreateDescriptorPoolRequest r;
        r.device = cd.device;
        r.max_sets = 4;
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 8});
        return r;
    };
    const std::uint64_t pool = backend.create_descriptor_pool(make_pool()).pool;
    VKR_CHECK(pool != 0);
    {
        // (GL/zink): a duplicate pool-size type and a STORAGE_BUFFER type are now ACCEPTED
        // (the real pool is the per-type budget authority); maxSets 0 is still rejected.
        auto r = make_pool();
        r.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 1}); // duplicate type
        VKR_CHECK(backend.create_descriptor_pool(r).ok);
        r = make_pool();
        r.pool_sizes[0].type = 7; // STORAGE_BUFFER
        VKR_CHECK(backend.create_descriptor_pool(r).ok);
        r = make_pool();
        r.max_sets = 0; // out of range
        VKR_CHECK(!backend.create_descriptor_pool(r).ok);
    }

    // Allocate: one set from each layout. Budget accounting rejects an oversize batch.
    auto alloc_one = [&](std::uint64_t layout) {
        vkrpc::AllocateDescriptorSetsRequest r;
        r.device = cd.device;
        r.pool = pool;
        r.set_layouts = {layout};
        return backend.allocate_descriptor_sets(r);
    };
    const std::uint64_t set = alloc_one(set_layout).descriptor_sets.front();
    const std::uint64_t set_uninit = alloc_one(set_layout).descriptor_sets.front();
    const std::uint64_t set_b = alloc_one(set_layout_b).descriptor_sets.front();
    const std::uint64_t set_neg = alloc_one(set_layout).descriptor_sets.front(); // 4th = maxSets
    {
        vkrpc::AllocateDescriptorSetsRequest r;
        r.device = cd.device;
        r.pool = pool;
        r.set_layouts = {set_layout, set_layout, set_layout}; // 3 more -> 7 > maxSets 4
        VKR_CHECK(!backend.allocate_descriptor_sets(r).ok);
        r.set_layouts.clear();
        VKR_CHECK(!backend.allocate_descriptor_sets(r).ok); // empty
    }

    // Update: validate-then-apply, with the negative cases.
    auto make_write_u = [&](std::uint64_t dst, std::uint64_t buffer, std::uint64_t range) {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = cd.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = dst;
        w.dst_binding = 0;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        w.descriptor_count = 1;
        w.buffer_infos.push_back({buffer, 0, range});
        r.writes.push_back(w);
        return r;
    };
    // Valid updates to `set`: ubo_small, then repoint to ubo_big (the worked example below
    // relies on the repoint). A rejected update POISONS its target, so the
    // negatives must target set_neg, NOT `set` (which the happy-path draw uses).
    VKR_CHECK(backend.update_descriptor_sets(make_write_u(set, ubo_small, vkrpc::kVkWholeSize)).ok);
    VKR_CHECK(
        backend.update_descriptor_sets(make_write_u(set, ubo_big, 4096)).ok); // set -> ubo_big
    // Negatives (each poisons set_neg, which is fine -- it is re-validated below): the
    // faithful update only rejects GENUINE errors -- an unbound / nonexistent buffer, a binding not
    // in the layout, an out-of-range array element, or an info-list whose length disagrees with the
    // count. Buffer usage / offset / range and intra-batch overlap are the host driver's job now.
    VKR_CHECK(
        !backend.update_descriptor_sets(make_write_u(set_neg, ubo_unbound, 256)).ok); // unbound
    VKR_CHECK(
        !backend.update_descriptor_sets(make_write_u(set_neg, 0, 256)).ok); // nonexistent buffer
    {
        auto r = make_write_u(set_neg, ubo_small, 256);
        r.writes[0].descriptor_type =
            1; // CIS type but buffer_infos populated -> info-list mismatch
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
        r = make_write_u(set_neg, ubo_small, 256);
        r.writes[0].dst_binding = 5; // binding not in the layout
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
        r = make_write_u(set_neg, ubo_small, 256);
        r.writes[0].dst_array_element = 1; // out of the binding's single element
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
        // Info-list length must match the descriptor count.
        r = make_write_u(set_neg, ubo_small, 256);
        r.writes[0].descriptor_count = 2; // 2 vs the single buffer-info entry
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
        r = make_write_u(set_neg, ubo_small, vkrpc::kVkWholeSize);
        r.writes[0].buffer_infos[0].offset = 256; // no byte remains in the logical buffer
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
        r = make_write_u(set_neg, ubo_small, 257); // explicit range crosses the logical end
        VKR_CHECK(!backend.update_descriptor_sets(r).ok);
        r = make_write_u(set_neg, ubo_small, 256); // exact logical extent remains valid
        VKR_CHECK(backend.update_descriptor_sets(r).ok);
    }

    // Pipeline layout referencing the set layout + the draw graph (bufferless pipeline).
    const int kFormat = 44;
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const std::uint64_t surf = backend.create_surface(sr).surface;
    vkrpc::CreateSwapchainRequest scr;
    scr.device = cd.device;
    scr.surface = surf;
    scr.image_format = kFormat;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const std::uint64_t sc = backend.create_swapchain(scr).swapchain;
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc;
    const std::uint64_t img0 = backend.get_swapchain_images(gir).images.front();
    vkrpc::CreateImageViewRequest ivr;
    ivr.image = img0;
    ivr.view_type = 1;
    ivr.format = kFormat;
    ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = 0;
    ivr.aspect = 1;
    ivr.base_mip_level = 0;
    ivr.level_count = 1;
    ivr.base_array_layer = 0;
    ivr.layer_count = 1;
    const std::uint64_t iv = backend.create_image_view(ivr).image_view;
    VKR_CHECK(iv != 0);
    auto make_sm = [&](std::size_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = cd.device;
        r.code = std::string(bytes, '\0');
        r.code_size = bytes;
        return r;
    };
    const std::uint64_t vs = backend.create_shader_module(make_sm(8)).shader_module;
    const std::uint64_t fs = backend.create_shader_module(make_sm(8)).shader_module;
    vkrpc::CreateRenderPassRequest rpr;
    rpr.device = cd.device;
    vkrpc::AttachmentDesc att;
    att.format = kFormat;
    att.samples = 1;
    att.load_op = 1;
    att.store_op = 0;
    att.stencil_load_op = 2;
    att.stencil_store_op = 1;
    att.initial_layout = 0;
    att.final_layout = 1000001002;
    rpr.attachments.push_back(att);
    rpr.color_attachment = 0;
    rpr.color_layout = 2;
    const std::uint64_t rp = backend.create_render_pass(rpr).render_pass;
    vkrpc::CreateFramebufferRequest fbr;
    fbr.device = cd.device;
    fbr.render_pass = rp;
    fbr.image_view = iv;
    fbr.width = 256;
    fbr.height = 256;
    fbr.layers = 1;
    const std::uint64_t fb = backend.create_framebuffer(fbr).framebuffer;
    // Pipeline layout WITH the set layout (the addition) + an empty one for the negatives.
    auto make_pl = [&](std::vector<std::uint64_t> sls) {
        vkrpc::CreatePipelineLayoutRequest r;
        r.device = cd.device;
        r.set_layout_count = static_cast<int>(sls.size());
        r.push_constant_range_count = 0;
        r.set_layouts = std::move(sls);
        return r;
    };
    const std::uint64_t pl = backend.create_pipeline_layout(make_pl({set_layout})).pipeline_layout;
    VKR_CHECK(pl != 0);
    {
        auto r = make_pl({set_layout});
        r.set_layout_count = 0; // count disagrees with the list
        VKR_CHECK(!backend.create_pipeline_layout(r).ok);
        r = make_pl({0xDEAD}); // unknown set layout handle
        VKR_CHECK(!backend.create_pipeline_layout(r).ok);
        // a VALID push-constant range is accepted; count must agree with the carried list.
        r = make_pl({set_layout});
        r.push_constant_range_count = 1; // disagrees with the empty ranges list
        VKR_CHECK(!backend.create_pipeline_layout(r).ok);
        r = make_pl({set_layout});
        {
            vkrpc::PushConstantRange pc;
            pc.stage_flags = 0x1; // VK_SHADER_STAGE_VERTEX_BIT
            pc.offset = 0;
            pc.size = 64;
            r.push_constant_ranges.push_back(pc);
            r.push_constant_range_count = 1;
        }
        const std::uint64_t pcpl = backend.create_pipeline_layout(r).pipeline_layout;
        VKR_CHECK(pcpl != 0);
        vkrpc::HandleRequest dpcpl;
        dpcpl.handle = pcpl;
        VKR_CHECK(backend.destroy_pipeline_layout(dpcpl).ok); // don't dangle the set layout
        r.push_constant_ranges[0].size = 3;                   // not 4-aligned -> rejected
        VKR_CHECK(!backend.create_pipeline_layout(r).ok);
        // Over the set-layout cap (parity): kMax+1 layouts rejected.
        r = make_pl(std::vector<std::uint64_t>(
            static_cast<std::size_t>(vkrpc::kMaxPipelineLayoutSetLayouts) + 1, set_layout));
        VKR_CHECK(!backend.create_pipeline_layout(r).ok);
    }
    // A bufferless graphics pipeline whose layout has the set layout.
    vkrpc::CreateGraphicsPipelinesRequest gpr;
    gpr.device = cd.device;
    vkrpc::ShaderStageDesc s0;
    s0.stage = 1;
    s0.module = vs;
    s0.entry = "main";
    vkrpc::ShaderStageDesc s1;
    s1.stage = 16;
    s1.module = fs;
    s1.entry = "main";
    gpr.stages = {s0, s1};
    gpr.topology = 3;
    gpr.vertex_binding_count = 0;
    gpr.vertex_attribute_count = 0;
    gpr.cull_mode = 0;
    gpr.front_face = 1;
    gpr.dynamic_states = {0, 1};
    gpr.layout = pl;
    gpr.render_pass = rp;
    gpr.subpass = 0;
    const std::uint64_t pipe = backend.create_graphics_pipelines(gpr).pipeline;
    VKR_CHECK(pipe != 0);

    auto begin = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "begin_render_pass";
        c.render_pass = rp;
        c.framebuffer = fb;
        c.render_area_w = 256;
        c.render_area_h = 256;
        return c;
    };
    auto simple = [&](const char* kind) {
        vkrpc::RecordedCommand c;
        c.kind = kind;
        return c;
    };
    auto bind_pipe = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "bind_pipeline";
        c.pipeline = pipe;
        return c;
    };
    auto vp = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_viewport";
        c.vp_w = 256.0;
        c.vp_h = 256.0;
        c.vp_max_depth = 1.0;
        return c;
    };
    auto scis = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_scissor";
        c.sc_w = 256;
        c.sc_h = 256;
        return c;
    };
    auto bind_sets = [&](std::uint64_t layout, std::vector<std::uint64_t> sets) {
        vkrpc::RecordedCommand c;
        c.kind = "bind_descriptor_sets";
        c.desc_layout = layout;
        c.first_set = 0;
        c.descriptor_sets = std::move(sets);
        return c;
    };
    auto draw = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "draw";
        c.vertex_count = 3;
        c.instance_count = 1;
        c.first_vertex = 0;
        c.first_instance = 0;
        return c;
    };
    auto rec = [&](std::vector<vkrpc::RecordedCommand> cmds) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        r.commands = std::move(cmds);
        return backend.record_command_buffer(r).ok;
    };
    // Valid: bind the (updated) set, then draw.
    VKR_CHECK(rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set}), draw(),
                   simple("end_render_pass")}));
    // Reject: draw without any descriptor bind (the layout needs one).
    VKR_CHECK(!rec({begin(), bind_pipe(), vp(), scis(), draw(), simple("end_render_pass")}));
    // Reject: bind a set allocated from a DIFFERENT layout than the pipeline layout's slot 0.
    VKR_CHECK(!rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set_b}), draw(),
                    simple("end_render_pass")}));
    // Reject: bind an UNINITIALIZED set (allocated, never updated) -> draw not ready.
    VKR_CHECK(!rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set_uninit}), draw(),
                    simple("end_render_pass")}));
    // Reject: wrong set count for the layout (exact).
    VKR_CHECK(!rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set, set_b}), draw(),
                    simple("end_render_pass")}));

    // Poison-on-failed-update regression: a valid update makes set_neg
    // draw-ready (a recording binds + draws it successfully); then an INVALID update to it POISONS
    // it, so the same bind/draw is refused -- a rejected update cannot leave a previously-valid set
    // draw-ready.
    VKR_CHECK(backend.update_descriptor_sets(make_write_u(set_neg, ubo_small, 256)).ok);
    VKR_CHECK(rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set_neg}), draw(),
                   simple("end_render_pass")}));
    {
        auto bad = make_write_u(set_neg, ubo_small, 256);
        bad.writes[0].dst_binding =
            5; // genuinely invalid (binding not in layout) -> poisons set_neg
        VKR_CHECK(!backend.update_descriptor_sets(bad).ok);
    }
    VKR_CHECK(!rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set_neg}), draw(),
                    simple("end_render_pass")}));
    // Re-validate set_neg so it is not a confound for the buffer-destroy tests below.
    VKR_CHECK(backend.update_descriptor_sets(make_write_u(set_neg, ubo_small, 256)).ok);

    // Destroy ordering + the CB -> set -> buffer UAF consult.
    VKR_CHECK(rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set}), draw(),
                   simple("end_render_pass")}));
    vkrpc::QueueSubmitRequest qs;
    qs.queue = queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok);
    // The set was updated ubo_small -> ubo_big, so it now references ubo_big. The dynamic
    // set->buffer consult invalidates ONLY on the CURRENT reference (worked
    // example): destroying an unrelated buffer, or the OLD (repointed-away) ubo_small, leaves the
    // recording valid.
    {
        vkrpc::HandleRequest hv;
        hv.handle = vbo_only;
        VKR_CHECK(backend.destroy_buffer(hv).ok);
        VKR_CHECK(backend.queue_submit(qs).ok); // unrelated buffer -> still valid
        hv.handle = ubo_small;
        VKR_CHECK(backend.destroy_buffer(hv).ok);
        VKR_CHECK(backend.queue_submit(qs).ok); // repointed-away buffer -> still valid
    }
    // Destroying ubo_big (the buffer the set CURRENTLY references) marks the slot dangling and
    // invalidates the recorded command buffer through CB -> set -> buffer.
    {
        vkrpc::HandleRequest hu;
        hu.handle = ubo_big;
        VKR_CHECK(backend.destroy_buffer(hu).ok);
        VKR_CHECK(!backend.queue_submit(qs).ok);
    }
    // Set-layout destroy is blocked while a live pipeline layout references it and while
    // live sets were allocated from it.
    {
        vkrpc::HandleRequest hsl;
        hsl.handle = set_layout;
        VKR_CHECK(!backend.destroy_descriptor_set_layout(hsl).ok); // referenced + has live sets
        vkrpc::HandleRequest hdev;
        hdev.handle = cd.device;
        VKR_CHECK(!backend.destroy_device(hdev).ok); // descriptor children block the device
    }
    // Destroying the pipeline layout a recording baked invalidates it too.
    // Rebuild a valid recording first (the buffer-destroy above left the CB invalid + the set
    // dangling): a fresh UBO + update repoints the set at a live buffer.
    const std::uint64_t ubo_fresh = make_ubo(256, 8 * 1024);
    VKR_CHECK(backend.update_descriptor_sets(make_write_u(set, ubo_fresh, 256)).ok);
    VKR_CHECK(rec({begin(), bind_pipe(), vp(), scis(), bind_sets(pl, {set}), draw(),
                   simple("end_render_pass")}));
    VKR_CHECK(backend.queue_submit(qs).ok);
    {
        vkrpc::HandleRequest hpl;
        hpl.handle = pl;
        VKR_CHECK(backend.destroy_pipeline_layout(hpl).ok);
        VKR_CHECK(!backend.queue_submit(qs).ok); // recording invalidated by the layout destroy
    }
    // Tear down (pipeline layout already destroyed): pipeline -> pool (cascades sets) -> set
    // layouts.
    vkrpc::HandleRequest h;
    h.handle = pipe;
    VKR_CHECK(backend.destroy_pipeline(h).ok);
    h.handle = pool;
    VKR_CHECK(backend.destroy_descriptor_pool(h).ok); // frees set / set_uninit / set_b
    h.handle = set_layout;
    VKR_CHECK(backend.destroy_descriptor_set_layout(h).ok); // now unreferenced + no live sets
    h.handle = set_layout_b;
    VKR_CHECK(backend.destroy_descriptor_set_layout(h).ok);
}

// A rejected cross-device descriptor update must not poison another device's set:
// the poison is scoped to the request device. Builds two full draw contexts on one instance and
// verifies a bad update on device A naming device B's set leaves B's set draw-ready.
void test_descriptor_cross_device_poison() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    const int kFormat = 44;

    // One device's full UBO-draw context + a draw-ready descriptor set.
    struct Ctx {
        std::uint64_t device = 0, cmd = 0, pl = 0, pipe = 0, rp = 0, fb = 0, set = 0, ubo = 0;
    };
    auto build = [&](Ctx& c) {
        vkrpc::CreateDeviceRequest cdr;
        cdr.instance = ci.instance;
        cdr.physical_device = phys;
        const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
        c.device = cd.device;
        vkrpc::GetDeviceQueueRequest gq;
        gq.device = cd.device;
        gq.queue_family_index = cd.queue_family_index;
        (void) backend.get_device_queue(gq);
        vkrpc::CreateCommandPoolRequest cpr;
        cpr.device = cd.device;
        cpr.queue_family_index = cd.queue_family_index;
        const std::uint64_t poolc = backend.create_command_pool(cpr).command_pool;
        vkrpc::AllocateCommandBuffersRequest acb;
        acb.command_pool = poolc;
        acb.count = 1;
        c.cmd = backend.allocate_command_buffers(acb).command_buffers.front();
        vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
        mpq.physical_device = phys;
        const auto mp = backend.get_physical_device_memory_properties(mpq);
        int coherent = -1;
        for (std::size_t i = 0; i < mp.types.size(); ++i) {
            const std::uint64_t want =
                vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
            if ((mp.types[i].property_flags & want) == want) {
                coherent = static_cast<int>(i);
            }
        }
        vkrpc::AllocateMemoryRequest am;
        am.device = cd.device;
        am.allocation_size = 4096;
        am.memory_type_index = coherent;
        const std::uint64_t mem = backend.allocate_memory(am).memory;
        vkrpc::CreateBufferRequest cbq;
        cbq.device = cd.device;
        cbq.size = 256;
        cbq.usage = vkrpc::kBufferUsageUniformBuffer;
        cbq.sharing_mode = 0;
        c.ubo = backend.create_buffer(cbq).buffer;
        vkrpc::BindBufferMemoryRequest bbm;
        bbm.buffer = c.ubo;
        bbm.memory = mem;
        bbm.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bbm).ok);
        vkrpc::CreateDescriptorSetLayoutRequest dslr;
        dslr.device = cd.device;
        vkrpc::DescriptorSetLayoutBindingDesc bd;
        bd.binding = 0;
        bd.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        bd.descriptor_count = 1;
        bd.stage_flags = 1;
        dslr.bindings.push_back(bd);
        const std::uint64_t sl = backend.create_descriptor_set_layout(dslr).set_layout;
        vkrpc::CreateDescriptorPoolRequest dpr;
        dpr.device = cd.device;
        dpr.max_sets = 1;
        dpr.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 1});
        const std::uint64_t dpool = backend.create_descriptor_pool(dpr).pool;
        vkrpc::AllocateDescriptorSetsRequest adsr;
        adsr.device = cd.device;
        adsr.pool = dpool;
        adsr.set_layouts = {sl};
        c.set = backend.allocate_descriptor_sets(adsr).descriptor_sets.front();
        vkrpc::UpdateDescriptorSetsRequest udr;
        udr.device = cd.device;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = c.set;
        w.dst_binding = 0;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        w.descriptor_count = 1;
        w.buffer_infos.push_back({c.ubo, 0, 256});
        udr.writes.push_back(w);
        VKR_CHECK(backend.update_descriptor_sets(udr).ok);
        vkrpc::CreateSurfaceRequest sr;
        sr.instance = ci.instance;
        const std::uint64_t surf = backend.create_surface(sr).surface;
        vkrpc::CreateSwapchainRequest scr;
        scr.device = cd.device;
        scr.surface = surf;
        scr.image_format = kFormat;
        scr.present_mode = 2;
        scr.width = 256;
        scr.height = 256;
        scr.min_image_count = 2;
        scr.image_usage = vkrpc::kImageUsageColorAttachment;
        const std::uint64_t sc = backend.create_swapchain(scr).swapchain;
        vkrpc::GetSwapchainImagesRequest gir;
        gir.swapchain = sc;
        const std::uint64_t img0 = backend.get_swapchain_images(gir).images.front();
        vkrpc::CreateImageViewRequest ivr;
        ivr.image = img0;
        ivr.view_type = 1;
        ivr.format = kFormat;
        ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = 0;
        ivr.aspect = 1;
        ivr.base_mip_level = 0;
        ivr.level_count = 1;
        ivr.base_array_layer = 0;
        ivr.layer_count = 1;
        const std::uint64_t iv = backend.create_image_view(ivr).image_view;
        auto sm = [&](std::size_t n) {
            vkrpc::CreateShaderModuleRequest r;
            r.device = cd.device;
            r.code = std::string(n, '\0');
            r.code_size = n;
            return backend.create_shader_module(r).shader_module;
        };
        const std::uint64_t vs = sm(8), fs = sm(8);
        vkrpc::CreateRenderPassRequest rpr;
        rpr.device = cd.device;
        vkrpc::AttachmentDesc att;
        att.format = kFormat;
        att.samples = 1;
        att.load_op = 1;
        att.store_op = 0;
        att.stencil_load_op = 2;
        att.stencil_store_op = 1;
        att.initial_layout = 0;
        att.final_layout = 1000001002;
        rpr.attachments.push_back(att);
        rpr.color_attachment = 0;
        rpr.color_layout = 2;
        c.rp = backend.create_render_pass(rpr).render_pass;
        vkrpc::CreateFramebufferRequest fbr;
        fbr.device = cd.device;
        fbr.render_pass = c.rp;
        fbr.image_view = iv;
        fbr.width = 256;
        fbr.height = 256;
        fbr.layers = 1;
        c.fb = backend.create_framebuffer(fbr).framebuffer;
        vkrpc::CreatePipelineLayoutRequest plr;
        plr.device = cd.device;
        plr.set_layout_count = 1;
        plr.push_constant_range_count = 0;
        plr.set_layouts = {sl};
        c.pl = backend.create_pipeline_layout(plr).pipeline_layout;
        vkrpc::CreateGraphicsPipelinesRequest gpr;
        gpr.device = cd.device;
        vkrpc::ShaderStageDesc s0;
        s0.stage = 1;
        s0.module = vs;
        s0.entry = "main";
        vkrpc::ShaderStageDesc s1;
        s1.stage = 16;
        s1.module = fs;
        s1.entry = "main";
        gpr.stages = {s0, s1};
        gpr.topology = 3;
        gpr.vertex_binding_count = 0;
        gpr.vertex_attribute_count = 0;
        gpr.cull_mode = 0;
        gpr.front_face = 1;
        gpr.dynamic_states = {0, 1};
        gpr.layout = c.pl;
        gpr.render_pass = c.rp;
        gpr.subpass = 0;
        c.pipe = backend.create_graphics_pipelines(gpr).pipeline;
    };
    Ctx a, b;
    build(a);
    build(b);

    // Record a UBO draw on a context binding its own set -> ok iff the set is draw-ready.
    auto draw_ok = [&](const Ctx& c) {
        auto cmd0 = [&](const char* k) {
            vkrpc::RecordedCommand r;
            r.kind = k;
            return r;
        };
        vkrpc::RecordedCommand begin = cmd0("begin_render_pass");
        begin.render_pass = c.rp;
        begin.framebuffer = c.fb;
        begin.render_area_w = 256;
        begin.render_area_h = 256;
        vkrpc::RecordedCommand bp = cmd0("bind_pipeline");
        bp.pipeline = c.pipe;
        vkrpc::RecordedCommand vp = cmd0("set_viewport");
        vp.vp_w = 256.0;
        vp.vp_h = 256.0;
        vp.vp_max_depth = 1.0;
        vkrpc::RecordedCommand sc = cmd0("set_scissor");
        sc.sc_w = 256;
        sc.sc_h = 256;
        vkrpc::RecordedCommand bds = cmd0("bind_descriptor_sets");
        bds.desc_layout = c.pl;
        bds.first_set = 0;
        bds.descriptor_sets = {c.set};
        vkrpc::RecordedCommand dr = cmd0("draw");
        dr.vertex_count = 3;
        dr.instance_count = 1;
        dr.first_vertex = 0;
        dr.first_instance = 0;
        vkrpc::RecordCommandBufferRequest req;
        req.command_buffer = c.cmd;
        req.commands = {begin, bp, vp, sc, bds, dr, cmd0("end_render_pass")};
        return backend.record_command_buffer(req).ok;
    };
    VKR_CHECK(draw_ok(b)); // device B's set is draw-ready

    // A malformed update on device A that names device B's set must reject WITHOUT poisoning B's
    // set.
    vkrpc::UpdateDescriptorSetsRequest bad;
    bad.device = a.device;
    vkrpc::WriteDescriptorSetDesc w;
    w.dst_set = b.set; // a set on device B
    w.dst_binding = 0;
    w.dst_array_element = 0;
    w.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
    w.descriptor_count = 1;
    w.buffer_infos.push_back({a.ubo, 0, 256}); // device-A buffer; cross-device set -> rejected
    bad.writes.push_back(w);
    VKR_CHECK(!backend.update_descriptor_sets(bad).ok);
    VKR_CHECK(draw_ok(b)); // device B's set STILL draw-ready (not poisoned by the device-A request)
}

// --- Textures + depth = literal vkcube  --------

// VkFormat values the test spells out (the wire reasons by value; these match <vulkan_core.h>).
constexpr int kFmtRgba8 = 37; // VK_FORMAT_R8G8B8A8_UNORM
constexpr int kFmtSwap = 44;  // an arbitrary swapchain color format used elsewhere in this file

void test_image_depth_wire() {
    // Format properties request/response.
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fp;
    fp.physical_device = 0x11;
    fp.format = kFmtRgba8;
    const auto fp2 = vkrpc::GetPhysicalDeviceFormatPropertiesRequest::from_body(fp.to_body());
    VKR_CHECK_EQ(fp2.physical_device, static_cast<std::uint64_t>(0x11));
    VKR_CHECK_EQ(fp2.format, kFmtRgba8);
    vkrpc::GetPhysicalDeviceFormatPropertiesResponse fpr;
    fpr.ok = true;
    fpr.linear_tiling_features = vkrpc::kFormatFeatureSampledImage;
    fpr.optimal_tiling_features = 0xFFFFFFFFull; // a full 32-bit flag set must survive
    const auto fpr2 = vkrpc::GetPhysicalDeviceFormatPropertiesResponse::from_body(fpr.to_body());
    VKR_CHECK(fpr2.ok);
    VKR_CHECK_EQ(fpr2.linear_tiling_features, vkrpc::kFormatFeatureSampledImage);
    VKR_CHECK_EQ(fpr2.optimal_tiling_features, 0xFFFFFFFFull);

    // Create-image request + response (response carries memreqs + LINEAR subresource layout).
    vkrpc::CreateImageRequest im;
    im.device = 7;
    im.image_type = vkrpc::kImageType2D;
    im.format = vkrpc::kFormatD16Unorm;
    im.width = 256;
    im.height = 128;
    im.depth = 1;
    im.mip_levels = 1;
    im.array_layers = 1;
    im.samples = 1;
    im.tiling = vkrpc::kImageTilingOptimal;
    im.usage = vkrpc::kImageUsageDepthStencilAttachment;
    im.sharing_mode = 0;
    im.initial_layout = 0;
    const auto im2 = vkrpc::CreateImageRequest::from_body(im.to_body());
    VKR_CHECK_EQ(im2.format, vkrpc::kFormatD16Unorm);
    VKR_CHECK_EQ(im2.width, 256);
    VKR_CHECK_EQ(im2.usage, vkrpc::kImageUsageDepthStencilAttachment);
    vkrpc::CreateImageResponse imr;
    imr.ok = true;
    imr.image = 0x9;
    imr.mem_size = 0x100000000ull; // a >32-bit size must survive (decimal-string u64)
    imr.mem_alignment = 256;
    imr.mem_type_bits = 0x1;
    imr.has_subresource_layout = true;
    imr.sr_row_pitch = 1024;
    const auto imr2 = vkrpc::CreateImageResponse::from_body(imr.to_body());
    VKR_CHECK(imr2.ok && imr2.has_subresource_layout);
    VKR_CHECK_EQ(imr2.mem_size, 0x100000000ull);
    VKR_CHECK_EQ(imr2.sr_row_pitch, static_cast<std::uint64_t>(1024));

    // Bind-image-memory request.
    vkrpc::BindImageMemoryRequest bim;
    bim.image = 0x9;
    bim.memory = 0xA;
    bim.memory_offset = 0x80000000ull;
    const auto bim2 = vkrpc::BindImageMemoryRequest::from_body(bim.to_body());
    VKR_CHECK_EQ(bim2.image, static_cast<std::uint64_t>(0x9));
    VKR_CHECK_EQ(bim2.memory_offset, 0x80000000ull);

    // Render pass with a depth attachment.
    vkrpc::CreateRenderPassRequest rp;
    rp.device = 7;
    vkrpc::AttachmentDesc col;
    col.format = kFmtSwap;
    col.samples = 1;
    col.load_op = 1;
    col.store_op = 0;
    col.stencil_load_op = 2;
    col.stencil_store_op = 1;
    col.initial_layout = 0;
    col.final_layout = 1000001002;
    vkrpc::AttachmentDesc dep;
    dep.format = vkrpc::kFormatD16Unorm;
    dep.samples = 1;
    dep.load_op = 1;
    dep.store_op = 1; // DONT_CARE
    dep.stencil_load_op = 2;
    dep.stencil_store_op = 1;
    dep.initial_layout = 0;
    dep.final_layout = vkrpc::kImageLayoutDepthStencilAttachmentOptimal;
    rp.attachments = {col, dep};
    rp.color_attachment = 0;
    rp.color_layout = 2;
    rp.depth_attachment = 1;
    rp.depth_layout = vkrpc::kImageLayoutDepthStencilAttachmentOptimal;
    const auto rp2 = vkrpc::CreateRenderPassRequest::from_body(rp.to_body());
    VKR_CHECK_EQ(static_cast<int>(rp2.attachments.size()), 2);
    VKR_CHECK_EQ(rp2.depth_attachment, 1);
    VKR_CHECK_EQ(rp2.depth_layout, vkrpc::kImageLayoutDepthStencilAttachmentOptimal);
    VKR_CHECK_EQ(rp2.attachments[1].format, vkrpc::kFormatD16Unorm);

    // A color-only render pass must round-trip depth_attachment as -1 (no depth).
    vkrpc::CreateRenderPassRequest rp_color;
    rp_color.device = 7;
    rp_color.attachments = {col};
    rp_color.color_attachment = 0;
    rp_color.color_layout = 2;
    const auto rp_color2 = vkrpc::CreateRenderPassRequest::from_body(rp_color.to_body());
    VKR_CHECK_EQ(rp_color2.depth_attachment, -1);
    // MRT: a scalar-only (legacy / old-ICD) request round-trips an EMPTY color_refs vector -- the
    // worker's single-ref fallback path keys off exactly that.
    VKR_CHECK(rp_color2.color_refs.empty());

    // MRT: the full ordered ref vector round-trips, UNUSED holes (-1 wide) included.
    vkrpc::CreateRenderPassRequest rp_mrt;
    rp_mrt.device = 7;
    rp_mrt.attachments = {col, col, col};
    vkrpc::ColorRefDesc r0;
    r0.attachment = 0;
    r0.layout = 2;
    vkrpc::ColorRefDesc r1;
    r1.attachment = vkrpc::kColorRefUnused; // a gapped glDrawBuffers hole
    vkrpc::ColorRefDesc r2;
    r2.attachment = 2;
    r2.layout = 2;
    rp_mrt.color_refs = {r0, r1, r2};
    rp_mrt.color_attachment = 0;
    rp_mrt.color_layout = 2;
    const auto rp_mrt2 = vkrpc::CreateRenderPassRequest::from_body(rp_mrt.to_body());
    VKR_CHECK_EQ(static_cast<int>(rp_mrt2.color_refs.size()), 3);
    VKR_CHECK_EQ(static_cast<long long>(rp_mrt2.color_refs[0].attachment), 0ll);
    VKR_CHECK(!vkrpc::color_ref_used(rp_mrt2.color_refs[1])); // the hole survives the wire
    VKR_CHECK_EQ(static_cast<long long>(rp_mrt2.color_refs[2].attachment), 2ll);
    VKR_CHECK_EQ(rp_mrt2.color_refs[2].layout, 2);

    // Framebuffer carries a depth view handle.
    vkrpc::CreateFramebufferRequest fb;
    fb.device = 7;
    fb.render_pass = 0x20;
    fb.image_view = 0x21;
    fb.depth_image_view = 0x22;
    fb.width = 256;
    fb.height = 256;
    fb.layers = 1;
    const auto fb2 = vkrpc::CreateFramebufferRequest::from_body(fb.to_body());
    VKR_CHECK_EQ(fb2.depth_image_view, static_cast<std::uint64_t>(0x22));
    // MRT: scalar-only round-trips an empty view vector (the worker's legacy path keys off it);
    // a populated positional vector survives verbatim.
    VKR_CHECK(fb2.attachment_views.empty());
    fb.attachment_views = {0x21, 0x23, 0x24};
    const auto fb3 = vkrpc::CreateFramebufferRequest::from_body(fb.to_body());
    VKR_CHECK_EQ(static_cast<int>(fb3.attachment_views.size()), 3);
    VKR_CHECK_EQ(fb3.attachment_views[1], static_cast<std::uint64_t>(0x23));
    // Native imageless metadata survives structurally, including wide flags/usage and formats.
    fb.imageless = true;
    fb.attachment_count = 1;
    vkrpc::FramebufferAttachmentInfoDesc info;
    info.flags = 0x100000001ull;
    info.usage = 0x200000011ull;
    info.width = 1920;
    info.height = 1080;
    info.layer_count = 2;
    info.view_formats = {44, 50};
    fb.attachment_infos = {info};
    const auto fb4 = vkrpc::CreateFramebufferRequest::from_body(fb.to_body());
    VKR_CHECK(fb4.imageless);
    VKR_CHECK_EQ(static_cast<int>(fb4.attachment_infos.size()), 1);
    VKR_CHECK_EQ(fb4.attachment_infos[0].flags, 0x100000001ull);
    VKR_CHECK_EQ(fb4.attachment_infos[0].usage, 0x200000011ull);
    VKR_CHECK_EQ(fb4.attachment_infos[0].width, 1920);
    VKR_CHECK_EQ(fb4.attachment_infos[0].height, 1080);
    VKR_CHECK_EQ(fb4.attachment_infos[0].layer_count, 2);
    VKR_CHECK_EQ(static_cast<int>(fb4.attachment_infos[0].view_formats.size()), 2);
    VKR_CHECK_EQ(fb4.attachment_infos[0].view_formats[1], 50);
    // Valid imageless metadata is unchanged. A transient Mesa-style attachment envelope smaller
    // than the framebuffer bounds both the host framebuffer and its render area.
    const auto faithful_extent = vkrpc::host_safe_framebuffer_extent(1280, 720, {info});
    VKR_CHECK_EQ(faithful_extent.width, 1280);
    VKR_CHECK_EQ(faithful_extent.height, 720);
    info.width = 1276;
    info.height = 717;
    const auto bounded_extent = vkrpc::host_safe_framebuffer_extent(1276, 1481, {info});
    VKR_CHECK_EQ(bounded_extent.width, 1276);
    VKR_CHECK_EQ(bounded_extent.height, 717);
    VKR_CHECK_EQ(vkrpc::host_safe_render_extent(0, 1481, bounded_extent.height), 717);
    VKR_CHECK_EQ(vkrpc::host_safe_render_extent(10, 700, bounded_extent.height), 700);
    VKR_CHECK_EQ(vkrpc::host_safe_render_extent(700, 100, bounded_extent.height), 17);
    VKR_CHECK_EQ(vkrpc::host_safe_render_extent(717, 1, bounded_extent.height), 0);

    // MRT: DeviceCaps.max_color_attachments is additive -- absent (an old worker's body) decodes
    // as 0 = unknown, which is the ICD's keep-the-old-gate signal.
    vkrpc::DeviceCaps caps;
    caps.device_name = "x";
    caps.max_color_attachments = 8;
    const auto caps2 = vkrpc::DeviceCaps::from_body(caps.to_body());
    VKR_CHECK_EQ(static_cast<int>(caps2.max_color_attachments), 8);
    vkrpc::DeviceCaps caps_old; // an old worker never sets it
    caps_old.device_name = "x";
    json::Value old_body = caps_old.to_body();
    // (to_body writes the field; simulate an OLD worker by decoding a body WITHOUT it)
    json::Value stripped = json::Value::make_object();
    stripped.set("device_name", json::Value(std::string("x")));
    VKR_CHECK_EQ(static_cast<int>(vkrpc::DeviceCaps::from_body(stripped).max_color_attachments), 0);
    // DeviceCaps.raw_readback is additive the same way -- absent (old worker)
    // decodes false, so the client keeps requesting the JSON+hex readback response.
    caps.raw_readback = true;
    VKR_CHECK(vkrpc::DeviceCaps::from_body(caps.to_body()).raw_readback);
    VKR_CHECK(!vkrpc::DeviceCaps::from_body(stripped).raw_readback);
    // geometry-stream: DeviceCaps.rasterization_stream_state is additive the
    // same way -- absent (an old worker's body) decodes 0, the ICD's keep-the-legacy-stream-
    // rejection signal (graphics_pipeline_ok's allow flag stays off toward that worker).
    caps.rasterization_stream_state = 1;
    VKR_CHECK_EQ(
        static_cast<int>(vkrpc::DeviceCaps::from_body(caps.to_body()).rasterization_stream_state),
        1);
    VKR_CHECK_EQ(
        static_cast<int>(vkrpc::DeviceCaps::from_body(stripped).rasterization_stream_state), 0);
    // Core indirect-draw vocabulary follows the same additive negotiation rule. A new ICD must
    // fail the void command locally when paired with an old worker, never send an unknown kind.
    caps.core_indirect_draw = 1;
    VKR_CHECK_EQ(static_cast<int>(vkrpc::DeviceCaps::from_body(caps.to_body()).core_indirect_draw),
                 1);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::DeviceCaps::from_body(stripped).core_indirect_draw), 0);
    // The scalar payload spelling is negotiated separately: a milestone-A worker advertises the
    // command vocabulary above but lacks this key, so a new ICD must use the positional spelling.
    caps.core_indirect_draw_scalar_payload = 1;
    VKR_CHECK_EQ(
        static_cast<int>(
            vkrpc::DeviceCaps::from_body(caps.to_body()).core_indirect_draw_scalar_payload),
        1);
    VKR_CHECK_EQ(
        static_cast<int>(vkrpc::DeviceCaps::from_body(stripped).core_indirect_draw_scalar_payload),
        0);
    caps.core_indirect_draw_count = 1;
    VKR_CHECK_EQ(
        static_cast<int>(vkrpc::DeviceCaps::from_body(caps.to_body()).core_indirect_draw_count), 1);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::DeviceCaps::from_body(stripped).core_indirect_draw_count),
                 0);

    // Pipeline carries depth-stencil state.
    vkrpc::CreateGraphicsPipelinesRequest gp;
    gp.device = 7;
    gp.has_depth_stencil = true;
    gp.depth_test_enable = 1;
    gp.depth_write_enable = 1;
    gp.depth_compare_op = vkrpc::kCompareOpLessOrEqual;
    // (GL/zink) RENDER CORRECTNESS: the static stencil op state (OpenCSG), the depth-clip
    // rasterization pNext (VK_EXT_depth_clip_enable), and the line-state pNext
    // (VK_EXT_line_rasterization) must round-trip the wire -- previously these were DROPPED, which
    // silently broke OpenSCAD's CSG (stencil) + clipped geometry away (depth-clip).
    gp.stencil_test_enable = 1;
    gp.stencil_front_fail_op = 1;       // VK_STENCIL_OP_ZERO
    gp.stencil_front_pass_op = 2;       // VK_STENCIL_OP_REPLACE
    gp.stencil_front_depth_fail_op = 3; // VK_STENCIL_OP_INCREMENT_AND_CLAMP
    gp.stencil_front_compare_op = 4;    // VK_COMPARE_OP_GREATER
    gp.stencil_front_compare_mask = 0xAB;
    gp.stencil_front_write_mask = 0xCD;
    gp.stencil_front_reference = 0x12;
    gp.stencil_back_fail_op = 5; // VK_STENCIL_OP_DECREMENT_AND_CLAMP
    gp.stencil_back_pass_op = 6; // VK_STENCIL_OP_INVERT
    gp.stencil_back_depth_fail_op = 7;
    gp.stencil_back_compare_op = 3; // VK_COMPARE_OP_LESS_OR_EQUAL
    gp.stencil_back_compare_mask = 0x34;
    gp.stencil_back_write_mask = 0x56;
    gp.stencil_back_reference = 0x78;
    gp.min_depth_bounds = 0.25;
    gp.max_depth_bounds = 0.75;
    gp.depth_clip_state_present = 1;
    gp.depth_clip_enable = 0; // GL_DEPTH_CLAMP: clipping DISABLED
    gp.line_state_present = 1;
    gp.line_rasterization_mode = 1; // VK_LINE_RASTERIZATION_MODE_RECTANGULAR
    gp.line_stipple_enable = 0;
    gp.line_stipple_factor = 3;
    gp.line_stipple_pattern = 0xBEEF;
    gp.stream_state_present = 1;
    gp.rasterization_stream = 0xFFFFFFFFll; // wide: a legal-high u32 survives the wire un-narrowed
    const auto gp2 = vkrpc::CreateGraphicsPipelinesRequest::from_body(gp.to_body());
    VKR_CHECK(gp2.has_depth_stencil);
    VKR_CHECK_EQ(gp2.depth_test_enable, 1);
    VKR_CHECK_EQ(gp2.depth_compare_op, vkrpc::kCompareOpLessOrEqual);
    VKR_CHECK_EQ(gp2.stencil_test_enable, 1);
    VKR_CHECK_EQ(gp2.stencil_front_fail_op, 1);
    VKR_CHECK_EQ(gp2.stencil_front_pass_op, 2);
    VKR_CHECK_EQ(gp2.stencil_front_depth_fail_op, 3);
    VKR_CHECK_EQ(gp2.stencil_front_compare_op, 4);
    VKR_CHECK_EQ(gp2.stencil_front_compare_mask, 0xAB);
    VKR_CHECK_EQ(gp2.stencil_front_write_mask, 0xCD);
    VKR_CHECK_EQ(gp2.stencil_front_reference, 0x12);
    VKR_CHECK_EQ(gp2.stencil_back_fail_op, 5);
    VKR_CHECK_EQ(gp2.stencil_back_pass_op, 6);
    VKR_CHECK_EQ(gp2.stencil_back_depth_fail_op, 7);
    VKR_CHECK_EQ(gp2.stencil_back_compare_op, 3);
    VKR_CHECK_EQ(gp2.stencil_back_compare_mask, 0x34);
    VKR_CHECK_EQ(gp2.stencil_back_write_mask, 0x56);
    VKR_CHECK_EQ(gp2.stencil_back_reference, 0x78);
    VKR_CHECK(gp2.min_depth_bounds > 0.24 && gp2.min_depth_bounds < 0.26);
    VKR_CHECK(gp2.max_depth_bounds > 0.74 && gp2.max_depth_bounds < 0.76);
    VKR_CHECK_EQ(gp2.depth_clip_state_present, 1);
    VKR_CHECK_EQ(gp2.depth_clip_enable, 0);
    VKR_CHECK_EQ(gp2.line_state_present, 1);
    VKR_CHECK_EQ(gp2.line_rasterization_mode, 1);
    VKR_CHECK_EQ(gp2.line_stipple_factor, 3);
    VKR_CHECK_EQ(gp2.line_stipple_pattern, 0xBEEF);
    VKR_CHECK_EQ(gp2.stream_state_present, 1);
    VKR_CHECK_EQ(gp2.rasterization_stream, 0xFFFFFFFFll);
    // Old-peer decode (a TRUE old payload, not a default-value round-trip --
    // to_body() would WRITE the new keys, so build bodies that genuinely OMIT them).
    {
        // A legacy pipeline body: no stream keys at all -> absent (present 0, stream 0), which
        // the shared validator accepts as "no stream pNext".
        json::Value old_gp = json::Value::make_object();
        old_gp.set("device", json::Value(std::string("1")));
        const auto old_gp2 = vkrpc::CreateGraphicsPipelinesRequest::from_body(old_gp);
        VKR_CHECK_EQ(old_gp2.stream_state_present, 0);
        VKR_CHECK_EQ(old_gp2.rasterization_stream, 0ll);
        // A legacy device body: no geometry_streams_feature_enabled key -> the -1 OMITTED
        // sentinel (NOT explicit false) -- the real worker derives the enabled state from the
        // forwarded chain instead of mismatch-rejecting the old payload (integration_real_draw
        // proves the derive path on the real worker; the mock below is a scalar-only oracle).
        json::Value old_dev = json::Value::make_object();
        old_dev.set("instance", json::Value(std::string("1")));
        old_dev.set("physical_device", json::Value(std::string("2")));
        const auto old_dev2 = vkrpc::CreateDeviceRequest::from_body(old_dev);
        VKR_CHECK_EQ(old_dev2.geometry_streams_feature_enabled,
                     vkrpc::kGeometryStreamsScalarOmitted);
        // An EXPLICIT 0/1 on the wire survives as itself (never collapses into the sentinel).
        vkrpc::CreateDeviceRequest explicit_off;
        explicit_off.geometry_streams_feature_enabled = 0;
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(explicit_off.to_body())
                         .geometry_streams_feature_enabled,
                     0);
        // Omission is a PRESENCE fact -- it cannot be TRANSMITTED. A forged -1
        // (claiming legacy status to dodge the scalar/chain agreement check), a 2, another
        // negative, and a wrong-typed value all decode INVALID, never omitted.
        for (const long long forged : {-1ll, 2ll, -5ll}) {
            json::Value forged_body = json::Value::make_object();
            forged_body.set("geometry_streams_feature_enabled", json::Value(forged));
            VKR_CHECK_EQ(
                vkrpc::CreateDeviceRequest::from_body(forged_body).geometry_streams_feature_enabled,
                vkrpc::kGeometryStreamsScalarInvalid);
        }
        json::Value forged_str = json::Value::make_object();
        forged_str.set("geometry_streams_feature_enabled", json::Value(std::string("1")));
        VKR_CHECK_EQ(
            vkrpc::CreateDeviceRequest::from_body(forged_str).geometry_streams_feature_enabled,
            vkrpc::kGeometryStreamsScalarInvalid);
    }
    // geometry-stream device scalar round-trips.
    {
        vkrpc::CreateDeviceRequest cdr;
        cdr.geometry_streams_feature_enabled = 1;
        VKR_CHECK_EQ(
            vkrpc::CreateDeviceRequest::from_body(cdr.to_body()).geometry_streams_feature_enabled,
            1);
    }

    // begin_render_pass depth clear: presence flag + value round-trip; a command WITHOUT a depth
    // clear must decode has_depth_clear == false (not a silent 0.0).
    vkrpc::RecordCommandBufferRequest rc;
    rc.command_buffer = 3;
    vkrpc::RecordedCommand withd;
    withd.kind = "begin_render_pass";
    withd.has_depth_clear = true;
    withd.depth_clear = 1.0;
    vkrpc::RecordedCommand without;
    without.kind = "begin_render_pass"; // has_depth_clear defaults false
    rc.commands = {withd, without};
    const auto rc2 = vkrpc::RecordCommandBufferRequest::from_body(rc.to_body());
    VKR_CHECK_EQ(static_cast<int>(rc2.commands.size()), 2);
    VKR_CHECK(rc2.commands[0].has_depth_clear);
    VKR_CHECK_EQ(rc2.commands[0].depth_clear, 1.0);
    VKR_CHECK(!rc2.commands[1].has_depth_clear);
}

// Image + depth mock state machine (the dual-platform oracle): format props, image create/bind with
// the device-local memory class, depth views, a depth render pass / framebuffer / pipeline, and a
// recorded bufferless triangle with the depth clear -- plus the fail-closed edges.
void test_image_depth_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const vkrpc::EnumeratePhysicalDevicesResponse en = backend.enumerate_physical_devices(er);
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    const std::uint64_t dev = cd.device;

    // Format properties: honest per-format table; an unknown physical device is rejected.
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpq;
    fpq.physical_device = phys;
    fpq.format = vkrpc::kFormatD16Unorm;
    const auto fpd = backend.get_physical_device_format_properties(fpq);
    VKR_CHECK(fpd.ok);
    VKR_CHECK((fpd.optimal_tiling_features & vkrpc::kFormatFeatureDepthStencilAttachment) != 0);
    fpq.format = kFmtRgba8;
    const auto fpc = backend.get_physical_device_format_properties(fpq);
    VKR_CHECK((fpc.linear_tiling_features & vkrpc::kFormatFeatureSampledImage) != 0);
    {
        auto bad = fpq;
        bad.physical_device = 0xDEAD;
        VKR_CHECK(!backend.get_physical_device_format_properties(bad).ok);
    }

    // Create a depth image (D16, OPTIMAL, DEPTH_STENCIL_ATTACHMENT, UNDEFINED) -> device-local
    // memreqs.
    auto make_depth = [&]() {
        vkrpc::CreateImageRequest r;
        r.device = dev;
        r.image_type = vkrpc::kImageType2D;
        r.format = vkrpc::kFormatD16Unorm;
        r.width = 256;
        r.height = 256;
        r.depth = 1;
        r.mip_levels = 1;
        r.array_layers = 1;
        r.samples = 1;
        r.tiling = vkrpc::kImageTilingOptimal;
        r.usage = vkrpc::kImageUsageDepthStencilAttachment;
        r.sharing_mode = 0;
        r.initial_layout = 0;
        return r;
    };
    const vkrpc::CreateImageResponse depth = backend.create_image(make_depth());
    VKR_CHECK(depth.ok && depth.image != 0);
    VKR_CHECK(!depth.has_subresource_layout); // OPTIMAL: no subresource layout
    VKR_CHECK(depth.mem_type_bits != 0);

    // create_image rejections (the bounded subset).
    {
        auto r = make_depth();
        r.image_type = 0;
        VKR_CHECK(!backend.create_image(r).ok); // not 2D
    }
    {
        // Multi-mip / multi-layer images are ADMITTED now (tracked faithfully; the widened
        // copy_buffer_to_image validates per-region bounds against them) -- only the structural
        // ceilings stay fail-closed.
        auto r = make_depth();
        r.mip_levels = 2;
        VKR_CHECK(backend.create_image(r).ok);
        r = make_depth();
        r.mip_levels = 17; // above the sane ceiling
        VKR_CHECK(!backend.create_image(r).ok);
        r = make_depth();
        r.mip_levels = 0;
        VKR_CHECK(!backend.create_image(r).ok);
    }
    {
        auto r = make_depth();
        r.tiling = vkrpc::kImageTilingLinear;
        VKR_CHECK(!backend.create_image(r).ok); // depth must be OPTIMAL
    }
    {
        // A depth/stencil image's usage may now COMBINE DEPTH_STENCIL_ATTACHMENT with TRANSFER_DST
        // (the copy-target path enabled here) and SAMPLED -- a pure transfer target or a
        // sampled depth texture is legal. Only usage OUTSIDE that set (COLOR_ATTACHMENT) fails.
        auto r = make_depth();
        r.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst);
        VKR_CHECK(backend.create_image(r).ok);
        r = make_depth();
        r.usage = vkrpc::kImageUsageDepthStencilAttachment |
                  static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst);
        VKR_CHECK(backend.create_image(r).ok);
        r = make_depth();
        r.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageColorAttachment);
        VKR_CHECK(!backend.create_image(r).ok); // a depth image cannot be a COLOR_ATTACHMENT
    }
    {
        auto r = make_depth();
        r.format = kFmtRgba8;
        r.usage = vkrpc::kImageUsageDepthStencilAttachment;
        VKR_CHECK(!backend.create_image(r).ok); // color image with depth usage
    }
    {
        auto r = make_depth();
        r.format = kFmtRgba8;
        r.tiling = vkrpc::kImageTilingOptimal;
        r.usage = vkrpc::kImageUsageSampled;
        r.initial_layout = vkrpc::kImageLayoutPreinitialized;
        VKR_CHECK(!backend.create_image(r).ok); // PREINITIALIZED requires LINEAR
    }

    // A LINEAR color texture carries a subresource layout (rowPitch = width * 4).
    {
        vkrpc::CreateImageRequest r;
        r.device = dev;
        r.image_type = vkrpc::kImageType2D;
        r.format = kFmtRgba8;
        r.width = 64;
        r.height = 64;
        r.depth = 1;
        r.mip_levels = 1;
        r.array_layers = 1;
        r.samples = 1;
        r.tiling = vkrpc::kImageTilingLinear;
        r.usage = vkrpc::kImageUsageSampled;
        r.sharing_mode = 0;
        r.initial_layout = vkrpc::kImageLayoutPreinitialized;
        const vkrpc::CreateImageResponse tex = backend.create_image(r);
        VKR_CHECK(tex.ok && tex.has_subresource_layout);
        VKR_CHECK_EQ(tex.sr_row_pitch, static_cast<std::uint64_t>(64 * 4));
        backend.destroy_image({tex.image});
    }

    // Allocate device-local memory (type 0) + bind to the depth image. Host-visible memory (type 1)
    // is NOT in the OPTIMAL image's memoryTypeBits, so binding it is rejected.
    auto alloc = [&](long long type_index) {
        vkrpc::AllocateMemoryRequest r;
        r.device = dev;
        r.allocation_size = 4ull * 1024 * 1024;
        r.memory_type_index = type_index;
        return backend.allocate_memory(r);
    };
    const vkrpc::AllocateMemoryResponse dl = alloc(0); // DEVICE_LOCAL
    const vkrpc::AllocateMemoryResponse hv = alloc(1); // HOST_VISIBLE|HOST_COHERENT
    VKR_CHECK(dl.ok && hv.ok);
    VKR_CHECK(!backend.bind_image_memory({depth.image, hv.memory, 0}).ok); // wrong memory class
    VKR_CHECK(backend.bind_image_memory({depth.image, dl.memory, 0}).ok);
    VKR_CHECK(!backend.bind_image_memory({depth.image, dl.memory, 0}).ok); // already bound

    // Depth image view (DEPTH aspect). A COLOR-aspect view over a depth image is rejected.
    auto make_dv = [&](int aspect) {
        vkrpc::CreateImageViewRequest r;
        r.image = depth.image;
        r.view_type = 1;
        r.format = vkrpc::kFormatD16Unorm;
        r.swizzle_r = r.swizzle_g = r.swizzle_b = r.swizzle_a = 0;
        r.aspect = aspect;
        r.base_mip_level = 0;
        r.level_count = 1;
        r.base_array_layer = 0;
        r.layer_count = 1;
        return r;
    };
    const vkrpc::CreateImageViewResponse dv =
        backend.create_image_view(make_dv(vkrpc::kImageAspectDepth));
    VKR_CHECK(dv.ok && dv.image_view != 0);
    // (GL/zink): a mismatched aspect is now ACCEPTED (faithfully forwarded; the host
    // gates). A view over an app image not yet bound to memory is still rejected.
    {
        const vkrpc::CreateImageResponse unbound = backend.create_image(make_depth());
        VKR_CHECK(unbound.ok);
        auto r = make_dv(vkrpc::kImageAspectDepth);
        r.image = unbound.image;
        VKR_CHECK(!backend.create_image_view(r).ok);
        backend.destroy_image({unbound.image});
    }

    // Swapchain + color view for the framebuffer's color attachment.
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const std::uint64_t surface = backend.create_surface(sr).surface;
    vkrpc::CreateSwapchainRequest scr;
    scr.device = dev;
    scr.surface = surface;
    scr.image_format = kFmtSwap;
    scr.color_space = 0;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const std::uint64_t sc = backend.create_swapchain(scr).swapchain;
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc;
    const std::uint64_t img0 = backend.get_swapchain_images(gir).images.front();
    vkrpc::CreateImageViewRequest cvr;
    cvr.image = img0;
    cvr.view_type = 1;
    cvr.format = kFmtSwap;
    cvr.swizzle_r = cvr.swizzle_g = cvr.swizzle_b = cvr.swizzle_a = 0;
    cvr.aspect = vkrpc::kImageAspectColor;
    cvr.base_mip_level = 0;
    cvr.level_count = 1;
    cvr.base_array_layer = 0;
    cvr.layer_count = 1;
    const std::uint64_t color_view = backend.create_image_view(cvr).image_view;

    // Render pass with a depth attachment + a color-only one (for the pipeline-mismatch edge).
    auto make_rp = [&](bool with_depth) {
        vkrpc::CreateRenderPassRequest r;
        r.device = dev;
        vkrpc::AttachmentDesc a;
        a.format = kFmtSwap;
        a.samples = 1;
        a.load_op = 1;
        a.store_op = 0;
        a.stencil_load_op = 2;
        a.stencil_store_op = 1;
        a.initial_layout = 0;
        a.final_layout = 1000001002;
        r.attachments.push_back(a);
        r.color_attachment = 0;
        r.color_layout = 2;
        if (with_depth) {
            vkrpc::AttachmentDesc d;
            d.format = vkrpc::kFormatD16Unorm;
            d.samples = 1;
            d.load_op = 1;
            d.store_op = 1;
            d.stencil_load_op = 2;
            d.stencil_store_op = 1;
            d.initial_layout = 0;
            d.final_layout = vkrpc::kImageLayoutDepthStencilAttachmentOptimal;
            r.attachments.push_back(d);
            r.depth_attachment = 1;
            r.depth_layout = vkrpc::kImageLayoutDepthStencilAttachmentOptimal;
        }
        return r;
    };
    const std::uint64_t rp_depth = backend.create_render_pass(make_rp(true)).render_pass;
    const std::uint64_t rp_color = backend.create_render_pass(make_rp(false)).render_pass;
    VKR_CHECK(rp_depth != 0 && rp_color != 0);
    {
        auto r = make_rp(true);
        r.depth_attachment = 0; // must be 1
        VKR_CHECK(!backend.create_render_pass(r).ok);
    }
    {
        auto r = make_rp(true);
        r.attachments[1].format = kFmtRgba8; // not a depth format
        VKR_CHECK(!backend.create_render_pass(r).ok);
    }

    // Framebuffer: a depth render pass needs a depth view; a color-only pass must NOT carry one.
    auto make_fb = [&](std::uint64_t rpass, std::uint64_t cview, std::uint64_t dview) {
        vkrpc::CreateFramebufferRequest r;
        r.device = dev;
        r.render_pass = rpass;
        r.image_view = cview;
        r.depth_image_view = dview;
        r.width = 256;
        r.height = 256;
        r.layers = 1;
        return r;
    };
    const std::uint64_t fb =
        backend.create_framebuffer(make_fb(rp_depth, color_view, dv.image_view)).framebuffer;
    VKR_CHECK(fb != 0);
    VKR_CHECK(
        !backend.create_framebuffer(make_fb(rp_depth, color_view, 0)).ok); // missing depth view
    VKR_CHECK(!backend.create_framebuffer(make_fb(rp_color, color_view, dv.image_view))
                   .ok); // extra depth

    // MRT (mock == real): the widened vector path. Two app COLOR_ATTACHMENT images + views back a
    // 2-color render pass (full ref vector incl. an UNUSED-hole variant), a positional
    // framebuffer, and the reject matrix. The blend-count hardening rides the pipeline section
    // below (make_pipe is defined there).
    std::uint64_t rp_mrt = 0;
    {
        std::uint64_t mrt_views[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            vkrpc::CreateImageRequest cir;
            cir.device = dev;
            cir.image_type = vkrpc::kImageType2D;
            cir.format = kFmtRgba8;
            cir.width = 256;
            cir.height = 256;
            cir.depth = 1;
            cir.mip_levels = 1;
            cir.array_layers = 1;
            cir.samples = 1;
            cir.tiling = vkrpc::kImageTilingOptimal;
            cir.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageColorAttachment);
            cir.sharing_mode = 0;
            cir.initial_layout = 0;
            const vkrpc::CreateImageResponse im = backend.create_image(cir);
            VKR_CHECK(im.ok); // COLOR_ATTACHMENT usage is admitted now (the minimal MRT widening)
            const vkrpc::AllocateMemoryResponse m = alloc(0);
            VKR_CHECK(m.ok);
            VKR_CHECK(backend.bind_image_memory({im.image, m.memory, 0}).ok);
            auto vr = cvr; // the color-view shape, over the app image
            vr.image = im.image;
            vr.format = kFmtRgba8;
            const vkrpc::CreateImageViewResponse v = backend.create_image_view(vr);
            VKR_CHECK(v.ok);
            mrt_views[i] = v.image_view;
        }
        auto make_mrt_rp = [&](std::size_t ref_count) {
            vkrpc::CreateRenderPassRequest r;
            r.device = dev;
            for (int i = 0; i < 2; ++i) {
                vkrpc::AttachmentDesc a;
                a.format = kFmtRgba8;
                a.samples = 1;
                a.load_op = 1;
                a.store_op = 0;
                a.stencil_load_op = 2;
                a.stencil_store_op = 1;
                a.initial_layout = 0;
                a.final_layout = 2;
                r.attachments.push_back(a);
            }
            for (std::size_t i = 0; i < ref_count; ++i) {
                vkrpc::ColorRefDesc cr;
                cr.attachment = static_cast<long long>(i % 2);
                cr.layout = 2;
                r.color_refs.push_back(cr);
            }
            r.color_attachment = 0; // the scalar compat fields ride along (old-worker shape)
            r.color_layout = 2;
            return r;
        };
        rp_mrt = backend.create_render_pass(make_mrt_rp(2)).render_pass;
        VKR_CHECK(rp_mrt != 0);
        { // an UNUSED hole (gapped glDrawBuffers) is legal
            auto r = make_mrt_rp(2);
            r.color_refs[1].attachment = vkrpc::kColorRefUnused;
            VKR_CHECK(backend.create_render_pass(r).ok);
        }
        { // out-of-range ref
            auto r = make_mrt_rp(2);
            r.color_refs[1].attachment = 5;
            VKR_CHECK(!backend.create_render_pass(r).ok);
        }
        { // invalid layout on a used ref
            auto r = make_mrt_rp(2);
            r.color_refs[1].layout = -1;
            VKR_CHECK(!backend.create_render_pass(r).ok);
        }
        { // beyond the advertised limit (mock: 8)
            auto r = make_mrt_rp(9);
            VKR_CHECK(!backend.create_render_pass(r).ok);
        }
        // Positional framebuffer: exact count + per-position format; mismatches are OUR named
        // rejections (the old {1,2} count gate could not tell color+color from color+depth).
        auto make_mrt_fb = [&](std::vector<std::uint64_t> views) {
            vkrpc::CreateFramebufferRequest r;
            r.device = dev;
            r.render_pass = rp_mrt;
            r.image_view = views.empty() ? 0 : views[0];
            r.width = 256;
            r.height = 256;
            r.layers = 1;
            r.attachment_views = std::move(views);
            return r;
        };
        VKR_CHECK(backend.create_framebuffer(make_mrt_fb({mrt_views[0], mrt_views[1]})).ok);
        VKR_CHECK(!backend.create_framebuffer(make_mrt_fb({mrt_views[0]})).ok); // count mismatch
        VKR_CHECK(!backend.create_framebuffer(make_mrt_fb({mrt_views[0], dv.image_view}))
                       .ok); // a depth view at a color position (format mismatch)
    }

    // Shaders + empty pipeline layout + a depth-stencil pipeline (and the mismatch edges).
    auto make_sm = [&]() {
        vkrpc::CreateShaderModuleRequest r;
        r.device = dev;
        r.code = std::string(8, '\0');
        r.code_size = 8;
        return r;
    };
    const std::uint64_t vs = backend.create_shader_module(make_sm()).shader_module;
    const std::uint64_t fs = backend.create_shader_module(make_sm()).shader_module;
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = dev;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 0;
    const std::uint64_t layout = backend.create_pipeline_layout(plr).pipeline_layout;
    auto make_pipe = [&](std::uint64_t rpass, bool depth_state) {
        vkrpc::CreateGraphicsPipelinesRequest r;
        r.device = dev;
        vkrpc::ShaderStageDesc vertex;
        vertex.stage = 1;
        vertex.module = vs;
        vertex.entry = "main";
        vkrpc::ShaderStageDesc fragment;
        fragment.stage = 16;
        fragment.module = fs;
        fragment.entry = "main";
        r.stages = {vertex, fragment};
        r.topology = 3;
        r.vertex_binding_count = 0;
        r.vertex_attribute_count = 0;
        r.cull_mode = 0;
        r.front_face = 1;
        r.dynamic_states = {0, 1};
        r.layout = layout;
        r.render_pass = rpass;
        r.subpass = 0;
        if (depth_state) {
            r.has_depth_stencil = true;
            r.depth_test_enable = 1;
            r.depth_write_enable = 1;
            r.depth_compare_op = vkrpc::kCompareOpLessOrEqual;
        }
        return r;
    };
    const std::uint64_t pipe =
        backend.create_graphics_pipelines(make_pipe(rp_depth, true)).pipeline;
    VKR_CHECK(pipe != 0);
    VKR_CHECK(!backend.create_graphics_pipelines(make_pipe(rp_depth, false))
                   .ok); // depth pass needs state
    VKR_CHECK(
        !backend.create_graphics_pipelines(make_pipe(rp_color, true)).ok); // color pass forbids it
    {
        auto r = make_pipe(rp_depth, true);
        r.depth_compare_op = 0; // not LESS_OR_EQUAL
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    // MRT blend-count hardening (mock == real): an explicit blend array must equal
    // the subpass's color-ref count (incl. UNUSED holes) -- the driver-tolerated VUID violation
    // that silently half-rendered the 2-color probe; an empty array takes the N-default fallback.
    {
        auto r = make_pipe(rp_mrt, false);
        vkrpc::ColorBlendAttachmentDesc b;
        b.color_write_mask = 0xF;
        r.color_blend_attachments = {b}; // 1 != 2
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        r.color_blend_attachments = {b, b}; // 2 == 2
        VKR_CHECK(backend.create_graphics_pipelines(r).ok);
        r.color_blend_attachments.clear(); // empty -> the N-default fallback
        VKR_CHECK(backend.create_graphics_pipelines(r).ok);
    }

    // Record a bufferless triangle with a depth clear; the begin-depth-clear presence rules hold.
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = dev;
    cpr.queue_family_index = cd.queue_family_index;
    const std::uint64_t pool = backend.create_command_pool(cpr).command_pool;
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = pool;
    acb.count = 1;
    const std::uint64_t cmd = backend.allocate_command_buffers(acb).command_buffers.front();
    auto begin = [&](std::uint64_t rpass, std::uint64_t framebuffer, bool has_depth_clear) {
        vkrpc::RecordedCommand c;
        c.kind = "begin_render_pass";
        c.render_pass = rpass;
        c.framebuffer = framebuffer;
        c.render_area_w = 256;
        c.render_area_h = 256;
        c.has_depth_clear = has_depth_clear;
        c.depth_clear = 1.0;
        return c;
    };
    auto record = [&](const std::vector<vkrpc::RecordedCommand>& cmds) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        r.commands = cmds;
        return backend.record_command_buffer(r);
    };
    vkrpc::RecordedCommand vp;
    vp.kind = "set_viewport";
    vp.vp_w = 256;
    vp.vp_h = 256;
    vp.vp_max_depth = 1.0;
    vkrpc::RecordedCommand sci;
    sci.kind = "set_scissor";
    sci.sc_w = 256;
    sci.sc_h = 256;
    vkrpc::RecordedCommand bp;
    bp.kind = "bind_pipeline";
    bp.pipeline = pipe;
    vkrpc::RecordedCommand draw;
    draw.kind = "draw";
    draw.vertex_count = 3;
    draw.instance_count = 1;
    draw.first_vertex = 0;
    draw.first_instance = 0;
    vkrpc::RecordedCommand end;
    end.kind = "end_render_pass";
    VKR_CHECK(record({begin(rp_depth, fb, true), vp, sci, bp, draw, end}).ok);
    // A depth render pass begun WITHOUT a carried depth clear is rejected.
    VKR_CHECK(!record({begin(rp_depth, fb, false), vp, sci, bp, draw, end}).ok);

    // An app image is blocked while a live view (and the device while any image)
    // references it -- the swapchain -> view parent/child rule, applied to app images.
    VKR_CHECK(!backend.destroy_device({dev}).ok);        // device has live image + children
    VKR_CHECK(!backend.destroy_image({depth.image}).ok); // the depth view references the image

    // The recorded command buffer baked the depth view as a dependency (mirroring
    // the real backend), so destroying the depth view invalidates it -- a prior-valid submit now
    // fails until re-recording.
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = dev;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(gq).queue;
    VKR_CHECK(queue != 0);
    vkrpc::QueueSubmitRequest qs;
    qs.queue = queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok); // recorded depth frame is submittable
    VKR_CHECK(backend.destroy_image_view({dv.image_view}).ok);
    VKR_CHECK(!backend.queue_submit(qs).ok); // destroying the depth view invalidated the CB

    // With the depth view gone, the depth image is destroyable.
    VKR_CHECK(backend.destroy_image({depth.image}).ok);
}

// A command recorded against an app image bakes the image handle, so destroying the
// image invalidates the command buffer (app images have surface == swapchain == 0, so the generic
// referenced-object set -- not referenced_swapchains -- is what catches this). This is the
// stale-image foundation used by the copy/barrier path.
void test_image_command_invalidation_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    cdr.enabled_extensions = {vkrpc::kSync2ExtensionName};
    cdr.synchronization2_feature_enabled = 1;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    const std::uint64_t dev = cd.device;

    // A TRANSFER_DST color app image (the texture-upload target) bound to device-local
    // memory.
    vkrpc::CreateImageRequest ir;
    ir.device = dev;
    ir.image_type = vkrpc::kImageType2D;
    ir.format = vkrpc::kFormatR8G8B8A8Unorm;
    ir.width = 32;
    ir.height = 32;
    ir.depth = 1;
    ir.mip_levels = 1;
    ir.array_layers = 1;
    ir.samples = 1;
    ir.tiling = vkrpc::kImageTilingOptimal;
    ir.usage = vkrpc::kImageUsageTransferDst;
    ir.sharing_mode = 0;
    ir.initial_layout = 0;
    const vkrpc::CreateImageResponse img = backend.create_image(ir);
    VKR_CHECK(img.ok);
    vkrpc::AllocateMemoryRequest amr;
    amr.device = dev;
    amr.allocation_size = img.mem_size;
    amr.memory_type_index = 0; // DEVICE_LOCAL (the OPTIMAL image's only admissible type)
    const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(amr);
    VKR_CHECK(mem.ok);
    VKR_CHECK(backend.bind_image_memory({img.image, mem.memory, 0}).ok);

    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = dev;
    cpr.queue_family_index = cd.queue_family_index;
    const std::uint64_t pool = backend.create_command_pool(cpr).command_pool;
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = pool;
    acb.count = 1;
    const std::uint64_t cmd = backend.allocate_command_buffers(acb).command_buffers.front();
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = dev;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(gq).queue;

    // Record a pipeline_barrier against the app image (an image-targeting command).
    vkrpc::RecordedCommand bar;
    bar.kind = "pipeline_barrier";
    bar.image = img.image;
    bar.old_layout = 0; // UNDEFINED
    bar.new_layout = 7; // TRANSFER_DST_OPTIMAL
    bar.aspect = vkrpc::kImageAspectColor;
    bar.src_stage = 0x1;    // TOP_OF_PIPE
    bar.dst_stage = 0x1000; // TRANSFER
    bar.src_access = 0;
    bar.dst_access = 0;
    bar.barrier_base_mip = 0; // app-image barriers carry the explicit single subresource
    bar.barrier_level_count = 1;
    bar.barrier_base_layer = 0;
    bar.barrier_layer_count = 1;
    vkrpc::RecordCommandBufferRequest rc;
    rc.command_buffer = cmd;
    rc.commands = {bar};
    VKR_CHECK(backend.record_command_buffer(rc).ok);

    vkrpc::ImageMemoryBarrier2 sync2_barrier;
    sync2_barrier.src_stage = 0x1;
    sync2_barrier.dst_stage = 0x1000;
    sync2_barrier.old_layout = 0;
    sync2_barrier.new_layout = 7;
    sync2_barrier.src_queue_family = vkrpc::kVkQueueFamilyIgnored;
    sync2_barrier.dst_queue_family = vkrpc::kVkQueueFamilyIgnored;
    sync2_barrier.image = img.image;
    sync2_barrier.aspect = vkrpc::kImageAspectColor;
    sync2_barrier.level_count = 1;
    sync2_barrier.layer_count = 1;
    vkrpc::DependencyInfo2 sync2_dependency;
    sync2_dependency.image.push_back(sync2_barrier);
    vkrpc::RecordedCommand sync2_command;
    sync2_command.kind = "pipeline_barrier2";
    sync2_command.deps2.push_back(sync2_dependency);
    vkrpc::RecordCommandBufferRequest stale_sync2;
    stale_sync2.command_buffer = cmd;
    stale_sync2.commands.push_back(sync2_command);

    vkrpc::QueueSubmitRequest qs;
    qs.queue = queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok); // submittable while the image lives
    VKR_CHECK(backend.destroy_image({img.image}).ok);
    VKR_CHECK(!backend.queue_submit(qs).ok); // destroying the image invalidated the recorded CB
    const vkrpc::StatusResponse stale = backend.record_command_buffer(stale_sync2);
    VKR_CHECK(!stale.ok);
    VKR_CHECK_EQ(stale.reason, "sync2 image barrier references destroyed app image " +
                                   std::to_string(img.image) + " (destroy_image)");
}

void test_recording_resource_leases_mock() {
    // Additive codecs: generation 0 / absent capability are the legacy spelling.
    vkrpc::RecordCommandBufferRequest wire_request;
    wire_request.command_buffer = 7;
    wire_request.recording_generation = 9;
    const auto json_request = vkrpc::RecordCommandBufferRequest::from_body(wire_request.to_body());
    VKR_CHECK_EQ(json_request.recording_generation, 9ull);
    std::string wire_error;
    const auto raw_request =
        vkrpc::RecordCommandBufferRequest::from_wire(wire_request.to_wire(), wire_error);
    VKR_CHECK(wire_error.empty());
    VKR_CHECK_EQ(raw_request.recording_generation, 9ull);
    vkrpc::LeasedDestroyRequest destroy_wire;
    destroy_wire.handle = 11;
    destroy_wire.leases = {{7, 9}, {8, 3}};
    const auto destroy_roundtrip = vkrpc::LeasedDestroyRequest::from_body(destroy_wire.to_body());
    VKR_CHECK_EQ(destroy_roundtrip.handle, 11ull);
    VKR_CHECK_EQ(destroy_roundtrip.leases.size(), static_cast<std::size_t>(2));
    vkrpc::RetireCommandBufferRecordingsRequest retire_wire;
    retire_wire.recordings = destroy_wire.leases;
    VKR_CHECK_EQ(vkrpc::RetireCommandBufferRecordingsRequest::from_body(retire_wire.to_body())
                     .recordings.size(),
                 static_cast<std::size_t>(2));
    vkrpc::DeviceCaps lease_caps;
    lease_caps.recording_resource_leases_v1 = 1;
    VKR_CHECK_EQ(vkrpc::DeviceCaps::from_body(lease_caps.to_body()).recording_resource_leases_v1,
                 1u);
    VKR_CHECK_EQ(
        vkrpc::DeviceCaps::from_body(json::Value::make_object()).recording_resource_leases_v1, 0u);

    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const auto instance = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest enumerate;
    enumerate.instance = instance.instance;
    const std::uint64_t physical =
        backend.enumerate_physical_devices(enumerate).devices.front().handle;
    vkrpc::CreateDeviceRequest create_device;
    create_device.instance = instance.instance;
    create_device.physical_device = physical;
    const auto device = backend.create_device(create_device);
    VKR_CHECK(device.ok);
    vkrpc::CreateCommandPoolRequest create_pool;
    create_pool.device = device.device;
    create_pool.queue_family_index = device.queue_family_index;
    const std::uint64_t pool = backend.create_command_pool(create_pool).command_pool;
    vkrpc::AllocateCommandBuffersRequest allocate_cbs;
    allocate_cbs.command_pool = pool;
    allocate_cbs.count = 4;
    const auto cbs = backend.allocate_command_buffers(allocate_cbs).command_buffers;
    vkrpc::GetDeviceQueueRequest get_queue;
    get_queue.device = device.device;
    get_queue.queue_family_index = device.queue_family_index;
    get_queue.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(get_queue).queue;

    vkrpc::CreateImageRequest create_image;
    create_image.device = device.device;
    create_image.image_type = vkrpc::kImageType2D;
    create_image.format = vkrpc::kFormatR8G8B8A8Unorm;
    create_image.width = 16;
    create_image.height = 16;
    create_image.depth = 1;
    create_image.mip_levels = 1;
    create_image.array_layers = 1;
    create_image.samples = 1;
    create_image.tiling = vkrpc::kImageTilingOptimal;
    create_image.usage = vkrpc::kImageUsageTransferDst;
    create_image.sharing_mode = 0;
    create_image.initial_layout = 0;
    const auto image = backend.create_image(create_image);
    VKR_CHECK(image.ok);
    vkrpc::AllocateMemoryRequest allocate_memory;
    allocate_memory.device = device.device;
    allocate_memory.allocation_size = image.mem_size;
    allocate_memory.memory_type_index = 0;
    const auto memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(memory.ok);
    VKR_CHECK(backend.bind_image_memory({image.image, memory.memory, 0}).ok);

    // The leased opcode preserves the ordinary destroy's parent/child authority at the original
    // wire position: a live view rejects the image destroy and no retired state is created.
    const auto guarded_image = backend.create_image(create_image);
    VKR_CHECK(guarded_image.ok);
    allocate_memory.allocation_size = guarded_image.mem_size;
    const auto guarded_memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(guarded_memory.ok);
    VKR_CHECK(backend.bind_image_memory({guarded_image.image, guarded_memory.memory, 0}).ok);
    vkrpc::CreateImageViewRequest guarded_view_request;
    guarded_view_request.image = guarded_image.image;
    guarded_view_request.view_type = 1;
    guarded_view_request.format = vkrpc::kFormatR8G8B8A8Unorm;
    guarded_view_request.swizzle_r = guarded_view_request.swizzle_g =
        guarded_view_request.swizzle_b = guarded_view_request.swizzle_a = 0;
    guarded_view_request.aspect = vkrpc::kImageAspectColor;
    guarded_view_request.base_mip_level = 0;
    guarded_view_request.level_count = 1;
    guarded_view_request.base_array_layer = 0;
    guarded_view_request.layer_count = 1;
    const auto guarded_view = backend.create_image_view(guarded_view_request);
    VKR_CHECK(guarded_view.ok);
    vkrpc::LeasedDestroyRequest guarded_destroy;
    guarded_destroy.handle = guarded_image.image;
    guarded_destroy.leases = {{cbs[0], 1}};
    const vkrpc::StatusResponse guarded_rejection = backend.destroy_image_leased(guarded_destroy);
    VKR_CHECK(!guarded_rejection.ok);
    VKR_CHECK_EQ(guarded_rejection.reason, "image has live image views; destroy them first");
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(0));
    VKR_CHECK(backend.destroy_image_view({guarded_view.image_view}).ok);
    VKR_CHECK(backend.destroy_image({guarded_image.image}).ok);
    VKR_CHECK(backend.free_memory({guarded_memory.memory}).ok);

    vkrpc::RecordedCommand barrier;
    barrier.kind = "pipeline_barrier";
    barrier.image = image.image;
    barrier.old_layout = 0;
    barrier.new_layout = 7;
    barrier.src_stage = 1;
    barrier.dst_stage = 0x1000;
    barrier.src_access = 0;
    barrier.dst_access = 0;
    barrier.aspect = vkrpc::kImageAspectColor;
    barrier.barrier_base_mip = 0;
    barrier.barrier_level_count = 1;
    barrier.barrier_base_layer = 0;
    barrier.barrier_layer_count = 1;
    vkrpc::RecordCommandBufferRequest recording;
    recording.command_buffer = cbs[0];
    recording.recording_generation = 1;
    recording.commands = {barrier};

    // The ICD announces the exact not-yet-delivered generation at the destroy's original wire
    // position. Only that key can resolve the logically retired image afterward.
    vkrpc::LeasedDestroyRequest leased_destroy;
    leased_destroy.handle = image.image;
    leased_destroy.leases = {{cbs[0], 1}};
    vkrpc::LeasedDestroyRequest zero_generation = leased_destroy;
    zero_generation.leases = {{cbs[0], 0}};
    VKR_CHECK(!backend.destroy_image_leased(zero_generation).ok);
    VKR_CHECK(backend.destroy_image_leased(leased_destroy).ok);
    VKR_CHECK(backend.record_command_buffer(recording).ok);
    vkrpc::QueueSubmitRequest submit;
    submit.queue = queue;
    submit.command_buffers = {cbs[0]};
    VKR_CHECK(backend.queue_submit(submit).ok);

    // The object is gone from the ordinary live registry at the destroy's wire position.
    vkrpc::CreateImageViewRequest view;
    view.image = image.image;
    view.view_type = 1;
    view.format = vkrpc::kFormatR8G8B8A8Unorm;
    view.swizzle_r = view.swizzle_g = view.swizzle_b = view.swizzle_a = 0;
    view.aspect = vkrpc::kImageAspectColor;
    view.base_mip_level = 0;
    view.level_count = 1;
    view.base_array_layer = 0;
    view.layer_count = 1;
    VKR_CHECK(!backend.create_image_view(view).ok);
    vkrpc::RecordCommandBufferRequest wrong_generation = recording;
    wrong_generation.command_buffer = cbs[1];
    VKR_CHECK(!backend.record_command_buffer(wrong_generation).ok);

    // Free is logically accepted but physically retained behind the image. Retirement releases
    // image then allocation; both handles are unavailable immediately and remain unavailable.
    VKR_CHECK(backend.free_memory({memory.memory}).ok);
    VKR_CHECK(!backend.bind_image_memory({image.image, memory.memory, 0}).ok);
    vkrpc::RetireCommandBufferRecordingsRequest retire;
    retire.recordings = {{cbs[0], 1}};
    VKR_CHECK(backend.retire_command_buffer_recordings(retire).ok);
    const vkrpc::StatusResponse freed_again = backend.free_memory({memory.memory});
    VKR_CHECK(!freed_again.ok);
    VKR_CHECK_EQ(freed_again.reason, "unknown handle for this object type");
    VKR_CHECK(!backend.record_command_buffer(recording).ok);
    recording.recording_generation = 2;
    VKR_CHECK(!backend.record_command_buffer(recording).ok);

    // Buffer twin: a generation recorded while live remains submittable after leased destroy.
    vkrpc::CreateBufferRequest create_buffer;
    create_buffer.device = device.device;
    create_buffer.size = 64;
    create_buffer.usage = vkrpc::kBufferUsageVertexBuffer;
    create_buffer.sharing_mode = 0;
    const auto buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(buffer.ok);
    allocate_memory.allocation_size = buffer.mem_size;
    allocate_memory.memory_type_index = 1;
    const auto buffer_memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(buffer_memory.ok);
    VKR_CHECK(backend.bind_buffer_memory({buffer.buffer, buffer_memory.memory, 0}).ok);
    vkrpc::RecordedCommand bind;
    bind.kind = "bind_vertex_buffers";
    bind.first_binding = 0;
    bind.vertex_buffers = {buffer.buffer};
    bind.vertex_buffer_offsets = {0};

    // Begin assigns generations locally, while only End ships a record. An abandoned generation 1
    // is therefore invisible to the worker: the first delivered epoch 2 is valid, not a protocol
    // skip. Once 2 is adopted, 1 is stale.
    vkrpc::RecordCommandBufferRequest skipped_recording;
    skipped_recording.command_buffer = cbs[2];
    skipped_recording.recording_generation = 2;
    VKR_CHECK(backend.record_command_buffer(skipped_recording).ok);
    vkrpc::RecordCommandBufferRequest stale_recording = skipped_recording;
    stale_recording.recording_generation = 1;
    const vkrpc::StatusResponse stale_record_response =
        backend.record_command_buffer(stale_recording);
    VKR_CHECK(!stale_record_response.ok);
    VKR_CHECK_EQ(stale_record_response.reason, "recording generation is stale");

    // The same non-contiguous contract applies before End: a destroy in active epoch 2 can reach a
    // worker that still knows epoch 0. The matching record exposes the retired buffer, then a later
    // delivered epoch 4 releases the older exact lease. A lease older than 4 is stale.
    const auto skipped_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(skipped_buffer.ok);
    allocate_memory.allocation_size = skipped_buffer.mem_size;
    const auto skipped_memory = backend.allocate_memory(allocate_memory);
    VKR_CHECK(skipped_memory.ok);
    VKR_CHECK(backend.bind_buffer_memory({skipped_buffer.buffer, skipped_memory.memory, 0}).ok);
    vkrpc::LeasedDestroyRequest skipped_destroy;
    skipped_destroy.handle = skipped_buffer.buffer;
    skipped_destroy.leases = {{cbs[3], 2}};
    VKR_CHECK(backend.destroy_buffer_leased(skipped_destroy).ok);
    vkrpc::RecordCommandBufferRequest skipped_leased_record;
    skipped_leased_record.command_buffer = cbs[3];
    skipped_leased_record.recording_generation = 2;
    skipped_leased_record.commands = {bind};
    skipped_leased_record.commands[0].vertex_buffers = {skipped_buffer.buffer};
    VKR_CHECK(backend.record_command_buffer(skipped_leased_record).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(1));
    skipped_leased_record.recording_generation = 4;
    skipped_leased_record.commands.clear();
    VKR_CHECK(backend.record_command_buffer(skipped_leased_record).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(0));
    VKR_CHECK(backend.free_memory({skipped_memory.memory}).ok);

    const auto stale_lease_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(stale_lease_buffer.ok);
    vkrpc::LeasedDestroyRequest stale_destroy;
    stale_destroy.handle = stale_lease_buffer.buffer;
    stale_destroy.leases = {{cbs[3], 3}};
    const vkrpc::StatusResponse stale_destroy_response =
        backend.destroy_buffer_leased(stale_destroy);
    VKR_CHECK(!stale_destroy_response.ok);
    VKR_CHECK_EQ(stale_destroy_response.reason,
                 "leased buffer destroy references an invalid command-buffer generation");
    VKR_CHECK(backend.destroy_buffer({stale_lease_buffer.buffer}).ok);

    vkrpc::RecordCommandBufferRequest buffer_recording;
    buffer_recording.command_buffer = cbs[1];
    buffer_recording.recording_generation = 1;
    buffer_recording.commands = {bind};
    VKR_CHECK(backend.record_command_buffer(buffer_recording).ok);
    vkrpc::LeasedDestroyRequest buffer_destroy;
    buffer_destroy.handle = buffer.buffer;
    buffer_destroy.leases = {{cbs[1], 1}};
    VKR_CHECK(backend.destroy_buffer_leased(buffer_destroy).ok);
    submit.command_buffers = {cbs[1]};
    VKR_CHECK(backend.queue_submit(submit).ok);
    retire.recordings = {{cbs[1], 1}};
    VKR_CHECK(backend.retire_command_buffer_recordings(retire).ok);
    VKR_CHECK(backend.free_memory({buffer_memory.memory}).ok);
    const vkrpc::LifetimeLeaseStats lease_stats = backend.lifetime_lease_stats();
    VKR_CHECK_EQ(lease_stats.retired_resources, static_cast<std::size_t>(0));
    VKR_CHECK_EQ(lease_stats.retired_memories, static_cast<std::size_t>(0));
    VKR_CHECK_EQ(lease_stats.lease_edges, static_cast<std::size_t>(0));
    VKR_CHECK_EQ(lease_stats.capacity_rejections, static_cast<std::size_t>(0));

    // Per-device retained-resource cap: rejection is atomic and the exact pending generation can
    // release the whole bounded set. Keep this one loop terse; it is a state-machine capacity pin,
    // not 4096 independent behavioral assertions.
    vkrpc::LeasedDestroyRequest bounded_destroy;
    bounded_destroy.leases = {{cbs[0], 2}};
    for (std::size_t i = 0; i < vkrpc::kMaxRetiredRecordingResourcesPerDevice; ++i) {
        const auto retained = backend.create_buffer(create_buffer);
        if (!retained.ok) {
            VKR_CHECK(false);
            break;
        }
        bounded_destroy.handle = retained.buffer;
        if (!backend.destroy_buffer_leased(bounded_destroy).ok) {
            VKR_CHECK(false);
            break;
        }
    }
    const vkrpc::LifetimeLeaseStats at_capacity = backend.lifetime_lease_stats();
    VKR_CHECK_EQ(at_capacity.retired_resources, vkrpc::kMaxRetiredRecordingResourcesPerDevice);
    VKR_CHECK_EQ(at_capacity.lease_edges, vkrpc::kMaxRetiredRecordingResourcesPerDevice);
    const auto overflow_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(overflow_buffer.ok);
    bounded_destroy.handle = overflow_buffer.buffer;
    const vkrpc::StatusResponse overflow = backend.destroy_buffer_leased(bounded_destroy);
    VKR_CHECK(!overflow.ok);
    VKR_CHECK_EQ(overflow.reason, "recording resource lease capacity exceeded");
    const vkrpc::LifetimeLeaseStats after_overflow = backend.lifetime_lease_stats();
    VKR_CHECK_EQ(after_overflow.retired_resources, at_capacity.retired_resources);
    VKR_CHECK_EQ(after_overflow.lease_edges, at_capacity.lease_edges);
    VKR_CHECK_EQ(after_overflow.capacity_rejections, static_cast<std::size_t>(1));
    VKR_CHECK(backend.destroy_buffer({overflow_buffer.buffer}).ok);
    retire.recordings = {{cbs[0], 2}};
    VKR_CHECK(backend.retire_command_buffer_recordings(retire).ok);
    const vkrpc::LifetimeLeaseStats after_capacity_release = backend.lifetime_lease_stats();
    VKR_CHECK_EQ(after_capacity_release.retired_resources, static_cast<std::size_t>(0));
    VKR_CHECK_EQ(after_capacity_release.lease_edges, static_cast<std::size_t>(0));

    // Re-recording a later generation retires leases owned by the replaced generation.
    const auto rerecord_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(rerecord_buffer.ok);
    bounded_destroy.handle = rerecord_buffer.buffer;
    VKR_CHECK(backend.destroy_buffer_leased(bounded_destroy).ok); // pending generation 2
    vkrpc::RecordCommandBufferRequest empty_recording;
    empty_recording.command_buffer = cbs[0];
    empty_recording.recording_generation = 2;
    VKR_CHECK(backend.record_command_buffer(empty_recording).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(1));
    empty_recording.recording_generation = 3;
    VKR_CHECK(backend.record_command_buffer(empty_recording).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(0));

    // FreeCommandBuffers is a physical retirement boundary.
    const auto free_cb_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(free_cb_buffer.ok);
    vkrpc::LeasedDestroyRequest free_cb_destroy;
    free_cb_destroy.handle = free_cb_buffer.buffer;
    free_cb_destroy.leases = {{cbs[1], 2}};
    VKR_CHECK(backend.destroy_buffer_leased(free_cb_destroy).ok);
    vkrpc::FreeCommandBuffersRequest free_cb;
    free_cb.command_pool = pool;
    free_cb.command_buffers = {cbs[1]};
    VKR_CHECK(backend.free_command_buffers(free_cb).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(0));

    // A failed End/record keeps the pending exact lease until the ICD's explicit retirement.
    allocate_cbs.count = 1;
    const auto failed_cb = backend.allocate_command_buffers(allocate_cbs).command_buffers.front();
    const auto failed_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(failed_buffer.ok);
    vkrpc::LeasedDestroyRequest failed_destroy;
    failed_destroy.handle = failed_buffer.buffer;
    failed_destroy.leases = {{failed_cb, 1}};
    VKR_CHECK(backend.destroy_buffer_leased(failed_destroy).ok);
    vkrpc::RecordCommandBufferRequest failed_record;
    failed_record.command_buffer = failed_cb;
    failed_record.recording_generation = 1;
    failed_record.commands = {bind}; // exposed, then a structural validation failure
    failed_record.commands[0].vertex_buffers = {failed_buffer.buffer};
    failed_record.commands[0].first_binding = 1;
    VKR_CHECK(!backend.record_command_buffer(failed_record).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(1));
    retire.recordings = {{failed_cb, 1}};
    VKR_CHECK(backend.retire_command_buffer_recordings(retire).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(0));

    // DestroyCommandPool implicitly frees every CB and releases even a pending future generation.
    const auto pool_buffer = backend.create_buffer(create_buffer);
    VKR_CHECK(pool_buffer.ok);
    vkrpc::LeasedDestroyRequest pool_destroy;
    pool_destroy.handle = pool_buffer.buffer;
    pool_destroy.leases = {{failed_cb, 2}};
    VKR_CHECK(backend.destroy_buffer_leased(pool_destroy).ok);
    VKR_CHECK(backend.destroy_command_pool({pool}).ok);
    const vkrpc::LifetimeLeaseStats after_pool_destroy = backend.lifetime_lease_stats();
    VKR_CHECK_EQ(after_pool_destroy.retired_resources, static_cast<std::size_t>(0));
    VKR_CHECK_EQ(after_pool_destroy.lease_edges, static_cast<std::size_t>(0));
}

// Sampler, combined-image-sampler, and texture-upload wire round-trips.
void test_sampler_cis_wire() {
    // CreateSampler request/response.
    vkrpc::CreateSamplerRequest sm;
    sm.device = 0x7;
    sm.mag_filter = vkrpc::kFilterNearest;
    sm.min_filter = vkrpc::kFilterNearest;
    sm.mipmap_mode = vkrpc::kSamplerMipmapModeNearest;
    sm.address_mode_u = vkrpc::kSamplerAddressModeClampToEdge;
    sm.address_mode_v = vkrpc::kSamplerAddressModeClampToEdge;
    sm.address_mode_w = vkrpc::kSamplerAddressModeClampToEdge;
    sm.anisotropy_enable = 0;
    sm.compare_enable = 0;
    const auto sm2 = vkrpc::CreateSamplerRequest::from_body(sm.to_body());
    VKR_CHECK_EQ(sm2.device, static_cast<std::uint64_t>(0x7));
    VKR_CHECK_EQ(sm2.address_mode_w, vkrpc::kSamplerAddressModeClampToEdge);
    VKR_CHECK_EQ(sm2.anisotropy_enable, 0);
    // A sampler request with NO carried bool fields must decode anisotropy/compare as -1 (the
    // fail-closed sentinel), not 0 -- so a malformed/hostile RPC cannot pass the subset by
    // omission.
    vkrpc::CreateSamplerRequest empty;
    const auto empty2 = vkrpc::CreateSamplerRequest::from_body(json::Value::make_object());
    (void) empty;
    VKR_CHECK_EQ(empty2.anisotropy_enable, -1);
    VKR_CHECK_EQ(empty2.compare_enable, -1);
    vkrpc::CreateSamplerResponse smr;
    smr.ok = true;
    smr.sampler = 0x99;
    const auto smr2 = vkrpc::CreateSamplerResponse::from_body(smr.to_body());
    VKR_CHECK(smr2.ok);
    VKR_CHECK_EQ(smr2.sampler, static_cast<std::uint64_t>(0x99));

    // copy_buffer_to_image + generalized pipeline_barrier recorded-command round-trip. The upload
    // rides the faithful 13-per-region payload (the copy_image_to_buffer convention): a sub-region
    // mip-1 copy with buffer strides, > INT16 extents carried wide, and a DEPTH aspect (the wire
    // carries any aspect verbatim -- the validation is separate).
    vkrpc::RecordCommandBufferRequest rc;
    rc.command_buffer = 3;
    vkrpc::RecordedCommand cp;
    cp.kind = "copy_buffer_to_image";
    cp.src_buffer = 0x55;
    cp.image = 0x66;
    cp.args_i64 = {vkrpc::kImageLayoutTransferDstOptimal,
                   1, // regionCount
                   256,
                   4096,
                   2048,
                   vkrpc::kImageAspectDepth,
                   1,
                   0,
                   1,
                   32,
                   16,
                   0,
                   4096,
                   2048,
                   1};
    vkrpc::RecordedCommand bar;
    bar.kind = "pipeline_barrier";
    bar.image = 0x66;
    bar.old_layout = vkrpc::kImageLayoutTransferDstOptimal;
    bar.new_layout = vkrpc::kImageLayoutShaderReadOnlyOptimal;
    bar.aspect = vkrpc::kImageAspectColor;
    bar.src_stage = 0x1000;
    bar.dst_stage = 0x800;
    bar.barrier_base_mip = 0;
    bar.barrier_level_count = 1;
    bar.barrier_base_layer = 0;
    bar.barrier_layer_count = 1;
    rc.commands = {cp, bar};
    const auto rc2 = vkrpc::RecordCommandBufferRequest::from_body(rc.to_body());
    VKR_CHECK_EQ(static_cast<int>(rc2.commands.size()), 2);
    VKR_CHECK_EQ(rc2.commands[0].src_buffer, static_cast<std::uint64_t>(0x55));
    VKR_CHECK_EQ(static_cast<int>(rc2.commands[0].args_i64.size()), 15);
    VKR_CHECK_EQ(rc2.commands[0].args_i64[0], vkrpc::kImageLayoutTransferDstOptimal);
    VKR_CHECK_EQ(rc2.commands[0].args_i64[2], 256);                      // bufferOffset
    VKR_CHECK_EQ(rc2.commands[0].args_i64[5], vkrpc::kImageAspectDepth); // aspect rides the wire
    VKR_CHECK_EQ(rc2.commands[0].args_i64[12], 4096); // extent width > INT16, carried wide
    VKR_CHECK_EQ(rc2.commands[1].new_layout, vkrpc::kImageLayoutShaderReadOnlyOptimal);
    VKR_CHECK_EQ(rc2.commands[1].barrier_level_count, 1);
    // A barrier with NO carried range must decode all four range fields as -1 (the legacy
    // whole-subresource shorthand), distinct from an explicit {0,1,0,1}.
    vkrpc::RecordCommandBufferRequest rc_norange;
    rc_norange.command_buffer = 3;
    vkrpc::RecordedCommand bar_norange;
    bar_norange.kind = "pipeline_barrier";
    rc_norange.commands = {bar_norange};
    const auto rcn = vkrpc::RecordCommandBufferRequest::from_body(rc_norange.to_body());
    VKR_CHECK_EQ(rcn.commands[0].barrier_base_mip, -1);
    VKR_CHECK_EQ(rcn.commands[0].barrier_layer_count, -1);

    // WriteDescriptorSet image-info path round-trip.
    vkrpc::UpdateDescriptorSetsRequest up;
    up.device = 0x7;
    vkrpc::WriteDescriptorSetDesc w;
    w.dst_set = 0x10;
    w.dst_binding = 1;
    w.dst_array_element = 0;
    w.descriptor_type = vkrpc::kDescriptorTypeCombinedImageSampler;
    w.descriptor_count = 1;
    w.image_infos.push_back({0x99, 0xAA, vkrpc::kImageLayoutShaderReadOnlyOptimal});
    up.writes.push_back(w);
    const auto up2 = vkrpc::UpdateDescriptorSetsRequest::from_body(up.to_body());
    VKR_CHECK_EQ(static_cast<int>(up2.writes.size()), 1);
    VKR_CHECK_EQ(up2.writes[0].descriptor_type, vkrpc::kDescriptorTypeCombinedImageSampler);
    VKR_CHECK_EQ(static_cast<int>(up2.writes[0].image_infos.size()), 1);
    VKR_CHECK_EQ(up2.writes[0].image_infos[0].sampler, static_cast<std::uint64_t>(0x99));
    VKR_CHECK_EQ(up2.writes[0].image_infos[0].image_view, static_cast<std::uint64_t>(0xAA));
    VKR_CHECK_EQ(up2.writes[0].image_infos[0].image_layout,
                 vkrpc::kImageLayoutShaderReadOnlyOptimal);
}

// Combined-image-sampler descriptor lifecycle in the mock oracle: the 2-binding layout + 2-size
// pool, the image-info write validation + device-scoped poison, draw-readiness across BOTH
// bindings, and the CB -> set -> {sampler, image-view} destroy consult (both edges).
void test_combined_image_sampler_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    const std::uint64_t dev = cd.device;
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = dev;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(gq).queue;
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = dev;
    cpr.queue_family_index = cd.queue_family_index;
    const std::uint64_t pool_cmd = backend.create_command_pool(cpr).command_pool;
    auto alloc_cmd = [&]() {
        vkrpc::AllocateCommandBuffersRequest acb;
        acb.command_pool = pool_cmd;
        acb.count = 1;
        return backend.allocate_command_buffers(acb).command_buffers.front();
    };

    // Memory types: a coherent host-visible type (UBO) and a device-local type (texture).
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
    mpq.physical_device = phys;
    const auto mp = backend.get_physical_device_memory_properties(mpq);
    int coherent_type = -1;
    int device_local_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
        } else if ((mp.types[i].property_flags & vkrpc::kMemoryPropertyDeviceLocal) != 0) {
            device_local_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(coherent_type >= 0 && device_local_type >= 0);

    // A sampler: general GL samplers (LINEAR/REPEAT/anisotropy) are now ACCEPTED (mock ==
    // real, faithful forwarding); only a missing/malformed field or a bad device is rejected.
    vkrpc::CreateSamplerRequest smq;
    smq.device = dev;
    smq.mag_filter = vkrpc::kFilterNearest;
    smq.min_filter = vkrpc::kFilterNearest;
    smq.mipmap_mode = vkrpc::kSamplerMipmapModeNearest;
    smq.address_mode_u = vkrpc::kSamplerAddressModeClampToEdge;
    smq.address_mode_v = vkrpc::kSamplerAddressModeClampToEdge;
    smq.address_mode_w = vkrpc::kSamplerAddressModeClampToEdge;
    smq.anisotropy_enable = 0;
    smq.compare_enable = 0;
    const std::uint64_t sampler = backend.create_sampler(smq).sampler;
    VKR_CHECK(sampler != 0);
    {
        // A general LINEAR/REPEAT/anisotropy sampler is accepted; create + destroy so it doesn't
        // dangle.
        auto r = smq;
        r.mag_filter = 1;     // LINEAR
        r.address_mode_u = 0; // REPEAT
        r.anisotropy_enable = 1;
        r.max_anisotropy = 8.0;
        const std::uint64_t lin = backend.create_sampler(r).sampler;
        VKR_CHECK(lin != 0);
        vkrpc::HandleRequest dlin;
        dlin.handle = lin;
        VKR_CHECK(backend.destroy_sampler(dlin).ok);
        // A malformed field (negative enum) + a bad device are rejected.
        r = smq;
        r.mipmap_mode = -1;
        VKR_CHECK(!backend.create_sampler(r).ok);
        r = smq;
        r.device = 0xDEAD;
        VKR_CHECK(!backend.create_sampler(r).ok);
    }

    // A SAMPLED R8G8B8A8_UNORM texture image bound to device-local memory + a COLOR view over it.
    auto make_texture = [&]() {
        vkrpc::CreateImageRequest ir;
        ir.device = dev;
        ir.image_type = vkrpc::kImageType2D;
        ir.format = vkrpc::kFormatR8G8B8A8Unorm;
        ir.width = 16;
        ir.height = 16;
        ir.depth = 1;
        ir.mip_levels = 1;
        ir.array_layers = 1;
        ir.samples = 1;
        ir.tiling = vkrpc::kImageTilingOptimal;
        ir.usage = vkrpc::kImageUsageSampled;
        ir.sharing_mode = 0;
        ir.initial_layout = 0;
        const vkrpc::CreateImageResponse img = backend.create_image(ir);
        VKR_CHECK(img.ok);
        vkrpc::AllocateMemoryRequest am;
        am.device = dev;
        am.allocation_size = img.mem_size;
        am.memory_type_index = device_local_type;
        const std::uint64_t m = backend.allocate_memory(am).memory;
        VKR_CHECK(backend.bind_image_memory({img.image, m, 0}).ok);
        vkrpc::CreateImageViewRequest ivr;
        ivr.image = img.image;
        ivr.view_type = 1;
        ivr.format = vkrpc::kFormatR8G8B8A8Unorm;
        ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = 0;
        ivr.aspect = vkrpc::kImageAspectColor;
        ivr.base_mip_level = 0;
        ivr.level_count = 1;
        ivr.base_array_layer = 0;
        ivr.layer_count = 1;
        const std::uint64_t view = backend.create_image_view(ivr).image_view;
        VKR_CHECK(view != 0);
        return std::make_pair(img.image, view);
    };
    const auto tex = make_texture();
    const std::uint64_t tview = tex.second;

    // A UBO buffer bound to coherent memory (binding 0).
    vkrpc::AllocateMemoryRequest amu;
    amu.device = dev;
    amu.allocation_size = 4096;
    amu.memory_type_index = coherent_type;
    const std::uint64_t umem = backend.allocate_memory(amu).memory;
    vkrpc::CreateBufferRequest cbq;
    cbq.device = dev;
    cbq.size = 256;
    cbq.usage = vkrpc::kBufferUsageUniformBuffer;
    cbq.sharing_mode = 0;
    const std::uint64_t ubo = backend.create_buffer(cbq).buffer;
    VKR_CHECK(backend.bind_buffer_memory({ubo, umem, 0}).ok);
    // A second UBO is also a direct vertex-buffer reference in the lease-overlap pin below.
    cbq.usage = vkrpc::kBufferUsageUniformBuffer | vkrpc::kBufferUsageVertexBuffer;
    const std::uint64_t overlap_buffer = backend.create_buffer(cbq).buffer;
    VKR_CHECK(backend.bind_buffer_memory({overlap_buffer, umem, 256}).ok);

    // The 2-binding set layout: 0 = UNIFORM_BUFFER/VERTEX, 1 = COMBINED_IMAGE_SAMPLER/FRAGMENT.
    vkrpc::CreateDescriptorSetLayoutRequest dslr;
    dslr.device = dev;
    dslr.bindings.push_back({0, vkrpc::kDescriptorTypeUniformBuffer, 1, 1 /*VERTEX*/});
    dslr.bindings.push_back({1, vkrpc::kDescriptorTypeCombinedImageSampler, 1, 16 /*FRAGMENT*/});
    const std::uint64_t set_layout = backend.create_descriptor_set_layout(dslr).set_layout;
    VKR_CHECK(set_layout != 0);

    // The 2-size pool: UNIFORM + COMBINED_IMAGE_SAMPLER.
    vkrpc::CreateDescriptorPoolRequest dpr;
    dpr.device = dev;
    dpr.max_sets = 4;
    dpr.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 8});
    dpr.pool_sizes.push_back({vkrpc::kDescriptorTypeCombinedImageSampler, 8});
    const std::uint64_t pool = backend.create_descriptor_pool(dpr).pool;
    VKR_CHECK(pool != 0);
    auto alloc_set = [&]() {
        vkrpc::AllocateDescriptorSetsRequest r;
        r.device = dev;
        r.pool = pool;
        r.set_layouts = {set_layout};
        return backend.allocate_descriptor_sets(r).descriptor_sets.front();
    };
    const std::uint64_t set = alloc_set();
    const std::uint64_t set_partial = alloc_set(); // UBO-only (proves CIS draw-readiness)
    const std::uint64_t set_neg = alloc_set();     // poison target for the negatives
    const std::uint64_t set_overlap = alloc_set(); // direct + descriptor lease overlap

    auto write_ubo = [&](std::uint64_t dst, std::uint64_t buffer) {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = dev;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = dst;
        w.dst_binding = 0;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeUniformBuffer;
        w.descriptor_count = 1;
        w.buffer_infos.push_back({buffer, 0, vkrpc::kVkWholeSize});
        r.writes.push_back(w);
        return r;
    };
    auto write_cis = [&](std::uint64_t dst, std::uint64_t s, std::uint64_t v, int layout) {
        vkrpc::UpdateDescriptorSetsRequest r;
        r.device = dev;
        vkrpc::WriteDescriptorSetDesc w;
        w.dst_set = dst;
        w.dst_binding = 1;
        w.dst_array_element = 0;
        w.descriptor_type = vkrpc::kDescriptorTypeCombinedImageSampler;
        w.descriptor_count = 1;
        w.image_infos.push_back({s, v, layout});
        r.writes.push_back(w);
        return r;
    };
    // Good writes to `set` (both bindings).
    VKR_CHECK(backend.update_descriptor_sets(write_ubo(set, ubo)).ok);
    VKR_CHECK(backend
                  .update_descriptor_sets(
                      write_cis(set, sampler, tview, vkrpc::kImageLayoutShaderReadOnlyOptimal))
                  .ok);
    VKR_CHECK(backend.update_descriptor_sets(write_ubo(set_partial, ubo)).ok); // CIS uninitialized
    VKR_CHECK(backend.update_descriptor_sets(write_ubo(set_overlap, overlap_buffer)).ok);
    VKR_CHECK(backend
                  .update_descriptor_sets(write_cis(set_overlap, sampler, tview,
                                                    vkrpc::kImageLayoutShaderReadOnlyOptimal))
                  .ok);
    // CIS write negatives (each poisons set_neg, re-validated by being unused afterward):
    // the faithful update still rejects a dead sampler / dead view (the resource must exist on the
    // device); image-layout value and write-type-vs-binding-type are the host driver's authority
    // now.
    VKR_CHECK(!backend
                   .update_descriptor_sets(
                       write_cis(set_neg, 0xDEAD, tview, vkrpc::kImageLayoutShaderReadOnlyOptimal))
                   .ok); // dead sampler
    VKR_CHECK(!backend
                   .update_descriptor_sets(write_cis(set_neg, sampler, 0xDEAD,
                                                     vkrpc::kImageLayoutShaderReadOnlyOptimal))
                   .ok); // dead view
    // a view over a NON-SAMPLED image is now ACCEPTED by the relay (the host driver +
    // validation layer is the authority on usage compatibility). Build a TRANSFER_DST-only image +
    // view and confirm the faithful update forwards it.
    {
        vkrpc::CreateImageRequest ir;
        ir.device = dev;
        ir.image_type = vkrpc::kImageType2D;
        ir.format = vkrpc::kFormatR8G8B8A8Unorm;
        ir.width = 16;
        ir.height = 16;
        ir.depth = 1;
        ir.mip_levels = 1;
        ir.array_layers = 1;
        ir.samples = 1;
        ir.tiling = vkrpc::kImageTilingOptimal;
        ir.usage = vkrpc::kImageUsageTransferDst; // no SAMPLED
        ir.sharing_mode = 0;
        ir.initial_layout = 0;
        const auto nimg = backend.create_image(ir);
        vkrpc::AllocateMemoryRequest am;
        am.device = dev;
        am.allocation_size = nimg.mem_size;
        am.memory_type_index = device_local_type;
        const std::uint64_t nm = backend.allocate_memory(am).memory;
        VKR_CHECK(backend.bind_image_memory({nimg.image, nm, 0}).ok);
        vkrpc::CreateImageViewRequest ivr;
        ivr.image = nimg.image;
        ivr.view_type = 1;
        ivr.format = vkrpc::kFormatR8G8B8A8Unorm;
        ivr.swizzle_r = ivr.swizzle_g = ivr.swizzle_b = ivr.swizzle_a = 0;
        ivr.aspect = vkrpc::kImageAspectColor;
        ivr.base_mip_level = 0;
        ivr.level_count = 1;
        ivr.base_array_layer = 0;
        ivr.layer_count = 1;
        const std::uint64_t nview = backend.create_image_view(ivr).image_view;
        VKR_CHECK(backend
                      .update_descriptor_sets(write_cis(set_neg, sampler, nview,
                                                        vkrpc::kImageLayoutShaderReadOnlyOptimal))
                      .ok);
    }

    // The draw graph: a color swapchain framebuffer + a bufferless pipeline whose layout has the
    // 2-binding set layout. (The texture/sampler are the CIS referents, not the color attachment.)
    const int kFormat = 44;
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const std::uint64_t surf = backend.create_surface(sr).surface;
    vkrpc::CreateSwapchainRequest scr;
    scr.device = dev;
    scr.surface = surf;
    scr.image_format = kFormat;
    scr.present_mode = 2;
    scr.width = 64;
    scr.height = 64;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const std::uint64_t sc = backend.create_swapchain(scr).swapchain;
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = sc;
    const std::uint64_t img0 = backend.get_swapchain_images(gir).images.front();
    vkrpc::CreateImageViewRequest cvr;
    cvr.image = img0;
    cvr.view_type = 1;
    cvr.format = kFormat;
    cvr.swizzle_r = cvr.swizzle_g = cvr.swizzle_b = cvr.swizzle_a = 0;
    cvr.aspect = 1;
    cvr.base_mip_level = 0;
    cvr.level_count = 1;
    cvr.base_array_layer = 0;
    cvr.layer_count = 1;
    const std::uint64_t cview = backend.create_image_view(cvr).image_view;
    auto make_sm = [&](std::size_t bytes) {
        vkrpc::CreateShaderModuleRequest r;
        r.device = dev;
        r.code = std::string(bytes, '\0');
        r.code_size = bytes;
        return backend.create_shader_module(r).shader_module;
    };
    const std::uint64_t vs = make_sm(8);
    const std::uint64_t fs = make_sm(8);
    vkrpc::CreateRenderPassRequest rpr;
    rpr.device = dev;
    vkrpc::AttachmentDesc att;
    att.format = kFormat;
    att.samples = 1;
    att.load_op = 1;
    att.store_op = 0;
    att.stencil_load_op = 2;
    att.stencil_store_op = 1;
    att.initial_layout = 0;
    att.final_layout = 1000001002;
    rpr.attachments.push_back(att);
    rpr.color_attachment = 0;
    rpr.color_layout = 2;
    const std::uint64_t rp = backend.create_render_pass(rpr).render_pass;
    vkrpc::CreateFramebufferRequest fbr;
    fbr.device = dev;
    fbr.render_pass = rp;
    fbr.image_view = cview;
    fbr.width = 64;
    fbr.height = 64;
    fbr.layers = 1;
    const std::uint64_t fb = backend.create_framebuffer(fbr).framebuffer;
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = dev;
    plr.set_layout_count = 1;
    plr.push_constant_range_count = 0;
    plr.set_layouts = {set_layout};
    const std::uint64_t pl = backend.create_pipeline_layout(plr).pipeline_layout;
    vkrpc::CreateGraphicsPipelinesRequest gpr;
    gpr.device = dev;
    vkrpc::ShaderStageDesc s0;
    s0.stage = 1;
    s0.module = vs;
    s0.entry = "main";
    vkrpc::ShaderStageDesc s1;
    s1.stage = 16;
    s1.module = fs;
    s1.entry = "main";
    gpr.stages = {s0, s1};
    gpr.topology = 3;
    gpr.vertex_binding_count = 0;
    gpr.vertex_attribute_count = 0;
    gpr.cull_mode = 0;
    gpr.front_face = 1;
    gpr.dynamic_states = {0, 1};
    gpr.layout = pl;
    gpr.render_pass = rp;
    gpr.subpass = 0;
    const std::uint64_t pipe = backend.create_graphics_pipelines(gpr).pipeline;
    VKR_CHECK(pipe != 0);

    auto record_draw = [&](std::uint64_t cmd, std::uint64_t which_set, std::uint64_t generation = 0,
                           std::uint64_t direct_buffer = 0) {
        vkrpc::RecordCommandBufferRequest rc;
        rc.command_buffer = cmd;
        rc.recording_generation = generation;
        vkrpc::RecordedCommand begin;
        begin.kind = "begin_render_pass";
        begin.render_pass = rp;
        begin.framebuffer = fb;
        begin.render_area_w = 64;
        begin.render_area_h = 64;
        vkrpc::RecordedCommand bindp;
        bindp.kind = "bind_pipeline";
        bindp.pipeline = pipe;
        vkrpc::RecordedCommand vp;
        vp.kind = "set_viewport";
        vkrpc::RecordedCommand sci;
        sci.kind = "set_scissor";
        vkrpc::RecordedCommand binds;
        binds.kind = "bind_descriptor_sets";
        binds.desc_layout = pl;
        binds.first_set = 0;
        binds.descriptor_sets = {which_set};
        vkrpc::RecordedCommand draw;
        draw.kind = "draw";
        draw.vertex_count = 36;
        draw.instance_count = 1;
        draw.first_vertex = 0;
        draw.first_instance = 0;
        vkrpc::RecordedCommand end;
        end.kind = "end_render_pass";
        rc.commands = {begin, bindp, vp, sci, binds};
        if (direct_buffer != 0) {
            vkrpc::RecordedCommand bind_vertex;
            bind_vertex.kind = "bind_vertex_buffers";
            bind_vertex.first_binding = 0;
            bind_vertex.vertex_buffers = {direct_buffer};
            bind_vertex.vertex_buffer_offsets = {0};
            rc.commands.push_back(std::move(bind_vertex));
        }
        rc.commands.push_back(std::move(draw));
        rc.commands.push_back(std::move(end));
        return backend.record_command_buffer(rc);
    };
    auto submit = [&](std::uint64_t cmd) {
        vkrpc::QueueSubmitRequest qs;
        qs.queue = queue;
        qs.command_buffers = {cmd};
        return backend.queue_submit(qs);
    };

    // Draw-readiness: the fully-written set draws; the partial set (CIS binding uninitialized) does
    // not -- proving draw-readiness spans BOTH bindings.
    const std::uint64_t cmd_ok = alloc_cmd();
    VKR_CHECK(record_draw(cmd_ok, set).ok);
    VKR_CHECK(submit(cmd_ok).ok);
    const std::uint64_t cmd_partial = alloc_cmd();
    VKR_CHECK(
        !record_draw(cmd_partial, set_partial).ok); // CIS slot uninitialized -> not draw-ready

    // Descriptor resources are not independently leased in v1, but an exact generation that also
    // references the same buffer directly is safe: the retired host buffer remains alive. The set
    // bookkeeping may dangle for future recordings while this already-recorded generation stays
    // executable.
    const std::uint64_t cmd_overlap = alloc_cmd();
    VKR_CHECK(record_draw(cmd_overlap, set_overlap, 1, overlap_buffer).ok);
    vkrpc::LeasedDestroyRequest overlap_destroy;
    overlap_destroy.handle = overlap_buffer;
    overlap_destroy.leases = {{cmd_overlap, 1}};
    VKR_CHECK(backend.destroy_buffer_leased(overlap_destroy).ok);
    VKR_CHECK(submit(cmd_overlap).ok);
    vkrpc::RetireCommandBufferRecordingsRequest overlap_retire;
    overlap_retire.recordings = {{cmd_overlap, 1}};
    VKR_CHECK(backend.retire_command_buffer_recordings(overlap_retire).ok);
    VKR_CHECK_EQ(backend.lifetime_lease_stats().retired_resources, static_cast<std::size_t>(0));

    // Destroy consult -- destroying the bound sampler dangles `set` and invalidates the recorded
    // CB.
    VKR_CHECK(submit(cmd_ok).ok); // still submittable before the destroy
    VKR_CHECK(backend.destroy_sampler({sampler}).ok);
    VKR_CHECK(!submit(cmd_ok).ok); // sampler destroy dangled the set -> CB invalidated

    // Re-create a sampler, re-write the CIS slot, re-record -> ready again; then destroy the IMAGE
    // VIEW and prove the same consult fires for the view referent.
    const std::uint64_t sampler2 = backend.create_sampler(smq).sampler;
    VKR_CHECK(backend
                  .update_descriptor_sets(
                      write_cis(set, sampler2, tview, vkrpc::kImageLayoutShaderReadOnlyOptimal))
                  .ok);
    const std::uint64_t cmd_ok2 = alloc_cmd();
    VKR_CHECK(record_draw(cmd_ok2, set).ok);
    VKR_CHECK(submit(cmd_ok2).ok);
    // Repoint-away edge: destroying the OLD (already-destroyed) sampler is moot; instead prove that
    // destroying a sampler the set no longer references does NOT invalidate. Make a throwaway
    // sampler, never bound, and destroy it -> the recorded CB stays valid.
    const std::uint64_t sampler_unused = backend.create_sampler(smq).sampler;
    VKR_CHECK(backend.destroy_sampler({sampler_unused}).ok);
    VKR_CHECK(submit(cmd_ok2).ok); // unrelated sampler destroy did NOT dangle the set
    // Now destroy the image view the set references -> consult fires.
    VKR_CHECK(backend.destroy_image_view({tview}).ok);
    VKR_CHECK(!submit(cmd_ok2).ok);

    // A live sampler blocks the device's destroy (device-child rule).
    VKR_CHECK(!backend.destroy_device({dev}).ok);
}

// Texture upload (copy_buffer_to_image + the generalized pipeline_barrier) in the mock oracle: the
// (the Vulkan-1.3 opener -- EDS1): the six VK_EXT_extended_dynamic_state setters map to
// their CmdKinds, and the mock accepts a well-formed setter stream (single u32 in args_i64[0]) +
// rejects a malformed one -- mock == real (the real backend validates + replays the same shape).
void test_eds_dynamic_state_mock() {
    // The kind-string vocabulary (pure -- no backend).
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_cull_mode") == vkrpc::CmdKind::SetCullMode);
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_front_face") == vkrpc::CmdKind::SetFrontFace);
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_primitive_topology") ==
              vkrpc::CmdKind::SetPrimitiveTopology);
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_depth_test_enable") ==
              vkrpc::CmdKind::SetDepthTestEnable);
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_depth_write_enable") ==
              vkrpc::CmdKind::SetDepthWriteEnable);
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_depth_compare_op") ==
              vkrpc::CmdKind::SetDepthCompareOp);
    VKR_CHECK(vkrpc::cmd_kind_from_string("set_bogus_eds") == vkrpc::CmdKind::Unknown);

    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    auto setter = [](const char* kind, long long v) {
        vkrpc::RecordedCommand c;
        c.kind = kind;
        c.args_i64 = {v};
        return c;
    };
    auto make_pool = [&](std::uint64_t device) {
        vkrpc::CreateCommandPoolRequest cpr;
        cpr.device = device;
        return backend.create_command_pool(cpr).command_pool;
    };
    auto alloc_cmd = [&](std::uint64_t p) {
        vkrpc::AllocateCommandBuffersRequest acb;
        acb.command_pool = p;
        acb.count = 1;
        return backend.allocate_command_buffers(acb).command_buffers.front();
    };

    // With VK_EXT_extended_dynamic_state ENABLED: a well-formed setter stream records OK.
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    cdr.enabled_extensions = {"VK_EXT_extended_dynamic_state"};
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    const std::uint64_t pool_cmd = make_pool(cd.device);
    vkrpc::RecordCommandBufferRequest rec;
    rec.command_buffer = alloc_cmd(pool_cmd);
    rec.commands = {setter("set_cull_mode", 0 /*NONE*/),
                    setter("set_front_face", 0 /*CCW*/),
                    setter("set_primitive_topology", 3 /*TRIANGLE_LIST*/),
                    setter("set_depth_test_enable", 1),
                    setter("set_depth_write_enable", 1),
                    setter("set_depth_compare_op", 1 /*LESS*/)};
    VKR_CHECK(backend.record_command_buffer(rec).ok);
    // BindVertexBuffers2 logical ranges: VK_WHOLE_SIZE is valid only from an in-range offset;
    // explicit sizes may reach, but not cross, the guest-visible end.
    {
        vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
        mpq.physical_device = phys;
        const auto mp = backend.get_physical_device_memory_properties(mpq);
        vkrpc::AllocateMemoryRequest am;
        am.device = cd.device;
        am.allocation_size = 256;
        am.memory_type_index = 0;
        const auto mem = backend.allocate_memory(am);
        VKR_CHECK(mem.ok && !mp.types.empty());
        vkrpc::CreateBufferRequest br;
        br.device = cd.device;
        br.size = 64;
        br.usage = vkrpc::kBufferUsageVertexBuffer;
        br.sharing_mode = 0;
        const auto vb = backend.create_buffer(br);
        VKR_CHECK(vb.ok);
        vkrpc::BindBufferMemoryRequest bm;
        bm.buffer = vb.buffer;
        bm.memory = mem.memory;
        VKR_CHECK(backend.bind_buffer_memory(bm).ok);
        auto bind2 = [&](std::uint64_t offset, std::uint64_t size) {
            vkrpc::RecordedCommand c;
            c.kind = "bind_vertex_buffers2";
            c.args_i64 = {0, 1, 1, 0};
            c.args_u64 = {vb.buffer, offset, size};
            vkrpc::RecordCommandBufferRequest r;
            r.command_buffer = alloc_cmd(pool_cmd);
            r.commands = {c};
            return backend.record_command_buffer(r);
        };
        VKR_CHECK(bind2(16, vkrpc::kVkWholeSize).ok);
        VKR_CHECK(bind2(16, 48).ok);
        VKR_CHECK(!bind2(64, vkrpc::kVkWholeSize).ok);
        VKR_CHECK(!bind2(16, 49).ok);
    }
    // A malformed setter (no args, so a blind args_i64[0] would be UB) is fail-closed.
    vkrpc::RecordCommandBufferRequest bad;
    bad.command_buffer = alloc_cmd(pool_cmd);
    vkrpc::RecordedCommand malformed;
    malformed.kind = "set_cull_mode"; // args_i64 left empty
    bad.commands = {malformed};
    VKR_CHECK(!backend.record_command_buffer(bad).ok);

    // WITHOUT the extension enabled, an EDS setter is REJECTED -- a command is only
    // accepted where its replay entrypoint (the *EXT PFN) truly exists (mock == real).
    vkrpc::CreateDeviceRequest cdr_no_eds;
    cdr_no_eds.instance = ci.instance;
    cdr_no_eds.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd2 = backend.create_device(cdr_no_eds);
    vkrpc::RecordCommandBufferRequest no_eds;
    no_eds.command_buffer = alloc_cmd(make_pool(cd2.device));
    no_eds.commands = {setter("set_cull_mode", 0)};
    VKR_CHECK(!backend.record_command_buffer(no_eds).ok);
}

// (native lane): VK_KHR_dynamic_rendering. The mock is the structural oracle:
// the DR pipeline compatibility contract + the
// begin/end_rendering record shape + the unified render-scope state machine + the draw-time DR
// compatibility key.
void test_dynamic_rendering_mock() {
    // The kind-string vocabulary (pure).
    VKR_CHECK(vkrpc::cmd_kind_from_string("begin_rendering") == vkrpc::CmdKind::BeginRendering);
    VKR_CHECK(vkrpc::cmd_kind_from_string("end_rendering") == vkrpc::CmdKind::EndRendering);
    VKR_CHECK(vkrpc::cmd_kind_from_string("begin_renderingz") == vkrpc::CmdKind::Unknown);

    const int kFormat = 44; // VK_FORMAT_R8G8B8A8_UNORM-ish; the mock only compares equality

    // CreateGraphicsPipelinesRequest DR fields round-trip through to_body/from_body
    // (sparse-wire discipline: the JSON path is field-identical).
    {
        vkrpc::CreateGraphicsPipelinesRequest r;
        r.device = 7;
        r.has_dynamic_rendering = 1;
        r.dr_view_mask = 0;
        r.dr_color_formats = {kFormat, 0};
        r.dr_depth_format = 5;
        r.dr_stencil_format = 6;
        const auto rt = vkrpc::CreateGraphicsPipelinesRequest::from_body(r.to_body());
        VKR_CHECK_EQ(rt.has_dynamic_rendering, 1);
        VKR_CHECK_EQ(rt.dr_color_formats.size(), static_cast<std::size_t>(2));
        VKR_CHECK_EQ(rt.dr_color_formats[0], kFormat);
        VKR_CHECK_EQ(rt.dr_depth_format, 5);
        VKR_CHECK_EQ(rt.dr_stencil_format, 6);
        // An omitting (render-pass / compute) peer round-trips with has_dynamic_rendering 0.
        vkrpc::CreateGraphicsPipelinesRequest plain;
        VKR_CHECK_EQ(
            vkrpc::CreateGraphicsPipelinesRequest::from_body(plain.to_body()).has_dynamic_rendering,
            0);
    }

    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    // VK_EXT_transform_feedback is also enabled so the XFB-inside-DR case below can drive a
    // real begin_transform_feedback on this device.
    cdr.enabled_extensions = {"VK_KHR_dynamic_rendering", "VK_EXT_transform_feedback"};
    cdr.dynamic_rendering_feature_enabled = 1; // ext AND feature
    const std::uint64_t device = backend.create_device(cdr).device;

    auto make_pool = [&](std::uint64_t dev) {
        vkrpc::CreateCommandPoolRequest cpr;
        cpr.device = dev;
        return backend.create_command_pool(cpr).command_pool;
    };
    const std::uint64_t pool = make_pool(device);
    auto alloc_cmd = [&]() {
        vkrpc::AllocateCommandBuffersRequest acb;
        acb.command_pool = pool;
        acb.count = 1;
        return backend.allocate_command_buffers(acb).command_buffers.front();
    };

    // A color image view (over a swapchain image) whose format is the DR compat key.
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    const std::uint64_t surface = backend.create_surface(sr).surface;
    vkrpc::CreateSwapchainRequest scr;
    scr.device = device;
    scr.surface = surface;
    scr.image_format = kFormat;
    scr.present_mode = 2;
    scr.width = 256;
    scr.height = 256;
    scr.min_image_count = 2;
    scr.image_usage = vkrpc::kImageUsageColorAttachment;
    const std::uint64_t swapchain = backend.create_swapchain(scr).swapchain;
    vkrpc::GetSwapchainImagesRequest gir;
    gir.swapchain = swapchain;
    const std::uint64_t img0 = backend.get_swapchain_images(gir).images.front();
    vkrpc::CreateImageViewRequest ivr;
    ivr.image = img0;
    ivr.view_type = 1;
    ivr.format = kFormat;
    ivr.aspect = 1;
    ivr.level_count = 1;
    ivr.layer_count = 1;
    const std::uint64_t view = backend.create_image_view(ivr).image_view;
    VKR_CHECK(view != 0);

    auto make_sm = [&]() {
        vkrpc::CreateShaderModuleRequest r;
        r.device = device;
        r.code = std::string(8, '\0');
        r.code_size = 8;
        return backend.create_shader_module(r).shader_module;
    };
    const std::uint64_t vs = make_sm();
    const std::uint64_t fs = make_sm();
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = device;
    plr.set_layout_count = 0;
    plr.push_constant_range_count = 0;
    const std::uint64_t layout = backend.create_pipeline_layout(plr).pipeline_layout;
    VKR_CHECK(layout != 0);

    // A dynamic-rendering pipeline: render_pass == 0, has_dynamic_rendering, one color format.
    auto make_dr_gp = [&]() {
        vkrpc::CreateGraphicsPipelinesRequest r;
        r.device = device;
        vkrpc::ShaderStageDesc s0;
        s0.stage = 1;
        s0.module = vs;
        s0.entry = "main";
        vkrpc::ShaderStageDesc s1;
        s1.stage = 16;
        s1.module = fs;
        s1.entry = "main";
        r.stages = {s0, s1};
        r.topology = 3;
        r.vertex_binding_count = 0;
        r.vertex_attribute_count = 0;
        r.cull_mode = 2;
        r.front_face = 1;
        r.dynamic_states = {0, 1};
        r.layout = layout;
        r.render_pass = 0;
        r.subpass = 0;
        r.has_dynamic_rendering = 1;
        r.dr_color_formats = {kFormat};
        return r;
    };
    const vkrpc::CreateGraphicsPipelinesResponse dr_pipe =
        backend.create_graphics_pipelines(make_dr_gp());
    VKR_CHECK(dr_pipe.ok && dr_pipe.pipeline != 0);

    // Pipeline contract rejections.
    {
        auto r = make_dr_gp(); // (a) DR info with a non-zero render pass -> reject (no mixed mode)
        r.render_pass = 999;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        auto r = make_dr_gp(); // (b) a color-blend count that disagrees with the format count
        vkrpc::ColorBlendAttachmentDesc b0;
        vkrpc::ColorBlendAttachmentDesc b1;
        r.color_blend_attachments = {b0, b1}; // 2 blends vs 1 color format
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        // (c) Required-feature audit: a DR pipeline viewMask is GATED on the multiview
        // feature. `device` never enabled it, so a viewMask is fail-closed here; a negative mask is
        // malformed. The multiview-enabled positive path is proven in the dedicated block below.
        auto r = make_dr_gp();
        r.dr_view_mask = 1;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok); // no multiview feature -> reject
        r.dr_view_mask = -1;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok); // negative -> malformed
    }
    {
        // (d) render_pass == 0 WITHOUT DR info is a blind NULL-renderpass pipeline -> reject.
        auto r = make_dr_gp();
        r.has_dynamic_rendering = 0;
        r.dr_color_formats.clear();
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        // (e) a DR pipeline on a device that did NOT enable VK_KHR_dynamic_rendering -> reject.
        vkrpc::CreateDeviceRequest bare_cdr;
        bare_cdr.instance = ci.instance;
        bare_cdr.physical_device = phys;
        const std::uint64_t bare = backend.create_device(bare_cdr).device;
        auto r = make_dr_gp();
        r.device = bare;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }
    {
        // (f) EXTENSION enabled but the dynamicRendering FEATURE NOT enabled ->
        // reject BOTH a DR pipeline AND begin_rendering. Enabling the ext alone is insufficient
        // (Vulkan forbids using DR through it), so the oracle refuses both.
        vkrpc::CreateDeviceRequest ext_only;
        ext_only.instance = ci.instance;
        ext_only.physical_device = phys;
        ext_only.enabled_extensions = {"VK_KHR_dynamic_rendering"}; // feature bit left 0
        const std::uint64_t ext_only_dev = backend.create_device(ext_only).device;
        auto r = make_dr_gp();
        r.device = ext_only_dev;
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
        vkrpc::CreateCommandPoolRequest ecpr;
        ecpr.device = ext_only_dev;
        const std::uint64_t epool = backend.create_command_pool(ecpr).command_pool;
        vkrpc::AllocateCommandBuffersRequest eacb;
        eacb.command_pool = epool;
        eacb.count = 1;
        vkrpc::RecordCommandBufferRequest erec;
        erec.command_buffer = backend.allocate_command_buffers(eacb).command_buffers.front();
        vkrpc::RecordedCommand
            bcmd; // 0-attachment begin_rendering; the feature gate is checked first
        bcmd.kind = "begin_rendering";
        bcmd.args_i64 = {0, 0, 0, 256, 256, 1, 0, 0, 0};
        erec.commands = {bcmd};
        VKR_CHECK(!backend.record_command_buffer(erec).ok);
    }
    {
        // (g) a DR pipeline color-format count over the cap is rejected in the
        // mock too (mock == real -- the real backend already caps it).
        auto r = make_dr_gp();
        r.dr_color_formats.assign(
            static_cast<std::size_t>(vkrpc::kMaxDynamicRenderingColorAttachments) + 1, kFormat);
        VKR_CHECK(!backend.create_graphics_pipelines(r).ok);
    }

    // Command builders.
    auto begin_rendering = [&](const std::vector<std::uint64_t>& views, long long flags,
                               long long view_mask, long long layers) {
        vkrpc::RecordedCommand c;
        c.kind = "begin_rendering";
        c.args_i64 = {flags, 0,      0,         256,
                      256,   layers, view_mask, static_cast<long long>(views.size()),
                      0 /*no depth/stencil*/};
        for (const std::uint64_t v : views) {
            c.args_i64.push_back(2); // imageLayout
            c.args_i64.push_back(1); // loadOp CLEAR
            c.args_i64.push_back(0); // storeOp STORE
            c.args_u64.push_back(v);
            c.args_blob.append(vkrpc::kClearValueBytes, '\0'); // one VkClearValue
        }
        return c;
    };
    auto simple = [&](const char* kind) {
        vkrpc::RecordedCommand c;
        c.kind = kind;
        return c;
    };
    auto bind = [&](std::uint64_t pipe) {
        vkrpc::RecordedCommand c;
        c.kind = "bind_pipeline";
        c.pipeline = pipe;
        c.args_i64 = {0}; // GRAPHICS
        return c;
    };
    auto viewport = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_viewport";
        c.vp_w = 256.0;
        c.vp_h = 256.0;
        return c;
    };
    auto scissor = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "set_scissor";
        c.sc_w = 256;
        c.sc_h = 256;
        return c;
    };
    auto draw = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "draw";
        c.vertex_count = 3;
        c.instance_count = 1;
        c.first_vertex = 0;
        c.first_instance = 0;
        return c;
    };
    auto record = [&](std::vector<vkrpc::RecordedCommand> cmds) {
        vkrpc::RecordCommandBufferRequest rec;
        rec.command_buffer = alloc_cmd();
        rec.commands = std::move(cmds);
        return backend.record_command_buffer(rec).ok;
    };

    // The happy path: begin_rendering over the color view + bind the DR pipeline + dynamic
    // viewport/scissor + draw + end_rendering. The pipeline's declared format matches the view's,
    // so the draw-time DR compatibility check passes.
    VKR_CHECK(record({begin_rendering({view}, 0, 0, 1), bind(dr_pipe.pipeline), viewport(),
                      scissor(), draw(), simple("end_rendering")}));

    // Required-feature audit: the DR viewMask POSITIVE path on a device that enabled
    // BOTH dynamicRendering AND multiview. The pipeline accepts a viewMask, and a begin_rendering
    // carrying it is admitted when the attachment image covers the required layers (0x3 -> 2) and
    // rejected when it does not -- the same layer-sufficiency rule as the RP2 framebuffer path.
    {
        constexpr int kAppColor =
            37; // VK_FORMAT_R8G8B8A8_UNORM (the mock's app-image color format)
        vkrpc::CreateDeviceRequest mvcdr;
        mvcdr.instance = ci.instance;
        mvcdr.physical_device = phys;
        mvcdr.enabled_extensions = {"VK_KHR_dynamic_rendering"};
        mvcdr.dynamic_rendering_feature_enabled = 1;
        mvcdr.multiview_feature_enabled = 1;
        const std::uint64_t mv_dev = backend.create_device(mvcdr).device;
        VKR_CHECK(mv_dev != 0);
        vkrpc::CreateCommandPoolRequest mvcpr;
        mvcpr.device = mv_dev;
        const std::uint64_t mv_pool = backend.create_command_pool(mvcpr).command_pool;
        auto mv_sm = [&]() {
            vkrpc::CreateShaderModuleRequest r;
            r.device = mv_dev;
            r.code = std::string(8, '\0');
            r.code_size = 8;
            return backend.create_shader_module(r).shader_module;
        };
        const std::uint64_t mv_vs = mv_sm();
        const std::uint64_t mv_fs = mv_sm();
        vkrpc::CreatePipelineLayoutRequest mvplr;
        mvplr.device = mv_dev;
        mvplr.set_layout_count = 0;
        mvplr.push_constant_range_count = 0;
        const std::uint64_t mv_layout = backend.create_pipeline_layout(mvplr).pipeline_layout;
        auto make_mv_dr_gp = [&](int vmask) {
            vkrpc::CreateGraphicsPipelinesRequest r;
            vkrpc::ShaderStageDesc s0;
            s0.stage = 1;
            s0.module = mv_vs;
            s0.entry = "main";
            vkrpc::ShaderStageDesc s1;
            s1.stage = 16;
            s1.module = mv_fs;
            s1.entry = "main";
            r.device = mv_dev;
            r.stages = {s0, s1};
            r.topology = 3;
            r.vertex_binding_count = 0;
            r.vertex_attribute_count = 0;
            r.cull_mode = 2;
            r.front_face = 1;
            r.dynamic_states = {0, 1};
            r.layout = mv_layout;
            r.render_pass = 0;
            r.subpass = 0;
            r.has_dynamic_rendering = 1;
            r.dr_view_mask = vmask;
            r.dr_color_formats = {kAppColor};
            return r;
        };
        const vkrpc::CreateGraphicsPipelinesResponse mv_pipe =
            backend.create_graphics_pipelines(make_mv_dr_gp(0x3));
        VKR_CHECK(mv_pipe.ok && mv_pipe.pipeline != 0); // viewMask admitted with the feature

        // Depth-stencil state vs declared formats follows the SPEC exactly (Mesa >= 25 zink
        // attaches a DISABLED depth-stencil struct to color-only DR pipelines): carried state
        // WITHOUT a declared depth/stencil format is legal and IGNORED...
        {
            vkrpc::CreateGraphicsPipelinesRequest r = make_mv_dr_gp(0);
            r.has_depth_stencil = true; // struct present, tests disabled (the zink-25 shape)
            r.depth_test_enable = 0;
            r.depth_write_enable = 0;
            r.depth_compare_op = 3; // LESS_OR_EQUAL (the mock's admitted compare-op subset)
            const vkrpc::CreateGraphicsPipelinesResponse ok_resp =
                backend.create_graphics_pipelines(r);
            VKR_CHECK(ok_resp.ok && ok_resp.pipeline != 0);
        }
        // ...while a DECLARED depth/stencil format still REQUIRES the state (the fail-closed
        // direction is kept).
        {
            vkrpc::CreateGraphicsPipelinesRequest r = make_mv_dr_gp(0);
            r.dr_depth_format = 126; // VK_FORMAT_D32_SFLOAT
            r.has_depth_stencil = false;
            const vkrpc::CreateGraphicsPipelinesResponse rej = backend.create_graphics_pipelines(r);
            VKR_CHECK(!rej.ok);
            VKR_CHECK(rej.reason.find("declares a depth/stencil format but carries no "
                                      "depth-stencil state") != std::string::npos);
        }

        // Two app color views: a 2-layer one (suffices for 0x3) and a 1-layer one (does not).
        auto mv_view = [&](int layers) {
            vkrpc::CreateImageRequest cir;
            cir.device = mv_dev;
            cir.image_type = vkrpc::kImageType2D;
            cir.format = kAppColor;
            cir.width = 64;
            cir.height = 64;
            cir.depth = 1;
            cir.mip_levels = 1;
            cir.array_layers = layers;
            cir.samples = 1;
            cir.tiling = vkrpc::kImageTilingOptimal;
            cir.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageColorAttachment);
            cir.sharing_mode = 0;
            cir.initial_layout = 0;
            const std::uint64_t im = backend.create_image(cir).image;
            vkrpc::AllocateMemoryRequest am;
            am.device = mv_dev;
            am.allocation_size = 4ull * 1024 * 1024;
            am.memory_type_index = 0; // DEVICE_LOCAL
            const std::uint64_t mem = backend.allocate_memory(am).memory;
            VKR_CHECK(backend.bind_image_memory({im, mem, 0}).ok);
            vkrpc::CreateImageViewRequest vr;
            vr.image = im;
            vr.view_type = 1;
            vr.format = kAppColor;
            vr.aspect = vkrpc::kImageAspectColor;
            vr.level_count = 1;
            vr.layer_count = layers;
            return backend.create_image_view(vr).image_view;
        };
        const std::uint64_t mv_view2 = mv_view(2);
        const std::uint64_t mv_view1 = mv_view(1);
        auto mv_record = [&](std::vector<vkrpc::RecordedCommand> cmds) {
            vkrpc::AllocateCommandBuffersRequest acb;
            acb.command_pool = mv_pool;
            acb.count = 1;
            vkrpc::RecordCommandBufferRequest rec;
            rec.command_buffer = backend.allocate_command_buffers(acb).command_buffers.front();
            rec.commands = std::move(cmds);
            return backend.record_command_buffer(rec).ok;
        };
        // 2-layer view + viewMask 0x3 + a matching-viewMask pipeline: the full multiview DR draw.
        VKR_CHECK(mv_record({begin_rendering({mv_view2}, 0, 0x3, 1), bind(mv_pipe.pipeline),
                             viewport(), scissor(), draw(), simple("end_rendering")}));
        // 1-layer view + viewMask 0x3: too few array layers -> fail-closed at begin.
        VKR_CHECK(!mv_record({begin_rendering({mv_view1}, 0, 0x3, 1), simple("end_rendering")}));
    }

    // Fail-closed envelope (mock == real): non-zero flags, multiview, non-positive layerCount.
    VKR_CHECK(!record(
        {begin_rendering({view}, 1 /*a suspend/resume flag*/, 0, 1), simple("end_rendering")}));
    // a begin_rendering viewMask is GATED on the multiview feature -- `device` never
    // enabled it, so it is fail-closed (the multiview-enabled positive path is the dedicated block
    // below + the real serve-proof canary).
    VKR_CHECK(!record({begin_rendering({view}, 0, 1 /*viewMask*/, 1), simple("end_rendering")}));
    VKR_CHECK(!record({begin_rendering({view}, 0, 0, 0 /*layerCount*/), simple("end_rendering")}));
    {
        // A malformed attachment payload (u64 handle count != colorCount) is fail-closed.
        vkrpc::RecordedCommand bad = begin_rendering({view}, 0, 0, 1);
        bad.args_u64.clear(); // header still says colorCount 1
        VKR_CHECK(!record({bad, simple("end_rendering")}));
    }

    // Unified render-scope state machine.
    VKR_CHECK(!record({draw()}));                           // draw with no active scope
    VKR_CHECK(!record({simple("end_rendering")}));          // end_rendering with no scope
    VKR_CHECK(!record({begin_rendering({view}, 0, 0, 1)})); // stream ends inside DR scope
    VKR_CHECK(!record({begin_rendering({view}, 0, 0, 1),    //
                       simple("end_render_pass")}));        // wrong `end` kind for the scope
    VKR_CHECK(!record({begin_rendering({view}, 0, 0, 1),    //
                       begin_rendering({view}, 0, 0, 1)})); // begin inside a scope

    // Draw-time DR compatibility: a pipeline whose declared color format differs from the active
    // scope's attachment format is rejected. Build a second view with a DIFFERENT
    // format and begin over it; the kFormat pipeline no longer matches.
    {
        vkrpc::CreateImageViewRequest other = ivr;
        other.format = kFormat + 1;
        const std::uint64_t other_view = backend.create_image_view(other).image_view;
        VKR_CHECK(!record({begin_rendering({other_view}, 0, 0, 1), bind(dr_pipe.pipeline),
                           viewport(), scissor(), draw(), simple("end_rendering")}));
    }

    // A null color attachment (imageView == 0) is positional and NOT looked up: begin+end records
    // OK (a null handle is a real "no attachment", never resolved).
    VKR_CHECK(record({begin_rendering({0}, 0, 0, 1), simple("end_rendering")}));

    // Transform feedback must not leak across a DR scope close. A minimal
    // begin_transform_feedback (count 0) inside the DR scope sets XFB active; end_rendering must
    // then reject (mirroring the render-pass guard), so an invalid host stream never replays.
    auto begin_xfb = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "begin_transform_feedback";
        c.args_i64 = {0, 0, 0}; // firstCounterBuffer 0, counterBufferCount 0, no offsets
        return c;
    };
    VKR_CHECK(!record({begin_rendering({view}, 0, 0, 1), begin_xfb(), simple("end_rendering")}));
    // For contrast, the same XFB scope closed correctly (end_transform_feedback then end_rendering)
    // records fine -- proving it is the UNBALANCED case the guard rejects, not XFB-in-DR per se.
    auto end_xfb = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "end_transform_feedback";
        c.args_i64 = {0, 0, 0};
        return c;
    };
    VKR_CHECK(record(
        {begin_rendering({view}, 0, 0, 1), begin_xfb(), end_xfb(), simple("end_rendering")}));
}

// staging path records UNDEFINED->TRANSFER_DST, copy, TRANSFER_DST->SHADER_READ_ONLY, then submits;
// the out-of-subset transitions are rejected; destroying the staging buffer invalidates the CB.
void test_texture_upload_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    const std::uint64_t dev = cd.device;
    vkrpc::GetDeviceQueueRequest gq;
    gq.device = dev;
    gq.queue_family_index = cd.queue_family_index;
    gq.queue_index = 0;
    const std::uint64_t queue = backend.get_device_queue(gq).queue;
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = dev;
    cpr.queue_family_index = cd.queue_family_index;
    const std::uint64_t pool_cmd = backend.create_command_pool(cpr).command_pool;
    auto alloc_cmd = [&]() {
        vkrpc::AllocateCommandBuffersRequest acb;
        acb.command_pool = pool_cmd;
        acb.count = 1;
        return backend.allocate_command_buffers(acb).command_buffers.front();
    };

    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
    mpq.physical_device = phys;
    const auto mp = backend.get_physical_device_memory_properties(mpq);
    int coherent_type = -1;
    int device_local_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
        } else if ((mp.types[i].property_flags & vkrpc::kMemoryPropertyDeviceLocal) != 0) {
            device_local_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(coherent_type >= 0 && device_local_type >= 0);

    // A TRANSFER_SRC staging buffer (host-visible coherent) bound to memory.
    vkrpc::AllocateMemoryRequest amu;
    amu.device = dev;
    amu.allocation_size = 64 * 1024;
    amu.memory_type_index = coherent_type;
    const std::uint64_t smem = backend.allocate_memory(amu).memory;
    vkrpc::CreateBufferRequest sbq;
    sbq.device = dev;
    sbq.size = 16 * 16 * 4;
    sbq.usage = vkrpc::kBufferUsageTransferSrc;
    sbq.sharing_mode = 0;
    const vkrpc::CreateBufferResponse staging = backend.create_buffer(sbq);
    VKR_CHECK(staging.ok);
    const std::uint64_t sbuf = staging.buffer;
    VKR_CHECK(backend.bind_buffer_memory({sbuf, smem, 0}).ok);

    // A SAMPLED | TRANSFER_DST OPTIMAL texture image bound to device-local memory.
    vkrpc::CreateImageRequest ir;
    ir.device = dev;
    ir.image_type = vkrpc::kImageType2D;
    ir.format = vkrpc::kFormatR8G8B8A8Unorm;
    ir.width = 16;
    ir.height = 16;
    ir.depth = 1;
    ir.mip_levels = 3; // 16 / 8 / 4 -- the widened upload validates mip bounds against these
    ir.array_layers = 1;
    ir.samples = 1;
    ir.tiling = vkrpc::kImageTilingOptimal;
    ir.usage =
        vkrpc::kImageUsageSampled | static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst);
    ir.sharing_mode = 0;
    ir.initial_layout = 0;
    const vkrpc::CreateImageResponse img = backend.create_image(ir);
    VKR_CHECK(img.ok);
    vkrpc::AllocateMemoryRequest amt;
    amt.device = dev;
    amt.allocation_size = img.mem_size;
    amt.memory_type_index = device_local_type;
    const std::uint64_t tmem = backend.allocate_memory(amt).memory;
    VKR_CHECK(backend.bind_image_memory({img.image, tmem, 0}).ok);

    auto to_dst = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "pipeline_barrier";
        c.image = img.image;
        c.old_layout = vkrpc::kImageLayoutUndefinedC3;
        c.new_layout = vkrpc::kImageLayoutTransferDstOptimal;
        c.aspect = vkrpc::kImageAspectColor;
        c.src_stage = 0x1;    // TOP_OF_PIPE
        c.dst_stage = 0x1000; // TRANSFER
        c.src_access = 0;
        c.dst_access = 0x1000; // TRANSFER_WRITE
        c.barrier_base_mip = 0;
        c.barrier_level_count = 1;
        c.barrier_base_layer = 0;
        c.barrier_layer_count = 1;
        return c;
    };
    // The widened faithful upload payload (the copy_image_to_buffer 13-per-region convention):
    // args_i64=[dstImageLayout, regionCount, per region: bufferOffset, rowLength, imageHeight,
    // aspect, mip, baseLayer, layerCount, offX, offY, offZ, extW, extH, extD].
    auto copy_region = [](long long mip, long long off_x, long long off_y, long long ext_w,
                          long long ext_h) {
        return std::vector<long long>{
            0, 0, 0, vkrpc::kImageAspectColor, mip, 0, 1, off_x, off_y, 0, ext_w, ext_h, 1};
    };
    auto copy_of = [&](const std::vector<std::vector<long long>>& regions) {
        vkrpc::RecordedCommand c;
        c.kind = "copy_buffer_to_image";
        c.src_buffer = sbuf;
        c.image = img.image;
        c.args_i64 = {vkrpc::kImageLayoutTransferDstOptimal,
                      static_cast<long long>(regions.size())};
        for (const auto& r : regions) {
            c.args_i64.insert(c.args_i64.end(), r.begin(), r.end());
        }
        return c;
    };
    auto copy = [&]() { return copy_of({copy_region(0, 0, 0, 16, 16)}); };
    auto to_shader = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "pipeline_barrier";
        c.image = img.image;
        c.old_layout = vkrpc::kImageLayoutTransferDstOptimal;
        c.new_layout = vkrpc::kImageLayoutShaderReadOnlyOptimal;
        c.aspect = vkrpc::kImageAspectColor;
        c.src_stage = 0x1000; // TRANSFER
        c.dst_stage = 0x80;   // FRAGMENT_SHADER
        c.src_access = 0x1000;
        c.dst_access = 0x20; // SHADER_READ
        c.barrier_base_mip = 0;
        c.barrier_level_count = 1;
        c.barrier_base_layer = 0;
        c.barrier_layer_count = 1;
        return c;
    };
    // The full staging upload records + submits.
    const std::uint64_t cmd = alloc_cmd();
    vkrpc::RecordCommandBufferRequest rc;
    rc.command_buffer = cmd;
    rc.commands = {to_dst(), copy(), to_shader()};
    VKR_CHECK(backend.record_command_buffer(rc).ok);
    vkrpc::QueueSubmitRequest qs;
    qs.queue = queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok);

    // --- Depth/stencil upload (the copy_buffer_to_image depth/stencil widening) -----------------
    // The mock now admits a depth/stencil image as a TRANSFER_DST copy target and enforces the same
    // aspect-subset rule as the real worker: a region's aspect must be a SINGLE bit that the image
    // actually has. depth-only (D32_SFLOAT) takes DEPTH only; a combined (D24_UNORM_S8_UINT) image
    // takes DEPTH or STENCIL (one per region, never both). Aspect mismatches fail closed.
    {
        auto make_ds_image = [&](int format) {
            vkrpc::CreateImageRequest dir;
            dir.device = dev;
            dir.image_type = vkrpc::kImageType2D;
            dir.format = format;
            dir.width = 16;
            dir.height = 16;
            dir.depth = 1;
            dir.mip_levels = 1;
            dir.array_layers = 1;
            dir.samples = 1;
            dir.tiling = vkrpc::kImageTilingOptimal;
            dir.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst);
            dir.sharing_mode = 0;
            dir.initial_layout = 0;
            const vkrpc::CreateImageResponse dimg = backend.create_image(dir);
            VKR_CHECK(dimg.ok);
            vkrpc::AllocateMemoryRequest amd;
            amd.device = dev;
            amd.allocation_size = dimg.mem_size;
            amd.memory_type_index = device_local_type;
            const std::uint64_t dmem = backend.allocate_memory(amd).memory;
            VKR_CHECK(backend.bind_image_memory({dimg.image, dmem, 0}).ok);
            return dimg.image;
        };
        auto record_copy = [&](std::uint64_t image, int aspect) {
            vkrpc::RecordedCommand c;
            c.kind = "copy_buffer_to_image";
            c.src_buffer = sbuf;
            c.image = image;
            c.args_i64 = {vkrpc::kImageLayoutTransferDstOptimal,
                          1,
                          0,
                          0,
                          0,
                          aspect,
                          0,
                          0,
                          1,
                          0,
                          0,
                          0,
                          16,
                          16,
                          1};
            vkrpc::RecordCommandBufferRequest r;
            r.command_buffer = alloc_cmd();
            r.commands = {c};
            return backend.record_command_buffer(r);
        };
        // Depth-only image: DEPTH accepted; COLOR/STENCIL/both fail closed.
        const std::uint64_t d32 = make_ds_image(vkrpc::kFormatD32Sfloat);
        VKR_CHECK(record_copy(d32, vkrpc::kImageAspectDepth).ok);
        VKR_CHECK(!record_copy(d32, vkrpc::kImageAspectColor).ok);   // aspect not in the image
        VKR_CHECK(!record_copy(d32, vkrpc::kImageAspectStencil).ok); // D32 has no stencil
        VKR_CHECK(!record_copy(d32, vkrpc::kImageAspectDepth | vkrpc::kImageAspectStencil)
                       .ok); // not single
        // Combined depth+stencil image: DEPTH or STENCIL each accepted; both-in-one still rejected.
        const std::uint64_t d24s8 = make_ds_image(vkrpc::kFormatD24UnormS8Uint);
        VKR_CHECK(record_copy(d24s8, vkrpc::kImageAspectDepth).ok);
        VKR_CHECK(record_copy(d24s8, vkrpc::kImageAspectStencil).ok);
        VKR_CHECK(!record_copy(d24s8, vkrpc::kImageAspectColor).ok);
        VKR_CHECK(!record_copy(d24s8, vkrpc::kImageAspectDepth | vkrpc::kImageAspectStencil).ok);
        // Mock == its own format-properties: the mock advertises TRANSFER_SRC on the
        // depth formats, so creating a DS image with TRANSFER_DST|TRANSFER_SRC (the upload+readback
        // round-trip shape the real integration test uses) must SUCCEED, not be rejected.
        {
            vkrpc::CreateImageRequest sd;
            sd.device = dev;
            sd.image_type = vkrpc::kImageType2D;
            sd.format = vkrpc::kFormatD32Sfloat;
            sd.width = 16;
            sd.height = 16;
            sd.depth = 1;
            sd.mip_levels = 1;
            sd.array_layers = 1;
            sd.samples = 1;
            sd.tiling = vkrpc::kImageTilingOptimal;
            sd.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageTransferDst) |
                       static_cast<std::uint64_t>(vkrpc::kImageUsageTransferSrc);
            sd.sharing_mode = 0;
            sd.initial_layout = 0;
            VKR_CHECK(backend.create_image(sd).ok);
        }
    }

    // Out-of-subset edges (each on a fresh command buffer):
    auto record_one = [&](const vkrpc::RecordedCommand& c) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = alloc_cmd();
        r.commands = {c};
        return backend.record_command_buffer(r);
    };
    {
        // (GL/zink): barriers are now forwarded FAITHFULLY (the texture-upload allowlist +
        // fixed single-subresource shape are lifted; the host driver is the authority). Transitions
        // zink emits that the old allowlist rejected -- an arbitrary layout transition, a
        // multi-level range, a whole-image (REMAINING) range -- are accepted.
        auto t1 = to_dst();
        t1.new_layout = vkrpc::kImageLayoutShaderReadOnlyOptimal;
        VKR_CHECK(record_one(t1).ok);
        auto multi_level = to_dst();
        multi_level.barrier_level_count = 2;
        VKR_CHECK(record_one(multi_level).ok);
        auto whole_image = to_dst(); // all -1 -> REMAINING (whole image), a normal barrier shape
        whole_image.barrier_base_mip = -1;
        whole_image.barrier_level_count = -1;
        whole_image.barrier_base_layer = -1;
        whole_image.barrier_layer_count = -1;
        VKR_CHECK(record_one(whole_image).ok);
        // The widened upload (the ExtremeTuxRacer fix): sub-region, mip-level and multi-region
        // copies are ACCEPTED (the old subset required exactly one full-image mip-0 region)...
        VKR_CHECK(record_one(copy_of({copy_region(0, 4, 4, 8, 8)})).ok); // glTexSubImage2D shape
        VKR_CHECK(record_one(copy_of({copy_region(2, 0, 0, 4, 4)})).ok); // top mip (16>>2 = 4)
        VKR_CHECK(record_one(copy_of({copy_region(0, 0, 0, 16, 16),      // batched regions
                                      copy_region(1, 2, 2, 4, 4)}))
                      .ok);
        // ...while out-of-bounds/malformed shapes stay fail-closed, validated against the image:
        VKR_CHECK(!record_one(copy_of({copy_region(3, 0, 0, 1, 1)})).ok);   // mip out of range
        VKR_CHECK(!record_one(copy_of({copy_region(0, 12, 12, 8, 8)})).ok); // offset+extent OOB
        VKR_CHECK(!record_one(copy_of({copy_region(1, 4, 0, 8, 8)})).ok);   // OOB at the MIP size
        {
            auto bad = copy_of({copy_region(0, 0, 0, 16, 16)});
            bad.args_i64[0] = vkrpc::kImageLayoutShaderReadOnlyOptimal; // illegal dst layout
            VKR_CHECK(!record_one(bad).ok);
            bad = copy_of({copy_region(0, 0, 0, 16, 16)});
            bad.args_i64.pop_back(); // truncated region payload
            VKR_CHECK(!record_one(bad).ok);
            bad = copy_of({copy_region(0, 0, 0, 16, 16)});
            bad.args_i64[2] = 64 * 1024; // bufferOffset beyond the staging buffer
            VKR_CHECK(!record_one(bad).ok);
            bad = copy_of({copy_region(0, 0, 0, 16, 16)});
            bad.args_i64[3] = 8; // bufferRowLength < extent width (spec VU)
            VKR_CHECK(!record_one(bad).ok);
            bad = copy_of({copy_region(0, 0, 0, 16, 16)});
            bad.args_i64[7] = 2; // layer range beyond the single-layer image
            VKR_CHECK(!record_one(bad).ok);
        }
        // A copy from a non-TRANSFER_SRC buffer is rejected (make a UNIFORM buffer).
        vkrpc::CreateBufferRequest ubq;
        ubq.device = dev;
        ubq.size = 256;
        ubq.usage = vkrpc::kBufferUsageUniformBuffer;
        ubq.sharing_mode = 0;
        const std::uint64_t ub = backend.create_buffer(ubq).buffer;
        VKR_CHECK(backend.bind_buffer_memory({ub, smem, 1024}).ok);
        auto bad_src = copy();
        bad_src.src_buffer = ub;
        VKR_CHECK(!record_one(bad_src).ok);
        // An UNBOUND app image cannot be the target of a barrier or a copy (mirrors
        // the image-view guard -- a command against an unbound VkImage is a fail-closed boundary
        // violation). Build a SAMPLED|TRANSFER_DST image but do NOT bind it.
        vkrpc::CreateImageRequest uir = ir;
        const vkrpc::CreateImageResponse unbound = backend.create_image(uir);
        VKR_CHECK(unbound.ok);
        auto bad_barrier = to_dst();
        bad_barrier.image = unbound.image;
        VKR_CHECK(!record_one(bad_barrier).ok);
        auto bad_copy_dst = copy();
        bad_copy_dst.image = unbound.image;
        VKR_CHECK(!record_one(bad_copy_dst).ok);
    }

    // Destroying the staging buffer invalidates the recorded upload CB (recorded-resource
    // invalidation: a baked copy references the staging buffer).
    VKR_CHECK(backend.queue_submit(qs).ok); // still valid
    VKR_CHECK(backend.destroy_buffer({sbuf}).ok);
    VKR_CHECK(!backend.queue_submit(qs).ok); // staging-buffer destroy invalidated the upload CB
}

} // namespace

// born-correlated surface<->XID. The CreateSurfaceRequest topology fields round-trip
// (with legacy back-compat), and create_surface carrying a guest XID pends a worker-home registry
// entry keyed by that XID that destroy_surface drops -- the order-independence seam builds on.
// Driven on the mock so it pins the behavior dual-platform (mock == real: both backends share the
// WindowRegistry logic).
void test_surface_xid_registry() {
    // Wire round-trip of the topology fields.
    vkrpc::CreateSurfaceRequest wire;
    wire.instance = 7;
    wire.platform = "xcb";
    wire.xid = 0xABCD;
    const vkrpc::CreateSurfaceRequest wb = vkrpc::CreateSurfaceRequest::from_body(wire.to_body());
    VKR_CHECK_EQ(wb.platform, std::string("xcb"));
    VKR_CHECK_EQ(wb.xid, static_cast<std::uint64_t>(0xABCD));
    VKR_CHECK_EQ(wb.role_hint, std::string("UnknownPending"));
    // A legacy body (no topology) decodes to no platform/xid + the default advisory role_hint.
    const vkrpc::CreateSurfaceRequest lb =
        vkrpc::CreateSurfaceRequest::from_body(json::Value::make_object());
    VKR_CHECK(lb.platform.empty());
    VKR_CHECK_EQ(lb.xid, static_cast<std::uint64_t>(0));
    VKR_CHECK_EQ(lb.role_hint, std::string("UnknownPending"));

    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    VKR_CHECK(ci.ok);

    // A surface carrying a guest XID pends a registry entry keyed by that XID.
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    sr.platform = "xcb";
    sr.xid = 0x1234;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok && surf.surface != 0);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x1234), surf.surface);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x9999), static_cast<std::uint64_t>(0));

    // A surface with no XID (legacy/untopologied) pends nothing.
    vkrpc::CreateSurfaceRequest sr0;
    sr0.instance = ci.instance;
    const vkrpc::CreateSurfaceResponse surf0 = backend.create_surface(sr0);
    VKR_CHECK(surf0.ok);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0), static_cast<std::uint64_t>(0));

    // Destroying the correlated surface drops its registry entry.
    vkrpc::HandleRequest dh;
    dh.handle = surf.surface;
    VKR_CHECK(backend.destroy_surface(dh).ok);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x1234), static_cast<std::uint64_t>(0));

    // Two surfaces for the SAME guest window (a rebind): destroying the OLDER surface must NOT
    // erase the newer/active binding -- only destroying the bound surface clears it
    // (surface-specific unbind).
    vkrpc::CreateSurfaceRequest sa;
    sa.instance = ci.instance;
    sa.platform = "xcb";
    sa.xid = 0x5678;
    const std::uint64_t s1 = backend.create_surface(sa).surface;
    const std::uint64_t s2 = backend.create_surface(sa).surface; // rebinds 0x5678 -> s2
    VKR_CHECK(s1 != 0 && s2 != 0 && s1 != s2);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x5678), s2);
    vkrpc::HandleRequest d1;
    d1.handle = s1;
    VKR_CHECK(backend.destroy_surface(d1).ok);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x5678),
                 s2); // older destroy left s2 intact
    vkrpc::HandleRequest d2;
    d2.handle = s2;
    VKR_CHECK(backend.destroy_surface(d2).ok);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x5678), static_cast<std::uint64_t>(0));
}

// the worker-home toplevel registry driven through the mock backend's sidecar ops +
// create/destroy_surface, asserting the structural invariants AND that the mock's executor
// (placeholder-id set) stays in lockstep with the registry's placeholder_count -- the same property
// the real backend's HWND executor must hold. Mock == real on the state machine.
void test_mock_toplevel_registry_i9() {
    using vkr::sidecar::Representation;
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    VKR_CHECK(ci.ok);

    // Permutation 2: register_toplevel first -> one placeholder; create_surface PROMOTES it.
    vkr::sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x100;
    reg.generation = 1;
    reg.role = "toplevel";
    reg.width = 640;
    reg.height = 480;
    const vkr::sidecar::SidecarToplevelResponse rr = backend.register_toplevel(reg);
    VKR_CHECK(rr.ok && rr.applied);
    VKR_CHECK_EQ(rr.representation, std::string("placeholder"));
    VKR_CHECK(backend.debug_representation_for_xid(0x100) == Representation::Placeholder);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(backend.debug_executor_placeholder_count(), static_cast<std::size_t>(1));

    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    sr.platform = "xcb";
    sr.xid = 0x100;
    const vkrpc::CreateSurfaceResponse surf = backend.create_surface(sr);
    VKR_CHECK(surf.ok && surf.surface != 0);
    VKR_CHECK(backend.debug_representation_for_xid(0x100) == Representation::Surface);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x100), surf.surface);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(0));
    VKR_CHECK_EQ(backend.debug_executor_placeholder_count(), static_cast<std::size_t>(0));

    // Two toplevels -> two placeholders; unregister one -> only its representation goes (the
    // sibling is untouched -- isolation).
    vkr::sidecar::SidecarRegisterToplevelRequest a;
    a.xid = 0x200;
    a.generation = 1;
    a.width = 300;
    a.height = 200;
    vkr::sidecar::SidecarRegisterToplevelRequest b;
    b.xid = 0x300;
    b.generation = 1;
    b.width = 300;
    b.height = 200;
    VKR_CHECK(backend.register_toplevel(a).applied);
    VKR_CHECK(backend.register_toplevel(b).applied);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(backend.debug_executor_placeholder_count(), static_cast<std::size_t>(2));
    vkr::sidecar::SidecarUnregisterToplevelRequest ua;
    ua.xid = 0x200;
    ua.generation = 2;
    const vkr::sidecar::SidecarToplevelResponse ur = backend.unregister_toplevel(ua);
    VKR_CHECK(ur.ok && ur.applied);
    VKR_CHECK_EQ(ur.representation, std::string("none"));
    VKR_CHECK(backend.debug_representation_for_xid(0x300) == Representation::Placeholder);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(backend.debug_executor_placeholder_count(), static_cast<std::size_t>(1));

    // The FULLSCREEN class (the ExtremeTuxRacer fix): an override-redirect NON-popup registration
    // is a deliberate sidecar classification (a root-covering window, SFML 2.5's non-EWMH
    // fullscreen) and now REGISTERS like any toplevel -- the old refuse-all-non-popup gate
    // stranded it on the create_surface 256x256 default host. Unregistered again so the
    // placeholder counts below stay focused on 0x300.
    vkr::sidecar::SidecarRegisterToplevelRequest fullscreen;
    fullscreen.xid = 0x400;
    fullscreen.generation = 1;
    fullscreen.override_redirect = true; // is_popup stays false
    fullscreen.width = 2560;
    fullscreen.height = 1528;
    const vkr::sidecar::SidecarToplevelResponse pr = backend.register_toplevel(fullscreen);
    VKR_CHECK(pr.ok && pr.applied);
    VKR_CHECK(backend.debug_representation_for_xid(0x400) == Representation::Placeholder);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(2));
    vkr::sidecar::SidecarUnregisterToplevelRequest fs_unr;
    fs_unr.xid = 0x400;
    fs_unr.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(fs_unr).applied);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(1));

    // Permutation 1: surface first, then register_toplevel -> NEVER a placeholder.
    vkrpc::CreateSurfaceRequest sf;
    sf.instance = ci.instance;
    sf.platform = "xcb";
    sf.xid = 0x500;
    const vkrpc::CreateSurfaceResponse surf5 = backend.create_surface(sf);
    VKR_CHECK(surf5.ok);
    VKR_CHECK(backend.debug_representation_for_xid(0x500) == Representation::Surface);
    vkr::sidecar::SidecarRegisterToplevelRequest reg5;
    reg5.xid = 0x500;
    reg5.generation = 1;
    const vkr::sidecar::SidecarToplevelResponse rr5 = backend.register_toplevel(reg5);
    VKR_CHECK(rr5.ok && rr5.applied);
    VKR_CHECK_EQ(rr5.representation, std::string("surface"));
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(1)); // 0x300
    VKR_CHECK_EQ(backend.debug_executor_placeholder_count(), static_cast<std::size_t>(1));

    // Permutation 4: unregister while the surface is live -> keep the surface; destroy_surface
    // reaps.
    vkr::sidecar::SidecarUnregisterToplevelRequest u5;
    u5.xid = 0x500;
    u5.generation = 2;
    const vkr::sidecar::SidecarToplevelResponse ur5 = backend.unregister_toplevel(u5);
    VKR_CHECK(ur5.ok && ur5.applied);
    VKR_CHECK_EQ(ur5.representation, std::string("surface"));
    VKR_CHECK(!backend.debug_toplevel_registered(0x500));
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x500), surf5.surface);
    vkrpc::HandleRequest d5;
    d5.handle = surf5.surface;
    VKR_CHECK(backend.destroy_surface(d5).ok);
    VKR_CHECK_EQ(backend.debug_registry_surface_for_xid(0x500), static_cast<std::uint64_t>(0));

    // A stale/equal generation is dropped (idempotent, ok-but-not-applied): 0x300 survives.
    vkr::sidecar::SidecarUnregisterToplevelRequest stale;
    stale.xid = 0x300;
    stale.generation = 1; // <= the current generation (1)
    const vkr::sidecar::SidecarToplevelResponse sr2 = backend.unregister_toplevel(stale);
    VKR_CHECK(sr2.ok && !sr2.applied);
    VKR_CHECK(backend.debug_representation_for_xid(0x300) == Representation::Placeholder);
    VKR_CHECK_EQ(backend.debug_registry_placeholder_count(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(backend.debug_executor_placeholder_count(), static_cast<std::size_t>(1));
}

// a PaintChrome request filling the whole source with one BGRA color.
vkr::sidecar::SidecarPaintChromeRequest
make_chrome(std::uint64_t xid, std::uint64_t gen, std::uint64_t seq, std::uint32_t w,
            std::uint32_t h, unsigned char b, unsigned char g, unsigned char r, unsigned char a) {
    vkr::sidecar::SidecarPaintChromeRequest req;
    req.xid = xid;
    req.lifecycle_generation = gen;
    req.seq = seq;
    req.src_w = w;
    req.src_h = h;
    req.dirty_w = w;
    req.dirty_h = h;
    req.stride = w * 4;
    req.pixels.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < req.pixels.size(); i += 4) {
        req.pixels[i + 0] = static_cast<char>(b);
        req.pixels[i + 1] = static_cast<char>(g);
        req.pixels[i + 2] = static_cast<char>(r);
        req.pixels[i + 3] = static_cast<char>(a);
    }
    return req;
}

// the chrome paint accept->paint->commit dance + the DebugChromeState pixel-sample proof,
// through the mock backend (the synthetic pixel store stands in for the real DIB). mock == real.
void test_mock_chrome_paint() {
    using vkr::sidecar::Representation;
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    VKR_CHECK(ci.ok);

    // A placeholder toplevel, unpainted/hidden until its first chrome paint.
    vkr::sidecar::SidecarRegisterToplevelRequest reg;
    reg.xid = 0x700;
    reg.generation = 1;
    reg.width = 8;
    reg.height = 4;
    VKR_CHECK(backend.register_toplevel(reg).applied);
    VKR_CHECK(backend.debug_representation_for_xid(0x700) == Representation::Placeholder);
    VKR_CHECK(!backend.debug_placeholder_shown(0x700));

    // First paint: accepted + committed -> shown; the sampled pixel is the painted color.
    const vkr::sidecar::SidecarPaintResponse p1 =
        backend.paint_chrome(make_chrome(0x700, 1, 1, 8, 4, 0x11, 0x22, 0x33, 0xFF));
    VKR_CHECK(p1.ok && p1.applied && p1.shown);
    VKR_CHECK_EQ(p1.representation, std::string("placeholder"));
    VKR_CHECK_EQ(p1.last_seq, static_cast<std::uint64_t>(1));
    VKR_CHECK(backend.debug_placeholder_shown(0x700));

    vkr::sidecar::SidecarDebugChromeStateRequest q;
    q.xid = 0x700;
    q.sample_x = 3;
    q.sample_y = 2;
    const vkr::sidecar::SidecarDebugChromeStateResponse d1 = backend.debug_chrome_state(q);
    VKR_CHECK(d1.ok && d1.shown && d1.has_pixel);
    VKR_CHECK_EQ(d1.pixel_bgra,
                 static_cast<std::uint32_t>(0xFF332211u)); // (B)|(G<<8)|(R<<16)|(A<<24)

    // An out-of-bounds sample reports no pixel (but still the live shown/seq state).
    q.sample_x = 99;
    const vkr::sidecar::SidecarDebugChromeStateResponse d2 = backend.debug_chrome_state(q);
    VKR_CHECK(d2.ok && d2.shown && !d2.has_pixel);

    // A stale-seq paint is dropped (not applied); shown/last_seq unchanged.
    const vkr::sidecar::SidecarPaintResponse stale =
        backend.paint_chrome(make_chrome(0x700, 1, 1, 8, 4, 0, 0, 0, 0xFF)); // seq 1 <= last 1
    VKR_CHECK(stale.ok && !stale.applied);
    VKR_CHECK_EQ(stale.last_seq, static_cast<std::uint64_t>(1));

    // A newer paint updates the sampled pixel.
    VKR_CHECK(backend.paint_chrome(make_chrome(0x700, 1, 2, 8, 4, 0x44, 0x55, 0x66, 0xFF)).applied);
    q.sample_x = 0;
    q.sample_y = 0;
    VKR_CHECK_EQ(backend.debug_chrome_state(q).pixel_bgra, static_cast<std::uint32_t>(0xFF665544u));

    // A paint for a Surface (worker-present) toplevel is never accepted -- chrome is
    // Placeholder-only.
    vkrpc::CreateSurfaceRequest sr;
    sr.instance = ci.instance;
    sr.platform = "xcb";
    sr.xid = 0x800;
    VKR_CHECK(backend.create_surface(sr).ok); // surface-first -> Surface representation
    const vkr::sidecar::SidecarPaintResponse on_surface =
        backend.paint_chrome(make_chrome(0x800, 1, 1, 4, 4, 0, 0, 0, 0xFF));
    VKR_CHECK(on_surface.ok && !on_surface.applied);
    VKR_CHECK_EQ(on_surface.representation, std::string("surface"));

    // After unregister, DebugChromeState reports NO stale pixel (the chrome store is dropped when
    // the representation leaves Placeholder -- mock == real, where the placeholder HWND/DIB is
    // destroyed).
    vkr::sidecar::SidecarUnregisterToplevelRequest unreg;
    unreg.xid = 0x700;
    unreg.generation = 2;
    VKR_CHECK(backend.unregister_toplevel(unreg).applied);
    q.xid = 0x700;
    q.sample_x = 0;
    q.sample_y = 0;
    const vkr::sidecar::SidecarDebugChromeStateResponse after = backend.debug_chrome_state(q);
    VKR_CHECK(after.ok && !after.has_pixel);
    VKR_CHECK_EQ(after.representation, std::string("none"));

    // Promotion also drops the stale chrome (a placeholder painted, then a surface arrives ->
    // Surface).
    vkr::sidecar::SidecarRegisterToplevelRequest reg2;
    reg2.xid = 0x900;
    reg2.generation = 1;
    reg2.width = 4;
    reg2.height = 4;
    VKR_CHECK(backend.register_toplevel(reg2).applied);
    VKR_CHECK(backend.paint_chrome(make_chrome(0x900, 1, 1, 4, 4, 0x10, 0x20, 0x30, 0xFF)).applied);
    q.xid = 0x900;
    q.sample_x = 1;
    q.sample_y = 1;
    VKR_CHECK(backend.debug_chrome_state(q).has_pixel); // painted while a placeholder
    vkrpc::CreateSurfaceRequest sr2;
    sr2.instance = ci.instance;
    sr2.platform = "xcb";
    sr2.xid = 0x900;
    VKR_CHECK(backend.create_surface(sr2).ok); // promote -> Surface
    const vkr::sidecar::SidecarDebugChromeStateResponse promoted = backend.debug_chrome_state(q);
    VKR_CHECK(promoted.ok && !promoted.has_pixel);
    VKR_CHECK_EQ(promoted.representation, std::string("surface"));
}

// (GL/zink): wire round-trip for the new capability ops + the extended FormatProperties /
// CreateDevice fields. Body-level (no backend), so it stays dual-platform (no vulkan.h).
void test_zink_caps_wire() {
    {
        vkrpc::GetPhysicalDeviceFeaturesRequest req;
        req.physical_device = 0x1234;
        const auto rt = vkrpc::GetPhysicalDeviceFeaturesRequest::from_body(req.to_body());
        VKR_CHECK_EQ(rt.physical_device, static_cast<std::uint64_t>(0x1234));
        vkrpc::GetPhysicalDeviceFeaturesResponse resp;
        resp.ok = true;
        resp.feature_bits = 0x5AA55AA5ULL;
        const auto rr = vkrpc::GetPhysicalDeviceFeaturesResponse::from_body(resp.to_body());
        VKR_CHECK(rr.ok);
        VKR_CHECK_EQ(rr.feature_bits, static_cast<std::uint64_t>(0x5AA55AA5ULL));
    }
    {
        vkrpc::GetPhysicalDeviceImageFormatPropertiesRequest req;
        req.physical_device = 0x9;
        req.format = 37;
        req.image_type = 1;
        req.tiling = 0;
        req.usage = 0x16;
        req.flags = 0x2;
        const auto rt =
            vkrpc::GetPhysicalDeviceImageFormatPropertiesRequest::from_body(req.to_body());
        VKR_CHECK_EQ(rt.format, 37);
        VKR_CHECK_EQ(rt.image_type, 1);
        VKR_CHECK_EQ(rt.usage, static_cast<std::uint64_t>(0x16));
        vkrpc::GetPhysicalDeviceImageFormatPropertiesResponse resp;
        resp.ok = true;
        resp.result = -11; // negative VkResult must survive (get_i64, not get_index)
        resp.max_extent_width = 16384;
        resp.max_resource_size = 1ULL << 31;
        const auto rr =
            vkrpc::GetPhysicalDeviceImageFormatPropertiesResponse::from_body(resp.to_body());
        VKR_CHECK_EQ(rr.result, -11);
        VKR_CHECK_EQ(rr.max_extent_width, static_cast<std::uint32_t>(16384));
        VKR_CHECK_EQ(rr.max_resource_size, static_cast<std::uint64_t>(1ULL << 31));
    }
    {
        // FormatProperties response carries the 64-bit VkFormatProperties3 fields additively.
        vkrpc::GetPhysicalDeviceFormatPropertiesResponse resp;
        resp.ok = true;
        resp.optimal_tiling_features = 0x111;
        resp.optimal_tiling_features2 = 0xABCDEF1234ULL; // > 32-bit, proves the wide path
        const auto rr = vkrpc::GetPhysicalDeviceFormatPropertiesResponse::from_body(resp.to_body());
        VKR_CHECK_EQ(rr.optimal_tiling_features, static_cast<std::uint64_t>(0x111));
        VKR_CHECK_EQ(rr.optimal_tiling_features2, static_cast<std::uint64_t>(0xABCDEF1234ULL));
        // A legacy response (no *2 keys -- built by hand) decodes the 64-bit fields to 0.
        json::Value b = json::Value::make_object();
        b.set("ok", json::Value(true));
        b.set("reason", json::Value(std::string("ok")));
        b.set("optimal_tiling_features", json::Value(std::string("7"))); // decimal-string u64
        const auto lr = vkrpc::GetPhysicalDeviceFormatPropertiesResponse::from_body(b);
        VKR_CHECK_EQ(lr.optimal_tiling_features, static_cast<std::uint64_t>(0x7));
        VKR_CHECK_EQ(lr.optimal_tiling_features2, static_cast<std::uint64_t>(0));
    }
    {
        // CreateDeviceRequest carries the enabled extensions + feature bits additively.
        vkrpc::CreateDeviceRequest req;
        req.instance = 0x1;
        req.physical_device = 0x2;
        req.enabled_extensions = {"VK_KHR_swapchain", "VK_KHR_maintenance1"};
        req.enabled_feature_bits = 0xDEAD;
        req.enabled_feature_bits_authoritative = true;
        req.draw_indirect_count_enabled = 1;
        const auto rt = vkrpc::CreateDeviceRequest::from_body(req.to_body());
        VKR_CHECK_EQ(rt.enabled_extensions.size(), static_cast<std::size_t>(2));
        VKR_CHECK_EQ(rt.enabled_extensions[1], std::string("VK_KHR_maintenance1"));
        VKR_CHECK_EQ(rt.enabled_feature_bits, static_cast<std::uint64_t>(0xDEAD));
        VKR_CHECK(rt.enabled_feature_bits_authoritative);
        VKR_CHECK_EQ(rt.draw_indirect_count_enabled, 1);
        // Legacy request (no new keys) -> empty list + 0 bits (the worker's prior behavior).
        const auto legacy = vkrpc::CreateDeviceRequest::from_body([] {
            json::Value b = json::Value::make_object();
            b.set("instance", json::Value(std::string("1")));
            b.set("physical_device", json::Value(std::string("2")));
            return b;
        }());
        VKR_CHECK(legacy.enabled_extensions.empty());
        VKR_CHECK_EQ(legacy.enabled_feature_bits, static_cast<std::uint64_t>(0));
        VKR_CHECK(!legacy.enabled_feature_bits_authoritative);
        VKR_CHECK_EQ(legacy.draw_indirect_count_enabled, vkrpc::kDrawIndirectCountScalarOmitted);
        json::Value forged = req.to_body();
        forged.set("draw_indirect_count_enabled", json::Value(-1));
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(forged).draw_indirect_count_enabled,
                     vkrpc::kDrawIndirectCountScalarInvalid);
    }
}

// (GL/zink): the mock honest-cap handlers (mock == real shape).
void test_zink_caps_mock() {
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const std::uint64_t phys = backend.enumerate_physical_devices(er).devices.front().handle;

    // Features: a known physical device yields a nonzero set; an unknown one is rejected.
    vkrpc::GetPhysicalDeviceFeaturesRequest fr;
    fr.physical_device = phys;
    const auto feats = backend.get_physical_device_features(fr);
    VKR_CHECK(feats.ok);
    VKR_CHECK(feats.feature_bits != 0);
    {
        auto bad = fr;
        bad.physical_device = 0xDEAD;
        VKR_CHECK(!backend.get_physical_device_features(bad).ok);
    }

    // Image-format-properties: a 2D request is OK with maxima; a non-2D type reports NOT_SUPPORTED.
    vkrpc::GetPhysicalDeviceImageFormatPropertiesRequest ir;
    ir.physical_device = phys;
    ir.format = kFmtRgba8;
    ir.image_type = vkrpc::kImageType2D;
    ir.tiling = 0;
    ir.usage = 0x16;
    const auto img = backend.get_physical_device_image_format_properties(ir);
    VKR_CHECK(img.ok && img.result == 0 && img.max_extent_width > 0);
    {
        auto three_d = ir;
        three_d.image_type = 2; // 3D
        const auto r = backend.get_physical_device_image_format_properties(three_d);
        VKR_CHECK(r.ok && r.result == -11); // FORMAT_NOT_SUPPORTED
    }

    // FormatProperties mirrors the 32-bit flags into the 64-bit VkFormatProperties3 fields.
    vkrpc::GetPhysicalDeviceFormatPropertiesRequest fpq;
    fpq.physical_device = phys;
    fpq.format = kFmtRgba8;
    const auto fp = backend.get_physical_device_format_properties(fpq);
    VKR_CHECK(fp.ok);
    VKR_CHECK_EQ(fp.linear_tiling_features2, fp.linear_tiling_features);
    VKR_CHECK_EQ(fp.optimal_tiling_features2, fp.optimal_tiling_features);
}

// (GL/zink): timeline semaphores -- wire round-trips + the mock backend's timeline model
// (the parity guard for the real backend, which glxgears exercises end-to-end on the GPU).
void test_timeline_semaphores() {
    // --- wire round-trips ---
    {
        vkrpc::CreateSemaphoreRequest cs;
        cs.device = 7;
        cs.semaphore_type = 1;
        cs.initial_value = 42;
        const auto rt = vkrpc::CreateSemaphoreRequest::from_body(cs.to_body());
        VKR_CHECK_EQ(rt.semaphore_type, 1);
        VKR_CHECK_EQ(static_cast<long long>(rt.initial_value), 42LL);

        vkrpc::QueueSubmitRequest qs;
        qs.queue = 3;
        vkrpc::SubmitWait w;
        w.semaphore = 9;
        w.stage = 1;
        w.value = 100;
        qs.waits.push_back(w);
        qs.signal_semaphores = {11, 12};
        qs.signal_values = {200, 0};
        const auto qrt = vkrpc::QueueSubmitRequest::from_body(qs.to_body());
        VKR_CHECK_EQ(static_cast<long long>(qrt.waits.at(0).value), 100LL);
        VKR_CHECK_EQ(static_cast<long long>(qrt.signal_values.at(0)), 200LL);

        vkrpc::WaitSemaphoresRequest ws;
        ws.device = 7;
        ws.semaphores = {9};
        ws.values = {100};
        ws.timeout = 1234;
        ws.wait_any = 1;
        const auto wrt = vkrpc::WaitSemaphoresRequest::from_body(ws.to_body());
        VKR_CHECK_EQ(static_cast<long long>(wrt.values.at(0)), 100LL);
        VKR_CHECK_EQ(wrt.wait_any, 1);
        VKR_CHECK_EQ(static_cast<long long>(wrt.timeout), 1234LL);
        vkrpc::GetSemaphoreCounterValueResponse gr;
        gr.ok = true;
        gr.result = -4;
        gr.value = 77;
        const auto grt = vkrpc::GetSemaphoreCounterValueResponse::from_body(gr.to_body());
        VKR_CHECK_EQ(grt.result, -4);
        VKR_CHECK_EQ(static_cast<long long>(grt.value), 77LL);
        VKR_CHECK_EQ(
            vkrpc::GetSemaphoreCounterValueResponse::from_body(json::Value::make_object()).result,
            0); // pre-field payload -> VK_SUCCESS
    }

    // --- mock backend timeline model ---
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const std::uint64_t device = make_device(backend);
    VKR_CHECK(device != 0);

    vkrpc::CreateSemaphoreRequest cs;
    cs.device = device;
    cs.semaphore_type = 1; // TIMELINE
    cs.initial_value = 5;
    const auto tsem = backend.create_semaphore(cs);
    VKR_CHECK(tsem.ok && tsem.semaphore != 0);

    auto counter = [&](std::uint64_t s) {
        vkrpc::GetSemaphoreCounterValueRequest g;
        g.device = device;
        g.semaphore = s;
        return backend.get_semaphore_counter_value(g);
    };
    VKR_CHECK_EQ(static_cast<long long>(counter(tsem.semaphore).value), 5LL); // initial value

    vkrpc::SignalSemaphoreRequest sig;
    sig.device = device;
    sig.semaphore = tsem.semaphore;
    sig.value = 10;
    VKR_CHECK(backend.signal_semaphore(sig).ok);
    VKR_CHECK_EQ(static_cast<long long>(counter(tsem.semaphore).value), 10LL);
    sig.value = 8; // a timeline may only advance -> regression rejected
    VKR_CHECK(!backend.signal_semaphore(sig).ok);

    auto wait = [&](std::uint64_t v) {
        vkrpc::WaitSemaphoresRequest w;
        w.device = device;
        w.semaphores = {tsem.semaphore};
        w.values = {v};
        return backend.wait_semaphores(w);
    };
    VKR_CHECK_EQ(wait(10).result, vkrpc::kVkSuccess); // already at 10
    VKR_CHECK_EQ(wait(11).result, vkrpc::kVkTimeout); // not yet reached

    // A BINARY semaphore is not a timeline: the host ops reject it.
    vkrpc::CreateSemaphoreRequest bs;
    bs.device = device; // semaphore_type defaults to 0 (binary)
    const auto bsem = backend.create_semaphore(bs);
    VKR_CHECK(bsem.ok);
    VKR_CHECK(!counter(bsem.semaphore).ok);
    vkrpc::SignalSemaphoreRequest bsig;
    bsig.device = device;
    bsig.semaphore = bsem.semaphore;
    bsig.value = 1;
    VKR_CHECK(!backend.signal_semaphore(bsig).ok);
}

// Query pools (GL 3.3 / occlusion / xfb queries): wire round-trip + the mock oracle's create/
// destroy + recorded-command validation (mock == real) + GetQueryPoolResults.
void test_query_pools() {
    // Wire round-trip of the three messages (incl. absent timestamp field -> 0 already covered in
    // test_lifecycle_body_roundtrip; here the query-pool messages + the hex result blob).
    {
        vkrpc::CreateQueryPoolRequest req;
        req.device = 0x11;
        req.query_type = 2; // TIMESTAMP
        req.query_count = 8;
        req.pipeline_statistics = 0;
        const auto rt = vkrpc::CreateQueryPoolRequest::from_body(req.to_body());
        VKR_CHECK_EQ(rt.device, 0x11ull);
        VKR_CHECK_EQ(rt.query_type, 2);
        VKR_CHECK_EQ(rt.query_count, static_cast<std::uint32_t>(8));

        // hostQueryReset: the device-level reset request round-trips its four fields.
        vkrpc::ResetQueryPoolRequest rq;
        rq.device = 0x22;
        rq.query_pool = 0x33;
        rq.first_query = 2;
        rq.query_count = 5;
        const auto rqr = vkrpc::ResetQueryPoolRequest::from_body(rq.to_body());
        VKR_CHECK_EQ(rqr.device, 0x22ull);
        VKR_CHECK_EQ(rqr.query_pool, 0x33ull);
        VKR_CHECK_EQ(rqr.first_query, static_cast<std::uint32_t>(2));
        VKR_CHECK_EQ(rqr.query_count, static_cast<std::uint32_t>(5));

        vkrpc::GetQueryPoolResultsResponse resp;
        resp.ok = true;
        resp.vk_result = 5; // VK_NOT_READY
        resp.data = std::string("\x01\x02\x03\x04", 4);
        const auto rr = vkrpc::GetQueryPoolResultsResponse::from_body(resp.to_body());
        VKR_CHECK(rr.ok);
        VKR_CHECK_EQ(rr.vk_result, 5);
        VKR_CHECK_EQ(rr.data.size(), static_cast<std::size_t>(4));
        VKR_CHECK(rr.data == std::string("\x01\x02\x03\x04", 4));
    }

    // Mock oracle: create/destroy + destroy-ordering + recorded query commands.
    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const std::uint64_t dev = make_device(backend);
    VKR_CHECK(dev != 0);
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = dev;
    cpr.queue_family_index = 0;
    const std::uint64_t pool = backend.create_command_pool(cpr).command_pool;
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = pool;
    acb.count = 1;
    const std::uint64_t cmd = backend.allocate_command_buffers(acb).command_buffers.front();

    // Create a TIMESTAMP query pool of 4; reject an unsupported type + a zero count.
    auto make_qp = [&](int type, std::uint32_t count) {
        vkrpc::CreateQueryPoolRequest r;
        r.device = dev;
        r.query_type = type;
        r.query_count = count;
        return backend.create_query_pool(r);
    };
    const vkrpc::CreateQueryPoolResponse qp = make_qp(2 /*TIMESTAMP*/, 4);
    VKR_CHECK(qp.ok && qp.query_pool != 0);
    VKR_CHECK(!make_qp(2, 0).ok);   // zero count
    VKR_CHECK(!make_qp(999, 4).ok); // unsupported type
    // TRANSFORM_FEEDBACK_STREAM_EXT is fail-closed at creation (indexed begin unwired).
    VKR_CHECK(!make_qp(1000028004, 4).ok);
    { // PIPELINE_STATISTICS is core-command-driveable -> accepted (destroy so it doesn't linger).
        const vkrpc::CreateQueryPoolResponse ps = make_qp(1, 2);
        VKR_CHECK(ps.ok);
        vkrpc::HandleRequest hps;
        hps.handle = ps.query_pool;
        VKR_CHECK(backend.destroy_query_pool(hps).ok);
    }
    const vkrpc::CreateQueryPoolResponse occ = make_qp(0 /*OCCLUSION*/, 2);
    VKR_CHECK(occ.ok);

    auto rec = [&](std::vector<vkrpc::RecordedCommand> cmds) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        r.commands = std::move(cmds);
        return backend.record_command_buffer(r).ok;
    };
    auto reset = [&](std::uint64_t p, long long first, long long count) {
        vkrpc::RecordedCommand c;
        c.kind = "reset_query_pool";
        c.args_u64 = {p};
        c.args_i64 = {first, count};
        return c;
    };
    auto wts = [&](std::uint64_t p, long long q) {
        vkrpc::RecordedCommand c;
        c.kind = "write_timestamp";
        c.args_u64 = {p};
        c.args_i64 = {/*stage*/ 0x1000, q};
        return c;
    };
    auto beginq = [&](std::uint64_t p, long long q) {
        vkrpc::RecordedCommand c;
        c.kind = "begin_query";
        c.args_u64 = {p};
        c.args_i64 = {q, /*flags*/ 0};
        return c;
    };
    auto endq = [&](std::uint64_t p, long long q) {
        vkrpc::RecordedCommand c;
        c.kind = "end_query";
        c.args_u64 = {p};
        c.args_i64 = {q};
        return c;
    };
    // Accept: reset + timestamp write in range.
    VKR_CHECK(rec({reset(qp.query_pool, 0, 4), wts(qp.query_pool, 3)}));
    // Accept: a balanced occlusion begin/end.
    VKR_CHECK(
        rec({reset(occ.query_pool, 0, 2), beginq(occ.query_pool, 0), endq(occ.query_pool, 0)}));
    // Reject: unknown pool; out-of-range index; reset past the end.
    VKR_CHECK(!rec({wts(0xDEAD, 0)}));
    VKR_CHECK(!rec({wts(qp.query_pool, 4)}));      // index == count
    VKR_CHECK(!rec({reset(qp.query_pool, 2, 4)})); // 2+4 > 4
    // Reject: double-begin of the same index; end without begin; active at stream end.
    VKR_CHECK(!rec({beginq(occ.query_pool, 0), beginq(occ.query_pool, 0)}));
    VKR_CHECK(!rec({endq(occ.query_pool, 0)}));
    VKR_CHECK(!rec({beginq(occ.query_pool, 0)})); // never ended
    // Two distinct indices may be active at once, then both end.
    VKR_CHECK(rec({beginq(occ.query_pool, 0), beginq(occ.query_pool, 1), endq(occ.query_pool, 1),
                   endq(occ.query_pool, 0)}));

    // GetQueryPoolResults: in-range ok (mock zero-fills), out-of-range + unknown-pool reject.
    auto results = [&](std::uint64_t p, std::uint32_t first, std::uint32_t count,
                       std::uint64_t size) {
        vkrpc::GetQueryPoolResultsRequest r;
        r.device = dev;
        r.query_pool = p;
        r.first_query = first;
        r.query_count = count;
        r.data_size = size;
        r.stride = 8;
        r.flags = 0;
        return backend.get_query_pool_results(r);
    };
    const auto gr = results(qp.query_pool, 0, 4, 32);
    VKR_CHECK(gr.ok && gr.data.size() == 32);
    VKR_CHECK(!results(qp.query_pool, 2, 4, 16).ok); // 2+4 > 4
    VKR_CHECK(!results(0xDEAD, 0, 1, 8).ok);         // unknown pool
    // first_query + query_count must not WRAP in uint32 (UINT32_MAX + 1 = 0).
    VKR_CHECK(!results(qp.query_pool, 0xFFFFFFFFu, 1, 8).ok);
    VKR_CHECK(!results(qp.query_pool, 3, 0xFFFFFFFFu, 8).ok);
    // A NEGATIVE VkResult (VK_ERROR_DEVICE_LOST = -4) round-trips (get_index would clamp
    // it to -1).
    {
        vkrpc::GetQueryPoolResultsResponse neg;
        neg.ok = true;
        neg.vk_result = -4;
        VKR_CHECK_EQ(vkrpc::GetQueryPoolResultsResponse::from_body(neg.to_body()).vk_result, -4);
    }

    // hostQueryReset (hardening): device-level reset_query_pool --
    // the per-device feature GATE + overflow-safe range validation (mock == real).
    {
        // The mock is a scalar-based gate oracle: it never links Vulkan (it forwards the enabled-
        // feature chain OPAQUELY), so it validates the reset GATE off the scalar -- exactly like
        // the sync2/BDA mock devices. The scalar/chain-AGREEMENT normalization (rejecting a hostile
        // frame whose scalar and chain disagree) is the WORKER's oracle (real_vulkan_backend, the
        // proven BDA-normalization pattern); the real ICD always pairs the scalar with a matching
        // chain, so a real device is never scalar-only.
        auto make_hqr_device = [&]() {
            const vkrpc::CreateInstanceResponse ci2 = backend.create_instance({});
            vkrpc::EnumeratePhysicalDevicesRequest er2;
            er2.instance = ci2.instance;
            vkrpc::CreateDeviceRequest cdr2;
            cdr2.instance = ci2.instance;
            cdr2.physical_device = backend.enumerate_physical_devices(er2).devices.front().handle;
            cdr2.host_query_reset_feature_enabled = 1; // the app enabled the feature
            return backend.create_device(cdr2).device;
        };
        const std::uint64_t dev_hqr = make_hqr_device();
        VKR_CHECK(dev_hqr != 0);
        vkrpc::CreateQueryPoolRequest qpr;
        qpr.device = dev_hqr;
        qpr.query_type = 2; // TIMESTAMP
        qpr.query_count = 4;
        const std::uint64_t pool_hqr = backend.create_query_pool(qpr).query_pool;
        VKR_CHECK(pool_hqr != 0);
        auto rst = [&](std::uint64_t d, std::uint64_t p, std::uint32_t first, std::uint32_t count) {
            vkrpc::ResetQueryPoolRequest r;
            r.device = d;
            r.query_pool = p;
            r.first_query = first;
            r.query_count = count;
            return backend.reset_query_pool(r);
        };
        VKR_CHECK(rst(dev_hqr, pool_hqr, 0, 4).ok);            // full range
        VKR_CHECK(rst(dev_hqr, pool_hqr, 1, 2).ok);            // sub-range
        VKR_CHECK(!rst(dev_hqr, pool_hqr, 0, 0).ok);           // zero count is malformed
        VKR_CHECK(!rst(dev_hqr, pool_hqr, 3, 4).ok);           // 3 + 4 > 4
        VKR_CHECK(!rst(dev_hqr, pool_hqr, 0xFFFFFFFFu, 1).ok); // first + count wraps uint32
        VKR_CHECK(!rst(dev_hqr, 0xDEAD, 0, 1).ok);             // unknown pool
        // Wrong device: another feature-enabled device cannot reset dev_hqr's pool.
        const std::uint64_t dev_hqr2 = make_hqr_device();
        VKR_CHECK(dev_hqr2 != 0);
        VKR_CHECK(!rst(dev_hqr2, pool_hqr, 0, 4).ok);
        // The GATE: the base `dev` never enabled hostQueryReset, so a reset on its own pool is
        // fail-closed even though the range is valid -- a skewed client cannot drive an invalid
        // host vkResetQueryPool.
        VKR_CHECK(!rst(dev, qp.query_pool, 0, 4).ok);
    }

    // copy_query_pool_results (zink's read path) + the host-visible readback RPC. Make a live+bound
    // host-visible buffer, record a copy into it, and round-trip write -> readback.
    {
        // The mock's coherent host-visible memory type (write/read_memory_ranges require it).
        vkrpc::CreateBufferRequest cbq;
        cbq.device = dev;
        cbq.size = 64;
        cbq.usage = vkrpc::kBufferUsageTransferDst;
        cbq.sharing_mode = 0;
        const vkrpc::CreateBufferResponse buf = backend.create_buffer(cbq);
        VKR_CHECK(buf.ok);
        int coherent_type = 0;
        {
            const auto inst = backend.create_instance({});
            vkrpc::EnumeratePhysicalDevicesRequest er2;
            er2.instance = inst.instance;
            const auto phys = backend.enumerate_physical_devices(er2).devices.front().handle;
            vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
            mpq.physical_device = phys;
            const auto mp = backend.get_physical_device_memory_properties(mpq);
            for (std::size_t i = 0; i < mp.types.size(); ++i) {
                const std::uint64_t want =
                    vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
                if ((mp.types[i].property_flags & want) == want) {
                    coherent_type = static_cast<int>(i);
                }
            }
        }
        vkrpc::AllocateMemoryRequest am;
        am.device = dev;
        am.allocation_size = 256;
        am.memory_type_index = coherent_type;
        const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(am);
        VKR_CHECK(mem.ok);
        vkrpc::BindBufferMemoryRequest bbm;
        bbm.buffer = buf.buffer;
        bbm.memory = mem.memory;
        bbm.memory_offset = 0;
        VKR_CHECK(backend.bind_buffer_memory(bbm).ok);
        auto copyres = [&](std::uint64_t p, std::uint64_t dst, long long first, long long count) {
            vkrpc::RecordedCommand c;
            c.kind = "copy_query_pool_results";
            c.args_u64 = {p, dst};
            c.args_i64 = {first, count, /*dstOffset*/ 0, /*stride*/ 8, /*flags*/ 0};
            return c;
        };
        VKR_CHECK(rec({copyres(qp.query_pool, buf.buffer, 0, 4)}));  // accept
        VKR_CHECK(!rec({copyres(qp.query_pool, buf.buffer, 0, 5)})); // range > count
        VKR_CHECK(!rec({copyres(qp.query_pool, 0xDEAD, 0, 4)}));     // unknown dst buffer
        VKR_CHECK(!rec({copyres(0xDEAD, buf.buffer, 0, 4)}));        // unknown pool
        // read_memory_ranges round-trip: write bytes, read them back.
        vkrpc::WriteMemoryRangesRequest w;
        vkrpc::MemoryUpload up;
        up.memory = mem.memory;
        up.ranges.push_back({0, 8});
        w.uploads = {up};
        w.payload = std::string("\xDE\xAD\xBE\xEF\x01\x02\x03\x04", 8);
        VKR_CHECK(backend.write_memory_ranges(w).ok);
        vkrpc::ReadMemoryRangesRequest rr;
        rr.reads = {up};
        const vkrpc::ReadMemoryRangesResponse rresp = backend.read_memory_ranges(rr);
        VKR_CHECK(rresp.ok && rresp.payload == w.payload); // readback echoes the written bytes
        // Wire round-trip of the readback messages.
        const auto rq2 = vkrpc::ReadMemoryRangesRequest::from_body(rr.to_body());
        VKR_CHECK_EQ(rq2.reads.size(), static_cast<std::size_t>(1));
        VKR_CHECK_EQ(rq2.reads[0].memory, mem.memory);
        const auto rs2 = vkrpc::ReadMemoryRangesResponse::from_body(rresp.to_body());
        VKR_CHECK(rs2.payload == rresp.payload);
        // the RAW response framing round-trips byte-exact (incl. NUL/0xFF in
        // the payload), the request's raw_response flag rides the body (absent -> false so an
        // old client keeps the JSON reply), and a malformed raw frame fails CLOSED.
        {
            std::string err;
            const auto raw = vkrpc::ReadMemoryRangesResponse::from_wire(rresp.to_wire(), err);
            VKR_CHECK(err.empty() && raw.ok && raw.payload == rresp.payload);
            vkrpc::ReadMemoryRangesResponse fail;
            fail.ok = false;
            fail.reason = "no such memory";
            const auto fr = vkrpc::ReadMemoryRangesResponse::from_wire(fail.to_wire(), err);
            VKR_CHECK(err.empty() && !fr.ok && fr.reason == "no such memory" && fr.payload.empty());
            vkrpc::ReadMemoryRangesRequest rawreq;
            rawreq.raw_response = true;
            rawreq.reads = {up};
            VKR_CHECK(vkrpc::ReadMemoryRangesRequest::from_body(rawreq.to_body()).raw_response);
            VKR_CHECK(!rq2.raw_response); // explicit raw_response=false rides through unchanged
            // The version-skew pin proper: an OLD client's body carries NO
            // raw_response field at all -- absent must decode false (JSON+hex path).
            json::Value old_req = json::Value::make_object();
            old_req.set("reads", json::Value(json::Array{}));
            VKR_CHECK(!vkrpc::ReadMemoryRangesRequest::from_body(old_req).raw_response);
            // Fail-closed decode: short body / header cap exceeded / header overrun / garbage.
            vkrpc::ReadMemoryRangesResponse d;
            d = vkrpc::ReadMemoryRangesResponse::from_wire("ab", err);
            VKR_CHECK(!err.empty() && !d.ok);
            std::string huge(4, '\0');
            huge[0] = '\xFF';
            huge[1] = '\xFF';
            huge[2] = '\xFF';
            huge[3] = '\x7F';
            d = vkrpc::ReadMemoryRangesResponse::from_wire(huge, err);
            VKR_CHECK(!err.empty() && !d.ok);
            std::string overrun(4, '\0');
            overrun[0] = 100; // claims a 100-byte header with no bytes behind it
            d = vkrpc::ReadMemoryRangesResponse::from_wire(overrun, err);
            VKR_CHECK(!err.empty() && !d.ok);
            std::string garbage(4, '\0');
            garbage[0] = 3;
            garbage += "%%%";
            d = vkrpc::ReadMemoryRangesResponse::from_wire(garbage, err);
            VKR_CHECK(!err.empty() && !d.ok);
        }
        // Unknown memory -> reject.
        vkrpc::ReadMemoryRangesRequest bad;
        vkrpc::MemoryUpload bu;
        bu.memory = 0xDEAD;
        bu.ranges.push_back({0, 8});
        bad.reads = {bu};
        VKR_CHECK(!backend.read_memory_ranges(bad).ok);
        // A range past the allocation is rejected (mock == real bounds check); and an
        // offset+size that would WRAP is rejected too (the 256-byte allocation `mem`).
        {
            vkrpc::ReadMemoryRangesRequest oor;
            vkrpc::MemoryUpload ou;
            ou.memory = mem.memory;
            ou.ranges.push_back({200, 100}); // 200 + 100 = 300 > 256
            oor.reads = {ou};
            VKR_CHECK(!backend.read_memory_ranges(oor).ok);
            vkrpc::ReadMemoryRangesRequest wrap;
            vkrpc::MemoryUpload wu;
            wu.memory = mem.memory;
            wu.ranges.push_back({16, ~0ull - 8}); // offset + size overflows u64
            wrap.reads = {wu};
            VKR_CHECK(!backend.read_memory_ranges(wrap).ok);
        }
        // Tear the buffer + memory down (they'd block the device destroy below).
        vkrpc::HandleRequest hb;
        hb.handle = buf.buffer;
        VKR_CHECK(backend.destroy_buffer(hb).ok);
        hb.handle = mem.memory;
        VKR_CHECK(backend.free_memory(hb).ok);
    }
    // Re-record a query-only stream so the destroy-ordering checks below bake only qp (the buffer
    // is gone).
    VKR_CHECK(rec({reset(qp.query_pool, 0, 4), wts(qp.query_pool, 1)}));

    // Destroy ordering: the device blocks on a live query pool; destroying a baked pool invalidates
    // the recorded CB (a later submit is refused).
    VKR_CHECK(rec({reset(qp.query_pool, 0, 4), wts(qp.query_pool, 0)})); // re-record baking qp
    {
        vkrpc::HandleRequest h;
        h.handle = dev;
        VKR_CHECK(!backend.destroy_device(h).ok); // live query pools block it
    }
    vkrpc::QueueSubmitRequest qs;
    qs.queue = backend.get_device_queue({dev, 0, 0}).queue;
    qs.command_buffers = {cmd};
    VKR_CHECK(backend.queue_submit(qs).ok); // recorded + valid -> submits
    {
        vkrpc::HandleRequest h;
        h.handle = qp.query_pool;
        VKR_CHECK(backend.destroy_query_pool(h).ok);
        vkrpc::QueueSubmitRequest qs2;
        qs2.queue = qs.queue;
        qs2.command_buffers = {cmd};
        VKR_CHECK(!backend.queue_submit(qs2).ok); // baked pool destroyed -> CB invalidated
        h.handle = occ.query_pool;
        VKR_CHECK(backend.destroy_query_pool(h).ok);
    }
}

// Required-feature audit (multiview): the enabled-device GATE, the viewMask wire carry,
// and the layer-sufficiency (highest-set-bit + 1, NOT popcount) check -- the mock oracle + the pure
// helper. The worker's scalar/chain-agreement + the real host multiview render pass ride the
// GPU-gated integration test (a real device is required to build a
// VkRenderPassMultiviewCreateInfo).
void test_vertex_binding_divisors() {
    // The shared VK_EXT_vertex_attribute_divisor content validator (ICD == worker == mock). Two
    // bindings (0 VERTEX, 1 INSTANCE) so the reference / duplicate / count checks can be exercised.
    // Args: (present, bindings, divisors, ext_enabled, divisor_feature, zero_divisor_feature, why).
    const std::vector<vkrpc::VertexBindingDesc> binds = {{0, 20, 0}, {1, 16, 1}};
    std::string why;
    // present == 0 accepts an empty array (ext-independent); a non-empty array without the flag
    // rejects.
    VKR_CHECK(vkrpc::vertex_binding_divisors_ok(0, binds, {}, false, false, false, why));
    VKR_CHECK(!vkrpc::vertex_binding_divisors_ok(0, binds, {{1, 2}}, true, true, true, why));
    // ext gate: a divisor pNext on a device WITHOUT the extension rejects, even
    // for divisor == 1 (which needs no feature).
    VKR_CHECK(
        !vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 1}}, /*ext=*/false, true, true, why));
    // present == 1 (ext on) requires a non-empty array.
    VKR_CHECK(!vkrpc::vertex_binding_divisors_ok(1, binds, {}, true, true, true, why));
    // divisor == 1 needs no feature; != 1 needs RateDivisor; == 0 additionally needs ZeroDivisor.
    VKR_CHECK(vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 1}}, true, false, false, why));
    VKR_CHECK(!vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 2}}, true, false, false, why));
    VKR_CHECK(vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 2}}, true, true, false, why));
    VKR_CHECK(
        !vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 0}}, true, true, false, why)); // 0 off
    VKR_CHECK(vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 0}}, true, true, true, why));
    // a divisor naming a binding with no description is rejected.
    VKR_CHECK(!vkrpc::vertex_binding_divisors_ok(1, binds, {{7, 1}}, true, true, true, why));
    // duplicate divisor bindings are rejected.
    VKR_CHECK(
        !vkrpc::vertex_binding_divisors_ok(1, binds, {{1, 1}, {1, 2}}, true, true, true, why));
    // more divisor entries than bindings is rejected.
    VKR_CHECK(!vkrpc::vertex_binding_divisors_ok(1, binds, {{0, 1}, {1, 2}, {0, 1}}, true, true,
                                                 true, why));
    // an out-of-u32 divisor is rejected.
    VKR_CHECK(
        !vkrpc::vertex_binding_divisors_ok(1, binds, {{1, (1LL << 33)}}, true, true, true, why));

    // geometry-stream: the shared rasterization_stream_ok policy, exercised directly with
    // synthetic capability sets -- the SAME helper the mock (modeled 4/true) and
    // the real worker (cached host values) parameterize, so every branch is pinned
    // platform-neutrally. Args: (present, stream, ext_enabled, geometryStreams, max_streams,
    // stream_select, why).
    // Absent -> fine regardless of capabilities; a stray stream value without the flag rejects.
    VKR_CHECK(vkrpc::rasterization_stream_ok(0, 0, false, false, 0, false, why));
    VKR_CHECK(!vkrpc::rasterization_stream_ok(0, 1, true, true, 4, true, why));
    // Extension / feature gates (host support is not enough; the app must ENABLE).
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, 0, false, true, 4, true, why));
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, 0, true, false, 4, true, why));
    // Explicit stream ZERO is valid whenever ext+feature are on -- even max=1/select=false (the
    // named case: stream zero must not need StreamSelect).
    VKR_CHECK(vkrpc::rasterization_stream_ok(1, 0, true, true, 1, false, why));
    // Nonzero stream: needs StreamSelect AND < max.
    VKR_CHECK(vkrpc::rasterization_stream_ok(1, 2, true, true, 4, true, why));
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, 2, true, true, 4, false, why)); // no select
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, 4, true, true, 4, true, why));  // == max
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, 5, true, true, 4, true, why));  // > max
    // Range faithfulness: negative and >u32 reject BEFORE any narrowing cast.
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, -1, true, true, 4, true, why));
    VKR_CHECK(!vkrpc::rasterization_stream_ok(1, (1LL << 33), true, true, 4, true, why));
}

void test_multiview_mock() {
    // The layer-sufficiency arithmetic is the HIGHEST SET view bit + 1, never popcount(mask). These
    // pin the exact "not popcount" contract: 0b101 addresses gl_ViewIndex 2, so it
    // needs THREE layers (0,1,2), not two.
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(0x0), 1); // no multiview -> one layer
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(0x1), 1); // view 0 only
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(0x3), 2); // views 0,1
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(0x5), 3); // views 0,2 -> 3 (NOT popcount == 2)
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(0x4), 3); // view 2 only -> 3
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(0xF), 4); // views 0..3
    VKR_CHECK_EQ(vkrpc::multiview_required_layers(static_cast<int>(0x80000000u)), 32); // view 31

    // Wire round-trip: the CreateRenderPassRequest.view_mask + CreateDeviceRequest.multiview
    // scalar.
    {
        vkrpc::CreateRenderPassRequest r;
        r.device = 0x11;
        r.view_mask = 0x5;
        VKR_CHECK_EQ(vkrpc::CreateRenderPassRequest::from_body(r.to_body()).view_mask, 0x5);
        vkrpc::CreateRenderPassRequest r0; // absent -> 0 (an old peer round-trips unchanged)
        r0.device = 0x11;
        VKR_CHECK_EQ(vkrpc::CreateRenderPassRequest::from_body(r0.to_body()).view_mask, 0);
        vkrpc::CreateDeviceRequest cdr;
        cdr.multiview_feature_enabled = 1;
        VKR_CHECK_EQ(vkrpc::CreateDeviceRequest::from_body(cdr.to_body()).multiview_feature_enabled,
                     1);
    }

    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    constexpr int kColor = 37; // VK_FORMAT_R8G8B8A8_UNORM (the mock's app-color format)

    // Two devices: one WITHOUT the multiview feature, one WITH it (the scalar the ICD derives).
    auto make_mv_device = [&](int multiview) {
        const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
        vkrpc::EnumeratePhysicalDevicesRequest er;
        er.instance = ci.instance;
        vkrpc::CreateDeviceRequest cdr;
        cdr.instance = ci.instance;
        cdr.physical_device = backend.enumerate_physical_devices(er).devices.front().handle;
        cdr.multiview_feature_enabled = multiview;
        return backend.create_device(cdr).device;
    };
    const std::uint64_t dev_plain = make_mv_device(0);
    const std::uint64_t dev_mv = make_mv_device(1);
    VKR_CHECK(dev_plain != 0 && dev_mv != 0);

    // A single-color-attachment render pass carrying a viewMask (MRT vector path so no swapchain
    // subset applies).
    auto make_mv_rp = [&](std::uint64_t d, int view_mask) {
        vkrpc::CreateRenderPassRequest r;
        r.device = d;
        vkrpc::AttachmentDesc a;
        a.format = kColor;
        a.samples = 1;
        a.load_op = 1;
        a.store_op = 0;
        a.stencil_load_op = 2;
        a.stencil_store_op = 1;
        a.initial_layout = 0;
        a.final_layout = 2;
        r.attachments.push_back(a);
        vkrpc::ColorRefDesc cr;
        cr.attachment = 0;
        cr.layout = 2;
        r.color_refs.push_back(cr);
        r.color_attachment = 0;
        r.color_layout = 2;
        r.view_mask = view_mask;
        return r;
    };

    // The GATE: a viewMask pass is fail-closed on the device that never enabled multiview, and
    // admitted on the one that did. view_mask == 0 is a plain pass, accepted on both.
    VKR_CHECK(backend.create_render_pass(make_mv_rp(dev_plain, 0)).ok);
    VKR_CHECK(!backend.create_render_pass(make_mv_rp(dev_plain, 0x3)).ok); // no feature -> reject
    VKR_CHECK(!backend.create_render_pass(make_mv_rp(dev_mv, -1)).ok);     // negative -> malformed
    const std::uint64_t rp_mv3 = backend.create_render_pass(make_mv_rp(dev_mv, 0x5)).render_pass;
    const std::uint64_t rp_mv2 = backend.create_render_pass(make_mv_rp(dev_mv, 0x3)).render_pass;
    VKR_CHECK(rp_mv3 != 0 && rp_mv2 != 0);

    // Build an app COLOR image with a chosen array-layer count, bind it, and make a view (the mock
    // rejects a view over an unbound app image, so bind first).
    auto make_layered_view = [&](int array_layers) {
        vkrpc::CreateImageRequest cir;
        cir.device = dev_mv;
        cir.image_type = vkrpc::kImageType2D;
        cir.format = kColor;
        cir.width = 64;
        cir.height = 64;
        cir.depth = 1;
        cir.mip_levels = 1;
        cir.array_layers = array_layers;
        cir.samples = 1;
        cir.tiling = vkrpc::kImageTilingOptimal;
        cir.usage = static_cast<std::uint64_t>(vkrpc::kImageUsageColorAttachment);
        cir.sharing_mode = 0;
        cir.initial_layout = 0;
        const vkrpc::CreateImageResponse im = backend.create_image(cir);
        VKR_CHECK(im.ok);
        vkrpc::AllocateMemoryRequest am;
        am.device = dev_mv;
        am.allocation_size = 4ull * 1024 * 1024;
        am.memory_type_index = 0; // DEVICE_LOCAL (OPTIMAL tiling binds device-local)
        const vkrpc::AllocateMemoryResponse mem = backend.allocate_memory(am);
        VKR_CHECK(mem.ok);
        VKR_CHECK(backend.bind_image_memory({im.image, mem.memory, 0}).ok);
        vkrpc::CreateImageViewRequest vr;
        vr.image = im.image;
        vr.view_type = 1;
        vr.format = kColor;
        vr.swizzle_r = vr.swizzle_g = vr.swizzle_b = vr.swizzle_a = 0;
        vr.aspect = vkrpc::kImageAspectColor;
        vr.base_mip_level = 0;
        vr.level_count = 1;
        vr.base_array_layer = 0;
        vr.layer_count = array_layers;
        const vkrpc::CreateImageViewResponse v = backend.create_image_view(vr);
        VKR_CHECK(v.ok);
        return v.image_view;
    };
    const std::uint64_t view_1 = make_layered_view(1);
    const std::uint64_t view_2 = make_layered_view(2);
    const std::uint64_t view_3 = make_layered_view(3);

    auto make_mv_fb = [&](std::uint64_t rpass, std::uint64_t view) {
        vkrpc::CreateFramebufferRequest r;
        r.device = dev_mv;
        r.render_pass = rpass;
        r.image_view = view;
        r.width = 64;
        r.height = 64;
        r.layers = 1; // multiview: the framebuffer layers stay 1; layers come from the views
        r.attachment_views = {view};
        return r;
    };
    // Layer sufficiency: viewMask 0x5 needs 3 layers -- a 1- or 2-layer image is rejected, a
    // 3-layer image is accepted. viewMask 0x3 needs 2 -- a 2-layer image suffices.
    VKR_CHECK(!backend.create_framebuffer(make_mv_fb(rp_mv3, view_1)).ok); // 1 < 3 -> reject
    VKR_CHECK(!backend.create_framebuffer(make_mv_fb(rp_mv3, view_2)).ok); // 2 < 3 -> reject
    VKR_CHECK(backend.create_framebuffer(make_mv_fb(rp_mv3, view_3)).ok);  // 3 >= 3 -> accept
    VKR_CHECK(backend.create_framebuffer(make_mv_fb(rp_mv2, view_2)).ok);  // 2 >= 2 -> accept
    VKR_CHECK(!backend.create_framebuffer(make_mv_fb(rp_mv2, view_1)).ok); // 1 < 2 -> reject
}

// RpcProfile aggregation -- pure arithmetic, no clocks (the hooks pass microseconds in).
void test_rpc_profile() {
    // Bucket edges: <64 -> 0, then doubling, >= 65536us (64ms) -> the last bucket.
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(0)), 0);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(63)), 0);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(64)), 1);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(127)), 1);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(128)), 2);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(65535)), 10);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(65536)), 11);
    VKR_CHECK_EQ(static_cast<int>(vkrpc::profile_bucket(~0ull)), 11);

    // op_name is TOTAL: known name, the retired gap, and out-of-range.
    VKR_CHECK(vkrpc::profile_op_name(static_cast<std::uint32_t>(vkrpc::RpcOp::QueueSubmit)) ==
              "queue_submit");
    VKR_CHECK(vkrpc::profile_op_name(25) == "op_25");   // the retired opcode gap
    VKR_CHECK(vkrpc::profile_op_name(500) == "op_500"); // future/unknown

    vkrpc::RpcProfile p;
    // A raw wire value beyond the table lands in `unknown` -- never out-of-bounds.
    p.record(500, 10, 20, 100);
    VKR_CHECK_EQ(static_cast<long long>(p.unknown.count), 1ll);
    // Known-op aggregation: count/bytes/min/max/hist.
    const auto op_qs = static_cast<std::uint32_t>(vkrpc::RpcOp::QueueSubmit);
    p.record(op_qs, 100, 5, 70); // bucket 1
    p.record(op_qs, 50, 10, 30); // bucket 0
    VKR_CHECK_EQ(static_cast<long long>(p.per_op[op_qs].count), 2ll);
    VKR_CHECK_EQ(static_cast<long long>(p.per_op[op_qs].bytes_out), 150ll);
    VKR_CHECK_EQ(static_cast<long long>(p.per_op[op_qs].bytes_in), 15ll);
    VKR_CHECK_EQ(static_cast<long long>(p.per_op[op_qs].total_us), 100ll);
    VKR_CHECK_EQ(static_cast<long long>(p.per_op[op_qs].min_us), 30ll);
    VKR_CHECK_EQ(static_cast<long long>(p.per_op[op_qs].max_us), 70ll);
    VKR_CHECK_EQ(static_cast<int>(p.per_op[op_qs].hist[0]), 1);
    VKR_CHECK_EQ(static_cast<int>(p.per_op[op_qs].hist[1]), 1);

    // Frame semantics (locked): the present counts INTO the frame it closes; the FIRST mark only
    // OPENS timing (present-to-present needs two) and drops the bring-up ops from frame totals.
    p.frame_mark(1000);
    VKR_CHECK_EQ(static_cast<long long>(p.frames), 0ll);
    p.record(op_qs, 10, 10, 40); // one op inside the first timed frame
    p.frame_mark(5000);          // closes it: 4000us, 1 op, 20 bytes
    VKR_CHECK_EQ(static_cast<long long>(p.frames), 1ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_us_total), 4000ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_us_min), 4000ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_us_max), 4000ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_ops_total), 1ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_bytes_total), 20ll);
    p.frame_mark(6000); // an empty 1000us frame -- the report's FPS math: 2 / (5000us/1e6) = 400
    VKR_CHECK_EQ(static_cast<long long>(p.frames), 2ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_us_total), 5000ll);
    VKR_CHECK_EQ(static_cast<long long>(p.frame_us_min), 1000ll);

    // Dump grammar: fixed prefix, key=value, no unescaped spaces in values; op/unknown/frame
    // lines all present; the count reflects all three queue_submit records.
    std::vector<std::string> lines;
    p.dump([&](const std::string& l) { lines.push_back(l); });
    bool saw_qs = false;
    bool saw_unknown = false;
    bool saw_frames = false;
    for (const std::string& l : lines) {
        VKR_CHECK(l.rfind("VKRELAY2-PROFILE end=client ", 0) == 0);
        if (l.find(" op=queue_submit ") != std::string::npos) {
            saw_qs = true;
            VKR_CHECK(l.find(" count=3 ") != std::string::npos);
        }
        if (l.find(" op=op_500 ") != std::string::npos) {
            VKR_CHECK(false); // out-of-range records dump as op=unknown, not a synthetic slot
        }
        if (l.find(" op=unknown ") != std::string::npos) {
            saw_unknown = true;
        }
        if (l.find(" frames=2 ") != std::string::npos) {
            saw_frames = true;
        }
    }
    VKR_CHECK(saw_qs && saw_unknown && saw_frames);

    // The worker end label rides the used_as_worker flag.
    vkrpc::RpcProfile w;
    w.used_as_worker = true;
    w.record(1, 1, 1, 1);
    std::vector<std::string> wl;
    w.dump([&](const std::string& l) { wl.push_back(l); });
    VKR_CHECK(!wl.empty() && wl[0].rfind("VKRELAY2-PROFILE end=worker ", 0) == 0);

    // Ring (VKRELAY2_PROFILE_FRAMES): fixed window keeps the LAST kProfileFrameRing frames;
    // ring_total keeps counting past the wrap.
    vkrpc::RpcProfile r;
    r.ring_enabled = true;
    r.frame_mark(0);
    for (std::uint64_t i = 1; i <= vkrpc::kProfileFrameRing + 2; ++i) {
        r.frame_mark(i * 10); // every frame exactly 10us
    }
    VKR_CHECK_EQ(static_cast<long long>(r.ring_total),
                 static_cast<long long>(vkrpc::kProfileFrameRing + 2));
    std::vector<std::string> rl;
    r.dump([&](const std::string& l) { rl.push_back(l); });
    bool saw_ring = false;
    for (const std::string& l : rl) {
        if (l.find(" frame_ring_us=") != std::string::npos) {
            saw_ring = true;
            // The retained window is full-size and every entry is the 10us cadence.
            VKR_CHECK(l.find("frame_ring_us=10,10,") != std::string::npos);
        }
    }
    VKR_CHECK(saw_ring);
}

// the async-signal-safe dump path. format_dump must be BYTE-IDENTICAL to
// dump() (one grammar, one strict parser), the cstr op-name table must agree with the
// allocating one, and truncation must drop whole lines only (a partial line would break the
// strict profile_report.sh parser at exactly the moment it matters -- a killed run).
void test_rpc_profile_signal_safe_dump() {
    // cstr/name agreement over the full table incl. the retired gap + out-of-range.
    for (std::uint32_t op = 0; op < vkrpc::kProfileMaxOps; ++op) {
        const char* c = vkrpc::profile_op_name_cstr(op);
        if (c != nullptr) {
            VKR_CHECK(vkrpc::profile_op_name(op) == c);
        } else {
            VKR_CHECK(vkrpc::profile_op_name(op) == "op_" + std::to_string(op));
        }
    }
    VKR_CHECK(vkrpc::profile_op_name_cstr(25) == nullptr);  // the retired opcode gap
    VKR_CHECK(vkrpc::profile_op_name_cstr(500) == nullptr); // future/unknown

    // A profile exercising every record type: known ops, a GAP slot (op 25 -- forces the op_<n>
    // formatting branch), the unknown slot, u64-extreme durations, frames, and a wrapped ring.
    vkrpc::RpcProfile p;
    p.ring_enabled = true;
    p.record(static_cast<std::uint32_t>(vkrpc::RpcOp::QueueSubmit), 100, 5, 70);
    p.record(static_cast<std::uint32_t>(vkrpc::RpcOp::RecordCommandBuffer), 1u << 20, 3, 0);
    p.record(25, 1, 2, 3);                      // gap slot -> "op=op_25"
    p.record(999, 7, 8, ~0ull);                 // unknown slot + u64 max duration
    p.record(0, 0, 0, 18446744073709551615ull); // op 0 ("invalid") + max u64 again
    p.record_phases.count = 2;                  // the phase-split line
    p.record_phases.commands = 340;
    p.record_phases.json_parse_us = 1200;
    p.record_phases.decode_us = 900;
    p.record_phases.validate_us = 300;
    p.record_phases.replay_us = 4000;
    p.record_phases.execute_us = 4500;
    p.upload_sweep.count = 3; // The ICD sweep line.
    p.upload_sweep.scan_bytes = 300 << 20;
    p.upload_sweep.filtered_bytes = 5 << 20;
    p.upload_sweep.payload_bytes = 1234567;
    p.upload_sweep.us = 98765;
    p.frame_mark(1000);
    for (std::uint64_t i = 1; i <= vkrpc::kProfileFrameRing + 3; ++i) {
        p.record(static_cast<std::uint32_t>(vkrpc::RpcOp::QueuePresent), 10, 10, 50);
        p.frame_mark(1000 + i * 4321);
    }

    // Byte identity: dump() lines joined with '\n' == format_dump output.
    std::string expect;
    p.dump([&](const std::string& l) {
        expect += l;
        expect += '\n';
    });
    static char buf[96 * 1024];
    const std::size_t n = p.format_dump(buf, sizeof(buf));
    VKR_CHECK_EQ(static_cast<long long>(n), static_cast<long long>(expect.size()));
    VKR_CHECK(std::string(buf, n) == expect);

    // The phase-split line carries every key (grammar pinned: profile_report.sh dies on a
    // missing one) and appears in BOTH dump paths (identity above already proves format_dump).
    VKR_CHECK(expect.find(" record_phases=2 commands=340 json_parse_us=1200 decode_us=900 "
                          "validate_us=300 replay_us=4000 execute_us=4500") != std::string::npos);
    // Same pin for the upload-sweep line (all keys, one line, both dump paths).
    VKR_CHECK(expect.find(" upload_sweep=3 scan_bytes=314572800 filtered_bytes=5242880 "
                          "payload_bytes=1234567 sweep_us=98765") != std::string::npos);
    {
        // And a profile with no measured records emits NO phase/sweep line.
        vkrpc::RpcProfile q;
        q.record(1, 1, 1, 1);
        std::string qd;
        q.dump([&](const std::string& l) {
            qd += l;
            qd += '\n';
        });
        VKR_CHECK(qd.find("record_phases=") == std::string::npos);
        VKR_CHECK(qd.find("upload_sweep=") == std::string::npos);
    }

    // The worker label rides through the same path.
    vkrpc::RpcProfile w;
    w.used_as_worker = true;
    w.record(1, 1, 1, 1);
    const std::size_t wn = w.format_dump(buf, sizeof(buf));
    VKR_CHECK(std::string(buf, wn).rfind("VKRELAY2-PROFILE end=worker ", 0) == 0);

    // Truncation drops WHOLE lines: for every capacity, the output is a byte-prefix of the
    // full dump consisting only of complete lines (empty or '\n'-terminated), monotonically
    // covering more lines as the capacity grows.
    for (std::size_t cap : {std::size_t{0}, std::size_t{10}, std::size_t{80}, std::size_t{200},
                            expect.size() - 1, expect.size()}) {
        const std::size_t tn = p.format_dump(buf, cap);
        VKR_CHECK(tn <= cap);
        VKR_CHECK(tn == 0 || buf[tn - 1] == '\n');
        VKR_CHECK(expect.compare(0, tn, buf, tn) == 0); // a prefix of the one true dump
    }
    VKR_CHECK(p.format_dump(buf, expect.size() - 1) < expect.size()); // one line short, parseable
}

// the binary record body (RpcOp::RecordCommandBufferRaw). The pin that matters:
// from_wire(to_wire(req)) is FIELD-IDENTICAL to the request the JSON path decodes -- the
// validate-then-record boundary must see the same stream whichever codec carried it.
void test_record_raw_wire() {
    vkrpc::RecordCommandBufferRequest req;
    req.command_buffer = 0x1122334455667788ull;
    req.one_time_submit = true;
    vkrpc::RecordedCommand c;
    c.kind = "begin_render_pass";
    c.image = 0xAABBCCDDEEFF0011ull;
    c.old_layout = 5;
    c.new_layout = 7;
    c.src_stage = 0x10000;
    c.dst_stage = 0x400;
    c.src_access = 0x20;
    c.dst_access = 0x40;
    c.aspect = 1;
    c.layout = 2;
    c.r = 0.25;
    c.g = -1.5;
    c.b = 3.75;
    c.a = 1.0;
    c.render_pass = 0x101;
    c.framebuffer = 0x202;
    c.has_depth_clear = true;
    c.depth_clear = 0.5;
    c.pipeline = 0x303;
    c.render_area_x = -3;
    c.render_area_y = 4;
    c.render_area_w = 640;
    c.render_area_h = 480;
    c.vp_x = 1.5;
    c.vp_y = 2.5;
    c.vp_w = 640.0;
    c.vp_h = 480.0;
    c.vp_min_depth = 0.0;
    c.vp_max_depth = 1.0;
    c.sc_x = 1;
    c.sc_y = 2;
    c.sc_w = 3;
    c.sc_h = 4;
    c.vertex_count = 36;
    c.instance_count = 1;
    c.first_vertex = 0;
    c.first_instance = 0;
    c.first_binding = 0;
    c.vertex_buffers = {0x404, 0x505};
    c.vertex_buffer_offsets = {0, 256};
    c.desc_layout = 0x606;
    c.first_set = 0;
    c.descriptor_sets = {0x707};
    c.src_buffer = 0x808;
    c.copy_width = 64;
    c.copy_height = 32;
    c.copy_depth = 1;
    c.barrier_base_mip = 0;
    c.barrier_level_count = 3;
    c.barrier_base_layer = 1;
    c.barrier_layer_count = 2;
    c.args_u64 = {1, 2, 0xFFFFFFFFFFFFFFFFull};
    c.args_i64 = {-1, 42, 1ll << 40};
    c.args_f64 = {0.5, -2.25};
    c.args_blob = std::string("\x00\xFF\x7F\x80zz", 6); // NUL + high bytes survive raw
    vkrpc::RecordedCommand d;                           // a mostly-default second command
    d.kind = "end_render_pass";
    vkrpc::RecordedCommand indirect;
    indirect.kind = "draw_indirect";
    indirect.src_buffer = 0x909;
    indirect.indirect_offset = 12;
    indirect.indirect_draw_count = 2;
    indirect.indirect_stride = 16;
    vkrpc::RecordedCommand indexed_indirect = indirect;
    indexed_indirect.kind = "draw_indexed_indirect";
    indexed_indirect.src_buffer = 0xA0A;
    indexed_indirect.indirect_offset = 20;
    indexed_indirect.indirect_draw_count = 3;
    indexed_indirect.indirect_stride = 20;
    vkrpc::RecordedCommand indirect_count =
        vkrpc::make_core_indirect_count_draw_command(0xB0B, 24, 0xC0C, 8, 4, 16, false);
    vkrpc::RecordedCommand indexed_indirect_count =
        vkrpc::make_core_indirect_count_draw_command(0xD0D, 40, 0xE0E, 12, 5, 20, true);
    req.commands = {c, d, indirect, indexed_indirect, indirect_count, indexed_indirect_count};

    const std::string wire = req.to_wire();
    std::string err;
    const auto back = vkrpc::RecordCommandBufferRequest::from_wire(wire, err);
    VKR_CHECK(err.empty());
    // THE parity pin: both codecs land the identical struct (compared via the JSON projection,
    // which covers every field both paths carry).
    VKR_CHECK(back.to_body().dump(0) == req.to_body().dump(0));
    const auto via_json = vkrpc::RecordCommandBufferRequest::from_body(req.to_body());
    VKR_CHECK(via_json.to_body().dump(0) == back.to_body().dump(0));
    VKR_CHECK(back.commands[0].args_blob == c.args_blob); // bytes, not hex, still exact
    VKR_CHECK_EQ(back.commands[2].src_buffer, 0x909ull);
    VKR_CHECK_EQ(back.commands[2].indirect_offset, 12ull);
    VKR_CHECK_EQ(back.commands[2].indirect_draw_count, 2ll);
    VKR_CHECK_EQ(back.commands[2].indirect_stride, 16ll);
    VKR_CHECK_EQ(back.commands[3].src_buffer, 0xA0Aull);
    VKR_CHECK(vkrpc::cmd_kind_from_string(back.commands[2].kind) == vkrpc::CmdKind::DrawIndirect);
    VKR_CHECK(vkrpc::cmd_kind_from_string(back.commands[3].kind) ==
              vkrpc::CmdKind::DrawIndexedIndirect);
    VKR_CHECK_EQ(back.commands[4].kind, std::string("draw_indirect_count"));
    VKR_CHECK_EQ(back.commands[4].indirect_count_buffer, 0xC0Cull);
    VKR_CHECK_EQ(back.commands[4].indirect_count_buffer_offset, 8ull);
    VKR_CHECK_EQ(back.commands[4].indirect_draw_count, 4ll);
    VKR_CHECK_EQ(back.commands[5].kind, std::string("draw_indexed_indirect_count"));
    // New-to-new: the ICD hot-path spelling owns no kind string or payload vectors; the codec
    // emits the canonical kind and the worker decodes an ordinary command.
    vkrpc::RecordCommandBufferRequest hot_path;
    vkrpc::RecordedCommand hot_indexed =
        vkrpc::make_core_indirect_draw_command(0xA1A, 4, 1, 20, true, true);
    hot_path.commands = {hot_indexed};
    const auto hot_back = vkrpc::RecordCommandBufferRequest::from_wire(hot_path.to_wire(), err);
    VKR_CHECK(err.empty());
    VKR_CHECK(hot_indexed.kind.empty());
    VKR_CHECK(hot_indexed.args_u64.empty() && hot_indexed.args_i64.empty());
    VKR_CHECK_EQ(hot_back.commands[0].kind, std::string("draw_indexed_indirect"));
    VKR_CHECK_EQ(hot_back.commands[0].indirect_offset, 4ull);
    // Count producers are allocation-free too, and their raw shape always has frozen base group
    // 16 plus dedicated group 17. Base producers never set group 17.
    vkrpc::RecordCommandBufferRequest hot_count;
    vkrpc::RecordedCommand hot_counted =
        vkrpc::make_core_indirect_count_draw_command(0x111, 4, 0x222, 8, 2, 20, true);
    hot_count.commands = {hot_counted};
    const auto read_u64 = [](const std::string& bytes, std::size_t at) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[at + i]))
                     << (i * 8);
        }
        return value;
    };
    const auto one_command_mask_offset = [&](const std::string& bytes) {
        const std::uint32_t json_size =
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
        const std::size_t kind_len_at = 4 + json_size;
        return kind_len_at + 8 + static_cast<std::size_t>(read_u64(bytes, kind_len_at));
    };
    const std::string base_hot_wire = hot_path.to_wire();
    const std::string count_hot_wire = hot_count.to_wire();
    const std::uint64_t base_mask = read_u64(base_hot_wire, one_command_mask_offset(base_hot_wire));
    const std::uint64_t count_mask =
        read_u64(count_hot_wire, one_command_mask_offset(count_hot_wire));
    VKR_CHECK((base_mask & (std::uint64_t{1} << 16)) != 0);
    VKR_CHECK((base_mask & (std::uint64_t{1} << 17)) == 0);
    VKR_CHECK((count_mask & (std::uint64_t{1} << 16)) != 0);
    VKR_CHECK((count_mask & (std::uint64_t{1} << 17)) != 0);
    const auto count_back = vkrpc::RecordCommandBufferRequest::from_wire(count_hot_wire, err);
    VKR_CHECK(err.empty());
    VKR_CHECK(hot_counted.kind.empty() && hot_counted.args_u64.empty() &&
              hot_counted.args_i64.empty());
    VKR_CHECK_EQ(count_back.commands[0].kind, std::string("draw_indexed_indirect_count"));
    vkrpc::CoreIndirectCountDrawArgs count_args;
    const char* count_reason = "";
    VKR_CHECK(
        vkrpc::core_indirect_count_draw_args(count_back.commands[0], count_args, &count_reason));
    VKR_CHECK_EQ(count_args.count_buffer, 0x222ull);
    VKR_CHECK_EQ(count_args.count_buffer_offset, 8ull);
    auto mixed_count = count_back.commands[0];
    mixed_count.args_u64 = {0};
    VKR_CHECK(!vkrpc::core_indirect_count_draw_args(mixed_count, count_args, &count_reason));
    auto mixed_base = hot_back.commands[0];
    mixed_base.indirect_count_buffer = 0x333;
    vkrpc::CoreIndirectDrawArgs base_args;
    VKR_CHECK(!vkrpc::core_indirect_draw_args(mixed_base, base_args, &count_reason));

    // Dedicated fail-closed pins for group 17: truncation, an unknown next group, a base kind
    // carrying it, and a count kind missing it.
    (void) vkrpc::RecordCommandBufferRequest::from_wire(
        count_hot_wire.substr(0, count_hot_wire.size() - 1), err);
    VKR_CHECK(!err.empty());
    auto write_u64 = [](std::string& bytes, std::size_t at, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            bytes[at + i] = static_cast<char>((value >> (i * 8)) & 0xFF);
        }
    };
    std::string unknown_group = count_hot_wire;
    const std::size_t count_mask_at = one_command_mask_offset(unknown_group);
    write_u64(unknown_group, count_mask_at, count_mask | (std::uint64_t{1} << 18));
    (void) vkrpc::RecordCommandBufferRequest::from_wire(unknown_group, err);
    VKR_CHECK(!err.empty());
    std::string missing_group = count_hot_wire;
    write_u64(missing_group, count_mask_at, count_mask & ~(std::uint64_t{1} << 17));
    (void) vkrpc::RecordCommandBufferRequest::from_wire(missing_group, err);
    VKR_CHECK(!err.empty());
    std::string base_with_group = base_hot_wire;
    const std::size_t base_mask_at = one_command_mask_offset(base_with_group);
    write_u64(base_with_group, base_mask_at, base_mask | (std::uint64_t{1} << 17));
    base_with_group.append(16, '\0');
    (void) vkrpc::RecordCommandBufferRequest::from_wire(base_with_group, err);
    VKR_CHECK(!err.empty());
    // New ICD -> milestone-A worker: the absent scalar-payload capability selects exactly the old
    // positional spelling, while a new worker continues to accept it in the opposite skew.
    vkrpc::RecordedCommand legacy_indirect =
        vkrpc::make_core_indirect_draw_command(0xB0B, 28, 1, 16, false, false);
    VKR_CHECK_EQ(legacy_indirect.kind, std::string("draw_indirect"));
    VKR_CHECK_EQ(legacy_indirect.src_buffer, 0xB0Bull);
    VKR_CHECK(legacy_indirect.indirect_draw_count == -1 && legacy_indirect.indirect_stride == -1);
    VKR_CHECK(legacy_indirect.args_u64 == std::vector<std::uint64_t>{28});
    VKR_CHECK(legacy_indirect.args_i64 == std::vector<long long>({1, 16}));
    vkrpc::CoreIndirectDrawArgs legacy_args;
    const char* legacy_reason = "";
    VKR_CHECK(vkrpc::core_indirect_draw_args(legacy_indirect, legacy_args, &legacy_reason));
    VKR_CHECK_EQ(legacy_args.offset, 28ull);
    VKR_CHECK_EQ(legacy_args.draw_count, 1ll);
    VKR_CHECK_EQ(legacy_args.stride, 16ll);
    // i64 exactness: the wire is BIT-exact where JSON's number path would round (> 2^53).
    vkrpc::RecordCommandBufferRequest big;
    vkrpc::RecordedCommand bc;
    bc.kind = "x";
    bc.args_i64 = {(1ll << 60) + 12345};
    big.commands = {bc};
    const auto big2 = vkrpc::RecordCommandBufferRequest::from_wire(big.to_wire(), err);
    VKR_CHECK(err.empty() && big2.commands[0].args_i64[0] == (1ll << 60) + 12345);

    // Fail-closed: EVERY strict prefix of a valid wire is rejected (no partial acceptance)...
    for (std::size_t n = 0; n < wire.size(); ++n) {
        vkrpc::RecordCommandBufferRequest t =
            vkrpc::RecordCommandBufferRequest::from_wire(wire.substr(0, n), err);
        VKR_CHECK(!err.empty());
        VKR_CHECK(t.commands.empty()); // a failed decode returns an EMPTY request
    }
    // ...as are trailing bytes, an unknown version, and an alloc-bomb count.
    vkrpc::RecordCommandBufferRequest t =
        vkrpc::RecordCommandBufferRequest::from_wire(wire + "x", err);
    VKR_CHECK(!err.empty() && t.commands.empty());
    {
        json::Value h = json::Value::make_object();
        h.set("v", json::Value(3)); // future version
        h.set("count", json::Value(0ll));
        const std::string hj = h.dump(0);
        std::string w(4, '\0');
        w[0] = static_cast<char>(hj.size());
        w += hj;
        vkrpc::RecordCommandBufferRequest v3 = vkrpc::RecordCommandBufferRequest::from_wire(w, err);
        VKR_CHECK(!err.empty() && v3.commands.empty());
    }
    {
        json::Value h = json::Value::make_object();
        h.set("v", json::Value(1)); // the retired DENSE format: stale peers fail closed
        h.set("count", json::Value(0ll));
        const std::string hj = h.dump(0);
        std::string w(4, '\0');
        w[0] = static_cast<char>(hj.size());
        w += hj;
        vkrpc::RecordCommandBufferRequest v1 = vkrpc::RecordCommandBufferRequest::from_wire(w, err);
        VKR_CHECK(!err.empty() && v1.commands.empty());
    }
    {
        json::Value h = json::Value::make_object();
        h.set("v", json::Value(2));
        h.set("count", json::Value(1000000000ll)); // absurd count vs a tiny body
        const std::string hj = h.dump(0);
        std::string w(4, '\0');
        w[0] = static_cast<char>(hj.size());
        w += hj;
        vkrpc::RecordCommandBufferRequest bomb =
            vkrpc::RecordCommandBufferRequest::from_wire(w, err);
        VKR_CHECK(!err.empty() && bomb.commands.empty());
    }
    {
        // sparse pins: a DEFAULT-field command round-trips as kind + empty mask (identity
        // by construction), and an unknown presence bit fails closed.
        vkrpc::RecordCommandBufferRequest sparse;
        vkrpc::RecordedCommand d0;
        d0.kind = "end_render_pass"; // every field at its default -> mask 0
        sparse.commands = {d0};
        const std::string sw = sparse.to_wire();
        const auto sback = vkrpc::RecordCommandBufferRequest::from_wire(sw, err);
        VKR_CHECK(err.empty());
        VKR_CHECK(sback.to_body().dump(0) == sparse.to_body().dump(0));
        std::string bad = sw;
        // Flip an unknown high mask bit in the LAST 8 bytes (the mask of the only command).
        bad[bad.size() - 1] = static_cast<char>(0x80);
        vkrpc::RecordCommandBufferRequest ub =
            vkrpc::RecordCommandBufferRequest::from_wire(bad, err);
        VKR_CHECK(!err.empty() && ub.commands.empty());
    }
    // DeviceCaps.raw_record: additive like raw_readback (absent = old worker -> false).
    vkrpc::DeviceCaps caps;
    caps.raw_record = true;
    VKR_CHECK(vkrpc::DeviceCaps::from_body(caps.to_body()).raw_record);
    json::Value old_caps = json::Value::make_object();
    old_caps.set("device_name", json::Value(std::string("x")));
    VKR_CHECK(!vkrpc::DeviceCaps::from_body(old_caps).raw_record);
}

// --- Compute pipelines + dispatch --------------------------------------------------
// The full mock chain: caps honesty, create subset, pipeline KIND vs bind point, per-bind-point
// state (interleaved graphics/compute binds never disturb each other),
// dispatch readiness/limits (a zero dimension is a LEGAL no-op), indirect misuse, and the new
// global/buffer barrier kinds. Codecs: the new op round-trips; queue_flags is additive.
void test_compute_dispatch() {
    // Codec pins first (no backend): request/response round-trip + additive caps.
    {
        vkrpc::CreateComputePipelinesRequest r;
        r.device = 7;
        r.pipeline_cache = 0;
        r.layout = 9;
        r.shader_module = 11;
        r.entry_point = "main";
        const auto rt = vkrpc::CreateComputePipelinesRequest::from_body(r.to_body());
        VKR_CHECK(rt.device == 7 && rt.layout == 9 && rt.shader_module == 11 &&
                  rt.entry_point == "main" && rt.pipeline_cache == 0);
        vkrpc::CreateComputePipelinesResponse p;
        p.ok = true;
        p.reason = "ok";
        p.pipeline = 42;
        const auto pt = vkrpc::CreateComputePipelinesResponse::from_body(p.to_body());
        VKR_CHECK(pt.ok && pt.pipeline == 42);
        // queue_flags: additive (absent -> 0; present -> the wide value round-trips).
        vkrpc::DeviceCaps caps;
        caps.queue_flags = 0x7;
        VKR_CHECK(vkrpc::DeviceCaps::from_body(caps.to_body()).queue_flags == 0x7);
        json::Value legacy = json::Value::make_object(); // an old worker's caps body
        legacy.set("device_name", json::Value(std::string("old")));
        VKR_CHECK(vkrpc::DeviceCaps::from_body(legacy).queue_flags == 0);
        // The dispatch/bind-point/barrier kinds ride the FROZEN raw record codec's args
        // fields -- pin one through both codecs (to_body parity).
        vkrpc::RecordCommandBufferRequest rr;
        rr.command_buffer = 5;
        vkrpc::RecordedCommand dc;
        dc.kind = "dispatch";
        dc.args_u64 = {64, 1, 1};
        vkrpc::RecordedCommand bc;
        bc.kind = "bind_pipeline";
        bc.pipeline = 3;
        bc.args_i64 = {1}; // COMPUTE
        vkrpc::RecordedCommand mb;
        mb.kind = "memory_barrier";
        mb.src_stage = 0x800; // COMPUTE_SHADER
        mb.dst_stage = 0x1000;
        mb.src_access = 0x40;
        mb.dst_access = 0x800;
        rr.commands = {bc, dc, mb};
        std::string err;
        const auto rw = vkrpc::RecordCommandBufferRequest::from_wire(rr.to_wire(), err);
        VKR_CHECK(err.empty());
        VKR_CHECK(rw.to_body().dump(0) == rr.to_body().dump(0));
    }

    const std::vector<protocol::GpuDevice> devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    const vkrpc::CreateInstanceResponse ci = backend.create_instance({});
    vkrpc::EnumeratePhysicalDevicesRequest er;
    er.instance = ci.instance;
    const auto en = backend.enumerate_physical_devices(er);
    // Caps honesty: the mock's one family advertises GRAPHICS|COMPUTE|TRANSFER.
    VKR_CHECK_EQ(en.devices.front().caps.queue_flags, static_cast<std::uint64_t>(0x7));
    const std::uint64_t phys = en.devices.front().handle;
    vkrpc::CreateDeviceRequest cdr;
    cdr.instance = ci.instance;
    cdr.physical_device = phys;
    const vkrpc::CreateDeviceResponse cd = backend.create_device(cdr);
    vkrpc::CreateCommandPoolRequest cpr;
    cpr.device = cd.device;
    cpr.queue_family_index = cd.queue_family_index;
    const std::uint64_t pool_cmd = backend.create_command_pool(cpr).command_pool;
    vkrpc::AllocateCommandBuffersRequest acb;
    acb.command_pool = pool_cmd;
    acb.count = 1;
    const std::uint64_t cmd = backend.allocate_command_buffers(acb).command_buffers.front();

    // A dummy SPIR-V module (the mock does not parse code) + layouts.
    vkrpc::CreateShaderModuleRequest smr;
    smr.device = cd.device;
    smr.code = std::string(16, '\x1');
    smr.code_size = smr.code.size();
    const std::uint64_t cs = backend.create_shader_module(smr).shader_module;
    VKR_CHECK(cs != 0);

    // Coherent memory + the compute buffers: two STORAGE, one UNIFORM, one INDIRECT, and a
    // VERTEX-only one (for the indirect-usage reject).
    vkrpc::GetPhysicalDeviceMemoryPropertiesRequest mpq;
    mpq.physical_device = phys;
    const auto mp = backend.get_physical_device_memory_properties(mpq);
    int coherent_type = -1;
    for (std::size_t i = 0; i < mp.types.size(); ++i) {
        const std::uint64_t want =
            vkrpc::kMemoryPropertyHostVisible | vkrpc::kMemoryPropertyHostCoherent;
        if ((mp.types[i].property_flags & want) == want) {
            coherent_type = static_cast<int>(i);
        }
    }
    VKR_CHECK(coherent_type >= 0);
    vkrpc::AllocateMemoryRequest am;
    am.device = cd.device;
    am.allocation_size = 256 * 1024;
    am.memory_type_index = coherent_type;
    const std::uint64_t mem = backend.allocate_memory(am).memory;
    std::uint64_t next_off = 0;
    auto make_buf = [&](std::uint64_t size, std::uint64_t usage) {
        vkrpc::CreateBufferRequest r;
        r.device = cd.device;
        r.size = size;
        r.usage = usage;
        r.sharing_mode = 0;
        const std::uint64_t b = backend.create_buffer(r).buffer;
        vkrpc::BindBufferMemoryRequest bb;
        bb.buffer = b;
        bb.memory = mem;
        bb.memory_offset = next_off;
        next_off += (size + 255) & ~std::uint64_t{255};
        VKR_CHECK(backend.bind_buffer_memory(bb).ok);
        return b;
    };
    const std::uint64_t ssbo_in = make_buf(4096, vkrpc::kBufferUsageStorageBuffer);
    const std::uint64_t ssbo_out = make_buf(4096, vkrpc::kBufferUsageStorageBuffer);
    const std::uint64_t ubo = make_buf(256, vkrpc::kBufferUsageUniformBuffer);
    const std::uint64_t indirect = make_buf(64, vkrpc::kBufferUsageIndirectBuffer);
    const std::uint64_t vbo_only = make_buf(64, vkrpc::kBufferUsageVertexBuffer);

    // Set layout {0: STORAGE, 1: STORAGE, 2: UNIFORM} -> pool -> set -> writes.
    vkrpc::CreateDescriptorSetLayoutRequest dslr;
    dslr.device = cd.device;
    for (int b = 0; b < 3; ++b) {
        vkrpc::DescriptorSetLayoutBindingDesc d;
        d.binding = b;
        d.descriptor_type =
            b == 2 ? vkrpc::kDescriptorTypeUniformBuffer : vkrpc::kDescriptorTypeStorageBuffer;
        d.descriptor_count = 1;
        d.stage_flags = 0x20; // COMPUTE
        dslr.bindings.push_back(d);
    }
    const std::uint64_t dsl = backend.create_descriptor_set_layout(dslr).set_layout;
    vkrpc::CreateDescriptorPoolRequest dpr;
    dpr.device = cd.device;
    dpr.max_sets = 2;
    dpr.pool_sizes.push_back({vkrpc::kDescriptorTypeStorageBuffer, 4});
    dpr.pool_sizes.push_back({vkrpc::kDescriptorTypeUniformBuffer, 2});
    const std::uint64_t dpool = backend.create_descriptor_pool(dpr).pool;
    vkrpc::AllocateDescriptorSetsRequest adr;
    adr.device = cd.device;
    adr.pool = dpool;
    adr.set_layouts = {dsl};
    const std::uint64_t dset = backend.allocate_descriptor_sets(adr).descriptor_sets.front();
    {
        vkrpc::UpdateDescriptorSetsRequest ur;
        ur.device = cd.device;
        const std::uint64_t bufs[3] = {ssbo_in, ssbo_out, ubo};
        for (int b = 0; b < 3; ++b) {
            vkrpc::WriteDescriptorSetDesc w;
            w.dst_set = dset;
            w.dst_binding = b;
            w.dst_array_element = 0;
            w.descriptor_type =
                b == 2 ? vkrpc::kDescriptorTypeUniformBuffer : vkrpc::kDescriptorTypeStorageBuffer;
            w.descriptor_count = 1;
            w.buffer_infos.push_back({bufs[b], 0, b == 2 ? 256u : 4096u});
            ur.writes.push_back(w);
        }
        VKR_CHECK(backend.update_descriptor_sets(ur).ok);
    }

    // Pipeline layouts: the 1-set compute layout and an EMPTY one (imposes no set requirement).
    vkrpc::CreatePipelineLayoutRequest plr;
    plr.device = cd.device;
    plr.set_layout_count = 1;
    plr.push_constant_range_count = 0;
    plr.set_layouts = {dsl};
    const std::uint64_t pl = backend.create_pipeline_layout(plr).pipeline_layout;
    vkrpc::CreatePipelineLayoutRequest plr0;
    plr0.device = cd.device;
    plr0.set_layout_count = 0;
    plr0.push_constant_range_count = 0;
    const std::uint64_t pl_empty = backend.create_pipeline_layout(plr0).pipeline_layout;

    // Compute pipeline create + the reject set.
    auto make_cp = [&]() {
        vkrpc::CreateComputePipelinesRequest r;
        r.device = cd.device;
        r.pipeline_cache = 0;
        r.layout = pl;
        r.shader_module = cs;
        r.entry_point = "main";
        return r;
    };
    const vkrpc::CreateComputePipelinesResponse cp = backend.create_compute_pipelines(make_cp());
    VKR_CHECK(cp.ok && cp.pipeline != 0);
    auto cp_empty_req = make_cp();
    cp_empty_req.layout = pl_empty;
    const std::uint64_t cp_empty = backend.create_compute_pipelines(cp_empty_req).pipeline;
    VKR_CHECK(cp_empty != 0);
    {
        auto r = make_cp();
        r.pipeline_cache = 99; // must be 0
        VKR_CHECK(!backend.create_compute_pipelines(r).ok);
        r = make_cp();
        r.shader_module = 0xDEAD;
        VKR_CHECK(!backend.create_compute_pipelines(r).ok);
        r = make_cp();
        r.layout = 0xDEAD;
        VKR_CHECK(!backend.create_compute_pipelines(r).ok);
        r = make_cp();
        r.entry_point.clear();
        VKR_CHECK(!backend.create_compute_pipelines(r).ok);
    }

    // Recorded-command helpers.
    auto bind_cp = [&](std::uint64_t pipeline) {
        vkrpc::RecordedCommand c;
        c.kind = "bind_pipeline";
        c.pipeline = pipeline;
        c.args_i64 = {1}; // COMPUTE
        return c;
    };
    auto bind_sets_compute = [&]() {
        vkrpc::RecordedCommand c;
        c.kind = "bind_descriptor_sets";
        c.desc_layout = pl;
        c.first_set = 0;
        c.descriptor_sets = {dset};
        c.args_i64 = {1}; // COMPUTE point
        return c;
    };
    auto dispatch3 = [&](std::uint64_t x, std::uint64_t y, std::uint64_t z) {
        vkrpc::RecordedCommand c;
        c.kind = "dispatch";
        c.args_u64 = {x, y, z};
        return c;
    };
    auto record = [&](std::vector<vkrpc::RecordedCommand> cmds) {
        vkrpc::RecordCommandBufferRequest r;
        r.command_buffer = cmd;
        r.commands = std::move(cmds);
        return backend.record_command_buffer(r);
    };

    // Happy path: bind(COMPUTE) + sets(COMPUTE) + dispatch. And a ZERO dimension is legal.
    VKR_CHECK(record({bind_cp(cp.pipeline), bind_sets_compute(), dispatch3(64, 1, 1)}).ok);
    VKR_CHECK(record({bind_cp(cp.pipeline), bind_sets_compute(), dispatch3(0, 1, 1)}).ok);
    // Over the spec-minimum limit rejects.
    VKR_CHECK(!record({bind_cp(cp.pipeline), bind_sets_compute(), dispatch3(65536, 1, 1)}).ok);
    // No pipeline bound / no descriptor bind / GRAPHICS-point-only descriptor bind all reject
    // (the last is THE per-bind-point pin: a graphics-point bind must not satisfy a dispatch).
    VKR_CHECK(!record({dispatch3(1, 1, 1)}).ok);
    VKR_CHECK(!record({bind_cp(cp.pipeline), dispatch3(1, 1, 1)}).ok);
    {
        auto graphics_point_sets = bind_sets_compute();
        graphics_point_sets.args_i64 = {0}; // GRAPHICS point
        VKR_CHECK(!record({bind_cp(cp.pipeline), graphics_point_sets, dispatch3(1, 1, 1)}).ok);
    }
    // An empty-layout pipeline imposes no descriptor requirement.
    VKR_CHECK(record({bind_cp(cp_empty), dispatch3(1, 1, 1)}).ok);
    // KIND mismatch: the compute pipeline bound at the GRAPHICS point rejects at the bind.
    {
        auto wrong = bind_cp(cp.pipeline);
        wrong.args_i64 = {0};
        VKR_CHECK(!record({wrong}).ok);
        auto bad_point = bind_cp(cp.pipeline);
        bad_point.args_i64 = {2}; // not a bind point we carry
        VKR_CHECK(!record({bad_point}).ok);
    }

    // dispatch_indirect: valid; then usage/alignment/range rejects.
    auto dind = [&](std::uint64_t buffer, std::uint64_t off) {
        vkrpc::RecordedCommand c;
        c.kind = "dispatch_indirect";
        c.src_buffer = buffer;
        c.args_u64 = {off};
        return c;
    };
    VKR_CHECK(record({bind_cp(cp_empty), dind(indirect, 0)}).ok);
    VKR_CHECK(record({bind_cp(cp_empty), dind(indirect, 52)}).ok);  // 52+12 == 64
    VKR_CHECK(!record({bind_cp(cp_empty), dind(vbo_only, 0)}).ok);  // not INDIRECT usage
    VKR_CHECK(!record({bind_cp(cp_empty), dind(indirect, 2)}).ok);  // misaligned
    VKR_CHECK(!record({bind_cp(cp_empty), dind(indirect, 56)}).ok); // 56+12 > 64
    VKR_CHECK(!record({dind(indirect, 0)}).ok);                     // no compute pipeline

    // Barriers: the global and buffer kinds (valid + malformed + out-of-range).
    {
        vkrpc::RecordedCommand mb;
        mb.kind = "memory_barrier";
        mb.src_stage = 0x800; // COMPUTE_SHADER
        mb.dst_stage = 0x1000;
        mb.src_access = 0x40;
        mb.dst_access = 0x800;
        VKR_CHECK(record({mb}).ok);
        // zink emits ZERO stage masks (sync2 NONE semantics through the sync1 entry point) --
        // carried verbatim, the host driver is the authority. Pinned: accepted, not rejected.
        auto none_stage = mb;
        none_stage.src_stage = 0;
        none_stage.dst_stage = 0;
        VKR_CHECK(record({none_stage}).ok);
        auto bad = mb;
        bad.src_stage = 0x100000000LL; // exceeds 32-bit VkFlags: genuinely malformed
        VKR_CHECK(!record({bad}).ok);
        vkrpc::RecordedCommand bb;
        bb.kind = "buffer_memory_barrier";
        bb.src_buffer = ssbo_out;
        bb.src_stage = 0x800;
        bb.dst_stage = 0x1000;
        bb.src_access = 0x40;
        bb.dst_access = 0x800;
        bb.args_u64 = {0, ~0ull}; // WHOLE_SIZE
        VKR_CHECK(record({bb}).ok);
        auto oob = bb;
        oob.args_u64 = {4096, 4}; // offset == size
        VKR_CHECK(!record({oob}).ok);
        auto zero = bb;
        zero.args_u64 = {0, 0}; // zero size
        VKR_CHECK(!record({zero}).ok);
        auto unknown = bb;
        unknown.src_buffer = 0xDEAD;
        VKR_CHECK(!record({unknown}).ok);
    }
    // Dispatch after a shader-write -> transfer-read buffer barrier + copy out: the canary's
    // exact stream shape, accepted end to end.
    {
        vkrpc::RecordedCommand bb;
        bb.kind = "buffer_memory_barrier";
        bb.src_buffer = ssbo_out;
        bb.src_stage = 0x800;  // COMPUTE_SHADER
        bb.dst_stage = 0x1000; // TRANSFER
        bb.src_access = 0x40;  // SHADER_WRITE
        bb.dst_access = 0x800; // TRANSFER_READ
        bb.args_u64 = {0, ~0ull};
        vkrpc::RecordedCommand cp_cmd;
        cp_cmd.kind = "copy_buffer";
        cp_cmd.args_u64 = {ssbo_out, ssbo_in, 0, 0, 4096};
        VKR_CHECK(
            record({bind_cp(cp.pipeline), bind_sets_compute(), dispatch3(64, 1, 1), bb, cp_cmd})
                .ok);
    }

    // Interleave: an offscreen FBO + a graphics pipeline, then prove that
    // binding at one bind point never disturbs the other, BOTH directions.
    {
        vkrpc::CreateImageRequest cir;
        cir.device = cd.device;
        cir.image_type = vkrpc::kImageType2D;
        cir.format = 37; // RGBA8
        cir.width = 64;
        cir.height = 64;
        cir.depth = 1;
        cir.mip_levels = 1;
        cir.array_layers = 1;
        cir.samples = 1;
        cir.tiling = vkrpc::kImageTilingOptimal;
        cir.usage = vkrpc::kImageUsageColorAttachment;
        cir.sharing_mode = 0;
        cir.initial_layout = 0;
        const auto img = backend.create_image(cir);
        VKR_CHECK(img.ok);
        // An OPTIMAL image needs DEVICE_LOCAL memory (type 0), not the coherent upload type.
        vkrpc::AllocateMemoryRequest dl;
        dl.device = cd.device;
        dl.allocation_size = 4ull * 1024 * 1024;
        dl.memory_type_index = 0;
        const std::uint64_t img_mem = backend.allocate_memory(dl).memory;
        vkrpc::BindImageMemoryRequest bim;
        bim.image = img.image;
        bim.memory = img_mem;
        bim.memory_offset = 0;
        VKR_CHECK(backend.bind_image_memory(bim).ok);
        vkrpc::CreateImageViewRequest ivr;
        ivr.image = img.image;
        ivr.view_type = 1; // 2D
        ivr.format = 37;
        ivr.swizzle_r = 0;
        ivr.swizzle_g = 0;
        ivr.swizzle_b = 0;
        ivr.swizzle_a = 0;
        ivr.aspect = 1; // COLOR
        ivr.base_mip_level = 0;
        ivr.level_count = 1;
        ivr.base_array_layer = 0;
        ivr.layer_count = 1;
        const std::uint64_t view = backend.create_image_view(ivr).image_view;
        VKR_CHECK(view != 0);
        vkrpc::CreateRenderPassRequest rpr;
        rpr.device = cd.device;
        vkrpc::AttachmentDesc at;
        at.format = 37;
        at.samples = 1;
        at.load_op = 1;  // CLEAR
        at.store_op = 0; // STORE
        at.stencil_load_op = 2;
        at.stencil_store_op = 1;
        at.initial_layout = 0;
        at.final_layout = 2; // COLOR_ATTACHMENT_OPTIMAL
        rpr.attachments = {at};
        rpr.color_attachment = 0;
        rpr.color_layout = 2;
        rpr.color_refs = {{0, 2}}; // the faithful (MRT-era) path: offscreen final layouts legal
        const std::uint64_t rp = backend.create_render_pass(rpr).render_pass;
        VKR_CHECK(rp != 0);
        vkrpc::CreateFramebufferRequest fbr;
        fbr.device = cd.device;
        fbr.render_pass = rp;
        fbr.attachment_views = {view}; // the faithful positional path (offscreen FBO)
        fbr.width = 64;
        fbr.height = 64;
        fbr.layers = 1;
        const std::uint64_t fb = backend.create_framebuffer(fbr).framebuffer;
        VKR_CHECK(fb != 0);
        // A bufferless graphics pipeline against this render pass (the shape).
        vkrpc::CreateGraphicsPipelinesRequest gpr;
        gpr.device = cd.device;
        gpr.pipeline_cache = 0;
        vkrpc::ShaderStageDesc s0;
        s0.stage = 1;
        s0.module = cs;
        s0.entry = "main";
        vkrpc::ShaderStageDesc s1;
        s1.stage = 16;
        s1.module = cs;
        s1.entry = "main";
        gpr.stages = {s0, s1};
        gpr.topology = 3;
        gpr.vertex_binding_count = 0;
        gpr.vertex_attribute_count = 0;
        gpr.cull_mode = 0;
        gpr.front_face = 1;
        gpr.dynamic_states = {0, 1};
        gpr.layout = pl_empty;
        gpr.render_pass = rp;
        gpr.subpass = 0;
        const auto gp = backend.create_graphics_pipelines(gpr);
        VKR_CHECK(gp.ok && gp.pipeline != 0);
        // A graphics pipeline bound at the COMPUTE point rejects (kind mismatch, the reverse).
        {
            vkrpc::RecordedCommand wrong;
            wrong.kind = "bind_pipeline";
            wrong.pipeline = gp.pipeline;
            wrong.args_i64 = {1};
            VKR_CHECK(!record({wrong}).ok);
        }
        auto begin_rp = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "begin_render_pass";
            c.render_pass = rp;
            c.framebuffer = fb;
            c.render_area_w = 64;
            c.render_area_h = 64;
            return c;
        };
        auto end_rp = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "end_render_pass";
            return c;
        };
        auto bind_gp = [&]() {
            vkrpc::RecordedCommand c;
            c.kind = "bind_pipeline";
            c.pipeline = gp.pipeline;
            // No args_i64: the LEGACY encoding (old recordings) must still mean GRAPHICS.
            return c;
        };
        auto vp_sc_draw = [&]() {
            std::vector<vkrpc::RecordedCommand> v;
            vkrpc::RecordedCommand vp;
            vp.kind = "set_viewport";
            vp.vp_w = 64.0;
            vp.vp_h = 64.0;
            vp.vp_max_depth = 1.0;
            vkrpc::RecordedCommand sc;
            sc.kind = "set_scissor";
            sc.sc_w = 64;
            sc.sc_h = 64;
            vkrpc::RecordedCommand d;
            d.kind = "draw";
            d.vertex_count = 3;
            d.instance_count = 1;
            d.first_vertex = 0;
            d.first_instance = 0;
            v.push_back(vp);
            v.push_back(sc);
            v.push_back(d);
            return v;
        };
        // Direction 1: compute bound + dispatched FIRST; the graphics draw after is intact.
        {
            std::vector<vkrpc::RecordedCommand> cmds = {bind_cp(cp.pipeline), bind_sets_compute(),
                                                        dispatch3(8, 1, 1), begin_rp(), bind_gp()};
            for (auto& c : vp_sc_draw()) {
                cmds.push_back(c);
            }
            cmds.push_back(end_rp());
            VKR_CHECK(record(std::move(cmds)).ok);
        }
        // Direction 2: graphics bound FIRST (legacy encoding), compute bind + dispatch does NOT
        // disturb it -- the draw inside the pass still sees the graphics pipeline.
        {
            std::vector<vkrpc::RecordedCommand> cmds = {bind_gp(), bind_cp(cp.pipeline),
                                                        bind_sets_compute(), dispatch3(8, 1, 1),
                                                        begin_rp()};
            for (auto& c : vp_sc_draw()) {
                cmds.push_back(c);
            }
            cmds.push_back(end_rp());
            VKR_CHECK(record(std::move(cmds)).ok);
        }
        // In-pass rejects: dispatch and both barrier kinds are outside-pass only.
        VKR_CHECK(!record({bind_cp(cp_empty), begin_rp(), dispatch3(1, 1, 1)}).ok);
        {
            vkrpc::RecordedCommand mb;
            mb.kind = "memory_barrier";
            mb.src_stage = 0x800;
            mb.dst_stage = 0x1000;
            mb.src_access = 0;
            mb.dst_access = 0;
            VKR_CHECK(!record({begin_rp(), mb}).ok);
        }
    }
}

void test_pipeline_specialization() {
    using vkrpc::SpecializationInfoDesc;
    using vkrpc::SpecializationMapEntryDesc;
    auto ok = [](const std::vector<const SpecializationInfoDesc*>& infos,
                 std::size_t max_data = vkrpc::kMaxSpecializationDataBytesPerGraphicsRequest) {
        std::string reason;
        return vkrpc::pipeline_specialization_ok(infos, max_data, reason);
    };
    auto rejected_as =
        [](const std::vector<const SpecializationInfoDesc*>& infos, const char* expected,
           std::size_t max_data = vkrpc::kMaxSpecializationDataBytesPerGraphicsRequest) {
            std::string reason;
            VKR_CHECK(!vkrpc::pipeline_specialization_ok(infos, max_data, reason));
            VKR_CHECK_EQ(reason, std::string(expected));
        };

    SpecializationInfoDesc absent;
    VKR_CHECK(ok({&absent}));
    SpecializationInfoDesc empty;
    empty.present = true;
    VKR_CHECK(ok({&empty}));
    SpecializationInfoDesc data_without_entries = empty;
    data_without_entries.data = "unused";
    VKR_CHECK(ok({&data_without_entries}));

    // Registry-derived do-not-reject pins: odd offsets/sizes and overlapping byte ranges are
    // legal. Zero-sized entries are legal too, provided their offset is inside dataSize.
    SpecializationInfoDesc overlap = empty;
    overlap.data = "abcdef";
    overlap.map_entries = {{1, 1, 3}, {2, 2, 1}, {3, 0, 0}};
    VKR_CHECK(ok({&overlap}));

    SpecializationInfoDesc bad = absent;
    bad.data = "x";
    rejected_as({&bad}, "absent pipeline specialization carries entries or data");
    bad = empty;
    bad.map_entries.resize(vkrpc::kMaxSpecializationMapEntriesPerStage + 1);
    rejected_as({&bad}, "pipeline specialization map entry count exceeds per-stage cap");
    bad = empty;
    bad.data.resize(vkrpc::kMaxSpecializationDataBytesPerStage + 1);
    rejected_as({&bad}, "pipeline specialization data size exceeds per-stage cap");

    SpecializationInfoDesc at_entry_cap = empty;
    at_entry_cap.data = "x";
    for (std::size_t i = 0; i < vkrpc::kMaxSpecializationMapEntriesPerStage; ++i) {
        at_entry_cap.map_entries.push_back({static_cast<long long>(i), 0, 0});
    }
    VKR_CHECK(ok({&at_entry_cap, &at_entry_cap})); // exactly 512 request entries
    SpecializationInfoDesc one_more = empty;
    one_more.data = "x";
    one_more.map_entries.push_back({1000, 0, 0});
    rejected_as({&at_entry_cap, &at_entry_cap, &one_more},
                "pipeline specialization map entry count exceeds request cap");

    SpecializationInfoDesc at_data_cap = empty;
    at_data_cap.data.resize(vkrpc::kMaxSpecializationDataBytesPerStage, 'd');
    VKR_CHECK(ok({&at_data_cap}, vkrpc::kMaxSpecializationDataBytesPerStage));
    rejected_as({&at_data_cap, &at_data_cap},
                "pipeline specialization data size exceeds request cap",
                vkrpc::kMaxSpecializationDataBytesPerStage);

    bad = overlap;
    bad.map_entries[0].constant_id = -1;
    rejected_as({&bad}, "pipeline specialization constantID is outside uint32");
    bad = overlap;
    bad.map_entries[0].constant_id = 4294967296ll;
    rejected_as({&bad}, "pipeline specialization constantID is outside uint32");
    bad = overlap;
    bad.map_entries[0].offset = -1;
    rejected_as({&bad}, "pipeline specialization offset is outside uint32");
    bad = overlap;
    bad.map_entries[0].offset = 4294967296ll;
    rejected_as({&bad}, "pipeline specialization offset is outside uint32");
    bad = overlap;
    bad.map_entries[0].size = -1;
    rejected_as({&bad}, "pipeline specialization size is negative or not representable");
    bad = overlap;
    bad.map_entries[0] = {1, static_cast<long long>(bad.data.size()), 0};
    rejected_as({&bad}, "pipeline specialization offset must be less than dataSize");
    bad = overlap;
    bad.map_entries[0] = {1, 4, 3};
    rejected_as({&bad}, "pipeline specialization size exceeds dataSize minus offset");
    bad = overlap;
    bad.map_entries = {{7, 0, 1}, {7, 1, 1}};
    rejected_as({&bad}, "pipeline specialization constantID values must be unique");
    // Reason precedence: range errors beat duplicate-ID errors.
    bad.map_entries = {{7, 0, 1}, {7, 6, 0}};
    rejected_as({&bad}, "pipeline specialization offset must be less than dataSize");

    vkrpc::CreateGraphicsPipelinesRequest gp;
    vkrpc::ShaderStageDesc vs;
    vs.stage = 1;
    vs.module = 10;
    vs.entry = "main";
    vs.specialization = overlap;
    vkrpc::ShaderStageDesc fs;
    fs.stage = 16;
    fs.module = 11;
    fs.entry = "main";
    fs.specialization.present = true;
    fs.specialization.map_entries = {{19, 1, 2}};
    fs.specialization.data = "XYZ"; // distinct tail pins exact per-stage association
    gp.stages = {vs, fs};
    std::string err;
    const std::string graphics_wire = gp.to_wire(err);
    VKR_CHECK(err.empty() && !graphics_wire.empty());
    auto gp_rt = vkrpc::CreateGraphicsPipelinesRequest::from_wire(graphics_wire, err);
    VKR_CHECK(err.empty());
    VKR_CHECK_EQ(gp_rt.stages.size(), static_cast<std::size_t>(2));
    VKR_CHECK(gp_rt.stages[0].specialization.present);
    VKR_CHECK_EQ(gp_rt.stages[0].specialization.data, std::string("abcdef"));
    VKR_CHECK_EQ(gp_rt.stages[0].specialization.map_entries.size(), static_cast<std::size_t>(3));
    VKR_CHECK(gp_rt.stages[1].specialization.present);
    VKR_CHECK_EQ(gp_rt.stages[1].specialization.data, std::string("XYZ"));
    VKR_CHECK_EQ(gp_rt.stages[1].specialization.map_entries.size(), static_cast<std::size_t>(1));
    VKR_CHECK_EQ(gp_rt.stages[1].specialization.map_entries[0].constant_id, 19);
    // A present-empty info is semantically distinct from a null info.
    fs.specialization = {};
    fs.specialization.present = true;
    gp.stages = {fs};
    const std::string empty_wire = gp.to_wire(err);
    VKR_CHECK(err.empty() && !empty_wire.empty());
    const auto empty_rt = vkrpc::CreateGraphicsPipelinesRequest::from_wire(empty_wire, err);
    VKR_CHECK(err.empty());
    VKR_CHECK(empty_rt.stages[0].specialization.present);
    VKR_CHECK(empty_rt.stages[0].specialization.data.empty());
    VKR_CHECK(empty_rt.stages[0].specialization.map_entries.empty());
    // Legacy ops remain specialization-free, and decoding owns an independent copy of the tail.
    gp.stages = {vs, fs};
    const auto legacy = vkrpc::CreateGraphicsPipelinesRequest::from_body(gp.to_body());
    VKR_CHECK(!legacy.stages[0].specialization.present);
    gp.stages[0].specialization.data[0] = 'Z';
    VKR_CHECK_EQ(gp_rt.stages[0].specialization.data, std::string("abcdef"));

    vkrpc::CreateComputePipelinesRequest cp;
    cp.device = 1;
    cp.layout = 2;
    cp.shader_module = 3;
    cp.entry_point = "main";
    cp.specialization = overlap;
    const std::string compute_wire = cp.to_wire(err);
    VKR_CHECK(err.empty() && !compute_wire.empty());
    const auto cp_rt = vkrpc::CreateComputePipelinesRequest::from_wire(compute_wire, err);
    VKR_CHECK(err.empty());
    VKR_CHECK_EQ(cp_rt.specialization.data, std::string("abcdef"));
    VKR_CHECK(
        !vkrpc::CreateComputePipelinesRequest::from_body(cp.to_body()).specialization.present);

    // The legacy JSON codec stays byte-compatible, but its SEND helpers must reject rather than
    // silently drop a caller-supplied specialization. The no-I/O connection proves rejection
    // happens before an envelope is written.
    NoIoConnection no_io;
    vkrpc::RpcChannel no_io_rpc(no_io);
    const auto legacy_gp_send = vkrpc::create_graphics_pipelines(no_io_rpc, 1, gp);
    VKR_CHECK(!legacy_gp_send.ok);
    VKR_CHECK_EQ(legacy_gp_send.reason,
                 std::string("legacy graphics pipeline RPC cannot carry specialization constants"));
    const auto legacy_cp_send = vkrpc::create_compute_pipelines(no_io_rpc, 2, cp);
    VKR_CHECK(!legacy_cp_send.ok);
    VKR_CHECK_EQ(legacy_cp_send.reason,
                 std::string("legacy compute pipeline RPC cannot carry specialization constants"));
    VKR_CHECK_EQ(no_io.writes, 0);

    vkrpc::CreateGraphicsPipelinesRequest long_name_gp = gp;
    long_name_gp.stages[0].entry.assign(vkrpc::kMaxPipelineEntryPointNameBytes + 1, 'n');
    VKR_CHECK(long_name_gp.to_wire(err).empty());
    VKR_CHECK_EQ(err, std::string("pipeline stage entry-point name exceeds 4096-byte cap"));
    vkrpc::CreateComputePipelinesRequest long_name_cp = cp;
    long_name_cp.entry_point.assign(vkrpc::kMaxPipelineEntryPointNameBytes + 1, 'n');
    VKR_CHECK(long_name_cp.to_wire(err).empty());
    VKR_CHECK_EQ(err, std::string("pipeline stage entry-point name exceeds 4096-byte cap"));

    // Total decoder faults: prefix/header/JSON/type/triplet and exact-tail framing.
    auto frame = [](const std::string& json, const std::string& tail = std::string()) {
        std::string body(4, '\0');
        protocol::store_le32(static_cast<std::uint32_t>(json.size()),
                             reinterpret_cast<unsigned char*>(&body[0]));
        body += json;
        body += tail;
        return body;
    };
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire("x", err);
    VKR_CHECK(!err.empty());
    std::string over_header(4, '\0');
    protocol::store_le32(static_cast<std::uint32_t>(vkrpc::kMaxBinaryJsonHeaderBytes + 1),
                         reinterpret_cast<unsigned char*>(&over_header[0]));
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(over_header, err);
    VKR_CHECK(!err.empty());
    std::string overrun(4, '\0');
    protocol::store_le32(10, reinterpret_cast<unsigned char*>(&overrun[0]));
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(overrun, err);
    VKR_CHECK(!err.empty());
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(frame("{"), err);
    VKR_CHECK(!err.empty());
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(
        frame("{\"stages\":[{\"specialization\":7}]}"), err);
    VKR_CHECK(!err.empty());
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(
        frame("{\"stages\":[{\"specialization\":{\"entries\":[[1,2]],\"data_size\":0}}]}"), err);
    VKR_CHECK(!err.empty());
    (void) vkrpc::CreateComputePipelinesRequest::from_wire(
        frame("{\"entry_point\":\"main\",\"specialization\":{\"entries\":[[1e300,0,0]],"
              "\"data_size\":1}}",
              "x"),
        err);
    VKR_CHECK_EQ(err, std::string("pipeline specialization entry contains a non-integer scalar"));
    (void) vkrpc::CreateComputePipelinesRequest::from_wire(
        frame("{\"entry_point\":\"main\",\"specialization\":{\"entries\":[[7,1,0]],"
              "\"data_size\":1}}",
              "x"),
        err);
    VKR_CHECK_EQ(err, std::string("pipeline specialization offset must be less than dataSize"));
    std::string trailing = graphics_wire;
    trailing.push_back('x');
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(trailing, err);
    VKR_CHECK(!err.empty());
    std::string short_tail = graphics_wire;
    short_tail.pop_back();
    (void) vkrpc::CreateGraphicsPipelinesRequest::from_wire(short_tail, err);
    VKR_CHECK(!err.empty());

    // Capability is additive; omission is the old-worker value.
    vkrpc::DeviceCaps caps;
    caps.pipeline_specialization = 1;
    VKR_CHECK_EQ(vkrpc::DeviceCaps::from_body(caps.to_body()).pipeline_specialization, 1u);
    json::Value old_caps = json::Value::make_object();
    VKR_CHECK_EQ(vkrpc::DeviceCaps::from_body(old_caps).pipeline_specialization, 0u);

    // Mock boundary independently runs the same validator before minting pipeline state.
    const auto devices = protocol::probe_mocked();
    vkrpc::MockVulkanBackend backend(devices.front().name);
    MockDrawFixture fixture(backend, 32);
    auto mock_gp = fixture.make_pipeline();
    mock_gp.stages[0].specialization = overlap;
    VKR_CHECK(backend.create_graphics_pipelines(mock_gp).ok);
    mock_gp.stages[0].entry.assign(vkrpc::kMaxPipelineEntryPointNameBytes + 1, 'n');
    const auto mock_long_name = backend.create_graphics_pipelines(mock_gp);
    VKR_CHECK(!mock_long_name.ok);
    VKR_CHECK_EQ(mock_long_name.reason,
                 std::string("pipeline stage entry-point name exceeds 4096-byte cap"));
    mock_gp.stages[0].entry = "main";
    mock_gp.stages[0].specialization = bad; // range error wins before duplicate
    const auto mock_bad = backend.create_graphics_pipelines(mock_gp);
    VKR_CHECK(!mock_bad.ok);
    VKR_CHECK_EQ(mock_bad.pipeline, 0ull);
    VKR_CHECK_EQ(mock_bad.reason,
                 std::string("pipeline specialization offset must be less than dataSize"));

    vkrpc::CreateComputePipelinesRequest mock_cp;
    mock_cp.device = fixture.device.device;
    mock_cp.layout = fixture.pipeline_layout.pipeline_layout;
    mock_cp.shader_module = fixture.vertex_shader.shader_module;
    mock_cp.entry_point = "main";
    mock_cp.specialization = overlap;
    VKR_CHECK(backend.create_compute_pipelines(mock_cp).ok);
    mock_cp.entry_point.assign(vkrpc::kMaxPipelineEntryPointNameBytes + 1, 'n');
    const auto mock_cp_long_name = backend.create_compute_pipelines(mock_cp);
    VKR_CHECK(!mock_cp_long_name.ok);
    VKR_CHECK_EQ(mock_cp_long_name.reason,
                 std::string("pipeline stage entry-point name exceeds 4096-byte cap"));
}

int main() {
    test_device_loss_policy();
    test_rpc_profile();
    test_rpc_profile_signal_safe_dump();
    test_record_raw_wire();
    test_rpc_roundtrip();
    test_timeline_semaphores();
    test_query_pools();
    test_multiview_mock();
    test_vertex_binding_divisors();
    test_decode_totality();
    test_body_roundtrip();
    test_negotiation();
    test_lifecycle();
    test_lifecycle_body_roundtrip();
    test_handle_parsing();
    test_command_objects();
    test_handle_array_decode();
    test_sync_memory_objects();
    test_sync_events_and_fence_status_mock();
    test_sync2_mock();
    test_bda_mock();
    test_descriptor_indexing_mock();
    test_inline_uniform_block_mock();
    test_allocate_memory_type_index_required();
    test_get_device_queue_family_required();
    test_presentation_spine();
    test_swapchain_params_decode();
    test_acquire_present_latch();
    test_present_index_decode();
    test_command_record_submit();
    test_record_invalidated_by_destroy_swapchain();
    test_command_decode();
    test_surface_queries();
    test_draw_surface_wire();
    test_core_indirect_draw_mock();
    test_core_indirect_count_draw_mock();
    test_draw_surface_mock();
    test_memory_buffer_wire();
    test_memory_buffer_mock();
    test_descriptor_surface_wire();
    test_descriptor_surface_mock();
    test_descriptor_cross_device_poison();
    test_image_depth_wire();
    test_image_depth_mock();
    test_image_command_invalidation_mock();
    test_recording_resource_leases_mock();
    test_sampler_cis_wire();
    test_combined_image_sampler_mock();
    test_texture_upload_mock();
    test_eds_dynamic_state_mock();
    test_dynamic_rendering_mock();
    test_surface_xid_registry();
    test_zink_caps_wire();
    test_zink_caps_mock();
    test_mock_toplevel_registry_i9();
    test_mock_chrome_paint();
    test_pipeline_specialization();
    test_compute_dispatch();
    return vkr::test::finish("unit_vkrpc");
}
