// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "btop_shared.hpp"
#include "linux/drm_fdinfo.hpp"

namespace fs = std::filesystem;

using DrmFdinfo::engine_class;
using DrmFdinfo::engine_counters;

namespace {
	constexpr uint64_t second_ns = 1'000'000'000;

	//? Captured from gnome-shell on an Intel Raptor Lake iGPU, kernel 6.8
	constexpr std::string_view i915_fdinfo =
		"pos:\t0\n"
		"flags:\t02100002\n"
		"mnt_id:\t27\n"
		"ino:\t981\n"
		"drm-driver:\ti915\n"
		"drm-client-id:\t1927\n"
		"drm-pdev:\t0000:00:02.0\n"
		"drm-total-system0:\t641084 KiB\n"
		"drm-shared-system0:\t218892 KiB\n"
		"drm-active-system0:\t185272 KiB\n"
		"drm-resident-system0:\t631416 KiB\n"
		"drm-purgeable-system0:\t1940 KiB\n"
		"drm-total-stolen-system0:\t0\n"
		"drm-shared-stolen-system0:\t0\n"
		"drm-active-stolen-system0:\t0\n"
		"drm-resident-stolen-system0:\t0\n"
		"drm-purgeable-stolen-system0:\t0\n"
		"drm-engine-render:\t2827789073876 ns\n"
		"drm-engine-copy:\t1000 ns\n"
		"drm-engine-video:\t600 ns\n"
		"drm-engine-capacity-video:\t2\n"
		"drm-engine-video-enhance:\t50 ns\n";

	//? Keys as written by amdgpu, with the legacy drm-memory-* spelling next to the common ones
	constexpr std::string_view amdgpu_fdinfo =
		"pos:\t0\n"
		"drm-driver:\tamdgpu\n"
		"drm-client-id:\t42\n"
		"drm-pdev:\t0000:03:00.0\n"
		"pasid:\t32770\n"
		"drm-total-cpu:\t16 MiB\n"
		"drm-total-gtt:\t64 MiB\n"
		"drm-total-vram:\t512 MiB\n"
		"drm-memory-cpu:\t8192 KiB\n"
		"drm-memory-gtt:\t32768 KiB\n"
		"drm-memory-vram:\t262144 KiB\n"
		"amd-evicted-vram:\t0 KiB\n"
		"drm-engine-gfx:\t5000 ns\n"
		"drm-engine-compute:\t3000 ns\n"
		"drm-engine-dec:\t700 ns\n"
		"drm-engine-enc:\t200 ns\n"
		"drm-engine-enc_1:\t100 ns\n";

	//? xe reports cycles instead of time
	constexpr std::string_view xe_fdinfo =
		"drm-driver:\txe\n"
		"drm-client-id:\t7\n"
		"drm-pdev:\t0000:00:02.0\n"
		"drm-total-system:\t0\n"
		"drm-resident-vram0:\t2048 KiB\n"
		"drm-total-vram0:\t4096 KiB\n"
		"drm-cycles-rcs:\t28257900\n"
		"drm-total-cycles-rcs:\t7655183225\n"
		"drm-cycles-vcs:\t100\n"
		"drm-total-cycles-vcs:\t7655183225\n"
		"drm-engine-capacity-vcs:\t2\n";

	auto engines_of(const DrmFdinfo::client& client, engine_class type) -> const engine_counters& {
		return client.engines.at(static_cast<size_t>(type));
	}

	//* Build a client of the given driver and device with a single render engine counter
	auto make_client(const std::string& driver, uint64_t id, uint64_t render_ns, uint64_t mem_bytes) -> DrmFdinfo::client {
		DrmFdinfo::client client;
		client.driver = driver;
		client.pdev = "0000:00:02.0";
		client.id = id;
		client.has_id = true;
		client.engines.at(static_cast<size_t>(engine_class::render)).busy_ns = render_ns;
		client.mem_bytes = mem_bytes;
		return client;
	}
}

TEST(drm_fdinfo, classify_engine) {
	for (const auto* name : {"render", "copy", "compute", "gfx", "rcs", "bcs", "ccs", "gpu", "something-new"}) {
		EXPECT_EQ(DrmFdinfo::classify_engine(name), engine_class::render) << name;
	}
	for (const auto* name : {"video", "video-enhance", "vcs", "vecs", "dec", "jpeg", "jpeg_1", "vpe", "uvd"}) {
		EXPECT_EQ(DrmFdinfo::classify_engine(name), engine_class::decode) << name;
	}
	for (const auto* name : {"enc", "enc_1", "vce"}) {
		EXPECT_EQ(DrmFdinfo::classify_engine(name), engine_class::encode) << name;
	}
}

TEST(drm_fdinfo, parse_i915) {
	const auto client = DrmFdinfo::parse_client(i915_fdinfo);
	ASSERT_TRUE(client.has_value());
	EXPECT_EQ(client->driver, "i915");
	EXPECT_EQ(client->pdev, "0000:00:02.0");
	EXPECT_TRUE(client->has_id);
	EXPECT_EQ(client->id, 1927u);

	//? render + copy
	EXPECT_EQ(engines_of(*client, engine_class::render).busy_ns, 2827789073876u + 1000u);
	//? video is shared by two engines, the capacity line comes after the usage line
	EXPECT_EQ(engines_of(*client, engine_class::decode).busy_ns, 600u / 2 + 50u);
	EXPECT_EQ(engines_of(*client, engine_class::encode).busy_ns, 0u);

	//? Resident wins over total, regions are summed
	EXPECT_EQ(client->mem_bytes, 631416u * 1024u);
	EXPECT_TRUE(client->used());
}

TEST(drm_fdinfo, parse_amdgpu) {
	const auto client = DrmFdinfo::parse_client(amdgpu_fdinfo);
	ASSERT_TRUE(client.has_value());
	EXPECT_EQ(client->driver, "amdgpu");
	EXPECT_EQ(client->id, 42u);
	EXPECT_EQ(engines_of(*client, engine_class::render).busy_ns, 8000u);
	EXPECT_EQ(engines_of(*client, engine_class::decode).busy_ns, 700u);
	EXPECT_EQ(engines_of(*client, engine_class::encode).busy_ns, 300u);

	//? No resident values, so total is used. The cpu region isn't GPU memory.
	EXPECT_EQ(client->mem_bytes, (64u + 512u) * 1024u * 1024u);
}

TEST(drm_fdinfo, parse_legacy_memory_only) {
	const auto client = DrmFdinfo::parse_client(
		"drm-driver:\tamdgpu\n"
		"drm-client-id:\t3\n"
		"drm-memory-vram:\t1024 KiB\n"
		"drm-memory-gtt:\t512 KiB\n"
		"drm-memory-cpu:\t256 KiB\n");
	ASSERT_TRUE(client.has_value());
	EXPECT_EQ(client->pdev, "");
	EXPECT_EQ(client->mem_bytes, 1536u * 1024u);
}

TEST(drm_fdinfo, parse_xe_cycles) {
	const auto client = DrmFdinfo::parse_client(xe_fdinfo);
	ASSERT_TRUE(client.has_value());
	EXPECT_EQ(client->driver, "xe");
	EXPECT_EQ(engines_of(*client, engine_class::render).cycles, 28257900u);
	EXPECT_EQ(engines_of(*client, engine_class::render).total_cycles, 7655183225u);
	EXPECT_EQ(engines_of(*client, engine_class::render).busy_ns, 0u);
	EXPECT_EQ(engines_of(*client, engine_class::decode).cycles, 100u);
	EXPECT_EQ(client->mem_bytes, 2048u * 1024u);
}

TEST(drm_fdinfo, parse_units) {
	const auto client = DrmFdinfo::parse_client(
		"drm-driver: test\n"
		"drm-total-a: 5\n"
		"drm-total-b: 2 KiB\n"
		"drm-total-c: 3 MiB\n"
		"drm-total-d: 1 GiB\n");
	ASSERT_TRUE(client.has_value());
	EXPECT_FALSE(client->has_id);
	EXPECT_EQ(client->mem_bytes, 5u + 2u * 1024u + 3u * 1024u * 1024u + 1024u * 1024u * 1024u);
}

TEST(drm_fdinfo, parse_rejects_other_files) {
	//? fdinfo of a regular file
	EXPECT_FALSE(DrmFdinfo::parse_client("pos:\t0\nflags:\t0100002\nmnt_id:\t29\nino:\t1234\n").has_value());
	EXPECT_FALSE(DrmFdinfo::parse_client("").has_value());
	//? DRM keys without a driver can't be attributed to a device
	EXPECT_FALSE(DrmFdinfo::parse_client("drm-client-id:\t5\ndrm-engine-render:\t10 ns\n").has_value());
}

TEST(drm_fdinfo, parse_tolerates_malformed_lines) {
	const auto client = DrmFdinfo::parse_client(
		"no colon in this line\n"
		"drm-driver:\ti915\r\n"
		"drm-client-id:\tnot-a-number\n"
		"drm-engine-render:\t\n"
		"drm-engine-copy:\t77 ns");
	ASSERT_TRUE(client.has_value());
	EXPECT_EQ(client->driver, "i915");
	EXPECT_FALSE(client->has_id);
	EXPECT_EQ(engines_of(*client, engine_class::render).busy_ns, 77u);
}

TEST(drm_fdinfo, unused_client) {
	const auto client = DrmFdinfo::parse_client("drm-driver:\ti915\ndrm-client-id:\t9\ndrm-engine-render:\t0 ns\ndrm-total-system0:\t0\n");
	ASSERT_TRUE(client.has_value());
	EXPECT_FALSE(client->used());
}

TEST(drm_fdinfo, usage_percent_time) {
	const engine_counters prev{.busy_ns = 1000};
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.busy_ns = 1000 + second_ns / 4}, second_ns), 25.0);
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.busy_ns = 1000}, second_ns), 0.0);
	//? Busier than the interval allows, e.g. several engines in one class
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.busy_ns = 1000 + 3 * second_ns}, second_ns), 100.0);
	//? Counter went backwards, the client id was reused
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.busy_ns = 10}, second_ns), 0.0);
	//? No time passed
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.busy_ns = 5000}, 0), 0.0);
}

TEST(drm_fdinfo, usage_percent_cycles) {
	const engine_counters prev{.cycles = 100, .total_cycles = 1000};
	//? Cycle counters carry their own time base, the elapsed time doesn't matter
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.cycles = 600, .total_cycles = 2000}, 0), 50.0);
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.cycles = 100, .total_cycles = 2000}, second_ns), 0.0);
	EXPECT_DOUBLE_EQ(DrmFdinfo::usage_percent(prev, {.cycles = 600, .total_cycles = 1000}, second_ns), 0.0);
}

TEST(drm_fdinfo, account_usage_over_two_cycles) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	scanner.account({{100, make_client("i915", 1, 0, 4096)}}, second_ns, usage);
	ASSERT_TRUE(usage.contains(100));
	//? Nothing to compare the first sample with
	EXPECT_DOUBLE_EQ(usage.at(100).usage_p, 0.0);
	EXPECT_EQ(usage.at(100).mem, 4096u);
	EXPECT_EQ(usage.at(100).dev, "i915");
	EXPECT_EQ(usage.at(100).type, 0);

	usage.clear();
	scanner.account({{100, make_client("i915", 1, second_ns / 2, 8192)}}, 3 * second_ns, usage);
	EXPECT_DOUBLE_EQ(usage.at(100).usage_p, 25.0);
	EXPECT_EQ(usage.at(100).mem, 8192u);
}

TEST(drm_fdinfo, account_counts_shared_client_once) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	//? One client reachable from two descriptors of pid 300 and inherited by pid 200
	scanner.account({
		{300, make_client("i915", 5, 1000, 100)},
		{300, make_client("i915", 5, 1000, 100)},
		{200, make_client("i915", 5, 1000, 100)},
	}, second_ns, usage);
	ASSERT_EQ(usage.size(), 1u);
	EXPECT_EQ(usage.at(200).mem, 100u);

	//? The copies are read moments apart, the most recent counter is used
	usage.clear();
	scanner.account({
		{300, make_client("i915", 5, 1000 + second_ns / 10, 100)},
		{200, make_client("i915", 5, 1000 + second_ns / 5, 100)},
	}, 2 * second_ns, usage);
	ASSERT_EQ(usage.size(), 1u);
	EXPECT_DOUBLE_EQ(usage.at(200).usage_p, 20.0);
}

TEST(drm_fdinfo, account_sums_clients_and_devices_of_a_process) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	auto other_device = make_client("amdgpu", 1, 0, 50);
	other_device.pdev = "0000:03:00.0";
	scanner.account({
		{100, make_client("i915", 1, 0, 10)},
		{100, make_client("i915", 2, 0, 20)},
		{100, other_device},
	}, second_ns, usage);
	usage.clear();

	other_device.engines.at(static_cast<size_t>(engine_class::render)).busy_ns = second_ns / 2;
	scanner.account({
		{100, make_client("i915", 1, second_ns / 10, 10)},
		{100, make_client("i915", 2, second_ns / 5, 20)},
		{100, other_device},
	}, 2 * second_ns, usage);

	ASSERT_EQ(usage.size(), 1u);
	EXPECT_DOUBLE_EQ(usage.at(100).usage_p, 10.0 + 20.0 + 50.0);
	EXPECT_EQ(usage.at(100).mem, 80u);
	//? Devices are listed in the order of their driver names, whatever order they were seen in
	EXPECT_EQ(usage.at(100).dev, "amdgpu+i915");
}

TEST(drm_fdinfo, account_keeps_existing_usage) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	//? As left behind by another source for the same process, e.g. NVML
	usage[100].mem = 1000;
	usage[100].dev = "gpu0";
	usage[100].type = Proc::gpu_context_compute;

	scanner.account({{100, make_client("i915", 1, 0, 24)}}, second_ns, usage);
	EXPECT_EQ(usage.at(100).mem, 1024u);
	EXPECT_EQ(usage.at(100).dev, "gpu0+i915");
	EXPECT_EQ(usage.at(100).type, Proc::gpu_context_compute);
}

TEST(drm_fdinfo, account_skips_unused_clients) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;
	scanner.account({{100, make_client("i915", 1, 0, 0)}}, second_ns, usage);
	EXPECT_TRUE(usage.empty());
}

TEST(drm_fdinfo, account_forgets_clients_that_went_away) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	scanner.account({{100, make_client("i915", 1, second_ns, 10)}}, second_ns, usage);
	scanner.account({}, 2 * second_ns, usage);
	usage.clear();

	//? A new client that got the same id must not be compared with the counters of the old one
	scanner.account({{100, make_client("i915", 1, 5 * second_ns, 10)}}, 3 * second_ns, usage);
	EXPECT_DOUBLE_EQ(usage.at(100).usage_p, 0.0);
}

TEST(drm_fdinfo, account_without_client_id) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	auto anonymous = make_client("vendor", 0, 1000, 64);
	anonymous.has_id = false;

	//? Descriptors of one process are folded together, different processes stay apart
	scanner.account({{100, anonymous}, {100, anonymous}, {200, anonymous}}, second_ns, usage);
	ASSERT_EQ(usage.size(), 2u);
	EXPECT_EQ(usage.at(100).mem, 64u);
	EXPECT_EQ(usage.at(200).mem, 64u);
}

TEST(drm_fdinfo, gpu_proc_info_add) {
	Proc::gpu_proc_info parent;
	parent.usage_p = 10.0;
	parent.mem = 100;
	parent.type = Proc::gpu_context_graphic;
	parent.dev = "gpu0";

	Proc::gpu_proc_info child;
	child.usage_p = 5.0;
	child.enc_p = 2.0;
	child.dec_p = 3.0;
	child.mem = 50;
	child.type = Proc::gpu_context_compute;
	child.dev = "gpu0+i915";

	parent.add(child);
	EXPECT_DOUBLE_EQ(parent.usage_p, 15.0);
	EXPECT_DOUBLE_EQ(parent.enc_p, 2.0);
	EXPECT_DOUBLE_EQ(parent.dec_p, 3.0);
	EXPECT_EQ(parent.mem, 150u);
	EXPECT_EQ(parent.type, Proc::gpu_context_graphic | Proc::gpu_context_compute);
	EXPECT_EQ(parent.dev, "gpu0+i915");
	EXPECT_TRUE(parent.present());

	Proc::gpu_proc_info none;
	EXPECT_FALSE(none.present());
	none.add(Proc::gpu_proc_info{});
	EXPECT_FALSE(none.present());
}

class DrmFdinfoSysfs : public testing::Test {
protected:
	void SetUp() override {
		root = fs::temp_directory_path() / ("btop_drm_fdinfo_" + std::to_string(getpid()));
		fs::remove_all(root);
		fs::create_directories(root / "drivers" / "nvidia");
		fs::create_directories(root / "drivers" / "i915");
		fs::create_directories(root / "class");
	}

	void TearDown() override {
		fs::remove_all(root);
	}

	//* Create the class entry <name> for a device that is bound to <driver>
	void add_device(const std::string& name, const std::string& driver) const {
		fs::create_directories(root / "class" / name / "device");
		fs::create_directory_symlink(root / "drivers" / driver, root / "class" / name / "device" / "driver");
	}

	fs::path root;
};

TEST_F(DrmFdinfoSysfs, available) {
	EXPECT_FALSE(DrmFdinfo::available(root / "missing"));
	EXPECT_FALSE(DrmFdinfo::available(root / "class"));

	//? The proprietary NVIDIA driver has no fdinfo support, connectors aren't devices
	add_device("card0", "nvidia");
	add_device("card0-DP-1", "i915");
	add_device("renderD128", "i915");
	fs::create_directories(root / "class" / "card9");
	EXPECT_FALSE(DrmFdinfo::available(root / "class"));

	add_device("card1", "i915");
	EXPECT_TRUE(DrmFdinfo::available(root / "class"));
}

TEST(drm_fdinfo, client_key) {
	const auto first = make_client("i915", 1, 0, 0);
	auto second = make_client("i915", 2, 0, 0);
	EXPECT_EQ(DrmFdinfo::client_key({100, first}), DrmFdinfo::client_key({200, first}));
	EXPECT_NE(DrmFdinfo::client_key({100, first}), DrmFdinfo::client_key({100, second}));

	//? Same id on another device or driver is another client
	second.id = 1;
	second.pdev = "0000:03:00.0";
	EXPECT_NE(DrmFdinfo::client_key({100, first}), DrmFdinfo::client_key({100, second}));

	//? Without an id clients can only be told apart by their process
	auto anonymous = first;
	anonymous.has_id = false;
	EXPECT_EQ(DrmFdinfo::client_key({100, anonymous}), DrmFdinfo::client_key({100, anonymous}));
	EXPECT_NE(DrmFdinfo::client_key({100, anonymous}), DrmFdinfo::client_key({200, anonymous}));
	EXPECT_NE(DrmFdinfo::client_key({100, anonymous}), DrmFdinfo::client_key({100, first}));
}

TEST(drm_fdinfo, scan_process_that_cant_be_inspected) {
	std::vector<DrmFdinfo::observed_client> observed;
	EXPECT_TRUE(DrmFdinfo::scan_process(1, "/nonexistent/proc/1", observed).empty());
	EXPECT_TRUE(observed.empty());
}

TEST(drm_fdinfo, refresh_process_detects_stale_descriptors) {
	const auto pid = static_cast<size_t>(getpid());
	std::vector<DrmFdinfo::observed_client> observed = {{1, make_client("i915", 1, 0, 0)}};

	//? Nothing to read for a process without clients
	EXPECT_TRUE(DrmFdinfo::refresh_process(pid, "/proc/self", {}, observed));
	EXPECT_TRUE(DrmFdinfo::refresh_process(pid, "/proc/self", {{"0", ""}}, observed));

	//? A descriptor that got closed, and one that now belongs to something that isn't a DRM device
	const int null_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(null_fd, 0);
	EXPECT_FALSE(DrmFdinfo::refresh_process(pid, "/proc/self", {{"999999999", "i915|0000:00:02.0|1"}}, observed));
	EXPECT_FALSE(DrmFdinfo::refresh_process(pid, "/proc/self", {{std::to_string(null_fd), "i915|0000:00:02.0|1"}}, observed));
	close(null_fd);

	//? What was there before stays as it was
	ASSERT_EQ(observed.size(), 1u);
	EXPECT_EQ(observed.front().pid, 1u);
}

TEST(drm_fdinfo, scan_process_finds_own_drm_descriptor) {
	//? Needs a GPU, any render node will do
	int render_fd = -1;
	std::error_code ec;
	for (fs::directory_iterator it{"/dev/dri", ec}, end; not ec and it != end and render_fd < 0; it.increment(ec)) {
		if (it->path().filename().string().starts_with("renderD")) render_fd = open(it->path().c_str(), O_RDWR | O_CLOEXEC);
	}
	if (render_fd < 0) GTEST_SKIP() << "no accessible DRM render node";

	const auto pid = static_cast<size_t>(getpid());
	std::vector<DrmFdinfo::observed_client> observed;
	const auto drm_fds = DrmFdinfo::scan_process(pid, "/proc/self", observed);

	//? Drivers without fdinfo support (NVIDIA) have the descriptor but no client
	const auto own_fd = std::ranges::find(drm_fds, std::to_string(render_fd), &DrmFdinfo::drm_fd::name);
	ASSERT_NE(own_fd, drm_fds.end());
	EXPECT_EQ(own_fd->client.empty(), observed.empty());
	for (const auto& entry : observed) {
		EXPECT_EQ(entry.pid, pid);
		EXPECT_FALSE(entry.info.driver.empty());
	}

	//? Reading the same clients again through the descriptors that were found, each client once
	std::vector<DrmFdinfo::observed_client> refreshed;
	EXPECT_TRUE(DrmFdinfo::refresh_process(pid, "/proc/self", drm_fds, refreshed));
	EXPECT_LE(refreshed.size(), observed.size());
	EXPECT_EQ(refreshed.empty(), observed.empty());
	for (const auto& entry : refreshed) {
		EXPECT_NE(std::ranges::find(drm_fds, DrmFdinfo::client_key(entry), &DrmFdinfo::drm_fd::client), drm_fds.end());
	}

	//? Once the descriptor is closed its number no longer leads to the client
	close(render_fd);
	if (not own_fd->client.empty()) {
		EXPECT_FALSE(DrmFdinfo::refresh_process(pid, "/proc/self", {*own_fd}, refreshed));
	}
}

TEST(drm_fdinfo, collect_rescans_on_schedule) {
	DrmFdinfo::Scanner scanner;
	DrmFdinfo::usage_map usage;

	//? Pids that don't exist below the given procfs: nothing is found but the scheduling still applies
	std::vector<size_t> pids;
	for (size_t pid = 1; pid <= 4 * DrmFdinfo::Scanner::rescan_interval; ++pid) pids.push_back(pid);

	//? Everything is looked at once at startup
	scanner.collect(pids, "/nonexistent/proc", second_ns, usage);
	EXPECT_EQ(scanner.scanned_processes(), pids.size());

	//? After that an even slice per cycle, every process once per interval
	size_t scanned = 0;
	for (uint64_t cycle = 0; cycle < DrmFdinfo::Scanner::rescan_interval; ++cycle) {
		scanner.collect(pids, "/nonexistent/proc", (2 + cycle) * second_ns, usage);
		EXPECT_EQ(scanner.scanned_processes(), pids.size() / DrmFdinfo::Scanner::rescan_interval);
		scanned += scanner.scanned_processes();
	}
	EXPECT_EQ(scanned, pids.size());

	//? A process that appears later has just started, it is scanned every cycle for a while on
	//? top of the regular slice and then joins the rotation. The pid is chosen to never be due
	//? in the cycles where it is fresh, so the two reasons for a scan can't overlap.
	const size_t slice = pids.size() / DrmFdinfo::Scanner::rescan_interval;
	const uint64_t next_cycle = DrmFdinfo::Scanner::rescan_interval + 2;
	const size_t new_pid = 1'000'000 * DrmFdinfo::Scanner::rescan_interval - next_cycle - DrmFdinfo::Scanner::fresh_cycles;
	pids.push_back(new_pid);
	for (uint64_t cycle = 0; cycle < DrmFdinfo::Scanner::fresh_cycles; ++cycle) {
		scanner.collect(pids, "/nonexistent/proc", (20 + cycle) * second_ns, usage);
		EXPECT_EQ(scanner.scanned_processes(), slice + 1);
	}
	scanner.collect(pids, "/nonexistent/proc", 30 * second_ns, usage);
	EXPECT_EQ(scanner.scanned_processes(), slice + 1); //? Due as part of the rotation
	scanner.collect(pids, "/nonexistent/proc", 31 * second_ns, usage);
	EXPECT_EQ(scanner.scanned_processes(), slice);

	EXPECT_TRUE(usage.empty());
}
