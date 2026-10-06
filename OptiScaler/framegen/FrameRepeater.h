#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

// EXPERIMENT: "drops" a frame without skipping its Present.
// Before a kept frame is presented its back buffer is copied aside, before a dropped frame is presented the
// last kept frame is copied over its back buffer. The screen keeps showing the previous image while
// the swapchain and frame generation still see every Present. D3D12 only, copies run on the swapchain queue.
class FrameRepeater
{
  public:
    ~FrameRepeater() { Release(); }

    // Returns false when it can't be done (not D3D12, nothing saved yet, errors)
    bool Prepare(IDXGISwapChain* swapchain, IUnknown* deviceOrQueue, bool keep)
    {
        if (!Init(deviceOrQueue))
            return false;

        IDXGISwapChain3* sc3 = nullptr;
        if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))))
            return false;

        ID3D12Resource* backBuffer = nullptr;
        auto hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
        sc3->Release();

        if (FAILED(hr))
            return false;

        bool result = false;
        auto desc = backBuffer->GetDesc();

        if (EnsureSaved(desc) && (keep || _hasSaved))
        {
            auto i = _index++ % Count;

            if (_fence->GetCompletedValue() < _fenceValues[i])
            {
                _fence->SetEventOnCompletion(_fenceValues[i], _event);
                WaitForSingleObject(_event, 1000);
            }

            _allocators[i]->Reset();
            _lists[i]->Reset(_allocators[i], nullptr);

            auto src = keep ? backBuffer : _saved;
            auto dst = keep ? _saved : backBuffer;

            D3D12_RESOURCE_BARRIER barriers[2] {};
            barriers[0] = Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT,
                                     keep ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
            barriers[1] = Transition(_saved, _savedState,
                                     keep ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
            _lists[i]->ResourceBarrier(_savedState == barriers[1].Transition.StateAfter ? 1 : 2, barriers);
            _savedState = barriers[1].Transition.StateAfter;

            _lists[i]->CopyResource(dst, src);

            std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
            _lists[i]->ResourceBarrier(1, barriers);
            _lists[i]->Close();

            ID3D12CommandList* lists[] = { _lists[i] };
            _queue->ExecuteCommandLists(1, lists);
            _queue->Signal(_fence, ++_fenceValue);
            _fenceValues[i] = _fenceValue;

            if (keep)
                _hasSaved = true;

            result = true;
        }

        backBuffer->Release();
        return result;
    }

    void Release()
    {
        if (_fence != nullptr && _fence->GetCompletedValue() < _fenceValue)
        {
            _fence->SetEventOnCompletion(_fenceValue, _event);
            WaitForSingleObject(_event, 1000);
        }

        for (uint32_t i = 0; i < Count; i++)
        {
            SafeRelease(_lists[i]);
            SafeRelease(_allocators[i]);
            _fenceValues[i] = 0;
        }

        SafeRelease(_saved);
        SafeRelease(_fence);
        SafeRelease(_queue);
        SafeRelease(_device);

        if (_event != nullptr)
        {
            CloseHandle(_event);
            _event = nullptr;
        }

        _hasSaved = false;
        _initFailed = false;
    }

    // Swapchain buffers are about to change
    void ReleaseSaved()
    {
        if (_fence != nullptr && _fence->GetCompletedValue() < _fenceValue)
        {
            _fence->SetEventOnCompletion(_fenceValue, _event);
            WaitForSingleObject(_event, 1000);
        }

        SafeRelease(_saved);
        _hasSaved = false;
    }

  private:
    static constexpr uint32_t Count = 8;

    template <typename T> static void SafeRelease(T*& p)
    {
        if (p != nullptr)
        {
            p->Release();
            p = nullptr;
        }
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    bool Init(IUnknown* deviceOrQueue)
    {
        if (_queue != nullptr)
            return true;

        if (_initFailed || deviceOrQueue == nullptr)
            return false;

        _initFailed = true;

        if (FAILED(deviceOrQueue->QueryInterface(IID_PPV_ARGS(&_queue))))
            return false;

        if (FAILED(_queue->GetDevice(IID_PPV_ARGS(&_device))))
            return false;

        auto type = _queue->GetDesc().Type;

        for (uint32_t i = 0; i < Count; i++)
        {
            if (FAILED(_device->CreateCommandAllocator(type, IID_PPV_ARGS(&_allocators[i]))) ||
                FAILED(_device->CreateCommandList(0, type, _allocators[i], nullptr, IID_PPV_ARGS(&_lists[i]))))
            {
                return false;
            }

            _lists[i]->Close();
        }

        if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence))))
            return false;

        _event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        _initFailed = false;
        return true;
    }

    bool EnsureSaved(const D3D12_RESOURCE_DESC& desc)
    {
        if (_saved != nullptr)
        {
            auto savedDesc = _saved->GetDesc();
            if (savedDesc.Width == desc.Width && savedDesc.Height == desc.Height && savedDesc.Format == desc.Format)
                return true;

            ReleaseSaved();
        }

        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto savedDesc = desc;
        savedDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        _savedState = D3D12_RESOURCE_STATE_COPY_DEST;
        return SUCCEEDED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &savedDesc, _savedState,
                                                          nullptr, IID_PPV_ARGS(&_saved)));
    }

    ID3D12Device* _device = nullptr;
    ID3D12CommandQueue* _queue = nullptr;
    ID3D12CommandAllocator* _allocators[Count] {};
    ID3D12GraphicsCommandList* _lists[Count] {};
    ID3D12Fence* _fence = nullptr;
    UINT64 _fenceValues[Count] {};
    UINT64 _fenceValue = 0;
    HANDLE _event = nullptr;
    uint32_t _index = 0;

    ID3D12Resource* _saved = nullptr;
    D3D12_RESOURCE_STATES _savedState = D3D12_RESOURCE_STATE_COPY_DEST;
    bool _hasSaved = false;
    bool _initFailed = false;
};
