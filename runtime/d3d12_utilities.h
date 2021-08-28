#ifndef HELIUM_D3D12_UTILITIES_H
#define HELIUM_D3D12_UTILITIES_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace helium {
	auto transition(ID3D12Resource& resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
	{
		Expects(before != after); // Or else what, pray tell, is the purpose of your barrier?
		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = &resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		return barrier;
	}

	auto reverse(D3D12_RESOURCE_BARRIER& barrier) noexcept
	{
		Expects(barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
	}

	constexpr D3D12_CPU_DESCRIPTOR_HANDLE
	offset(D3D12_CPU_DESCRIPTOR_HANDLE handle, std::size_t size, std::size_t index)
	{
		return {handle.ptr + index * size};
	}

	constexpr D3D12_CPU_DESCRIPTOR_HANDLE
	offset(D3D12_GPU_DESCRIPTOR_HANDLE handle, std::size_t size, std::size_t index)
	{
		return {handle.ptr + index * size};
	}

	auto get_buffer(IDXGISwapChain& swap_chain, unsigned int index)
	{
		return winrt::capture<ID3D12Resource>(&swap_chain, &IDXGISwapChain::GetBuffer, index);
	}

	struct extent2d {
		unsigned int width;
		unsigned int height;
	};

	extent2d get_extent(IDXGISwapChain1& swap_chain)
	{
		DXGI_SWAP_CHAIN_DESC1 info {};
		winrt::check_hresult(swap_chain.GetDesc1(&info));
		return {info.Width, info.Height};
	}

	class gpu_fence {
	public:
		gpu_fence(ID3D12Device& device, std::uint64_t initial_value = 0) :
			m_value {initial_value},
			m_fence {
				winrt::capture<ID3D12Fence>(&device, &ID3D12Device::CreateFence, initial_value, D3D12_FENCE_FLAG_NONE)},
			m_event {winrt::check_pointer(CreateEvent(nullptr, false, false, nullptr))}
		{
		}

		// Otherwise counts get out of sync
		gpu_fence(gpu_fence&) = delete;
		gpu_fence& operator=(gpu_fence&) = delete;

		void bump(ID3D12CommandQueue& queue) { winrt::check_hresult(queue.Signal(m_fence.get(), ++m_value)); }

		// TODO: This may not be the right API...
		void block(std::uint64_t offset = 0)
		{
			if (m_fence->GetCompletedValue() < m_value - offset) {
				winrt::check_hresult(m_fence->SetEventOnCompletion(m_value, m_event.get()));
				winrt::check_bool(WaitForSingleObject(m_event.get(), INFINITE) == WAIT_OBJECT_0);
			}
		}

	private:
		std::uint64_t m_value {};
		const winrt::com_ptr<ID3D12Fence> m_fence {};
		const winrt::handle m_event {};
	};

	template <typename... list_types>
	void execute(ID3D12CommandQueue& queue, list_types&&... lists)
	{
		const std::array<ID3D12CommandList*, sizeof...(lists)> list_array {(&lists, ...)};
		queue.ExecuteCommandLists(gsl::narrow_cast<unsigned int>(list_array.size()), list_array.data());
	}

	auto create_buffer(ID3D12Device& device, std::size_t size, bool is_shader_visible = false)
	{
		D3D12_HEAP_PROPERTIES heap {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC info {};
		info.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		info.Width = size;
		info.Height = 1;
		info.DepthOrArraySize = 1;
		info.MipLevels = 1;
		info.Format = DXGI_FORMAT_UNKNOWN;
		info.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		info.SampleDesc.Count = 1;
		info.Flags = is_shader_visible ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

		return winrt::capture<ID3D12Resource>(
			&device,
			&ID3D12Device::CreateCommittedResource,
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&info,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr);
	}

	auto create_upload_buffer(ID3D12Device& device, std::size_t size)
	{
		D3D12_HEAP_PROPERTIES heap {};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC info {};
		info.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		info.Width = size;
		info.Height = 1;
		info.DepthOrArraySize = 1;
		info.MipLevels = 1;
		info.Format = DXGI_FORMAT_UNKNOWN;
		info.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		info.SampleDesc.Count = 1;
		info.Flags = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

		return winrt::capture<ID3D12Resource>(
			&device,
			&ID3D12Device::CreateCommittedResource,
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&info,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr);
	}

	void* map(ID3D12Resource& resource)
	{
		D3D12_RANGE range {};
		void* data {};
		winrt::check_hresult(resource.Map(0, &range, &data));
		return data;
	}

	void unmap(ID3D12Resource& resource)
	{
		D3D12_RANGE range {};
		resource.Unmap(0, &range);
	}
}

#endif
