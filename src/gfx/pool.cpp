// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "gfx/pool.hpp"

namespace hayai::gfx {

namespace {
constexpr uint32_t align_up(uint32_t v, uint32_t a)
{
	return (v + a - 1) & ~(a - 1);
}
} // namespace

bool Pool::create(dk::Device dev, uint32_t size, uint32_t flags)
{
	destroy();

	size = align_up(size, DK_MEMBLOCK_ALIGNMENT);
	block_ = dk::MemBlockMaker{ dev, size }.setFlags(flags).create();
	if(!block_)
		return false;

	cpu_ = static_cast<uint8_t *>(block_.getCpuAddr());
	gpu_ = block_.getGpuAddr();
	size_ = size;
	used_ = 0;
	return true;
}

void Pool::destroy()
{
	if(block_)
	{
		block_.destroy();
		block_ = nullptr;
	}
	cpu_ = nullptr;
	gpu_ = DK_GPU_ADDR_INVALID;
	size_ = 0;
	used_ = 0;
}

Pool::Alloc Pool::allocate(uint32_t size, uint32_t align)
{
	const uint32_t offset = align_up(used_, align ? align : 1);
	if(offset + size > size_)
		return {};

	used_ = offset + size;

	Alloc a;
	a.cpu = cpu_ ? cpu_ + offset : nullptr;
	a.gpu = gpu_ + offset;
	a.offset = offset;
	a.size = size;
	return a;
}

} // namespace hayai::gfx
