// SPDX-License-Identifier: Apache-2.0

#include "drm_fdinfo.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <memory>
#include <system_error>
#include <utility>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <fmt/format.h>

#include "../btop_tools.hpp"

namespace fs = std::filesystem;

namespace DrmFdinfo {

	namespace {
		//? Character device major number of every DRM device node
		constexpr unsigned int drm_major = 226;

		//? amdgpu's "cpu" region is plain system memory that isn't mapped by the GPU
		constexpr std::string_view cpu_region = "cpu";

		//* Closes a file descriptor when it goes out of scope
		class unique_fd {
		public:
			explicit unique_fd(int fd) : fd_(fd) {}
			~unique_fd() { if (fd_ >= 0) close(fd_); }
			unique_fd(const unique_fd&) = delete;
			unique_fd& operator=(const unique_fd&) = delete;
			[[nodiscard]] int get() const { return fd_; }
			[[nodiscard]] bool valid() const { return fd_ >= 0; }
			//* Hand the descriptor over to the caller, e.g. to fdopendir()
			[[nodiscard]] int release() { return std::exchange(fd_, -1); }
		private:
			int fd_;
		};

		struct dir_closer {
			void operator()(DIR* dir) const { closedir(dir); }
		};
		using unique_dir = std::unique_ptr<DIR, dir_closer>;

		//* Strip spaces, tabs and carriage returns from both ends of <str>
		auto strip(std::string_view str) -> std::string_view {
			constexpr std::string_view blanks = " \t\r";
			const auto first = str.find_first_not_of(blanks);
			if (first == std::string_view::npos) return {};
			return str.substr(first, str.find_last_not_of(blanks) - first + 1);
		}

		//* A number followed by an optional unit, as used for every numeric fdinfo value
		struct quantity {
			uint64_t value{};
			std::string_view unit{};
		};

		//* Parse "<uint>[ <unit>]". <text> must already be stripped. Returns nullopt if it doesn't start with a number.
		auto parse_quantity(std::string_view text) -> std::optional<quantity> {
			quantity out;
			const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), out.value);
			if (err != std::errc{}) return std::nullopt;
			out.unit = strip(text.substr(static_cast<size_t>(end - text.data())));
			return out;
		}

		//* Convert a memory quantity to bytes, the kernel uses binary units and omits the unit for bytes
		auto to_bytes(const quantity& qty) -> uint64_t {
			if (qty.unit == "KiB") return qty.value << 10;
			if (qty.unit == "MiB") return qty.value << 20;
			if (qty.unit == "GiB") return qty.value << 30;
			return qty.value;
		}

		//? Raw values of one engine, the capacity line can come before or after the usage line
		struct engine_raw {
			uint64_t busy_ns{};
			uint64_t capacity{1};
			uint64_t cycles{};
			uint64_t total_cycles{};
		};

		//? Sizes of one memory region in order of preference
		struct region_raw {
			std::optional<uint64_t> resident{}; // drm-resident-<region>, memory that is actually backed
			std::optional<uint64_t> total{};    // drm-total-<region>, all buffers of the client
			std::optional<uint64_t> legacy{};   // drm-memory-<region>, older spelling of resident
		};

		//* Read a whole small file into <out>. <name> is relative to the directory <dir_fd>, or
		//* any path if <dir_fd> is AT_FDCWD. Returns false if the file can't be read.
		auto read_file_at(int dir_fd, const char* name, std::string& out) -> bool {
			const unique_fd file{openat(dir_fd, name, O_RDONLY | O_CLOEXEC)};
			if (not file.valid()) return false;

			out.clear();
			std::array<char, 4096> chunk{};
			for (;;) {
				const ssize_t count = read(file.get(), chunk.data(), chunk.size());
				if (count < 0) return false;
				if (count == 0) return true;
				out.append(chunk.data(), static_cast<size_t>(count));
			}
		}

	}

	auto client_key(const observed_client& observed) -> std::string {
		const auto& info = observed.info;
		if (info.has_id) return fmt::format("{}|{}|{}", info.driver, info.pdev, info.id);
		return fmt::format("{}|{}|pid{}", info.driver, info.pdev, observed.pid);
	}

	auto scan_process(size_t pid, const fs::path& pid_path, std::vector<observed_client>& observed) -> std::vector<drm_fd> {
		std::vector<drm_fd> drm_fds;

		unique_fd fd_dir_fd{open((pid_path / "fd").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
		if (not fd_dir_fd.valid()) return drm_fds;
		const int fd_dir_no = fd_dir_fd.get();

		//? The stream owns the descriptor from here on
		const unique_dir fd_dir{fdopendir(fd_dir_no)};
		if (fd_dir == nullptr) return drm_fds;
		static_cast<void>(fd_dir_fd.release());

		const unique_fd info_dir{open((pid_path / "fdinfo").c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};

		std::string text;
		while (const dirent* entry = readdir(fd_dir.get())) {
			if (entry->d_name[0] < '0' or entry->d_name[0] > '9') continue;

			//? Following the link gives the device node behind the descriptor without opening it
			struct stat target{};
			if (fstatat(fd_dir_no, entry->d_name, &target, 0) != 0) continue;
			if (not S_ISCHR(target.st_mode) or major(target.st_rdev) != drm_major) continue;
			auto& found = drm_fds.emplace_back(entry->d_name);

			if (not info_dir.valid() or not read_file_at(info_dir.get(), entry->d_name, text)) continue;
			if (auto info = parse_client(text); info.has_value()) {
				observed.push_back({pid, std::move(*info)});
				found.client = client_key(observed.back());
			}
		}
		return drm_fds;
	}

	auto refresh_process(size_t pid, const fs::path& pid_path, const std::vector<drm_fd>& fds, std::vector<observed_client>& observed) -> bool {
		const size_t observed_before = observed.size();
		const auto info_path = pid_path / "fdinfo";

		std::string text;
		std::vector<std::string_view> clients_read;
		for (const auto& fd : fds) {
			if (fd.client.empty() or std::ranges::find(clients_read, fd.client) != clients_read.end()) continue;

			//? A descriptor number is reused once it is closed, so whatever is behind it now has
			//? to be the same client as before
			auto info = read_file_at(AT_FDCWD, (info_path / fd.name).c_str(), text) ? parse_client(text) : std::nullopt;
			if (not info.has_value() or client_key({pid, *info}) != fd.client) {
				observed.resize(observed_before);
				return false;
			}
			observed.push_back({pid, std::move(*info)});
			clients_read.push_back(fd.client);
		}
		return true;
	}

	bool client::used() const {
		return mem_bytes > 0 or std::ranges::any_of(engines, [](const engine_counters& engine) {
			return engine.busy_ns > 0 or engine.cycles > 0;
		});
	}

	auto classify_engine(std::string_view name) -> engine_class {
		//? i915: video, video-enhance. xe: vcs, vecs. amdgpu: dec, jpeg, vpe, enc, enc_1 and the older uvd, vce.
		//? Intel's video engine does both decode and encode, there is no separate counter for encode.
		constexpr std::array<std::string_view, 8> decode_names = {"video", "video-enhance", "vcs", "vecs", "dec", "jpeg", "vpe", "uvd"};
		constexpr std::array<std::string_view, 2> encode_names = {"enc", "vce"};

		//? Additional instances of an engine carry a numeric suffix, e.g. enc_1
		const auto base = name.substr(0, name.find('_'));
		if (std::ranges::find(encode_names, base) != encode_names.end()) return engine_class::encode;
		if (std::ranges::find(decode_names, base) != decode_names.end()) return engine_class::decode;
		return engine_class::render;
	}

	auto parse_client(std::string_view fdinfo) -> std::optional<client> {
		client out;
		//? Keyed by views into <fdinfo>, which outlives both maps
		std::unordered_map<std::string_view, engine_raw> engines;
		std::unordered_map<std::string_view, region_raw> regions;

		while (not fdinfo.empty()) {
			const auto line_end = fdinfo.find('\n');
			const auto line = fdinfo.substr(0, line_end);
			fdinfo.remove_prefix(line_end == std::string_view::npos ? fdinfo.size() : line_end + 1);

			const auto colon = line.find(':');
			if (colon == std::string_view::npos) continue;
			auto key = strip(line.substr(0, colon));
			const auto value = strip(line.substr(colon + 1));

			if (not key.starts_with("drm-")) continue;
			key.remove_prefix(4);

			if (key == "driver") { out.driver = value; continue; }
			if (key == "pdev") { out.pdev = value; continue; }

			const auto qty = parse_quantity(value);
			if (not qty.has_value()) continue;

			//? Longer prefixes first: "total-cycles-" before "total-", "engine-capacity-" before "engine-"
			if (key == "client-id") {
				out.id = qty->value;
				out.has_id = true;
			}
			else if (key.starts_with("engine-capacity-")) engines[key.substr(16)].capacity = std::max<uint64_t>(1, qty->value);
			else if (key.starts_with("engine-")) engines[key.substr(7)].busy_ns = qty->value;
			else if (key.starts_with("total-cycles-")) engines[key.substr(13)].total_cycles = qty->value;
			else if (key.starts_with("cycles-")) engines[key.substr(7)].cycles = qty->value;
			else if (key.starts_with("resident-")) regions[key.substr(9)].resident = to_bytes(*qty);
			else if (key.starts_with("total-")) regions[key.substr(6)].total = to_bytes(*qty);
			else if (key.starts_with("memory-")) regions[key.substr(7)].legacy = to_bytes(*qty);
		}

		if (out.driver.empty()) return std::nullopt;

		for (const auto& [name, raw] : engines) {
			auto& counters = out.engines.at(static_cast<size_t>(classify_engine(name)));
			counters.busy_ns += raw.busy_ns / raw.capacity;
			counters.cycles += raw.cycles;
			counters.total_cycles += raw.total_cycles;
		}

		for (const auto& [name, raw] : regions) {
			if (name == cpu_region) continue;
			out.mem_bytes += raw.resident.or_else([&raw] { return raw.total; }).or_else([&raw] { return raw.legacy; }).value_or(0);
		}

		return out;
	}

	auto usage_percent(const engine_counters& prev, const engine_counters& cur, uint64_t elapsed_ns) -> double {
		double percent = 0.0;

		//? Time based drivers report how long the engine was busy for the client
		if (elapsed_ns > 0 and cur.busy_ns > prev.busy_ns) {
			percent += 100.0 * static_cast<double>(cur.busy_ns - prev.busy_ns) / static_cast<double>(elapsed_ns);
		}

		//? Cycle based drivers (xe) report busy cycles next to the cycles that elapsed on the engine
		if (cur.total_cycles > prev.total_cycles and cur.cycles > prev.cycles) {
			percent += 100.0 * static_cast<double>(cur.cycles - prev.cycles) / static_cast<double>(cur.total_cycles - prev.total_cycles);
		}

		return std::clamp(percent, 0.0, 100.0);
	}

	auto available(const fs::path& sys_drm_path) -> bool {
		std::error_code ec;
		for (fs::directory_iterator it{sys_drm_path, ec}, end; not ec and it != end; it.increment(ec)) {
			//? Only the cardN entries are devices, cardN-<connector> are their outputs
			const auto name = it->path().filename().string();
			if (not name.starts_with("card") or name.contains('-')) continue;

			std::error_code link_ec;
			const auto driver = fs::read_symlink(it->path() / "device" / "driver", link_ec);
			if (not link_ec and driver.filename() != "nvidia") return true;
		}
		return false;
	}

	void Scanner::collect(const std::vector<size_t>& pids, const fs::path& proc_path, uint64_t now_ns, usage_map& usage) {
		const bool first_cycle = scan_cycle_ == 0;
		++scan_cycle_;
		scanned_processes_ = 0;

		std::vector<observed_client> observed;
		for (const auto pid : pids) {
			auto [state, is_new] = pids_.try_emplace(pid);
			//? A process that shows up after startup has just been started and may open a GPU any moment
			if (is_new and not first_cycle) state->second.fresh_until_cycle = scan_cycle_ + fresh_cycles;
			state->second.seen_cycle = scan_cycle_;

			//? Going through the descriptor table of every process is by far the most expensive part.
			//? It is done for everything once at startup, after that only for the processes that
			//? just started and for a rotating slice of the remaining ones.
			const bool fresh = scan_cycle_ < state->second.fresh_until_cycle;
			const bool due = (pid + scan_cycle_) % rescan_interval == 0;
			const bool full_scan = first_cycle or fresh or due;
			if (not full_scan and state->second.drm_fds.empty()) continue;

			//? In between the known DRM descriptors are enough to follow the clients of a process
			const auto pid_path = proc_path / std::to_string(pid);
			if (full_scan or not refresh_process(pid, pid_path, state->second.drm_fds, observed)) {
				state->second.drm_fds = scan_process(pid, pid_path, observed);
				++scanned_processes_;
			}
		}

		std::erase_if(pids_, [this](const auto& entry) { return entry.second.seen_cycle != scan_cycle_; });

		account(observed, now_ns, usage);
	}

	void Scanner::account(const std::vector<observed_client>& observed, uint64_t now_ns, usage_map& usage) {
		++account_cycle_;

		//? Fold the file descriptors that lead to the same client into one entry. Ordered by key,
		//? which starts with the driver, so that the device label of a process doesn't change
		//? from one cycle to the next.
		struct merged_client {
			size_t pid{};
			client info{};
		};
		std::map<std::string, merged_client> merged;
		for (const auto& entry : observed) {
			auto [slot, is_new] = merged.try_emplace(client_key(entry), merged_client{entry.pid, entry.info});
			if (is_new) continue;

			//? The copies are read moments apart, the highest counters are the most recent ones
			auto& known = slot->second;
			known.pid = std::min(known.pid, entry.pid);
			known.info.mem_bytes = std::max(known.info.mem_bytes, entry.info.mem_bytes);
			for (size_t i = 0; i < engine_class_count; ++i) {
				auto& counters = known.info.engines.at(i);
				const auto& other = entry.info.engines.at(i);
				counters.busy_ns = std::max(counters.busy_ns, other.busy_ns);
				counters.cycles = std::max(counters.cycles, other.cycles);
				counters.total_cycles = std::max(counters.total_cycles, other.total_cycles);
			}
		}

		for (const auto& [key, entry] : merged) {
			auto [state, is_new] = clients_.try_emplace(key);
			const uint64_t elapsed_ns = (is_new or now_ns <= state->second.sampled_ns) ? 0 : now_ns - state->second.sampled_ns;

			std::array<double, engine_class_count> percent{};
			if (not is_new) {
				for (size_t i = 0; i < engine_class_count; ++i) {
					percent.at(i) = usage_percent(state->second.engines.at(i), entry.info.engines.at(i), elapsed_ns);
				}
			}

			//? A repeated timestamp gives no usable interval, keep the older sample as reference
			if (is_new or elapsed_ns > 0) {
				state->second.engines = entry.info.engines;
				state->second.sampled_ns = now_ns;
			}
			state->second.seen_cycle = account_cycle_;

			if (not entry.info.used()) continue;

			Proc::gpu_proc_info client_usage;
			client_usage.usage_p = percent.at(static_cast<size_t>(engine_class::render));
			client_usage.dec_p = percent.at(static_cast<size_t>(engine_class::decode));
			client_usage.enc_p = percent.at(static_cast<size_t>(engine_class::encode));
			client_usage.mem = entry.info.mem_bytes;
			client_usage.dev = entry.info.driver;
			usage[entry.pid].add(client_usage);
		}

		std::erase_if(clients_, [this](const auto& entry) { return entry.second.seen_cycle != account_cycle_; });
	}
}
