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

#include "webnn_native/dml/GraphDML.h"

#include <algorithm>

#include "DMLUtils.h"
#include "common/Assert.h"
#include "common/Log.h"
#include "webnn_native/ErrorData.h"
#include "webnn_native/NamedInputs.h"
#include "webnn_native/NamedOutputs.h"
#include "webnn_native/Utils.h"
#include "webnn_native/dml/ContextDML.h"

#define WEBNN_CHECK(hr)                             \
    if (((HRESULT)(hr)) < 0) {                      \
        dawn::ErrorLog() << "Failed to do " << #hr; \
        DAWN_ASSERT(0);                             \
    }

namespace webnn_native { namespace dml {

    // An adapter called the "Microsoft Basic Render Driver" is always present. This adapter is a
    // render-only device that has no display outputs.
    HRESULT IsWarpAdapter(IDXGIAdapter1* pAdapter, bool* isWarpAdapter) {
        DXGI_ADAPTER_DESC1 pDesc;
        WEBNN_CHECK(pAdapter->GetDesc1(&pDesc));
        // see here for documentation on filtering WARP adapter:
        // https://docs.microsoft.com/en-us/windows/desktop/direct3ddxgi/d3d10-graphics-programming-guide-dxgi#new-info-about-enumerating-adapters-for-windows-8
        auto isBasicRenderDriverVendorId = pDesc.VendorId == 0x1414;
        auto isBasicRenderDriverDeviceId = pDesc.DeviceId == 0x8c;
        auto isSoftwareAdapter = pDesc.Flags == DXGI_ADAPTER_FLAG_SOFTWARE;
        *isWarpAdapter =
            isSoftwareAdapter || (isBasicRenderDriverVendorId && isBasicRenderDriverDeviceId);
        return S_OK;
    }

    void Graph::InitD3D12(DXGI_GPU_PREFERENCE gpuPreference, bool useGpu) {
#if defined(_DEBUG)
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
#endif
        Microsoft::WRL::ComPtr<IDXGIAdapter1> dxgiAdapter;
        if (useGpu) {
            ComPtr<IDXGIFactory6> dxgiFactory;
            WEBNN_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
            UINT i = 0;
            while (dxgiFactory->EnumAdapterByGpuPreference(
                       i++, gpuPreference, IID_PPV_ARGS(&dxgiAdapter)) != DXGI_ERROR_NOT_FOUND) {
                bool isWarpAdapter = false;
                WEBNN_CHECK(IsWarpAdapter(dxgiAdapter.Get(), &isWarpAdapter));
                if (!isWarpAdapter) {
                    break;
                }
            }
        }
        if (!useGpu || FAILED(D3D12CreateDevice(dxgiAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                                IID_PPV_ARGS(&mD3D12Device)))) {
            // If a computer's display driver is not functioning or is disabled, the computer's
            // primary (NULL) adapter might also be called "Microsoft Basic Render Driver."
            Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
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
    }

    void Graph::CloseExecuteResetWait() {
        WEBNN_CHECK(mCommandList->Close());
        ID3D12CommandList* commandLists[] = {mCommandList.Get()};
        mCommandQueue->ExecuteCommandLists(ARRAYSIZE(commandLists), commandLists);
        WEBNN_CHECK(mCommandQueue.Get()->GetDevice(IID_PPV_ARGS(mD3D12Device.GetAddressOf())));
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        WEBNN_CHECK(mD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(fence.GetAddressOf())));
        WEBNN_CHECK(mCommandQueue.Get()->Signal(fence.Get(), 1));
        WEBNN_CHECK(fence->SetEventOnCompletion(1, nullptr));
        WEBNN_CHECK(mCommandAllocator->Reset());
        WEBNN_CHECK(mCommandList->Reset(mCommandAllocator.Get(), nullptr));
    }

    // According to the input nodes info, add each intermediate node to the graph, and add the
    // related input edges(if exist) and intermediate edges by the way.
    MaybeError Graph::AddEdges(std::vector<std::shared_ptr<EdgeInfoBase>> edges) {
        std::unique_ptr<DML_OPERATOR_GRAPH_NODE_DESC> nodeDesc(new DML_OPERATOR_GRAPH_NODE_DESC);
        nodeDesc->Operator = mIntermediateNodesMap[mIntermediateNodes.size()].Get();

        for (size_t i = 0; i < edges.size(); ++i) {
            if (edges[i]->isInputEdge) {
                auto edge = reinterpret_cast<InputEdgeInfo*>(edges[i].get());
                std::unique_ptr<DML_INPUT_GRAPH_EDGE_DESC> inputEdgeDesc(
                    new DML_INPUT_GRAPH_EDGE_DESC);
                inputEdgeDesc->GraphInputIndex = edge->inputIndex;
                inputEdgeDesc->ToNodeIndex = mIntermediateNodes.size();
                inputEdgeDesc->ToNodeInputIndex = i;
                mInputEdges.push_back({DML_GRAPH_EDGE_TYPE_INPUT, inputEdgeDesc.get()});
                mInputEdgesDesc.push_back(std::move(inputEdgeDesc));
            } else {
                auto edge = reinterpret_cast<EdgeInfo*>(edges[i].get());
                std::unique_ptr<DML_INTERMEDIATE_GRAPH_EDGE_DESC> intermediateEdgeDesc(
                    new DML_INTERMEDIATE_GRAPH_EDGE_DESC);
                intermediateEdgeDesc->FromNodeIndex = edge->nodeIndex;
                intermediateEdgeDesc->FromNodeOutputIndex = edge->outputNodeIndex;
                intermediateEdgeDesc->ToNodeIndex = mIntermediateNodes.size();
                intermediateEdgeDesc->ToNodeInputIndex = i;
                mIntermediateEdges.push_back(
                    {DML_GRAPH_EDGE_TYPE_INTERMEDIATE, intermediateEdgeDesc.get()});
                mIntermediateEdgesDesc.push_back(std::move(intermediateEdgeDesc));
            }
        }
        mIntermediateNodes.push_back({DML_GRAPH_NODE_TYPE_OPERATOR, nodeDesc.get()});
        mIntermediateNodesDesc.push_back(std::move(nodeDesc));
        return {};
    }

    bool Graph::GetDmlTensorDesc(OperandDescriptor const* desc,
                                 DmlTensorDesc& dmlTensorDesc,
                                 DML_TENSOR_FLAGS tensorFlag) {
        size_t typeLength = 4;
        if (desc->type == wnn::OperandType::Float32) {
            dmlTensorDesc.bufferDesc.DataType = DML_TENSOR_DATA_TYPE_FLOAT32;
            typeLength = 4;
        } else if (desc->type == wnn::OperandType::Float16) {
            dmlTensorDesc.bufferDesc.DataType = DML_TENSOR_DATA_TYPE_FLOAT16;
            typeLength = 2;
        } else if (desc->type == wnn::OperandType::Int32) {
            dmlTensorDesc.bufferDesc.DataType = DML_TENSOR_DATA_TYPE_INT32;
        } else if (desc->type == wnn::OperandType::Uint32) {
            dmlTensorDesc.bufferDesc.DataType = DML_TENSOR_DATA_TYPE_UINT32;
        } else {
            return false;
        }

        size_t bufferLength = typeLength;
        if (desc->dimensionsCount > DML_TENSOR_DIMENSION_COUNT_MAX) {
            dawn::ErrorLog() << "Tensor dimension count " << desc->dimensionsCount
                             << " is greater than DML_TENSOR_DIMENSION_COUNT_MAX "
                             << DML_TENSOR_DIMENSION_COUNT_MAX;
            return false;
        }

        if (desc->dimensionsCount == 0) {
            dmlTensorDesc.dimensions.resize(1);
            dmlTensorDesc.dimensions[0] = 1;
        } else {
            dmlTensorDesc.dimensions.resize(desc->dimensionsCount);
            for (uint32_t i = 0; i < desc->dimensionsCount; ++i) {
                int32_t d = desc->dimensions[i];
                if (d < 0) {
                    dawn::ErrorLog() << "DML doesn't support the negative dimension value";
                    return false;
                }
                dmlTensorDesc.dimensions[i] = d;
                bufferLength *= d;
            }
        }
        dmlTensorDesc.bufferDesc.Flags = tensorFlag;
        dmlTensorDesc.bufferDesc.DimensionCount = desc->dimensionsCount;
        dmlTensorDesc.bufferDesc.Sizes = dmlTensorDesc.dimensions.data();
        dmlTensorDesc.bufferDesc.Strides = nullptr;
        dmlTensorDesc.bufferDesc.TotalTensorSizeInBytes = bufferLength;
        dmlTensorDesc.bufferDesc.GuaranteedBaseOffsetAlignment = 0;
        return true;
    }

    Graph::Graph(Context* context) : GraphBase(context) {
        wnn::DevicePreference devicePreference = GetContext()->GetContextOptions().devicePreference;
        bool useGpu = devicePreference == wnn::DevicePreference::Cpu ? false : true;

        wnn::PowerPreference powerPreference = GetContext()->GetContextOptions().powerPreference;
        DXGI_GPU_PREFERENCE gpuPreference;
        switch (powerPreference) {
            case wnn::PowerPreference::High_performance:
                gpuPreference = DXGI_GPU_PREFERENCE::DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
                break;
            case wnn::PowerPreference::Low_power:
                gpuPreference = DXGI_GPU_PREFERENCE::DXGI_GPU_PREFERENCE_MINIMUM_POWER;
                break;
            default:
                gpuPreference = DXGI_GPU_PREFERENCE::DXGI_GPU_PREFERENCE_UNSPECIFIED;
        }
        // Set up Direct3D 12.
        InitD3D12(gpuPreference, useGpu);

        // Create the DirectML device.
        DML_CREATE_DEVICE_FLAGS dmlCreateDeviceFlags = DML_CREATE_DEVICE_FLAG_NONE;
#if defined(_DEBUG)
        dmlCreateDeviceFlags = DML_CREATE_DEVICE_FLAG_DEBUG;
#endif
        WEBNN_CHECK(
            DMLCreateDevice(mD3D12Device.Get(), dmlCreateDeviceFlags, IID_PPV_ARGS(&mDevice)));
    }

    MaybeError Graph::AddConstant(const op::Constant* constant) {
        const OperandDescriptor* desc = constant->GetOperandDescriptor();
        mDmlTensorDescMap.insert(std::make_pair(mInputs.size(), DmlTensorDesc{}));
        if (!GetDmlTensorDesc(desc, mDmlTensorDescMap[mInputs.size()],
                              DML_TENSOR_FLAG_OWNED_BY_DML)) {
            return DAWN_INTERNAL_ERROR("Failed to get DML buffer tensor description.");
        }
        DML_TENSOR_DESC dmlTensorDesc = {DML_TENSOR_TYPE_BUFFER,
                                         &mDmlTensorDescMap[mInputs.size()].bufferDesc};

        InputEdgeInfo* inputEdgeInfo = new InputEdgeInfo();
        inputEdgeInfo->outputTensorDESC = dmlTensorDesc;
        inputEdgeInfo->name = "Input_Constant_" + std::to_string(mInputs.size());
        inputEdgeInfo->isInputEdge = true;
        inputEdgeInfo->inputIndex = mInputs.size();
        inputEdgeInfo->buffer = constant->GetBuffer();
        inputEdgeInfo->byteLength = constant->GetByteLength();
        inputEdgeInfo->isConstantInput = true;

        std::shared_ptr<EdgeInfoBase> edge(inputEdgeInfo);
        mGraphNodesMap[constant->PrimaryOutput()] = edge;
        mInputs.push_back(*inputEdgeInfo);
        return {};
    }

    MaybeError Graph::AddInput(const op::Input* input) {
        const OperandDescriptor* desc = input->GetOperandDescriptor();
        mDmlTensorDescMap.insert(std::make_pair(mInputs.size(), DmlTensorDesc{}));
        if (!GetDmlTensorDesc(desc, mDmlTensorDescMap[mInputs.size()])) {
            return DAWN_INTERNAL_ERROR("Failed to get DML buffer tensor description.");
        }
        DML_TENSOR_DESC dmlTensorDesc = {DML_TENSOR_TYPE_BUFFER,
                                         &mDmlTensorDescMap[mInputs.size()].bufferDesc};

        InputEdgeInfo* inputEdgeInfo = new InputEdgeInfo();
        inputEdgeInfo->outputTensorDESC = dmlTensorDesc;
        inputEdgeInfo->name = input->GetName();
        inputEdgeInfo->isInputEdge = true;
        inputEdgeInfo->inputIndex = mInputs.size();

        std::shared_ptr<EdgeInfoBase> edge(inputEdgeInfo);
        mGraphNodesMap[input->PrimaryOutput()] = edge;
        mInputs.push_back(*inputEdgeInfo);
        return {};
    }

    MaybeError Graph::AddBinary(const op::Binary* binary) {
        switch (binary->GetType()) {
            case op::BinaryOpType::kAdd: {
                DAWN_ASSERT(mGraphNodesMap.find(binary->Inputs()[0].Get()) != mGraphNodesMap.end());
                DAWN_ASSERT(mGraphNodesMap.find(binary->Inputs()[1].Get()) != mGraphNodesMap.end());
                auto edgeA = mGraphNodesMap[binary->Inputs()[0].Get()];
                auto edgeB = mGraphNodesMap[binary->Inputs()[1].Get()];
                DML_ELEMENT_WISE_ADD_OPERATOR_DESC dmlAddOperatorDesc{};
                dmlAddOperatorDesc.ATensor = &edgeA->outputTensorDESC;
                dmlAddOperatorDesc.BTensor = &edgeB->outputTensorDESC;

                DML_TENSOR_DESC outputTensorDesc = edgeA->outputTensorDESC;
                dmlAddOperatorDesc.OutputTensor = &outputTensorDesc;

                DML_OPERATOR_DESC dmlOperatorDesc = {};
                dmlOperatorDesc.Type = DML_OPERATOR_ELEMENT_WISE_ADD;
                dmlOperatorDesc.Desc = &dmlAddOperatorDesc;

                Microsoft::WRL::ComPtr<IDMLOperator> dmlOperator;
                WEBNN_CHECK(mDevice->CreateOperator(&dmlOperatorDesc, IID_PPV_ARGS(&dmlOperator)));
                mIntermediateNodesMap[mIntermediateNodes.size()] = dmlOperator;

                EdgeInfo* edgeInfo = new EdgeInfo();
                edgeInfo->nodeIndex = mIntermediateNodes.size();
                edgeInfo->outputTensorDESC = outputTensorDesc;
                std::shared_ptr<EdgeInfoBase> edge(edgeInfo);
                mGraphNodesMap[binary->PrimaryOutput()] = edge;

                return AddEdges({edgeA, edgeB});
            }
            default:
                return DAWN_UNIMPLEMENTED_ERROR(" Binary op is not implemented.");
        }
    }

    MaybeError Graph::AddUnary(const op::Unary* unary) {
        switch (unary->GetType()) {
            case op::UnaryOpType::kSigmoid: {
                Microsoft::WRL::ComPtr<IDMLOperator> dmlOperator;
                DAWN_ASSERT(mGraphNodesMap.find(unary->Inputs()[0].Get()) != mGraphNodesMap.end());
                auto edge0 = mGraphNodesMap[unary->Inputs()[0].Get()];
                DML_TENSOR_DESC inputTensorDesc = edge0->outputTensorDESC;
                DML_ACTIVATION_SIGMOID_OPERATOR_DESC dmlSigmoidOperatorDesc{};
                dmlSigmoidOperatorDesc.InputTensor = &inputTensorDesc;
                dmlSigmoidOperatorDesc.OutputTensor = &inputTensorDesc;

                DML_OPERATOR_DESC dmlOperatorDesc = {};
                dmlOperatorDesc.Type = DML_OPERATOR_ACTIVATION_SIGMOID;
                dmlOperatorDesc.Desc = &dmlSigmoidOperatorDesc;

                WEBNN_CHECK(mDevice->CreateOperator(&dmlOperatorDesc, IID_PPV_ARGS(&dmlOperator)));
                mIntermediateNodesMap[mIntermediateNodes.size()] = dmlOperator;

                EdgeInfo* edgeInfo = new EdgeInfo();
                edgeInfo->nodeIndex = mIntermediateNodes.size();
                edgeInfo->outputTensorDESC = inputTensorDesc;
                std::shared_ptr<EdgeInfoBase> edge(edgeInfo);
                mGraphNodesMap[unary->PrimaryOutput()] = edge;

                return AddEdges({edge0});
                break;
            }
            default:
                return DAWN_UNIMPLEMENTED_ERROR(" Unary op is not implemented.");
        }
        return {};
    }

    MaybeError Graph::AddOutput(const std::string& name, const OperandBase* output) {
        auto edge = mGraphNodesMap[output];
        if (edge->isInputEdge) {
            return DAWN_INTERNAL_ERROR("Graph for input = output is invalid.");
        }
        edge->name = name;
        std::unique_ptr<DML_OUTPUT_GRAPH_EDGE_DESC> outputEdgeDesc(new DML_OUTPUT_GRAPH_EDGE_DESC);
        auto outputEdgeInfo = reinterpret_cast<EdgeInfo*>(edge.get());
        outputEdgeDesc->FromNodeIndex = outputEdgeInfo->nodeIndex;
        outputEdgeDesc->FromNodeOutputIndex = outputEdgeInfo->outputNodeIndex;
        outputEdgeDesc->GraphOutputIndex = mOutputs.size();
        mOutputEdges.push_back({DML_GRAPH_EDGE_TYPE_OUTPUT, outputEdgeDesc.get()});
        mOutputEdgesDesc.push_back(std::move(outputEdgeDesc));

        mOutputs.push_back(*outputEdgeInfo);
        return {};
    }

    MaybeError Graph::AddBatchNorm(const op::BatchNorm* batchNorm) {
        return DAWN_UNIMPLEMENTED_ERROR("BatchNorm hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddConv2d(const op::Conv2d* conv2d) {
        return DAWN_UNIMPLEMENTED_ERROR("Conv2d hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddConvTranspose2d(const op::ConvTranspose2d* convTranspose2d) {
        return DAWN_UNIMPLEMENTED_ERROR("ConvTranspose2D has not been supported on DirectML.");
    }

    MaybeError Graph::AddGru(const op::Gru* gru) {
        return DAWN_UNIMPLEMENTED_ERROR("Gru hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddPad(const op::Pad* pad) {
        return DAWN_UNIMPLEMENTED_ERROR("Pad hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddPool2d(const op::Pool2d* pool2d) {
        return DAWN_UNIMPLEMENTED_ERROR("Pool2d hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddClamp(const op::Clamp* clamp) {
        return DAWN_UNIMPLEMENTED_ERROR("Clamp hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddReduce(const op::Reduce* reduce) {
        return DAWN_UNIMPLEMENTED_ERROR("Reduce hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddResample2d(const op::Resample2d* resample) {
        return DAWN_UNIMPLEMENTED_ERROR("Resample2d hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddReshape(const op::Reshape* reshape) {
        return DAWN_UNIMPLEMENTED_ERROR("Reshape hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddSlice(const op::Slice* slice) {
        return DAWN_UNIMPLEMENTED_ERROR("Slice hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddSplit(const op::Split* split) {
        return DAWN_UNIMPLEMENTED_ERROR("Split hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddSqueeze(const op::Squeeze* squeeze) {
        return DAWN_UNIMPLEMENTED_ERROR("Squeeze hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddTranspose(const op::Transpose* transpose) {
        return DAWN_UNIMPLEMENTED_ERROR("Transpose hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddInstanceNorm(const op::InstanceNorm* instanceNorm) {
        return DAWN_UNIMPLEMENTED_ERROR("InstanceNorm hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddConcat(const op::Concat* concat) {
        return DAWN_UNIMPLEMENTED_ERROR("Concat hasn't been supported on DirectML.");
    }

    MaybeError Graph::AddGemm(const op::Gemm* gemm) {
        return DAWN_UNIMPLEMENTED_ERROR("Gemm hasn't been supported on DirectML.");
    }

    MaybeError Graph::Finish() {
        if (mInputs.empty()) {
            return DAWN_VALIDATION_ERROR("Model inputs must be set.");
        }
        WEBNN_CHECK(mDevice.Get()->QueryInterface(IID_PPV_ARGS(&mDevice1)));

        // Compiles a graph of DirectML operators into an object that can be dispatched to the GPU.
        DML_GRAPH_DESC graphDesc = {};
        graphDesc.InputCount = static_cast<UINT>(mInputs.size());
        graphDesc.OutputCount = static_cast<UINT>(mOutputs.size());
        graphDesc.NodeCount = static_cast<UINT>(mIntermediateNodes.size());
        graphDesc.Nodes = mIntermediateNodes.data();
        graphDesc.InputEdgeCount = static_cast<UINT>(mInputEdges.size());
        graphDesc.InputEdges = mInputEdges.data();
        graphDesc.OutputEdgeCount = static_cast<UINT>(mOutputEdges.size());
        graphDesc.OutputEdges = mOutputEdges.data();
        graphDesc.IntermediateEdgeCount = static_cast<UINT>(mIntermediateEdges.size());
        graphDesc.IntermediateEdges = mIntermediateEdges.data();

        WEBNN_CHECK(mDevice1->CompileGraph(&graphDesc, DML_EXECUTION_FLAG_NONE,
                                           IID_PPV_ARGS(&mCompiledOperator)));
        return {};
    }

    MaybeError Graph::CompileImpl() {
        IDMLCompiledOperator* compiledOperators[] = {mCompiledOperator.Get()};
        Microsoft::WRL::ComPtr<IDMLOperatorInitializer> compiledOperatorInitializer;
        WEBNN_CHECK(mDevice->CreateOperatorInitializer(ARRAYSIZE(compiledOperators),
                                                       compiledOperators,
                                                       IID_PPV_ARGS(&compiledOperatorInitializer)));

        DML_BINDING_PROPERTIES initializeBindingProperties =
            compiledOperatorInitializer->GetBindingProperties();
        DML_BINDING_PROPERTIES executeBindingProperties = mCompiledOperator->GetBindingProperties();
        UINT descriptorCount = std::max(initializeBindingProperties.RequiredDescriptorCount,
                                        executeBindingProperties.RequiredDescriptorCount);

        // Create descriptor heaps.
        D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc{};
        descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptorHeapDesc.NumDescriptors = descriptorCount;
        descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        WEBNN_CHECK(mD3D12Device->CreateDescriptorHeap(&descriptorHeapDesc,
                                                       IID_PPV_ARGS(&mDescriptorHeap)));

        // Set the descriptor heap(s).
        ID3D12DescriptorHeap* descriptorHeaps[] = {mDescriptorHeap.Get()};
        mCommandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);

        // Create a binding table over the descriptor heap we just created.
        DML_BINDING_TABLE_DESC bindingTableDesc{};
        bindingTableDesc.Dispatchable = compiledOperatorInitializer.Get();
        bindingTableDesc.CPUDescriptorHandle =
            mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        bindingTableDesc.GPUDescriptorHandle =
            mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        bindingTableDesc.SizeInDescriptors = descriptorCount;

        WEBNN_CHECK(mDevice->CreateBindingTable(&bindingTableDesc, IID_PPV_ARGS(&mBindingTable)));

        UINT64 temporaryResourceSize = std::max(initializeBindingProperties.TemporaryResourceSize,
                                                executeBindingProperties.TemporaryResourceSize);
        UINT64 persistentResourceSize = executeBindingProperties.PersistentResourceSize;

        // Bind and initialize the operator on the GPU.
        if (temporaryResourceSize != 0) {
            mD3D12Device->CreateCommittedResource(
                &utils::CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &utils::CreateResourceDesc(temporaryResourceSize,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mTemporaryResource));

            if (initializeBindingProperties.TemporaryResourceSize != 0) {
                DML_BUFFER_BINDING bufferBinding{mPersistentResource.Get(), 0,
                                                 temporaryResourceSize};
                DML_BINDING_DESC bindingDesc{DML_BINDING_TYPE_BUFFER, &bufferBinding};
                mBindingTable->BindTemporaryResource(&bindingDesc);
            }
        }

        if (persistentResourceSize != 0) {
            mD3D12Device->CreateCommittedResource(
                &utils::CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &utils::CreateResourceDesc(persistentResourceSize,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mPersistentResource));

            // The persistent resource should be bound as the output to the
            // IDMLOperatorInitializer.
            DML_BUFFER_BINDING bufferBinding{mPersistentResource.Get(), 0, persistentResourceSize};
            DML_BINDING_DESC bindingDesc{DML_BINDING_TYPE_BUFFER, &bufferBinding};
            mBindingTable->BindOutputs(1, &bindingDesc);
        }

        // Initialize constant inputs.
        uint64_t constantInputsResourceSize = 0;
        for (auto& input : mInputs) {
            if (input.isConstantInput) {
                uint64_t offset = utils::RoundUpToMultiple(
                    constantInputsResourceSize, (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
                constantInputsResourceSize = offset + input.byteLength;
            }
        }

        if (constantInputsResourceSize) {
            std::vector<DML_BUFFER_BINDING> bufferBinding(mInputs.size());
            DML_BUFFER_ARRAY_BINDING dmlBufferArrayBinding = {};

            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &utils::CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                &utils::CreateResourceDesc(constantInputsResourceSize),
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mUploadResource)));

            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &utils::CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &utils::CreateResourceDesc(constantInputsResourceSize,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mInputResource)));

            D3D12_RANGE constantBufferRange{0, constantInputsResourceSize};
            int8_t* constantBuffer;
            WEBNN_CHECK(mUploadResource->Map(0, &constantBufferRange,
                                             reinterpret_cast<void**>(&constantBuffer)));
            uint64_t offset = 0;
            for (size_t i = 0; i < mInputs.size(); ++i) {
                auto input = mInputs[i];
                if (input.isConstantInput) {
                    uint32_t requiredAlignment = DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT;
                    offset = utils::RoundUpToMultiple(offset, (uint64_t)requiredAlignment);
                    bufferBinding[i].Buffer = mInputResource.Get();
                    bufferBinding[i].Offset = offset;
                    bufferBinding[i].SizeInBytes = input.byteLength;

                    void* dest = constantBuffer + offset;
                    const void* src = input.buffer;
                    memcpy(dest, src, static_cast<size_t>(input.byteLength));
                    offset = offset + input.byteLength;
                }
            }
            dmlBufferArrayBinding.BindingCount = bufferBinding.size();
            dmlBufferArrayBinding.Bindings = bufferBinding.data();
            mUploadResource->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER inputResourceBarrier = {};
            inputResourceBarrier.Transition.pResource = mInputResource.Get();
            inputResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            inputResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            inputResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            inputResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            inputResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            mCommandList->ResourceBarrier(1, &inputResourceBarrier);
            mCommandList->CopyBufferRegion(mInputResource.Get(), 0, mUploadResource.Get(), 0,
                                           constantInputsResourceSize);
            inputResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            inputResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            mCommandList->ResourceBarrier(1, &inputResourceBarrier);

            DML_BINDING_DESC inputBindingDesc{DML_BINDING_TYPE_BUFFER_ARRAY,
                                              &dmlBufferArrayBinding};
            mBindingTable->BindInputs(1, &inputBindingDesc);
        }

        // Record execution of the operator initializer.
        // The command recorder is a stateless object that records Dispatches into an existing
        // Direct3D 12 command list.
        WEBNN_CHECK(mDevice->CreateCommandRecorder(IID_PPV_ARGS(&mCommandRecorder)));
        mCommandRecorder->RecordDispatch(mCommandList.Get(), compiledOperatorInitializer.Get(),
                                         mBindingTable.Get());
        CloseExecuteResetWait();

        // Bind and execute the operator on the GPU.
        mCommandList->SetDescriptorHeaps(ARRAYSIZE(descriptorHeaps), descriptorHeaps);
        // Reset the binding table to bind for the operator we want to execute (it was
        // previously used to bind for the initializer).
        bindingTableDesc.Dispatchable = mCompiledOperator.Get();
        mBindingTable->Reset(&bindingTableDesc);

        if (temporaryResourceSize != 0) {
            DML_BUFFER_BINDING bufferBinding{mTemporaryResource.Get(), 0, temporaryResourceSize};
            DML_BINDING_DESC bindingDesc{DML_BINDING_TYPE_BUFFER, &bufferBinding};
            mBindingTable->BindTemporaryResource(&bindingDesc);
        }

        if (persistentResourceSize != 0) {
            DML_BUFFER_BINDING bufferBinding{mPersistentResource.Get(), 0, persistentResourceSize};
            DML_BINDING_DESC bindingDesc{DML_BINDING_TYPE_BUFFER, &bufferBinding};
            mBindingTable->BindPersistentResource(&bindingDesc);
        }
        return {};
    }

    WNNComputeGraphStatus Graph::ComputeImpl(NamedInputsBase* inputs, NamedOutputsBase* outputs) {
        auto namedInputs = inputs->GetRecords();

        // Initialize common inputs.
        uint64_t inputsResourceSize = 0;
        for (auto& input : mInputs) {
            // All the inputs must be set.

            if (!input.isConstantInput && namedInputs.find(input.name) == namedInputs.end()) {
                dawn::ErrorLog() << "The input must be set.";
                return WNNComputeGraphStatus_Error;
            }

            if (!input.isConstantInput) {
                auto& resource = namedInputs[input.name].resource;
                uint64_t offset = utils::RoundUpToMultiple(
                    inputsResourceSize, (uint64_t)DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT);
                inputsResourceSize = offset + resource.byteLength;
            }
        }

        if (inputsResourceSize) {
            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &utils::CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
                &utils::CreateResourceDesc(inputsResourceSize), D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&mUploadResource)));

            WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
                &utils::CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
                &utils::CreateResourceDesc(inputsResourceSize,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&mInputResource)));

            std::vector<DML_BINDING_DESC> bindingDesc(mInputs.size());
            std::vector<DML_BUFFER_BINDING> bufferBinding(mInputs.size());

            D3D12_RANGE inputBufferRange{0, inputsResourceSize};
            int8_t* inputBuffer;
            WEBNN_CHECK(
                mUploadResource->Map(0, &inputBufferRange, reinterpret_cast<void**>(&inputBuffer)));

            uint64_t offset = 0;
            for (size_t i = 0; i < mInputs.size(); ++i) {
                auto input = mInputs[i];
                if (!input.isConstantInput) {
                    uint32_t requiredAlignment = DML_MINIMUM_BUFFER_TENSOR_ALIGNMENT;
                    offset = utils::RoundUpToMultiple(offset, (uint64_t)requiredAlignment);
                    auto& resource = namedInputs[input.name].resource;
                    bufferBinding[i].Buffer = mInputResource.Get();
                    bufferBinding[i].Offset = offset;
                    bufferBinding[i].SizeInBytes = resource.byteLength;
                    bindingDesc[i] = {DML_BINDING_TYPE_BUFFER, &bufferBinding[i]};
                    void* dest = inputBuffer + offset;
                    memcpy(dest, static_cast<int8_t*>(resource.buffer) + resource.byteOffset,
                           resource.byteLength);
                    offset = offset + bufferBinding[i].SizeInBytes;
                }
            }
            mUploadResource->Unmap(0, nullptr);

            D3D12_RESOURCE_BARRIER inputResourceBarrier = {};
            inputResourceBarrier.Transition.pResource = mInputResource.Get();
            inputResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            inputResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            inputResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            inputResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            inputResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            mCommandList->ResourceBarrier(1, &inputResourceBarrier);
            mCommandList->CopyBufferRegion(mInputResource.Get(), 0, mUploadResource.Get(), 0,
                                           inputsResourceSize);
            inputResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            inputResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            mCommandList->ResourceBarrier(1, &inputResourceBarrier);
            mBindingTable->BindInputs(bindingDesc.size(), bindingDesc.data());
        }

        // Prepare for outputs and read back buffer from Gpu.
        uint64_t outputsResourceSize = 0;
        auto namedOutputs = outputs->GetRecords();
        for (auto namedOutput : outputs->GetRecords()) {
            const ArrayBufferView output = namedOutput.second;
            DAWN_ASSERT(output.buffer != nullptr && output.byteLength != 0);
            outputsResourceSize += output.byteLength;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> outputResource;
        WEBNN_CHECK(mD3D12Device->CreateCommittedResource(
            &utils::CreateHeapProperties(), D3D12_HEAP_FLAG_NONE,
            &utils::CreateResourceDesc(outputsResourceSize,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outputResource)));

        DML_BUFFER_BINDING outputBufferBinding{outputResource.Get(), 0, outputsResourceSize};
        DML_BINDING_DESC outputBindingDesc{DML_BINDING_TYPE_BUFFER, &outputBufferBinding};
        mBindingTable->BindOutputs(1, &outputBindingDesc);

        // Record execution of the compiled operator.
        mCommandRecorder->RecordDispatch(mCommandList.Get(), mCompiledOperator.Get(),
                                         mBindingTable.Get());
        CloseExecuteResetWait();

        Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
        mD3D12Device->CreateCommittedResource(
            &utils::CreateHeapProperties(D3D12_HEAP_TYPE_READBACK), D3D12_HEAP_FLAG_NONE,
            &utils::CreateResourceDesc(outputsResourceSize), D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&readbackBuffer));

        D3D12_RESOURCE_BARRIER outputResourceBarrier = {};
        outputResourceBarrier.Transition.pResource = outputResource.Get();
        outputResourceBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        outputResourceBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        outputResourceBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        outputResourceBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        outputResourceBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        mCommandList->ResourceBarrier(1, &outputResourceBarrier);
        mCommandList->CopyResource(readbackBuffer.Get(), outputResource.Get());

        CloseExecuteResetWait();

        D3D12_RANGE tensorBufferRange{0, outputsResourceSize};
        int8_t* outputBuffer;
        WEBNN_CHECK(
            readbackBuffer->Map(0, &tensorBufferRange, reinterpret_cast<void**>(&outputBuffer)));

        std::vector<std::string> outputNames;
        for (auto& output : mOutputs) {
            outputNames.push_back(output.name);
        }

        uint64_t offset = 0;
        for (size_t i = 0; i < outputNames.size(); ++i) {
            std::string outputName = outputNames[i];
            auto namedOutputs = outputs->GetRecords();
            if (namedOutputs.find(outputName) != namedOutputs.end()) {
                ArrayBufferView output = namedOutputs[outputName];
                memcpy(static_cast<int8_t*>(output.buffer) + output.byteOffset,
                       outputBuffer + offset, output.byteLength);
                offset += output.byteLength;
            }
        }

        readbackBuffer->Unmap(0, nullptr);
        return WNNComputeGraphStatus_Success;
    }
}}  // namespace webnn_native::dml
