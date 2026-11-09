#include "graphics/host_gpu/renderer/dispatchDump.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderBindings.h"
#include "kernel/memory.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics {

namespace {

constexpr uint64_t PageSize       = 4096;
constexpr uint64_t MaxBufferBytes = 256ull << 20;
constexpr uint64_t MaxDumpBytes   = 1ull << 30;

enum class Skip : uint32_t {
	Images,
	Samplers,
	Gds,
	RawPointers,
	ImageHeap,
	Scratch,
	Calls,
	RayTracing,
	NoProgram,
	Walker,
	HugeBuffer,
	TooBig,
	Count
};

constexpr std::array<const char*, static_cast<size_t>(Skip::Count)> SkipNames = {
    "images", "samplers", "gds", "raw-pointers", "image-heap", "scratch",
    "calls", "ray-tracing", "no-program", "walker", "huge-buffer", "too-big",
};

struct DumpState {
	std::unordered_set<uint64_t>                        dumped;
	std::array<uint64_t, static_cast<size_t>(Skip::Count)> skips {};
	uint64_t                                            eligible = 0;
	uint32_t                                            sequence = 0;
};

DumpState& State() {
	static DumpState state;
	return state;
}

std::string SkipSummary() {
	std::string summary;
	const auto& state = State();
	for (size_t i = 0; i < SkipNames.size(); i++) {
		if (state.skips[i] == 0) continue;
		summary += fmt::format("{}{}={}", summary.empty() ? "" : " ", SkipNames[i], state.skips[i]);
	}
	return summary.empty() ? "none" : summary;
}

// Reads guest memory a page at a time; pages that can't be read split the range.
void ReadPages(uint64_t addr, uint64_t size, uint32_t kinds, std::vector<DispatchDumpRange>& ranges,
               std::vector<std::vector<uint8_t>>& data) {
	std::array<uint8_t, PageSize> page {};
	bool open = false;
	for (uint64_t at = addr; at < addr + size; at += PageSize) {
		if (!LibKernel::Memory::TryReadBacking(at, page.data(), PageSize)) {
			open = false;
			continue;
		}
		if (!open) {
			ranges.push_back({.addr = at, .size = 0, .kinds = kinds});
			data.emplace_back();
			open = true;
		}
		ranges.back().size += PageSize;
		data.back().insert(data.back().end(), page.begin(), page.end());
	}
}

} // namespace

void DispatchDump::Begin(RenderContext& context, const HW::ComputeShaderInfo& regs,
                         const ShaderComputeInputInfo& info, const uint32_t dispatch[3],
                         uint32_t initiator) {
	m_active = false;
	if (!Config::DispatchDumpEnabled()) return;
	auto&          state = State();
	const uint32_t frame = static_cast<uint32_t>(context.GetGpu().GetFrameNum());
	if (state.dumped.size() >= Config::GetDispatchDumpCount() ||
	    frame < Config::GetDispatchDumpAfterFrame()) {
		return;
	}
	const auto& module    = *info.stage.module;
	const auto& program   = module.info;
	const auto& resources = *info.stage.resources;
	if (state.dumped.contains(program.shader_hash)) return;

	const auto skip = [&](Skip reason) { state.skips[static_cast<size_t>(reason)]++; };
	const auto& shader_info = program.info;
	const auto  bound       = context.GetPipelineCache().LastBoundPlan();
	const auto& cs          = regs.cs_regs;
	if (!shader_info.images.empty()) return skip(Skip::Images);
	if (!shader_info.samplers.empty()) return skip(Skip::Samplers);
	if (shader_info.uses_gds) return skip(Skip::Gds);
	if (shader_info.uses_dma || !shader_info.address_bases.empty()) return skip(Skip::RawPointers);
	if (shader_info.uses_image_heap) return skip(Skip::ImageHeap);
	if (cs.scratch_en || shader_info.function_scratch) return skip(Skip::Scratch);
	if (bound.plan == nullptr || bound.has_calls) return skip(Skip::Calls);
	if (shader_info.bvh) return skip(Skip::RayTracing);

	uint32_t code_bytes = 0, program_bytes = 0;
	if (!ShaderGetProgramRange(cs.data_addr, code_bytes, program_bytes)) return skip(Skip::NoProgram);

	// Byte ranges the dispatch reads, before page rounding.
	std::vector<std::pair<uint64_t, uint64_t>> reads;
	std::vector<uint32_t>                      read_kinds;
	const auto add = [&](uint64_t addr, uint64_t size, uint32_t kinds) {
		if (size == 0) return;
		reads.emplace_back(addr, size);
		read_kinds.push_back(kinds);
	};
	add(cs.data_addr, program_bytes, DispatchDumpProgram);

	const auto user_data = std::span<const uint32_t>(regs.cs_user_sgpr.value, cs.user_sgpr);
	ShaderRecompiler::IR::SrtRuntime runtime {
	    .user_data             = user_data,
	    .shader_base           = cs.data_addr,
	    .workgroup_counts      = info.workgroup_counts,
	    .indirect_args_address = info.indirect_args_address,
	};
	std::vector<uint32_t>                      flat;
	std::vector<std::pair<uint64_t, uint64_t>> walked;
	if (bound.plan->srt_code != nullptr &&
	    !ShaderRecompiler::IR::WalkSrt(*bound.plan, runtime, flat, &walked)) {
		return skip(Skip::Walker);
	}
	for (const auto& [addr, size]: walked) add(addr, size, DispatchDumpTable);
	for (const auto& [addr, size]: resources.specialization_reads) add(addr, size, DispatchDumpTable);

	m_outputs.clear();
	for (uint32_t i = 0; i < shader_info.buffers.size(); i++) {
		const auto r    = DecodeNativeDescriptor<ShaderBufferResource>(resources.buffers[i]);
		const auto size = r.GetSize();
		if (size > MaxBufferBytes) return skip(Skip::HugeBuffer);
		const bool written = shader_info.buffers[i].written;
		add(r.Base48(), size, DispatchDumpBuffer | (written ? DispatchDumpWritten : 0u));
		if (written && size != 0) m_outputs.push_back({.addr = r.Base48(), .size = size});
	}

	// Round to pages and merge.
	std::map<uint64_t, std::pair<uint64_t, uint32_t>> pages; // start -> (end, kinds)
	for (size_t i = 0; i < reads.size(); i++) {
		auto start = reads[i].first & ~(PageSize - 1);
		auto end   = (reads[i].first + reads[i].second + PageSize - 1) & ~(PageSize - 1);
		auto kinds = read_kinds[i];
		auto it    = pages.upper_bound(start);
		if (it != pages.begin() && std::prev(it)->second.first >= start) --it;
		while (it != pages.end() && it->first <= end) {
			start = std::min(start, it->first);
			end   = std::max(end, it->second.first);
			kinds |= it->second.second;
			it = pages.erase(it);
		}
		pages[start] = {end, kinds};
	}
	uint64_t total = 0;
	for (const auto& [start, value]: pages) total += value.first - start;
	if (total > MaxDumpBytes) return skip(Skip::TooBig);

	state.eligible++;
	auto& buffers = context.GetBufferCache();
	m_ranges.clear();
	m_range_data.clear();
	for (const auto& [start, value]: pages) {
		// Bring back what earlier GPU work left in the cache, so guest memory is current.
		buffers.SyncRangeToGuest(start, value.first - start);
		ReadPages(start, value.first - start, value.second, m_ranges, m_range_data);
	}

	m_header                 = {};
	m_header.shader_hash     = program.shader_hash;
	m_header.shader_addr     = cs.data_addr;
	m_header.code_bytes      = code_bytes;
	m_header.program_bytes   = program_bytes;
	m_header.rsrc1           = cs.rsrc1_raw;
	m_header.rsrc2           = cs.rsrc2_raw;
	m_header.rsrc3           = cs.rsrc3_raw;
	m_header.resource_limits = cs.resource_limits_raw;
	m_header.num_thread[0]   = cs.num_thread_x;
	m_header.num_thread[1]   = cs.num_thread_y;
	m_header.num_thread[2]   = cs.num_thread_z;
	std::copy_n(cs.start, 3, m_header.start);
	std::copy_n(dispatch, 3, m_header.dispatch);
	m_header.initiator       = initiator;
	m_header.wave_size       = info.wave_size;
	m_header.user_sgpr_count = cs.user_sgpr;
	std::copy_n(regs.cs_user_sgpr.value, HW::UserSgprInfo::SGPRS_MAX, m_header.user_sgpr);
	m_header.frame           = frame;
	m_header.range_count     = static_cast<uint32_t>(m_ranges.size());
	m_header.output_count    = static_cast<uint32_t>(m_outputs.size());
	m_active                 = true;
}

void DispatchDump::Finish(RenderContext& context) {
	if (!m_active) return;
	m_active    = false;
	auto& state = State();
	auto& cache = context.GetBufferCache();

	// Written buffers as Kyty's Vulkan path leaves them: the before bytes, with every byte a
	// cached buffer holds copied over them once the GPU is done.
	std::vector<std::vector<uint8_t>> outputs;
	for (const auto& output: m_outputs) {
		auto& bytes = outputs.emplace_back(output.size, 0);
		for (size_t i = 0; i < m_ranges.size(); i++) {
			const auto& range = m_ranges[i];
			const auto  start = std::max(range.addr, output.addr);
			const auto  end   = std::min(range.addr + range.size, output.addr + output.size);
			if (start < end) {
				std::copy_n(m_range_data[i].data() + (start - range.addr), end - start,
				            bytes.data() + (start - output.addr));
			}
		}
		(void)cache.ReadDeviceRange(output.addr, output.size, bytes.data());
	}

	const auto folder = Config::GetDispatchDumpFolder();
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	const auto path = folder / fmt::format("dispatch-{:03}-{:016x}.kydisp", state.sequence++,
	                                       m_header.shader_hash);
	std::ofstream file(path, std::ios::binary);
	const auto    write = [&](const void* data, size_t size) {
		file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	};
	write(&m_header, sizeof(m_header));
	write(m_ranges.data(), m_ranges.size() * sizeof(DispatchDumpRange));
	write(m_outputs.data(), m_outputs.size() * sizeof(DispatchDumpOutput));
	uint64_t total = 0;
	for (const auto& data: m_range_data) {
		write(data.data(), data.size());
		total += data.size();
	}
	for (const auto& data: outputs) write(data.data(), data.size());
	file.close();
	state.dumped.insert(m_header.shader_hash);

	Log::WriteToConsoleAndLog(fmt::format(
	    "dispatch dump {}: shader 0x{:016x} at 0x{:x}, frame {}, groups {}x{}x{}, {} ranges "
	    "({} KiB), {} outputs; {} of {} dumped; skipped so far: {}\n",
	    path.string(), m_header.shader_hash, m_header.shader_addr, m_header.frame,
	    m_header.dispatch[0], m_header.dispatch[1], m_header.dispatch[2], m_ranges.size(),
	    total / 1024, m_outputs.size(), state.dumped.size(), Config::GetDispatchDumpCount(),
	    SkipSummary()));
	if (!file) {
		Log::WriteToConsoleAndLog(fmt::format("dispatch dump: could not write {}\n", path.string()));
	}
}

} // namespace Libs::Graphics
