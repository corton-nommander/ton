#include "overlay/traffic-update-yield-policy.h"
#include "td/utils/tests.h"

TEST(OverlayTrafficUpdateYieldPolicy, ExactRecurringQuantum) {
  ton::overlay::detail::TrafficUpdateYieldPolicy policy;

  for (std::uint32_t round = 0; round < 3; ++round) {
    for (std::uint32_t update = 1; update < ton::overlay::detail::TrafficUpdateYieldPolicy::kUpdatesPerYield;
         ++update) {
      ASSERT_TRUE(!policy.on_update());
    }
    ASSERT_TRUE(policy.on_update());
  }
}

TEST(OverlayTrafficUpdateYieldPolicy, InstancesAreIndependent) {
  ton::overlay::detail::TrafficUpdateYieldPolicy first;
  ton::overlay::detail::TrafficUpdateYieldPolicy second;

  for (std::uint32_t update = 1; update < ton::overlay::detail::TrafficUpdateYieldPolicy::kUpdatesPerYield; ++update) {
    ASSERT_TRUE(!first.on_update());
  }
  ASSERT_TRUE(first.on_update());
  ASSERT_TRUE(!second.on_update());
}

TEST(OverlayFecCallbackYieldPolicy, CombinedClusteredCallbacksShareOneQuantum) {
  ton::overlay::detail::FecCallbackYieldPolicy policy;
  auto generated_callback = [&] { return policy.on_callback(); };
  auto signed_callback = [&] { return policy.on_callback(); };

  for (std::uint32_t callback = 0; callback < 32; ++callback) {
    ASSERT_TRUE(!generated_callback());
  }
  for (std::uint32_t callback = 0; callback < 31; ++callback) {
    ASSERT_TRUE(!signed_callback());
  }
  ASSERT_TRUE(generated_callback());

  for (std::uint32_t callback = 1; callback < ton::overlay::detail::FecCallbackYieldPolicy::kCallbacksPerYield;
       ++callback) {
    ASSERT_TRUE(!signed_callback());
  }
  ASSERT_TRUE(signed_callback());
}

TEST(OverlayFecCallbackYieldPolicy, InterleavedCallbacksYieldEveryQuantum) {
  ton::overlay::detail::FecCallbackYieldPolicy policy;
  constexpr auto quantum = ton::overlay::detail::FecCallbackYieldPolicy::kCallbacksPerYield;
  auto generated_callback = [&] { return policy.on_callback(); };
  auto signed_callback = [&] { return policy.on_callback(); };

  for (std::uint32_t callback = 1; callback <= quantum * 3; ++callback) {
    bool should_yield = callback % 2 == 0 ? generated_callback() : signed_callback();
    ASSERT_EQ(should_yield, callback % quantum == 0);
  }
}

TEST(OverlayFecCallbackYieldPolicy, InstancesAreIndependent) {
  ton::overlay::detail::FecCallbackYieldPolicy first;
  ton::overlay::detail::FecCallbackYieldPolicy second;

  for (std::uint32_t callback = 1; callback < ton::overlay::detail::FecCallbackYieldPolicy::kCallbacksPerYield;
       ++callback) {
    ASSERT_TRUE(!first.on_callback());
  }
  ASSERT_TRUE(first.on_callback());
  ASSERT_TRUE(!second.on_callback());
}
