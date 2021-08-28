#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include <gsl/gsl>

#include <Windows.h>

#include <winrt/base.h>

#include <DirectXMath.h>
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

		auto create_device(IDXGIFactory6& factory, bool enable_debugging)
		{
			if (enable_debugging)
				winrt::capture<ID3D12Debug>(D3D12GetDebugInterface)->EnableDebugLayer();

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

		auto create_descriptor_heap(
			ID3D12Device& device,
			D3D12_DESCRIPTOR_HEAP_TYPE type,
			unsigned int size,
			D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE)
		{
			D3D12_DESCRIPTOR_HEAP_DESC info {};
			info.NumDescriptors = size;
			info.Type = type;
			info.Flags = flags;
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
			info.RasterizerState.FrontCounterClockwise = true;
			info.DepthStencilState.DepthEnable = true;
			info.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
			info.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			info.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			info.NumRenderTargets = 1;
			info.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
			info.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			info.SampleDesc.Count = 1;

			D3D12_INPUT_ELEMENT_DESC position {};
			position.Format = DXGI_FORMAT_R32G32B32_FLOAT;
			position.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
			position.SemanticName = "POSITION";

			info.InputLayout.NumElements = 1;
			info.InputLayout.pInputElementDescs = &position;

			return winrt::capture<ID3D12PipelineState>(&device, &ID3D12Device::CreateGraphicsPipelineState, &info);
		}

		auto create_root_signature(ID3D12Device& device)
		{
			D3D12_ROOT_PARAMETER constants {};
			constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			constants.Constants.Num32BitValues = 4 * 4 * 2;

			D3D12_ROOT_SIGNATURE_DESC info {};
			info.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			info.NumParameters = 1;
			info.pParameters = &constants;

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

		struct vertex_buffer {
			winrt::com_ptr<ID3D12Resource> buffer;
			D3D12_VERTEX_BUFFER_VIEW view;
		};

		struct index_buffer {
			winrt::com_ptr<ID3D12Resource> buffer;
			D3D12_INDEX_BUFFER_VIEW view;
			unsigned int size;
		};

		index_buffer create_index_buffer(ID3D12Device& device, unsigned int size)
		{
			auto buffer = create_buffer(device, size * sizeof(unsigned int));
			D3D12_INDEX_BUFFER_VIEW view {};
			view.BufferLocation = buffer->GetGPUVirtualAddress();
			view.SizeInBytes = gsl::narrow_cast<UINT>(size * sizeof(unsigned int));
			view.Format = DXGI_FORMAT_R32_UINT;
			return {std::move(buffer), view, size};
		}

		vertex_buffer create_vertex_buffer(ID3D12Device& device, std::uint64_t size, std::uint64_t elem_size)
		{
			auto buffer = create_buffer(device, size * elem_size);
			D3D12_VERTEX_BUFFER_VIEW view {};
			view.BufferLocation = buffer->GetGPUVirtualAddress();
			view.SizeInBytes = gsl::narrow<UINT>(size * elem_size);
			view.StrideInBytes = gsl::narrow<UINT>(elem_size);
			return {std::move(buffer), view};
		}

		struct view_matrices {
			DirectX::XMMATRIX view;
			DirectX::XMMATRIX projection;
		};

		struct descriptor_heaps {
			const winrt::com_ptr<ID3D12DescriptorHeap> rtv_heap {};
			const winrt::com_ptr<ID3D12DescriptorHeap> dsv_heap {};
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv_base {};
			const D3D12_CPU_DESCRIPTOR_HANDLE dsv_base {};
			const UINT rtv_size {};

			descriptor_heaps() = default;

			descriptor_heaps(ID3D12Device& device) :
				rtv_heap {create_descriptor_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2)},
				dsv_heap {create_descriptor_heap(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1)},
				rtv_base {rtv_heap->GetCPUDescriptorHandleForHeapStart()},
				dsv_base {dsv_heap->GetCPUDescriptorHandleForHeapStart()},
				rtv_size {device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)}
			{
			}
		};

		winrt::com_ptr<ID3D12Resource> create_matrix_buffer(ID3D12Device& device, D3D12_CPU_DESCRIPTOR_HANDLE cbv)
		{
			D3D12_HEAP_PROPERTIES heap {};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;

			D3D12_RESOURCE_DESC info {};
			info.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			info.Width = sizeof(view_matrices);
			info.Height = 1;
			info.DepthOrArraySize = 1;
			info.MipLevels = 1;
			info.Format = DXGI_FORMAT_UNKNOWN;
			info.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			info.SampleDesc.Count = 1;

			const auto buffer = create_buffer(device, sizeof(view_matrices), true);
			D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_info {};
			cbv_info.BufferLocation = buffer->GetGPUVirtualAddress();
			cbv_info.SizeInBytes = gsl::narrow<UINT>(sizeof(view_matrices));
			device.CreateConstantBufferView(&cbv_info, cbv);

			return buffer;
		}

		struct geometry_buffers {
			vertex_buffer vertices {};
			index_buffer indices {};
		};

		geometry_buffers geometry_load(
			ID3D12Device& device,
			ID3D12GraphicsCommandList& list,
			ID3D12CommandAllocator& allocator,
			ID3D12CommandQueue& queue,
			gpu_fence& fence)
		{
			geometry_buffers geometry;

			const auto object = load_wavefront("cube.wv");
			const auto& vertices = object.positions;
			std::vector<unsigned int> indices(object.faces.size() * 3);
			for (gsl::index i {}; i < gsl::narrow_cast<gsl::index>(object.faces.size()); ++i) {
				const auto& face = object.faces.at(i);
				for (gsl::index j {}; j < 3; ++j)
					indices.at(i * 3 + j) = face.indices.at(j) - 1;
			}

			const auto upload_buffer = create_upload_buffer(
				device, indices.size() * sizeof(unsigned int) + vertices.size() * sizeof(vector3));

			geometry.vertices = create_vertex_buffer(device, vertices.size(), sizeof(vector3));
			geometry.indices = create_index_buffer(device, gsl::narrow<unsigned int>(indices.size()));

			const auto data = map(*upload_buffer);
			std::memcpy(data, indices.data(), indices.size() * sizeof(unsigned int));
			std::memcpy(
				std::next(static_cast<char*>(data), indices.size() * sizeof(unsigned int)),
				vertices.data(),
				vertices.size() * sizeof(vector3));

			unmap(*upload_buffer);
			winrt::check_hresult(allocator.Reset());
			winrt::check_hresult(list.Reset(&allocator, nullptr));

			list.CopyBufferRegion(
				geometry.indices.buffer.get(), 0, upload_buffer.get(), 0, indices.size() * sizeof(unsigned int));

			list.CopyBufferRegion(
				geometry.vertices.buffer.get(),
				0,
				upload_buffer.get(),
				indices.size() * sizeof(unsigned int),
				vertices.size() * sizeof(vector3));

			// TODO: check resource states for vertex, index, and view-matrix buffers

			winrt::check_hresult(list.Close());
			execute(queue, list);
			fence.bump(queue);
			fence.block();

			return geometry;
		}

		struct render_state {
			const winrt::com_ptr<ID3D12Resource> m_depth_buffer {};
			geometry_buffers geometry;
			view_matrices m_matrices {};

			render_state() = default;

			render_state(ID3D12Device& device, IDXGISwapChain1& swap_chain, D3D12_CPU_DESCRIPTOR_HANDLE dsv) :
				m_depth_buffer {create_depth_buffer(device, dsv, get_extent(swap_chain))},
				geometry {},
				m_matrices {
					DirectX::XMMatrixTranslation(0.0f, 0.0f, -0.9f),
					DirectX::XMMatrixOrthographicLH(1.0f, 1.0f, 0.0f, 100.0f)}
			{
			}
		};

		void record_commands(
			ID3D12GraphicsCommandList& command_list,
			ID3D12CommandAllocator& allocator,
			ID3D12PipelineState& pipeline_state,
			ID3D12RootSignature& root_signature,
			ID3D12Resource& buffer,
			D3D12_CPU_DESCRIPTOR_HANDLE rtv,
			D3D12_CPU_DESCRIPTOR_HANDLE dsv,
			const render_state& state)
		{
			winrt::check_hresult(command_list.Reset(&allocator, &pipeline_state));

			command_list.SetGraphicsRootSignature(&root_signature);
			command_list.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			command_list.IASetVertexBuffers(0, 1, &state.geometry.vertices.view);
			command_list.IASetIndexBuffer(&state.geometry.indices.view);
			command_list.OMSetRenderTargets(1, &rtv, false, &dsv);
			maximize_rasterizer(command_list, buffer);

			command_list.SetGraphicsRoot32BitConstants(0, 4 * 4 * 2, &state.m_matrices, 0);

			std::array barriers {transition(buffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
			command_list.ResourceBarrier(gsl::narrow_cast<UINT>(barriers.size()), barriers.data());

			std::array clear_color {0.0f, 0.0f, 0.0f, 1.0f};
			command_list.ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			command_list.ClearRenderTargetView(rtv, clear_color.data(), 0, nullptr);
			command_list.DrawIndexedInstanced(state.geometry.indices.size, 1, 0, 0, 0);

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
				// Need to execute copy commands here
				auto& list = *m_command_lists.front();
				auto& allocator = *m_allocators.front();
				m_state.geometry = geometry_load(*m_device, list, allocator, *m_queue, m_fence);
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

				// FIXME: This thing is really, really oversized / hyper-specialized
				record_commands(
					list,
					allocator,
					*m_pipeline,
					*m_root_signature,
					*get_buffer(*m_swap_chain, index),
					offset(m_heaps.rtv_base, m_heaps.rtv_size, index),
					m_heaps.dsv_base,
					m_state);

				execute(*m_queue, list);
				winrt::check_hresult(m_swap_chain->Present(1, 0));
				m_fence.bump(*m_queue);
			}

		private:
			const winrt::com_ptr<ID3D12Device4> m_device {};
			const winrt::com_ptr<ID3D12CommandQueue> m_queue {};
			const winrt::com_ptr<ID3D12Resource> m_upload_buffer {};
			const descriptor_heaps m_heaps {};
			gpu_fence m_fence;

			const winrt::com_ptr<ID3D12RootSignature> m_root_signature {};
			const winrt::com_ptr<ID3D12PipelineState> m_pipeline {};
			const std::array<winrt::com_ptr<ID3D12CommandAllocator>, 2> m_allocators {};
			const std::array<winrt::com_ptr<ID3D12GraphicsCommandList>, 2> m_command_lists {};

			const winrt::com_ptr<IDXGISwapChain3> m_swap_chain {};
			render_state m_state {};

			// FIXME: a horrid hack, we should only have one upload ringbuffer
			d3d12_renderer(IDXGIFactory6& factory, HWND window, bool enable_debugging) :
				m_device {create_device(factory, enable_debugging)},
				m_queue {create_command_queue(*m_device)},
				m_upload_buffer {create_upload_buffer(*m_device, sizeof(view_matrices))},
				m_heaps {*m_device},
				m_fence {*m_device},
				m_root_signature {create_root_signature(*m_device)},
				m_pipeline {create_default_pipeline_state(*m_device, *m_root_signature)},
				m_allocators {create_command_allocator(*m_device), create_command_allocator(*m_device)},
				m_command_lists {create_command_list(*m_device), create_command_list(*m_device)},
				m_swap_chain {attach_swap_chain(factory, *m_device, window, *m_queue, m_heaps.rtv_base)},
				m_state {*m_device, *m_swap_chain, m_heaps.dsv_base}
			{
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

/*
	I can identify four main complaints I have:
	- d3d12_renderer is a *thicc* class
	- record_commands is an oversized / over-specialized function
	- there's a lot of duplicated code surrounding buffer creation
	- upload buffer management is ad-hoc or non-existent
	- Note that a lot of these resources can be easily grouped by usage
	- Don't forget to never over-generalize
*/

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
