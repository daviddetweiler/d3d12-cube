#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include "shader_loading.h"

namespace helium {
	namespace {
		constexpr auto is_d3d12_debugging_enabled = true;
		constexpr auto ready_message = WM_USER;

		LRESULT handle_message(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
		{
			switch (message) {
			case ready_message:
				ShowWindow(window, SW_SHOW);
				return 0;

			case WM_CLOSE:
				ShowWindow(window, SW_HIDE);
				PostQuitMessage(0);
				return 0;

			default:
				return DefWindowProc(window, message, w, l);
			}
		}

		auto get_high_performance_device(IDXGIFactory6& factory)
		{
			const auto adapter = winrt::capture<IDXGIAdapter>(
				&factory, &IDXGIFactory6::EnumAdapterByGpuPreference, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE);

			return winrt::capture<ID3D12Device4>(D3D12CreateDevice, adapter.get(), D3D_FEATURE_LEVEL_12_1);
		}

		auto create_command_queue(ID3D12Device& device)
		{
			D3D12_COMMAND_QUEUE_DESC info {};
			info.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			return winrt::capture<ID3D12CommandQueue>(&device, &ID3D12Device::CreateCommandQueue, &info);
		}

		class gpu_fence {
		public:
			gpu_fence(ID3D12Device& device) :
				m_value {},
				m_fence {
					winrt::capture<ID3D12Fence>(&device, &ID3D12Device::CreateFence, m_value, D3D12_FENCE_FLAG_NONE)},
				m_event {winrt::check_pointer(CreateEvent(nullptr, false, false, nullptr))}
			{
			}

			// Otherwise counts get out of sync
			gpu_fence(gpu_fence&) = delete;
			gpu_fence& operator=(gpu_fence&) = delete;

			void bump(ID3D12CommandQueue& queue) { winrt::check_hresult(queue.Signal(m_fence.get(), ++m_value)); }

			void block()
			{
				if (m_fence->GetCompletedValue() < m_value) {
					winrt::check_hresult(m_fence->SetEventOnCompletion(m_value, m_event.get()));
					winrt::check_bool(WaitForSingleObject(m_event.get(), INFINITE) == WAIT_OBJECT_0);
				}
			}

		private:
			std::uint64_t m_value {};
			winrt::com_ptr<ID3D12Fence> m_fence {};
			winrt::handle m_event {};
		};

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

		auto create_descriptor_heap(ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned int size)
		{
			D3D12_DESCRIPTOR_HEAP_DESC info {};
			info.NumDescriptors = size;
			info.Type = type;
			return winrt::capture<ID3D12DescriptorHeap>(&device, &ID3D12Device::CreateDescriptorHeap, &info);
		}

		auto create_pipeline_state(ID3D12Device& device)
		{
			const auto vertex_shader = load_compiled_shader(L"vertex.cso");
			const auto pixel_shader = load_compiled_shader(L"pixel.cso");

			// Used only for validating the pipeline, afaik
			// Why do we even declare it in HLSL?
			auto root_signature = winrt::capture<ID3D12RootSignature>(
				&device, &ID3D12Device::CreateRootSignature, 0, vertex_shader.data(), vertex_shader.size());

			D3D12_GRAPHICS_PIPELINE_STATE_DESC info {};
			info.pRootSignature = root_signature.get();
			info.VS.BytecodeLength = vertex_shader.size();
			info.VS.pShaderBytecode = vertex_shader.data();
			info.PS.BytecodeLength = pixel_shader.size();
			info.PS.pShaderBytecode = pixel_shader.data();
			info.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
			info.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			info.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			info.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
			info.RasterizerState.DepthClipEnable = true;
			info.DepthStencilState.DepthEnable = true;
			info.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
			info.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			info.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			info.NumRenderTargets = 1;
			info.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			info.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			info.SampleDesc.Count = 1;

			return std::make_tuple(
				std::move(root_signature),
				winrt::capture<ID3D12PipelineState>(&device, &ID3D12Device::CreateGraphicsPipelineState, &info));
		}

		auto create_depth_buffer(
			ID3D12Device& device, D3D12_CPU_DESCRIPTOR_HANDLE dsv, unsigned int width, unsigned int height)
		{
			D3D12_HEAP_PROPERTIES properties {};
			properties.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC info {};
			info.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			info.DepthOrArraySize = 1;
			info.Width = width;
			info.Height = height;
			info.MipLevels = 1;
			info.SampleDesc.Count = 1;
			info.Format = DXGI_FORMAT_D32_FLOAT;
			info.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE clear_value {};
			clear_value.DepthStencil.Depth = 1.0f;
			clear_value.Format = info.Format;

			const auto buffer = winrt::capture<ID3D12Resource>(
				&device,
				&ID3D12Device::CreateCommittedResource,
				&properties,
				D3D12_HEAP_FLAG_NONE,
				&info,
				D3D12_RESOURCE_STATE_DEPTH_WRITE,
				&clear_value);

			D3D12_DEPTH_STENCIL_VIEW_DESC dsv_info {};
			dsv_info.Format = info.Format;
			dsv_info.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			device.CreateDepthStencilView(buffer.get(), &dsv_info, dsv);

			return buffer;
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

		auto attach_swap_chain(
			IDXGIFactory3& factory,
			ID3D12Device& device,
			HWND window,
			ID3D12CommandQueue& queue,
			D3D12_CPU_DESCRIPTOR_HANDLE rtv_base)
		{
			DXGI_SWAP_CHAIN_DESC1 info {};
			info.BufferCount = 2;
			info.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			info.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			info.SampleDesc.Count = 1;
			info.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			winrt::com_ptr<IDXGISwapChain1> swap_chain {};
			winrt::check_hresult(
				factory.CreateSwapChainForHwnd(&queue, window, &info, nullptr, nullptr, swap_chain.put()));

			const auto rtv_size = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			for (gsl::index i {}; i < 2; ++i) {
				D3D12_RENDER_TARGET_VIEW_DESC rtv_info {};
				rtv_info.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				rtv_info.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				device.CreateRenderTargetView(
					get_buffer(*swap_chain, gsl::narrow_cast<unsigned int>(i)).get(),
					&rtv_info,
					offset(rtv_base, rtv_size, i));
			}

			return swap_chain.as<IDXGISwapChain3>();
		}

		void record_commands(
			ID3D12GraphicsCommandList& command_list,
			ID3D12CommandAllocator& allocator,
			ID3D12PipelineState& pipeline_state,
			ID3D12RootSignature& root_signature,
			ID3D12Resource& buffer,
			D3D12_CPU_DESCRIPTOR_HANDLE rtv,
			D3D12_CPU_DESCRIPTOR_HANDLE dsv)
		{
			winrt::check_hresult(command_list.Reset(&allocator, &pipeline_state));

			command_list.SetGraphicsRootSignature(&root_signature);
			command_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			command_list.OMSetRenderTargets(1, &rtv, false, &dsv);

			const auto buffer_info = buffer.GetDesc();

			D3D12_RECT scissor {};
			scissor.right = gsl::narrow_cast<long>(buffer_info.Width);
			scissor.bottom = gsl::narrow_cast<long>(buffer_info.Height);

			D3D12_VIEWPORT viewport {};
			viewport.Width = gsl::narrow_cast<float>(buffer_info.Width);
			viewport.Height = gsl::narrow_cast<float>(buffer_info.Height);
			viewport.MaxDepth = 1.0f;

			command_list.RSSetScissorRects(1, &scissor);
			command_list.RSSetViewports(1, &viewport);

			std::array<D3D12_RESOURCE_BARRIER, 1> barriers {
				transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};

			command_list.ResourceBarrier(gsl::narrow_cast<UINT>(barriers.size()), barriers.data());

			std::array<float, 4> clear_color {0.0f, 0.0f, 0.0f, 1.0f};
			command_list.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			command_list.ClearRenderTargetView(rtv, clear_color.data(), 0, nullptr);
			command_list.DrawInstanced(3, 1, 0, 0);

			reverse(barriers.at(0));
			command_list.ResourceBarrier(gsl::narrow_cast<UINT>(barriers.size()), barriers.data());

			winrt::check_hresult(command_list.Close());
		}

		auto get_extent(IDXGISwapChain1& swap_chain)
		{
			DXGI_SWAP_CHAIN_DESC1 info {};
			winrt::check_hresult(swap_chain.GetDesc1(&info));
			return std::make_tuple(info.Width, info.Height);
		}

		auto create_command_allocator(ID3D12Device& device)
		{
			return winrt::capture<ID3D12CommandAllocator>(
				&device, &ID3D12Device::CreateCommandAllocator, D3D12_COMMAND_LIST_TYPE_DIRECT);
		}

		auto create_command_list(ID3D12Device4& device)
		{
			return winrt::capture<ID3D12GraphicsCommandList>(
				&device,
				&ID3D12Device4::CreateCommandList1,
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				D3D12_COMMAND_LIST_FLAG_NONE);
		}

		// TODO: Descriptor spans! Maybe...

		void execute_game_thread(const std::atomic_bool& is_exit_required, HWND window)
		{
			if constexpr (is_d3d12_debugging_enabled)
				winrt::capture<ID3D12Debug>(D3D12GetDebugInterface)->EnableDebugLayer();

			const auto factory = winrt::capture<IDXGIFactory6>(
				CreateDXGIFactory2, is_d3d12_debugging_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0);

			const auto device = get_high_performance_device(*factory);
			const auto queue = create_command_queue(*device);
			gpu_fence fence {*device};
			const auto rtv_heap = create_descriptor_heap(*device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
			const auto rtv_base = rtv_heap->GetCPUDescriptorHandleForHeapStart();
			const auto dsv_heap = create_descriptor_heap(*device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1);
			const auto dsv_base = dsv_heap->GetCPUDescriptorHandleForHeapStart();
			const auto swap_chain
				= attach_swap_chain(*factory, *device, window, *queue, rtv_heap->GetCPUDescriptorHandleForHeapStart());

			const auto [width, height] = get_extent(*swap_chain);
			const auto depth_buffer
				= create_depth_buffer(*device, dsv_heap->GetCPUDescriptorHandleForHeapStart(), width, height);

			const auto [root_signature, pipeline_state] = create_pipeline_state(*device);
			const auto allocator = create_command_allocator(*device);
			const auto list = create_command_list(*device);
			const auto rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			winrt::check_bool(PostMessage(window, ready_message, 0, 0));
			while (!is_exit_required) {
				fence.block();
				const auto index = swap_chain->GetCurrentBackBufferIndex();
				winrt::check_hresult(allocator->Reset());
				record_commands(
					*list,
					*allocator,
					*pipeline_state,
					*root_signature,
					*get_buffer(*swap_chain, index),
					offset(rtv_base, rtv_size, index),
					dsv_base);

				ID3D12CommandList* const list_pointer {list.get()};
				queue->ExecuteCommandLists(1, &list_pointer);
				winrt::check_hresult(swap_chain->Present(1, 0));
				fence.bump(*queue);
			}

			fence.block();
		}
	}
}

int WinMain(HINSTANCE self, HINSTANCE, char*, int)
{
	using namespace helium;

	WNDCLASS window_class {};
	window_class.hInstance = self;
	window_class.lpszClassName = L"helium::window";
	window_class.hCursor = winrt::check_pointer(LoadCursor(nullptr, IDC_ARROW));
	window_class.lpfnWndProc = handle_message;
	winrt::check_bool(RegisterClass(&window_class));

	std::atomic_bool is_size_updated {};
	const auto window = winrt::check_pointer(CreateWindowEx(
		WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
		window_class.lpszClassName,
		L"Helium Prototype",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		nullptr,
		nullptr,
		self,
		&is_size_updated));

	std::atomic_bool is_exit_required {};
	std::thread game_thread {
		[&is_exit_required, &is_size_updated, window] { execute_game_thread(is_exit_required, window); }};

	MSG message {};
	while (GetMessage(&message, nullptr, 0, 0)) {
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	is_exit_required = true;
	game_thread.join();

	return 0;
}
