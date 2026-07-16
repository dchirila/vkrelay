#include "common/protocol/smoke_matrix.hpp"
#include "common/util/json.hpp"
#include "tests/test_assert.hpp"

using namespace vkr::protocol;

namespace {

void test_completeness() {
    const auto entries = default_matrix();
    // 2 display backends x 2 frontends x 3 surface shapes.
    VKR_CHECK_EQ(entries.size(), static_cast<std::size_t>(12));
    VKR_CHECK(matrix_covers_all_backends(entries));

    // Removing every Wayland-native entry must break completeness.
    std::vector<SmokeEntry> partial;
    for (const auto& e : entries) {
        if (e.display != DisplayBackend::WaylandNative) {
            partial.push_back(e);
        }
    }
    VKR_CHECK(!matrix_covers_all_backends(partial));
}

void test_json_round_trip() {
    const std::string text = matrix_to_json(default_matrix());
    vkr::json::Value parsed = vkr::json::Value::parse(text);
    VKR_CHECK(parsed.is_object());
    const vkr::json::Value* entries = parsed.find("entries");
    VKR_CHECK(entries != nullptr && entries->is_array());
    if (entries != nullptr) {
        VKR_CHECK_EQ(entries->as_array().size(), static_cast<std::size_t>(12));
    }
}

} // namespace

int main() {
    test_completeness();
    test_json_round_trip();
    return vkr::test::finish("unit_smoke_matrix");
}
