// SPDX-License-Identifier: Apache-2.0
//
// Per-process GPU usage from the kernel's DRM client usage stats, the text files found at
// /proc/<pid>/fdinfo/<fd> for every open DRM file descriptor. The format is specified in the
// kernel tree under Documentation/gpu/drm-usage-stats.rst and is implemented by, among others,
// i915, xe, amdgpu, nouveau, msm, panfrost and v3d. The proprietary NVIDIA driver doesn't
// implement it, processes on those devices are queried through NVML instead.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../btop_shared.hpp"

namespace DrmFdinfo {

	//* Groups of engines that are reported separately for a process
	enum class engine_class : size_t {
		render = 0, //? 3D, compute and anything not recognised as a video engine
		decode = 1, //? Video decode and video post-processing
		encode = 2, //? Dedicated video encode
	};
	inline constexpr size_t engine_class_count = 3;

	//* Cumulative busy counters of one engine class of a client
	struct engine_counters {
		uint64_t busy_ns{};        // Busy time (drm-engine-<name>), divided by the engine capacity
		uint64_t cycles{};         // Busy cycles (drm-cycles-<name>)
		uint64_t total_cycles{};   // Elapsed cycles (drm-total-cycles-<name>)
	};

	//* One DRM client as described by a single fdinfo file
	struct client {
		std::string driver{};      // drm-driver, never empty for a parsed client
		std::string pdev{};        // drm-pdev, empty for devices that aren't on a PCI bus
		uint64_t id{};             // drm-client-id, only meaningful if has_id
		bool has_id{};             // False on kernels that don't report drm-client-id
		std::array<engine_counters, engine_class_count> engines{};
		uint64_t mem_bytes{};      // GPU memory of the client summed over its memory regions

		//* True if the client ever executed work on the GPU or holds memory on it
		[[nodiscard]] bool used() const;
	};

	//* A client together with the process whose file descriptor it was read from
	struct observed_client {
		size_t pid{};
		client info{};
	};

	//* A DRM file descriptor of a process and the client it leads to
	struct drm_fd {
		std::string name{};     // File name of the descriptor below /proc/<pid>/fd and /proc/<pid>/fdinfo
		std::string client{};   // client_key() of its client, empty if the driver reports no usage stats
	};

	using usage_map = std::unordered_map<size_t, Proc::gpu_proc_info>;

	//* Returns the engine class of the engine <name>, i.e. the part of a "drm-engine-<name>" or
	//* "drm-cycles-<name>" key after the prefix. Names that aren't known video engines count as render.
	[[nodiscard]] auto classify_engine(std::string_view name) -> engine_class;

	//* Parse the contents of one fdinfo file.
	//* <fdinfo> is the complete text of the file, lines separated by '\n'.
	//* Returns nullopt if the text has no "drm-driver" key, i.e. doesn't belong to a DRM file descriptor.
	[[nodiscard]] auto parse_client(std::string_view fdinfo) -> std::optional<client>;

	//* Utilization in percent, 0 - 100, of one engine class between two samples of the same client.
	//* <prev> and <cur> are the older and newer counters, <elapsed_ns> the monotonic time between them.
	//* Counters that moved backwards (the client id got reused) and an <elapsed_ns> of 0 yield 0.
	[[nodiscard]] auto usage_percent(const engine_counters& prev, const engine_counters& cur, uint64_t elapsed_ns) -> double;

	//* Identity of a client across file descriptors and processes: driver, device and client id.
	//* On kernels that don't report a client id all DRM file descriptors a process has open on
	//* one device count as a single client, which is the best that can be done without one.
	[[nodiscard]] auto client_key(const observed_client& observed) -> std::string;

	//* Go through all file descriptors of one process and append the clients behind the DRM ones to <observed>.
	//* <pid_path> is the procfs directory of the process <pid>, e.g. /proc/1234.
	//* Returns the DRM file descriptors of the process, also those that yielded no client. The
	//* result is empty, and <observed> untouched, for a process without any or one that can't be inspected.
	auto scan_process(size_t pid, const std::filesystem::path& pid_path, std::vector<observed_client>& observed) -> std::vector<drm_fd>;

	//* Read the clients of a process again through the DRM file descriptors <fds> that an earlier
	//* scan_process() of the same <pid> and <pid_path> returned, and append them to <observed>.
	//* This avoids going through the whole descriptor table, which can hold thousands of entries,
	//* and reads a client that is reachable from several descriptors only once.
	//* Returns false and leaves <observed> untouched if a descriptor doesn't lead to its client
	//* any more, the process then has to be looked at with scan_process() again.
	//* A descriptor the process opened since the scan is not noticed.
	auto refresh_process(size_t pid, const std::filesystem::path& pid_path, const std::vector<drm_fd>& fds, std::vector<observed_client>& observed) -> bool;

	//* True if at least one DRM device of this system is bound to a driver that can report client
	//* usage stats, i.e. any driver except the proprietary NVIDIA one.
	//* <sys_drm_path> is the sysfs DRM class directory, only overridden by tests.
	[[nodiscard]] auto available(const std::filesystem::path& sys_drm_path = "/sys/class/drm") -> bool;

	//* Keeps the state that is needed between sampling cycles to turn the cumulative counters of
	//* the DRM clients into per-process utilization. One instance must see every cycle, in order.
	class Scanner {
	public:
		//* Scan the DRM file descriptors of <pids> and add the usage of their clients to <usage>.
		//* <pids> must hold every process of the current cycle, a pid that is missing from it is
		//* forgotten. <proc_path> is the mount point of procfs. <now_ns> is a monotonic timestamp
		//* of this cycle and must not decrease between calls.
		//* Processes that can't be inspected (owned by another user) are skipped silently.
		//* To keep the cost down the descriptor table of a process is only gone through when the
		//* process is new and then every rescan_interval cycles. In between only the DRM
		//* descriptors that were found are read, so a long running process that opens a GPU late
		//* shows up with a delay of at most that many cycles.
		void collect(const std::vector<size_t>& pids, const std::filesystem::path& proc_path, uint64_t now_ns, usage_map& usage);

		//* Turn the clients seen in one sampling cycle into per-process usage and add it to <usage>.
		//* A client that is reachable from several file descriptors or processes is counted once
		//* and attributed to the lowest pid holding it, as the kernel documentation asks for.
		//* Utilization is 0 for a client that is seen for the first time.
		//* Split from collect() so that the accounting can be tested without a procfs.
		void account(const std::vector<observed_client>& observed, uint64_t now_ns, usage_map& usage);

		//* Number of processes whose whole descriptor table the last collect() went through
		[[nodiscard]] auto scanned_processes() const -> size_t { return scanned_processes_; }

		//? Cycles for which the descriptor table of a newly started process is gone through every cycle
		static constexpr uint64_t fresh_cycles = 5;
		//? Cycles between two passes over the descriptor table of a process
		static constexpr uint64_t rescan_interval = 10;

	private:
		struct client_state {
			std::array<engine_counters, engine_class_count> engines{};
			uint64_t sampled_ns{};
			uint64_t seen_cycle{};
		};

		struct pid_state {
			uint64_t fresh_until_cycle{}; // Scanned every cycle before this one, 0 for processes found at startup
			uint64_t seen_cycle{};
			std::vector<drm_fd> drm_fds{}; // Result of the last scan_process()
		};

		std::unordered_map<std::string, client_state> clients_{};
		std::unordered_map<size_t, pid_state> pids_{};
		uint64_t scan_cycle_{};    // Number of collect() calls, schedules the rescans
		uint64_t account_cycle_{}; // Number of account() calls, expires clients that went away
		size_t scanned_processes_{};
	};
}
