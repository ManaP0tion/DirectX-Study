#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// DX12 전역 리소스
ComPtr<ID3D12Device> device;
ComPtr<IDXGIFactory6> dxgiFactory;
ComPtr<ID3D12CommandQueue> commandQueue;
ComPtr<IDXGISwapChain4> swapChain;
ComPtr<ID3D12DescriptorHeap> rtvHeap;
ComPtr<ID3D12Resource> renderTargets[2];
ComPtr<ID3D12CommandAllocator> commandAllocator;
ComPtr<ID3D12GraphicsCommandList> commandList;
ComPtr<ID3D12Fence> fence;
UINT64 fenceValue = 0;
HANDLE fenceEvent;
UINT rtvDescriptorSize;
UINT frameIndex;

// 윈도우 프로시저
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// 헬퍼: 리소스 장벽 생성 (CD3DX12 없어도 수동으로 작성)
D3D12_RESOURCE_BARRIER Transition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return barrier;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    // 윈도우 클래스 등록
    const wchar_t CLASS_NAME[] = L"MyWindowClass";

    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    // 윈도우 생성
    HWND hWnd = CreateWindowEx(
        0, CLASS_NAME, L"DirectX 12 윈도우",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 720, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd) return 0;
    ShowWindow(hWnd, nCmdShow);

    // DXGI 팩토리 생성
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) return -1;

    // 디바이스 생성
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) return -1;

    // 커맨드 큐 생성
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(hr)) return -1;

    // 스왑체인 생성
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = 2;
    scDesc.Width = 1280;
    scDesc.Height = 720;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    hr = dxgiFactory->CreateSwapChainForHwnd(
        commandQueue.Get(), hWnd, &scDesc, nullptr, nullptr, &tempSwapChain);
    if (FAILED(hr)) return -1;

    hr = tempSwapChain.As(&swapChain);
    if (FAILED(hr)) return -1;

    frameIndex = swapChain->GetCurrentBackBufferIndex();

    // RTV 힙 생성
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 2;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(hr)) return -1;

    rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // 백버퍼 → RTV 생성
    for (UINT i = 0; i < 2; ++i) {
        hr = swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i]));
        if (FAILED(hr)) return -1;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += i * rtvDescriptorSize;
        device->CreateRenderTargetView(renderTargets[i].Get(), nullptr, handle);
    }

    // 커맨드 할당자, 리스트 생성
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr)) return -1;

    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&commandList));
    if (FAILED(hr)) return -1;
    commandList->Close();

    // 펜스 초기화
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return -1;
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // 메시지 루프
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            // 커맨드 리스트 준비
            commandAllocator->Reset();
            commandList->Reset(commandAllocator.Get(), nullptr);

            // 상태 전환: PRESENT → RENDER_TARGET
            D3D12_RESOURCE_BARRIER barrier = Transition(
                renderTargets[frameIndex].Get(),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            commandList->ResourceBarrier(1, &barrier);

            // 클리어
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtvHandle.ptr += frameIndex * rtvDescriptorSize;

            FLOAT clearColor[] = { 0.0f, 0.2f, 0.6f, 1.0f }; // 파란색
            commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

            // 상태 전환: RENDER_TARGET → PRESENT
            barrier = Transition(
                renderTargets[frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT
            );
            commandList->ResourceBarrier(1, &barrier);

            commandList->Close();

            // 커맨드 리스트 제출
            ID3D12CommandList* lists[] = { commandList.Get() };
            commandQueue->ExecuteCommandLists(1, lists);

            // Present
            swapChain->Present(1, 0);

            // GPU 대기
            fenceValue++;
            commandQueue->Signal(fence.Get(), fenceValue);
            if (fence->GetCompletedValue() < fenceValue) {
                fence->SetEventOnCompletion(fenceValue, fenceEvent);
                WaitForSingleObject(fenceEvent, INFINITE);
            }

            frameIndex = swapChain->GetCurrentBackBufferIndex();
        }
    }

    CloseHandle(fenceEvent);
    return 0;
}
