// SPDX-License-Identifier: Apache-2.0

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_shared.hpp"

#ifdef GPU_SUPPORT

namespace {
	//* A device that reports only the given stats
	auto make_gpu(bool utilization, bool memory, bool power) -> Gpu::gpu_info {
		Gpu::gpu_info gpu;
		gpu.supported_functions.gpu_utilization = utilization;
		gpu.supported_functions.mem_total = memory;
		gpu.supported_functions.mem_used = memory;
		gpu.supported_functions.pwr_usage = power;
		return gpu;
	}

	//* Key of the picked series, empty if there is none
	auto picked(const Gpu::gpu_info& gpu, const std::string& choice, const std::string& avoid = "") -> std::string {
		const auto series = Gpu::pick_graph_series(gpu, choice, avoid);
		return series.has_value() ? series->key : "";
	}
}

TEST(gpu_graph_series, config_values_match_the_series) {
	//? Every value the options accept, apart from "Auto", has to name a series and the other way round
	ASSERT_EQ(Config::gpu_graph_values.size(), Gpu::graph_series_list.size() + 1);
	EXPECT_EQ(Config::gpu_graph_values.front(), "Auto");
	for (size_t i = 0; i < Gpu::graph_series_list.size(); ++i) {
		EXPECT_EQ(Config::gpu_graph_values.at(i + 1), Gpu::graph_series_list.at(i).key);
		EXPECT_FALSE(Gpu::graph_series_list.at(i).gradient.empty());
		EXPECT_FALSE(Gpu::graph_series_list.at(i).label.empty());
	}
}

TEST(gpu_graph_series, auto_shows_utilization_over_memory) {
	const auto gpu = make_gpu(true, true, true);
	const auto upper = picked(gpu, "Auto");
	EXPECT_EQ(upper, "gpu-totals");
	EXPECT_EQ(picked(gpu, "Auto", upper), "gpu-vram-totals");
}

TEST(gpu_graph_series, explicit_choice_is_kept) {
	const auto gpu = make_gpu(true, true, true);
	EXPECT_EQ(picked(gpu, "gpu-pwr-totals"), "gpu-pwr-totals");
	EXPECT_EQ(picked(gpu, "gpu-vram-totals"), "gpu-vram-totals");
	//? Also if that repeats the other half
	EXPECT_EQ(picked(gpu, "gpu-totals", "gpu-totals"), "gpu-totals");
}

TEST(gpu_graph_series, auto_lower_half_avoids_the_upper_one) {
	const auto gpu = make_gpu(true, true, true);
	EXPECT_EQ(picked(gpu, "Auto", "gpu-vram-totals"), "gpu-totals");
	EXPECT_EQ(picked(gpu, "Auto", "gpu-pwr-totals"), "gpu-totals");
}

TEST(gpu_graph_series, unreported_stats_are_replaced) {
	//? E.g. an Intel iGPU: utilization and power but no memory
	const auto no_memory = make_gpu(true, false, true);
	EXPECT_EQ(picked(no_memory, "gpu-vram-totals"), "gpu-totals");
	EXPECT_EQ(picked(no_memory, "Auto", "gpu-totals"), "gpu-pwr-totals");

	const auto memory_only = make_gpu(false, true, false);
	EXPECT_EQ(picked(memory_only, "Auto"), "gpu-vram-totals");
	EXPECT_EQ(picked(memory_only, "gpu-totals"), "gpu-vram-totals");
}

TEST(gpu_graph_series, lower_half_mirrors_when_nothing_else_is_reported) {
	const auto utilization_only = make_gpu(true, false, false);
	EXPECT_EQ(picked(utilization_only, "Auto", "gpu-totals"), "gpu-totals");
}

TEST(gpu_graph_series, no_series_without_stats) {
	const auto gpu = make_gpu(false, false, false);
	EXPECT_FALSE(Gpu::pick_graph_series(gpu, "Auto").has_value());
	EXPECT_FALSE(Gpu::pick_graph_series(gpu, "gpu-totals").has_value());
}

TEST(gpu_graph_series, unknown_choice_counts_as_auto) {
	const auto gpu = make_gpu(true, true, true);
	EXPECT_EQ(picked(gpu, "something-else"), "gpu-totals");
	EXPECT_EQ(picked(gpu, ""), "gpu-totals");
}

#endif // GPU_SUPPORT
