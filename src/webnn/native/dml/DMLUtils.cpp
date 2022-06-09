// Copyright 2021 The WebNN-native Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "DMLUtils.h"

namespace webnn::native::dml {

    bool IsWarpAdapter(IDXGIAdapter1* pAdapter) {
        DXGI_ADAPTER_DESC1 pDesc;
        WEBNN_CHECK(pAdapter->GetDesc1(&pDesc));
        // See here for documentation on filtering WARP adapter:
        // https://docs.microsoft.com/en-us/windows/desktop/direct3ddxgi/d3d10-graphics-programming-guide-dxgi#new-info-about-enumerating-adapters-for-windows-8
        auto isBasicRenderDriverVendorId = pDesc.VendorId == 0x1414;
        auto isBasicRenderDriverDeviceId = pDesc.DeviceId == 0x8c;
        auto isSoftwareAdapter = pDesc.Flags == DXGI_ADAPTER_FLAG_SOFTWARE;
        return isSoftwareAdapter || (isBasicRenderDriverVendorId && isBasicRenderDriverDeviceId);
    }

    void Device::CloseExecuteResetWait() {
        WEBNN_CHECK(mCommandList->Close());
        ID3D12CommandList* mCommandLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(ARRAYSIZE(mCommandLists), mCommandLists);
        WEBNN_CHECK(mCommandQueue.Get()->GetDevice(IID_PPV_ARGS(mD3D12Device.GetAddressOf())));
        ComPtr<ID3D12Fence> fence;
        WEBNN_CHECK(mD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(fence.GetAddressOf())));
        WEBNN_CHECK(mCommandQueue.Get()->Signal(fence.Get(), 1));
        WEBNN_CHECK(fence->SetEventOnCompletion(1, nullptr));
        WEBNN_CHECK(mCommandAllocator->Reset());
        WEBNN_CHECK(mCommandList->Reset(mCommandAllocator.Get(), nullptr));
    }

    void Device::Init(DXGI_GPU_PREFERENCE gpuPreference, bool useGpu) {
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
#endif
        ComPtr<IDXGIAdapter1> dxgiAdapter;
        if (useGpu) {
            ComPtr<IDXGIFactory6> dxgiFactory;
            WEBNN_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
            UINT i = 0;
            while (dxgiFactory->EnumAdapterByGpuPreference(
                       i++, gpuPreference, IID_PPV_ARGS(&dxgiAdapter)) != DXGI_ERROR_NOT_FOUND) {
                if (!IsWarpAdapter(dxgiAdapter.Get())) {
                    break;
                }
            }
        }
        if (!useGpu || FAILED(D3D12CreateDevice(dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(&mD3D12Device)))) {
            // If a computer's display driver is not functioning or is disabled, the computer's
            // primary (NULL) adapter might also be called "Microsoft Basic Render Driver."
            ComPtr<IDXGIFactory4> dxgiFactory;
            WEBNN_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
            WEBNN_CHECK(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&dxgiAdapter)));
            WEBNN_CHECK(D3D12CreateDevice(dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                          IID_PPV_ARGS(&mD3D12Device)));
        }

        D3D12_COMMAND_QUEUE_DESC commandQueueDesc{};
        commandQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        commandQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        WEBNN_CHECK(
            mD3D12Device->CreateCommandQueue(&commandQueueDesc, IID_PPV_ARGS(&mCommandQueue)));
        WEBNN_CHECK(mD3D12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&mCommandAllocator)));
        WEBNN_CHECK(mD3D12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                    mCommandAllocator.Get(), nullptr,
                                                    IID_PPV_ARGS(&mCommandList)));

        // Create the DirectML device.
        DML_CREATE_DEVICE_FLAGS dmlCreateDeviceFlags = DML_CREATE_DEVICE_FLAG_NONE;
#if defined(_DEBUG)
        dmlCreateDeviceFlags = DML_CREATE_DEVICE_FLAG_DEBUG;
#endif
        if (dmlCreateDeviceFlags == DML_CREATE_DEVICE_FLAG_DEBUG) {
            if (DMLCreateDevice(mD3D12Device.Get(), DML_CREATE_DEVICE_FLAG_DEBUG,
                                IID_PPV_ARGS(&mDevice)) < 0) {
                dawn::WarningLog() << "Failed to create a DirectML device with debug flag, "
                                      "will fall back to use none flag.";
                WEBNN_CHECK(DMLCreateDevice(mD3D12Device.Get(), DML_CREATE_DEVICE_FLAG_NONE,
                                            IID_PPV_ARGS(&mDevice)));
            }
        } else {
            WEBNN_CHECK(DMLCreateDevice(mD3D12Device.Get(), DML_CREATE_DEVICE_FLAG_NONE,
                                        IID_PPV_ARGS(&mDevice)));
        }
    };

    void Device::CreateResourcesForCompiledOperatorInitializer() {
        if (mConstantInputsResourceSize != 0) {
            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mConstantInputsResourceSize), D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&mUploadResource)));

            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mConstantInputsResourceSize,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mInputResource)));
        }

        if (mTemporaryResourceSize != 0 && mTemporaryResource == nullptr) {
            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mTemporaryResourceSize,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mTemporaryResource)));
        }

        if (mPersistentResourceSize != 0 && mPersistentResource == nullptr) {
            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mPersistentResourceSize,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&mPersistentResource)));
        }
    };

    void Device::CreateResourcesForCompiledOperator() {
        if (mNonConstantInputsResourceSize) {
            // Release the upload resource and input resource which has been allocated for
            // initializing constant inputs and then re-allocate them with new size to prepare
            // for initializing common inputs.
            mUploadResource = nullptr;
            mInputResource = nullptr;
            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mNonConstantInputsResourceSize),
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mUploadResource)));

            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mNonConstantInputsResourceSize,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mInputResource)));
        }

        if (mOutputResourceSize) {
            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mOutputResourceSize,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mOutputResource)));

            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &CreateHeapProperties(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE,
                &CreateResourceDesc(mOutputResourceSize), D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&mReadBackResource)));
        }
    };

    void Device::BindTemporaryResource(bool bindForInitializer) {
        if (mTemporaryResourceSize != 0) {
            if ((bindForInitializer && mInitializedTemporaryResourceSize != 0) ||
                !bindForInitializer) {
                DML_BUFFER_BINDING bufferBinding{mTemporaryResource.Get(), 0,
                                                 mTemporaryResourceSize};
                DML_BINDING_DESC bindingDesc{DML_BINDING_TYPE_BUFFER, &bufferBinding};
                mBindingTable->BindTemporaryResource(&bindingDesc);
            }
        }
    };

    void Device::BindPersistentResource(bool bindForInitializer) {
        if (mPersistentResourceSize != 0) {
            DML_BUFFER_BINDING bufferBinding{mPersistentResource.Get(), 0, mPersistentResourceSize};
            DML_BINDING_DESC bindingDesc{DML_BINDING_TYPE_BUFFER, &bufferBinding};
            if (bindForInitializer) {
                mBindingTable->BindOutputs(1, &bindingDesc);
            } else {
                mBindingTable->BindPersistentResource(&bindingDesc);
            }
        }
    };

    void Device::CopyBufferRegion(ComPtr<ID3D12Resource> srcResource,
                                  ComPtr<ID3D12Resource> destResource,
                                  UINT64 resourceSize,
                                  D3D12_RESOURCE_STATES state,
                                  bool needBarrierEnd) {
        D3D12_RESOURCE_BARRIER resourceBarrier;
        if (state == D3D12_RESOURCE_STATE_COPY_DEST) {
            resourceBarrier.Transition.pResource = destResource.Get();
        } else if (state == D3D12_RESOURCE_STATE_COPY_SOURCE) {
            resourceBarrier.Transition.pResource = srcResource.Get();
        } else {
            dawn::ErrorLog() << "Unsupported D3D12_RESOURCE_STATES.";
            DAWN_ASSERT(0);
        }
        resourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        resourceBarrier.Transition.StateAfter = state;
        resourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        resourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        resourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        mCommandList->ResourceBarrier(1, &resourceBarrier);
        mCommandList->CopyBufferRegion(destResource.Get(), 0, srcResource.Get(), 0, resourceSize);
        if (needBarrierEnd) {
            resourceBarrier.Transition.StateBefore = state;
            resourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            mCommandList->ResourceBarrier(1, &resourceBarrier);
        }
    }

    void Device::FillUploadResourceAndInputBindings(
        uint64_t uploadResourceSize,
        std::vector<DML_BUFFER_BINDING>& inputBufferBinding,
        const std::vector<std::shared_ptr<InputNode>>& inputNodes,
        std::unordered_map<std::string, Input> namedInputs) {
        D3D12_RANGE uploadBufferRange{0, uploadResourceSize};
        int8_t* uploadBuffer;
        WEBNN_CHECK(
            mUploadResource->Map(0, &uploadBufferRange, reinterpret_cast<void**>(&uploadBuffer)));
        uint64_t offset = 0;
        for (size_t i = 0; i < inputNodes.size(); ++i) {
            auto inputNode = inputNodes[i];
            if (namedInputs.empty()) {
                if (inputNode->type == NodeType::ConstantInput) {
                    offset =
                        RoundUpToMultiple(offset, (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
                    inputBufferBinding[i].Buffer = mInputResource.Get();
                    inputBufferBinding[i].Offset = offset;
                    inputBufferBinding[i].SizeInBytes = inputNode->byteLength;
                    memcpy(uploadBuffer + offset, inputNode->buffer,
                           static_cast<size_t>(inputNode->byteLength));
                    offset = offset + inputNode->byteLength;
                }
            } else {
                if (inputNode->type == NodeType::NonConstantInput) {
                    offset =
                        RoundUpToMultiple(offset, (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
                    auto arrayBufferView = namedInputs[inputNode->name].resource.arrayBufferView;
                    inputBufferBinding[i].Buffer = mInputResource.Get();
                    inputBufferBinding[i].Offset = offset;
                    inputBufferBinding[i].SizeInBytes = arrayBufferView.byteLength;
                    memcpy(
                        uploadBuffer + offset,
                        static_cast<int8_t*>(arrayBufferView.buffer) + arrayBufferView.byteOffset,
                        arrayBufferView.byteLength);
                    offset = offset + arrayBufferView.byteLength;
                }
            }
        }
        mUploadResource->Unmap(0, nullptr);
    }

    void Device::InitializeGraph(const CompiledGraph* compiledGraph) {
        DAWN_ASSERT(compiledGraph != nullptr);
        DML_BINDING_PROPERTIES initializeBindingProperties =
            compiledGraph->compiledOperatorInitializer->GetBindingProperties();
        DML_BINDING_PROPERTIES executeBindingProperties =
            compiledGraph->compiledOperator->GetBindingProperties();
        UINT descriptorCount = std::max(initializeBindingProperties.RequiredDescriptorCount,
                                        executeBindingProperties.RequiredDescriptorCount);
        mInitializedTemporaryResourceSize = initializeBindingProperties.TemporaryResourceSize;
        mTemporaryResourceSize = std::max(mInitializedTemporaryResourceSize,
                                          executeBindingProperties.TemporaryResourceSize);
        mPersistentResourceSize = executeBindingProperties.PersistentResourceSize;

        // Describe and create a constant buffer view (CBV), Shader resource view (SRV), and
        // unordered access view (UAV) descriptor heap.
        D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
        descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptorHeapDesc.NumDescriptors = descriptorCount;
        descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        WEBNN_CHECK(mD3D12Device->CreateDescriptorHeap(&descriptorHeapDesc,
                                                       IID_PPV_ARGS(&mDescriptorHeap)));

        // Create a binding table over the descriptor heap we just created.
        mBindingTableDesc.Dispatchable = compiledGraph->compiledOperatorInitializer.Get();
        mBindingTableDesc.CPUDescriptorHandle =
            mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        mBindingTableDesc.GPUDescriptorHandle =
            mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        // The size of the binding table, in descriptors. This is the maximum number of
        // descriptors that DirectML is permitted to write, from the start of both the
        // supplied CPU and GPU descriptor handles.
        mBindingTableDesc.SizeInDescriptors = descriptorCount;
        WEBNN_CHECK(mDevice->CreateBindingTable(&mBindingTableDesc, IID_PPV_ARGS(&mBindingTable)));

        // Initialize constant inputs.
        auto inputNodes = compiledGraph->inputNodes;
        for (auto& inputNode : inputNodes) {
            if (inputNode->type == NodeType::ConstantInput) {
                uint64_t offset = RoundUpToMultiple(mConstantInputsResourceSize,
                                                    (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
                mConstantInputsResourceSize = offset + inputNode->byteLength;
            } else {
                uint64_t offset = RoundUpToMultiple(mNonConstantInputsResourceSize,
                                                    (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
                mNonConstantInputsResourceSize = offset + inputNode->byteLength;
            }
        }
        // Set the descriptor heap(s).
        ID3D12DescriptorHeap* descriptorHeaps[] = {mDescriptorHeap.Get()};
        mCommandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);

        CreateResourcesForCompiledOperatorInitializer();
        BindTemporaryResource();
        BindPersistentResource();

        if (mConstantInputsResourceSize) {
            std::vector<DML_BUFFER_BINDING> inputBufferBinding(inputNodes.size());
            FillUploadResourceAndInputBindings(mConstantInputsResourceSize, inputBufferBinding,
                                               inputNodes);
            // Copy buffer from uploadResource to inputResource.
            CopyBufferRegion(mUploadResource, mInputResource, mConstantInputsResourceSize,
                             D3D12_RESOURCE_STATE_COPY_DEST);

            DML_BUFFER_ARRAY_BINDING inputBufferArrayBinding = {};
            inputBufferArrayBinding.BindingCount = inputBufferBinding.size();
            inputBufferArrayBinding.Bindings = inputBufferBinding.data();
            DML_BINDING_DESC inputBindingDesc{DML_BINDING_TYPE_BUFFER_ARRAY,
                                              &inputBufferArrayBinding};
            mBindingTable->BindInputs(1, &inputBindingDesc);
        }

        // Record execution of the operator initializer.
        // The command recorder is a stateless object that records Dispatches into an existing
        // Direct3D 12 command list.
        WEBNN_CHECK(mDevice->CreateCommandRecorder(IID_PPV_ARGS(&mCommandRecorder)));
        mCommandRecorder->RecordDispatch(mCommandList.Get(),
                                         compiledGraph->compiledOperatorInitializer.Get(),
                                         mBindingTable.Get());
        CloseExecuteResetWait();

        // Bind and execute the operator on the GPU.
        // Reset the binding table to bind for the operator we want to execute (it was
        // previously used to bind for the initializer).
        auto outputNodes = compiledGraph->outputNodes;
        for (size_t i = 0; i < outputNodes.size(); ++i) {
            uint64_t byteLength = reinterpret_cast<const DML_BUFFER_TENSOR_DESC*>(
                                      outputNodes[i].outputTensorDesc.Desc)
                                      ->TotalTensorSizeInBytes;
            uint64_t offset = RoundUpToMultiple(mOutputResourceSize,
                                                (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
            mOutputResourceSize = offset + byteLength;
        }

        mBindingTableDesc.Dispatchable = compiledGraph->compiledOperator.Get();
        mBindingTable->Reset(&mBindingTableDesc);

        CreateResourcesForCompiledOperator();
        BindTemporaryResource(false);
        BindPersistentResource(false);
    };

    void Device::ExecuteGraph(const CompiledGraph* compiledGraph,
                              std::unordered_map<std::string, Input> namedInputs,
                              NamedOutputsBase* outputs) {
        DAWN_ASSERT(compiledGraph != nullptr);
        DAWN_ASSERT(outputs != nullptr);
        // Initialize non-constant inputs.
        auto inputNodes = compiledGraph->inputNodes;
        if (mNonConstantInputsResourceSize) {
            std::vector<DML_BUFFER_BINDING> inputBufferBinding(inputNodes.size());
            FillUploadResourceAndInputBindings(mNonConstantInputsResourceSize, inputBufferBinding,
                                               inputNodes, namedInputs);
            // Copy buffer from uploadResource to inputResource.
            CopyBufferRegion(mUploadResource, mInputResource, mNonConstantInputsResourceSize,
                             D3D12_RESOURCE_STATE_COPY_DEST);

            std::vector<DML_BINDING_DESC> inputBindingDesc(inputNodes.size());
            for (size_t i = 0; i < inputBufferBinding.size(); ++i) {
                if (inputBufferBinding[i].Buffer != nullptr) {
                    inputBindingDesc[i] = {DML_BINDING_TYPE_BUFFER, &inputBufferBinding[i]};
                }
            }
            mBindingTable->BindInputs(inputBindingDesc.size(), inputBindingDesc.data());
        }

        // Prepare for outputs and read back buffer from Gpu.
        std::vector<ArrayBufferView> outputArrayBufferViews;
        ArrayBufferView outputArrayBufferView;
        auto outputNodes = compiledGraph->outputNodes;
        for (size_t i = 0; i < outputNodes.size(); ++i) {
            std::string name = outputNodes[i].name;
            auto namedOutputs = outputs->GetRecords();
            if (namedOutputs.find(name) != namedOutputs.end()) {
                outputArrayBufferView = namedOutputs[name].arrayBufferView;
                outputArrayBufferViews.push_back(outputArrayBufferView);
                DAWN_ASSERT(outputArrayBufferView.buffer != nullptr &&
                            outputArrayBufferView.byteLength != 0);
            } else {
                size_t byteLength = reinterpret_cast<const DML_BUFFER_TENSOR_DESC*>(
                                        outputNodes[i].outputTensorDesc.Desc)
                                        ->TotalTensorSizeInBytes;
                // It is an unuseful output of dml graph. We need not read back and copy buffer
                // to it, just reserve it as a placeholder.
                outputArrayBufferView = {nullptr, byteLength, 0};
                outputArrayBufferViews.push_back(outputArrayBufferView);
            }
        }

        std::vector<DML_BINDING_DESC> outputBindingDesc(outputNodes.size());
        std::vector<DML_BUFFER_BINDING> outputBufferBinding(outputNodes.size());

        uint64_t outputOffset = 0;
        for (size_t i = 0; i < outputNodes.size(); ++i) {
            auto output = outputArrayBufferViews[i];
            outputOffset =
                RoundUpToMultiple(outputOffset, (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
            outputBufferBinding[i].Buffer = mOutputResource.Get();
            outputBufferBinding[i].Offset = outputOffset;
            outputBufferBinding[i].SizeInBytes = output.byteLength;
            outputBindingDesc[i] = {DML_BINDING_TYPE_BUFFER, &outputBufferBinding[i]};
            outputOffset = outputOffset + output.byteLength;
        }
        mBindingTable->BindOutputs(outputBindingDesc.size(), outputBindingDesc.data());

        // Record execution of the compiled operator.
        ID3D12DescriptorHeap* descriptorHeaps[] = {mDescriptorHeap.Get()};
        mCommandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);
        mCommandRecorder->RecordDispatch(mCommandList.Get(), compiledGraph->compiledOperator.Get(),
                                         mBindingTable.Get());

        // Copy buffer from outputResource to readBackResource.
        CopyBufferRegion(mOutputResource, mReadBackResource, mOutputResourceSize,
                         D3D12_RESOURCE_STATE_COPY_SOURCE, false);
        CloseExecuteResetWait();

        D3D12_RANGE tensorBufferRange{0, mOutputResourceSize};
        int8_t* readBackBuffer;
        WEBNN_CHECK(mReadBackResource->Map(0, &tensorBufferRange,
                                           reinterpret_cast<void**>(&readBackBuffer)));

        uint64_t offset = 0;
        for (size_t i = 0; i < outputNodes.size(); ++i) {
            offset = RoundUpToMultiple(offset, (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
            ArrayBufferView output = outputArrayBufferViews[i];
            if (output.buffer) {
                memcpy(static_cast<int8_t*>(output.buffer) + output.byteOffset,
                       readBackBuffer + offset, output.byteLength);
            }
            offset += output.byteLength;
        }

        mReadBackResource->Unmap(0, nullptr);
    }
}  // namespace webnn::native::dml