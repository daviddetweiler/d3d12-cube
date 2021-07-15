#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include "d3d12_utilities.h"
#include "shader_loading.h"
#include "wavefront_loader.h"

namespace helium {
	namespace {
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

		auto create_device(IDXGIFactory6& factory)
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

		auto create_descriptor_heap(ID3D12Device& device, D3D12_DESCRIPTOR_HEAP_TYPE type, unsigned int size)
		{
			D3D12_DESCRIPTOR_HEAP_DESC info {};
			info.NumDescriptors = size;
			info.Type = type;
			return winrt::capture<ID3D12DescriptorHeap>(&device, &ID3D12Device::CreateDescriptorHeap, &info);
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

		auto create_default_pipeline_state(ID3D12Device& device, ID3D12RootSignature& root_signature)
		{
			const auto vertex_shader = load_compiled_shader(L"vertex.cso");
			const auto pixel_shader = load_compiled_shader(L"pixel.cso");

			D3D12_GRAPHICS_PIPELINE_STATE_DESC info {};
			info.pRootSignature = &root_signature;
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

			return winrt::capture<ID3D12PipelineState>(&device, &ID3D12Device::CreateGraphicsPipelineState, &info);
		}

		auto create_root_signature(ID3D12Device& device)
		{
			D3D12_ROOT_SIGNATURE_DESC info {};
			info.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			winrt::com_ptr<ID3DBlob> result {};
			winrt::com_ptr<ID3DBlob> error {};
			winrt::check_hresult(
				D3D12SerializeRootSignature(&info, D3D_ROOT_SIGNATURE_VERSION_1, result.put(), error.put()));
			return winrt::capture<ID3D12RootSignature>(
				&device, &ID3D12Device::CreateRootSignature, 0, result->GetBufferPointer(), result->GetBufferSize());
		}

		auto create_depth_buffer(ID3D12Device& device, D3D12_CPU_DESCRIPTOR_HANDLE dsv, const extent2d& size)
		{
			D3D12_HEAP_PROPERTIES properties {};
			properties.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC info {};
			info.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			info.DepthOrArraySize = 1;
			info.Width = size.width;
			info.Height = size.height;
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

		auto attach_swap_chain(
			IDXGIFactory3& factory,
			ID3D12Device& device,
			HWND window,
			ID3D12CommandQueue& queue,
			D3D12_CPU_DESCRIPTOR_HANDLE rtvs)
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
					offset(rtvs, rtv_size, i));
			}

			return swap_chain.as<IDXGISwapChain3>();
		}

		void maximize_rasterizer(ID3D12GraphicsCommandList& list, ID3D12Resource& target)
		{
			const auto info = target.GetDesc();

			D3D12_RECT scissor {};
			scissor.right = gsl::narrow_cast<long>(info.Width);
			scissor.bottom = gsl::narrow_cast<long>(info.Height);

			D3D12_VIEWPORT viewport {};
			viewport.Width = gsl::narrow_cast<float>(info.Width);
			viewport.Height = gsl::narrow_cast<float>(info.Height);
			viewport.MaxDepth = 1.0f;

			list.RSSetScissorRects(1, &scissor);
			list.RSSetViewports(1, &viewport);
		}

		auto create_upload_buffer(ID3D12Device& device, std::uint64_t size)
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

			return winrt::capture<ID3D12Resource>(
				&device,
				&ID3D12Device::CreateCommittedResource,
				&heap,
				D3D12_HEAP_FLAG_NONE,
				&info,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr);
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
			maximize_rasterizer(command_list, buffer);

			std::array barriers {transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
			command_list.ResourceBarrier(gsl::narrow_cast<UINT>(barriers.size()), barriers.data());

			std::array clear_color {0.0f, 0.0f, 0.0f, 1.0f};
			command_list.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			command_list.ClearRenderTargetView(rtv, clear_color.data(), 0, nullptr);
			command_list.DrawInstanced(3, 1, 0, 0);

			reverse(barriers.at(0));
			command_list.ResourceBarrier(gsl::narrow_cast<UINT>(barriers.size()), barriers.data());

			winrt::check_hresult(command_list.Close());
		}

		class d3d12_renderer {
		public:
			d3d12_renderer(HWND window, bool enable_debugging) :
				d3d12_renderer {
					*winrt::capture<IDXGIFactory6>(
						CreateDXGIFactory2, enable_debugging ? DXGI_CREATE_FACTORY_DEBUG : 0),
					window,
					enable_debugging}
			{
				load_wavefront("cube.wv");
			}

			d3d12_renderer(d3d12_renderer&) = delete;
			d3d12_renderer(d3d12_renderer&&) = delete;
			d3d12_renderer& operator=(d3d12_renderer&) = delete;
			d3d12_renderer& operator=(d3d12_renderer&&) = delete;

			~d3d12_renderer() noexcept
			{
				GSL_SUPPRESS(f .6)
				m_fence.block();
			}

			void render()
			{
				m_fence.block(1);
				const auto index = m_swap_chain->GetCurrentBackBufferIndex();
				auto& allocator = *m_allocators.at(index);
				auto& list = *m_command_lists.at(index);
				winrt::check_hresult(allocator.Reset());
				record_commands(
					list,
					allocator,
					*m_pipeline,
					*m_root_signature,
					*get_buffer(*m_swap_chain, index),
					offset(
						m_rtv_heap->GetCPUDescriptorHandleForHeapStart(),
						m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV),
						index),
					m_dsv_heap->GetCPUDescriptorHandleForHeapStart());

				execute(*m_queue, list);
				winrt::check_hresult(m_swap_chain->Present(1, 0));
				m_fence.bump(*m_queue);
			}

		private:
			const winrt::com_ptr<ID3D12Device4> m_device {};
			const winrt::com_ptr<ID3D12CommandQueue> m_queue {};
			const winrt::com_ptr<ID3D12DescriptorHeap> m_rtv_heap {};
			const winrt::com_ptr<ID3D12DescriptorHeap> m_dsv_heap {};
			gpu_fence m_fence;

			const winrt::com_ptr<ID3D12RootSignature> m_root_signature {};
			const winrt::com_ptr<ID3D12PipelineState> m_pipeline {};
			const std::array<winrt::com_ptr<ID3D12CommandAllocator>, 2> m_allocators {};
			const std::array<winrt::com_ptr<ID3D12GraphicsCommandList>, 2> m_command_lists {};

			const winrt::com_ptr<IDXGISwapChain3> m_swap_chain {};
			const winrt::com_ptr<ID3D12Resource> m_depth_buffer {};

			winrt::com_ptr<ID3D12Resource> m_upload_buffer {};

			d3d12_renderer(IDXGIFactory6& factory, HWND window, bool enable_debugging) :
				m_device {[enable_debugging, &factory] {
					if (enable_debugging)
						winrt::capture<ID3D12Debug>(D3D12GetDebugInterface)->EnableDebugLayer();

					return create_device(factory);
				}()},
				m_queue {create_command_queue(*m_device)},
				m_rtv_heap {create_descriptor_heap(*m_device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2)},
				m_dsv_heap {create_descriptor_heap(*m_device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1)},
				m_fence {*m_device, 1},
				m_root_signature {create_root_signature(*m_device)},
				m_pipeline {create_default_pipeline_state(*m_device, *m_root_signature)},
				m_allocators {create_command_allocator(*m_device), create_command_allocator(*m_device)},
				m_command_lists {create_command_list(*m_device), create_command_list(*m_device)},
				m_swap_chain {attach_swap_chain(
					factory, *m_device, window, *m_queue, m_rtv_heap->GetCPUDescriptorHandleForHeapStart())},
				m_depth_buffer {create_depth_buffer(
					*m_device, m_dsv_heap->GetCPUDescriptorHandleForHeapStart(), get_extent(*m_swap_chain))},
				m_upload_buffer {}
			{
				// Load object vertices, etc.

				// Compute upload buffer size
				// allocate buffer + buffers and views
				// Execute upload steps (map, copy, unmap, gpu copies, wait for fence
			}
		};

		void execute_game_thread(const std::atomic_bool& is_exit_required, HWND window, bool enable_debugging)
		{
			d3d12_renderer renderer {window, enable_debugging};
			winrt::check_bool(PostMessage(window, ready_message, 0, 0));
			while (!is_exit_required)
				renderer.render();
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
		nullptr));

	std::atomic_bool is_exit_required {};
	std::thread game_thread {
		[&is_exit_required, window] { execute_game_thread(is_exit_required, window, IsDebuggerPresent()); }};

	MSG message {};
	while (GetMessage(&message, nullptr, 0, 0)) {
		TranslateMessage(&message);
		DispatchMessage(&message);
	}

	is_exit_required = true;
	game_thread.join();

	return 0;
}
