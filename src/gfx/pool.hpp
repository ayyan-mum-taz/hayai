// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <deko3d.hpp>

#include <cstdint>

namespace hayai::gfx {

// A bump allocator over a single deko3d memory block.
//
// Everything hayai allocates on the GPU is either permanent (shaders, vertex
// data, descriptor sets) or tied to a swapchain image, so nothing is ever freed
// mid-session and a bump allocator is the whole story. No free list, no
// fragmentation, no allocator on the frame path.
class Pool
{
public:
	struct Alloc
	{
		void *cpu = nullptr;
		DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
		uint32_t offset = 0;
		uint32_t size = 0;

		// Validity is size-based, not cpu-based: GPU-only pools (framebuffer
		// images, which carry no CPU-access flags) legitimately have a null
		// cpu pointer, and testing that instead rejected every valid
		// framebuffer allocation.
		explicit operator bool() const { return size != 0; }
	};

	Pool() = default;
	~Pool() { destroy(); }
	Pool(const Pool &) = delete;
	Pool &operator=(const Pool &) = delete;

	bool create(dk::Device dev, uint32_t size, uint32_t flags);
	void destroy();

	// Returns a null Alloc if the pool is exhausted; callers size pools up
	// front, so that is a programming error rather than a runtime condition.
	Alloc allocate(uint32_t size, uint32_t align);

	dk::MemBlock block() const { return block_; }
	uint32_t used() const { return used_; }
	uint32_t capacity() const { return size_; }

private:
	dk::MemBlock block_ = nullptr;
	uint8_t *cpu_ = nullptr;
	DkGpuAddr gpu_ = DK_GPU_ADDR_INVALID;
	uint32_t size_ = 0;
	uint32_t used_ = 0;
};

} // namespace hayai::gfx
