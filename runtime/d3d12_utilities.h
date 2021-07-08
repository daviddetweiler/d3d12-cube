#pragma once

#include <cstddef>
#include <tuple>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <dxgi1_6.h>

namespace helium {
	auto transition(ID3D12Resource& resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
	{
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

	auto get_buffer(IDXGISwapChain& swap_chain, unsigned int index)
	{
		return winrt::capture<ID3D12Resource>(&swap_chain, &IDXGISwapChain::GetBuffer, index);
	}

	auto get_extent(IDXGISwapChain1& swap_chain)
	{
		DXGI_SWAP_CHAIN_DESC1 info {};
		winrt::check_hresult(swap_chain.GetDesc1(&info));
		return std::make_tuple(info.Width, info.Height);
	}
}
