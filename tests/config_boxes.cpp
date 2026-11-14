// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "btop_config.hpp"
#include "btop_shared.hpp"
#include "btop_tools.hpp"

namespace fs = std::filesystem;

namespace {
	const std::string standard_boxes = "cpu mem net proc";
}

//* The boxes that are shown while the user hasn't chosen any. Works on the global config and
//* terminal state, which is put back the way it was after each test.
class DefaultBoxes : public testing::Test {
protected:
	void SetUp() override {
		saved_configured = Config::boxes_configured;
		saved_boxes = Config::getS("shown_boxes");
		saved_current = Config::current_boxes;
		saved_width = Term::width;
		saved_height = Term::height;
	#ifdef GPU_SUPPORT
		saved_gpu_count = Gpu::count;
		saved_gpu_offsets = Gpu::gpu_b_height_offsets;
	#endif

		Config::boxes_configured = false;
		Config::strings.at("shown_boxes") = standard_boxes;
		Config::set_boxes(standard_boxes);
		set_terminal(200, 60);
		set_gpus(1);
	}

	void TearDown() override {
	#ifdef GPU_SUPPORT
		Gpu::count = saved_gpu_count;
		Gpu::gpu_b_height_offsets = saved_gpu_offsets;
	#endif
		Term::width = saved_width;
		Term::height = saved_height;
		Config::strings.at("shown_boxes") = saved_boxes;
		Config::current_boxes = saved_current;
		Config::boxes_configured = saved_configured;
	}

	static void set_terminal(int width, int height) {
		Term::width = width;
		Term::height = height;
	}

	//* Pretend that <count> GPUs were found, each with a stats box of 9 rows
	static void set_gpus([[maybe_unused]] int count) {
	#ifdef GPU_SUPPORT
		Gpu::count = count;
		Gpu::gpu_b_height_offsets.assign(count, 9);
	#endif
	}

	//* The line of the config file that holds shown_boxes
	static auto config_line() -> std::string {
		const auto config = Config::current_config();
		const auto pos = config.find("shown_boxes = ");
		const auto start = config.rfind('\n', pos) + 1;
		return config.substr(start, config.find('\n', pos) - start);
	}

private:
	bool saved_configured{};
	std::string saved_boxes;
	std::vector<std::string> saved_current;
	int saved_width{};
	int saved_height{};
#ifdef GPU_SUPPORT
	int saved_gpu_count{};
	std::vector<int> saved_gpu_offsets;
#endif
};

TEST_F(DefaultBoxes, standard_boxes_without_gpu) {
	set_gpus(0);
	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes);
	EXPECT_EQ(Config::current_boxes.size(), 4u);
	EXPECT_FALSE(Config::boxes_configured);
}

#ifdef GPU_SUPPORT
TEST_F(DefaultBoxes, first_gpu_is_shown_when_it_fits) {
	set_gpus(2);
	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes + " gpu0");
	EXPECT_EQ(Config::current_boxes.size(), 5u);
	EXPECT_FALSE(Config::boxes_configured);
}

TEST_F(DefaultBoxes, no_gpu_box_in_a_terminal_of_minimum_size) {
	set_terminal(80, 24);
	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes);
}

TEST_F(DefaultBoxes, follows_the_size_of_the_terminal) {
	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes + " gpu0");

	set_terminal(100, 30);
	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes);

	set_terminal(200, 60);
	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes + " gpu0");
}

TEST_F(DefaultBoxes, hiding_the_gpu_box_is_a_choice_that_sticks) {
	Config::update_default_boxes();
	ASSERT_EQ(Config::getS("shown_boxes"), standard_boxes + " gpu0");

	ASSERT_TRUE(Config::toggle_box("gpu0"));
	EXPECT_TRUE(Config::boxes_configured);
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes);

	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), standard_boxes);
	EXPECT_EQ(config_line(), "shown_boxes = \"" + standard_boxes + "\"");
}
#endif // GPU_SUPPORT

TEST_F(DefaultBoxes, chosen_boxes_are_left_alone) {
	Config::boxes_configured = true;
	Config::strings.at("shown_boxes") = "cpu proc";
	Config::set_boxes("cpu proc");

	Config::update_default_boxes();
	EXPECT_EQ(Config::getS("shown_boxes"), "cpu proc");
	EXPECT_EQ(Config::current_boxes.size(), 2u);
}

TEST_F(DefaultBoxes, a_toggle_that_does_not_fit_is_no_choice) {
	set_terminal(10, 5);
	EXPECT_FALSE(Config::toggle_box("net"));
	EXPECT_FALSE(Config::boxes_configured);
}

TEST_F(DefaultBoxes, default_is_kept_out_of_the_config_file) {
	Config::update_default_boxes();
	//? Commented out, so that reading the file back leaves the boxes unchosen
	EXPECT_EQ(config_line(), "#shown_boxes = \"" + Config::getS("shown_boxes") + "\"");

	Config::boxes_configured = true;
	EXPECT_EQ(config_line(), "shown_boxes = \"" + Config::getS("shown_boxes") + "\"");
}

TEST_F(DefaultBoxes, config_file_decides_whether_boxes_are_chosen) {
	const auto path = fs::temp_directory_path() / ("btop_config_boxes_" + std::to_string(getpid()) + ".conf");
	std::vector<std::string> warnings;

	//? What btop itself writes while nothing was chosen
	Config::update_default_boxes();
	std::ofstream{path} << Config::current_config();
	Config::load(path, warnings);
	EXPECT_FALSE(Config::boxes_configured);

	std::ofstream{path} << "#? Config file\nshown_boxes = \"cpu proc\"\n";
	Config::load(path, warnings);
	EXPECT_TRUE(Config::boxes_configured);
	EXPECT_EQ(Config::getS("shown_boxes"), "cpu proc");

	fs::remove(path);
}
