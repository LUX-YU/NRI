// © 2021 NVIDIA Corporation

Result FenceD3D12::Create(const FenceDesc& fenceDesc) {
    if (fenceDesc.initialValue != SWAPCHAIN_SEMAPHORE) {
        D3D12_FENCE_FLAGS flags = fenceDesc.shared ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE;
        HRESULT hr = m_Device->CreateFence(fenceDesc.initialValue, flags, IID_PPV_ARGS(&m_Fence));
        NRI_RETURN_ON_BAD_HRESULT(&m_Device, hr, "ID3D12Device::CreateFence");

        m_Event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    } else if (fenceDesc.shared)
        return Result::UNSUPPORTED; // no binary fences

    return Result::SUCCESS;
}

Result FenceD3D12::Create(const FenceD3D12Desc& fenceD3D12Desc) {
    m_Fence = fenceD3D12Desc.d3d12Fence;
    m_Event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    return Result::SUCCESS;
}

Result FenceD3D12::GetSharedHandle(SharedHandle& sharedHandle) const {
    sharedHandle = {};

    HANDLE handle = nullptr;
    HRESULT hr = m_Device->CreateSharedHandle(m_Fence.GetInterface(), nullptr, GENERIC_ALL, nullptr, &handle);
    NRI_RETURN_ON_BAD_HRESULT(&m_Device, hr, "ID3D12Device::CreateSharedHandle");

    sharedHandle.handle = handle;
    sharedHandle.type = SharedHandleType::D3D12_FENCE;

    return Result::SUCCESS;
}

NRI_INLINE uint64_t FenceD3D12::GetFenceValue() const {
    return m_Fence ? m_Fence->GetCompletedValue() : 0;
}

NRI_INLINE Result FenceD3D12::QueueSignal(QueueD3D12& queue, uint64_t value) {
    if (m_Fence) {
        HRESULT hr = ((ID3D12CommandQueue*)queue)->Signal(m_Fence, value);
        NRI_RETURN_ON_BAD_HRESULT(&m_Device, hr, "ID3D12CommandQueue::Signal");
    }
    return Result::SUCCESS;
}

NRI_INLINE Result FenceD3D12::QueueWait(QueueD3D12& queue, uint64_t value) {
    if (m_Fence && value) {
        HRESULT hr = ((ID3D12CommandQueue*)queue)->Wait(m_Fence, value);
        NRI_RETURN_ON_BAD_HRESULT(&m_Device, hr, "ID3D12CommandQueue::Wait");
    }
    return Result::SUCCESS;
}

NRI_INLINE Result FenceD3D12::Wait(uint64_t value) {
    if (!m_Fence)
        return Result::SUCCESS;

    if (m_Event == 0 || m_Event == INVALID_HANDLE_VALUE) {
        // Busy wait
        while (m_Fence->GetCompletedValue() < value)
            ;
    } else if (m_Fence->GetCompletedValue() < value) {
        // Event-based wait
        ResetEvent(m_Event);

        HRESULT hr = m_Fence->SetEventOnCompletion(value, m_Event);
        NRI_RETURN_ON_BAD_HRESULT(&m_Device, hr, "ID3D12Fence::SetEventOnCompletion");

        uint32_t result = WaitForSingleObjectEx(m_Event, NRI_TIMEOUT_FENCE, FALSE);
        NRI_RETURN_ON_FAILURE(&m_Device, result == WAIT_OBJECT_0, Result::FAILURE, "WaitForSingleObjectEx() failed, result = 0x%08X!", result);
    }

    return Result::SUCCESS;
}

NRI_INLINE Result FenceD3D12::GetValueChecked(uint64_t& value) const {
    if (!m_Fence)
        return Result::INVALID_ARGUMENT;
    uint64_t completed = m_Fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        return Result::DEVICE_LOST;
    value = completed;
    return Result::SUCCESS;
}

NRI_INLINE Result FenceD3D12::WaitChecked(uint64_t value, uint64_t timeoutNs, bool& ready) {
    ready = false;
    uint64_t completed = 0;
    Result status = GetValueChecked(completed);
    if (status != Result::SUCCESS)
        return status;
    if (completed >= value) {
        ready = true;
        return Result::SUCCESS;
    }
    if (timeoutNs == 0)
        return Result::SUCCESS;
    ExclusiveScope lock(m_CheckedLock);
    status = GetValueChecked(completed);
    if (status != Result::SUCCESS)
        return status;
    if (completed >= value) {
        ready = true;
        return Result::SUCCESS;
    }
    if (!m_CheckedEvent)
        m_CheckedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_CheckedEvent)
        return Result::FAILURE;
    // Keep the event alive through a timed-out registration. Never reset an event still pending in the kernel.
    if (m_CheckedValue == 0 || completed >= m_CheckedValue) {
        ResetEvent(m_CheckedEvent);
        HRESULT hr = m_Fence->SetEventOnCompletion(value, m_CheckedEvent);
        if (FAILED(hr))
            return GetResultFromHRESULT(hr);
        m_CheckedValue = value;
    }
    uint64_t milliseconds = timeoutNs / 1000000 + (timeoutNs % 1000000 != 0);
    DWORD timeout = timeoutNs == UINT64_MAX ? INFINITE : (DWORD)std::min<uint64_t>(milliseconds, INFINITE - 1);
    DWORD waited = WaitForSingleObject(m_CheckedEvent, timeout);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT)
        return Result::FAILURE;
    status = GetValueChecked(completed);
    ready = status == Result::SUCCESS && completed >= value;
    return status;
}
