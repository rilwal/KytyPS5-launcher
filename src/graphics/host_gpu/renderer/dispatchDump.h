#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_DISPATCHDUMP_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_DISPATCHDUMP_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

class RenderContext;
struct ShaderComputeInputInfo;
namespace HW {
struct ComputeShaderInfo;
}

// Dumps compute dispatches for replay outside Kyty (--dispatch-dump-folder): the registers, user
// SGPRs, program, and the guest memory the dispatch reads before it runs, then the bytes of the
// buffers it writes as Kyty's Vulkan path leaves them. Only buffer-only dispatches qualify; images,
// samplers, GDS, raw global pointers, scratch and GPU-selected calls are counted and skipped.
// One dump per distinct shader, up to --dispatch-dump-count. Each dump waits for the GPU twice.
//
// File layout (little endian): DispatchDumpHeader, range_count DispatchDumpRange, output_count
// DispatchDumpOutput, then each range's bytes, then each output's bytes, in order.
struct DispatchDumpHeader {
	char     magic[8]        = {'K', 'Y', 'D', 'I', 'S', 'P', '0', '1'};
	uint32_t version         = 1;
	uint32_t header_bytes    = sizeof(DispatchDumpHeader);
	uint64_t shader_hash     = 0;
	uint64_t shader_addr     = 0;
	uint32_t code_bytes      = 0; // instructions
	uint32_t program_bytes   = 0; // instructions and the literal buffer after them
	uint32_t rsrc1           = 0;
	uint32_t rsrc2           = 0;
	uint32_t rsrc3           = 0;
	uint32_t resource_limits = 0;
	uint32_t num_thread[3]   = {};
	uint32_t start[3]        = {};
	uint32_t dispatch[3]     = {}; // DISPATCH_DIRECT's arguments as the guest gave them
	uint32_t initiator       = 0;
	uint32_t wave_size       = 64;
	uint32_t user_sgpr_count = 0;
	uint32_t user_sgpr[32]   = {};
	uint32_t frame           = 0;
	uint32_t range_count     = 0;
	uint32_t output_count    = 0;
	uint32_t reserved        = 0;
};

enum DispatchDumpRangeKind : uint32_t {
	DispatchDumpProgram = 1u << 0,
	DispatchDumpTable   = 1u << 1, // scalar loads (descriptor tables, constants)
	DispatchDumpBuffer  = 1u << 2,
	DispatchDumpWritten = 1u << 3,
};

// Page-aligned guest memory before the dispatch.
struct DispatchDumpRange {
	uint64_t addr  = 0;
	uint64_t size  = 0;
	uint32_t kinds = 0;
	uint32_t reserved = 0;
};

// A written buffer, exactly as its descriptor bounds it, after the dispatch.
struct DispatchDumpOutput {
	uint64_t addr = 0;
	uint64_t size = 0;
};

class DispatchDump {
public:
	// Before the dispatch is recorded, with its program bound. Decides whether to dump it and
	// captures what it reads.
	void Begin(RenderContext& context, const HW::ComputeShaderInfo& regs,
	           const ShaderComputeInputInfo& info, const uint32_t dispatch[3], uint32_t initiator);
	// After the dispatch is recorded: reads the written buffers back and writes the file.
	void Finish(RenderContext& context);

	[[nodiscard]] bool Active() const { return m_active; }

private:
	bool                            m_active = false;
	DispatchDumpHeader              m_header;
	std::vector<DispatchDumpRange>  m_ranges;
	std::vector<std::vector<uint8_t>> m_range_data;
	std::vector<DispatchDumpOutput> m_outputs;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_HOST_GPU_RENDERER_DISPATCHDUMP_H_ */
