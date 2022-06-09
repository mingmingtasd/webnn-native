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

#ifndef WEBNN_NATIVE_DMLUTILS_H_
#define WEBNN_NATIVE_DMLUTILS_H_

#define DML_TARGET_VERSION_USE_LATEST 1

#include <dxgi1_6.h>
#include <webnn/webnn_cpp.h>
#include <wrl\client.h>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "DirectML.h"
#include "common/Assert.h"
#include "common/Log.h"
#include "webnn/native/NamedOutputs.h"
#include "webnn/native/webnn_platform.h"

namespace webnn::native::dml {
#define WEBNN_CHECK(hr)                             \
    if (((HRESULT)(hr)) < 0) {                      \
        dawn::ErrorLog() << "Failed to do " << #hr; \
        DAWN_ASSERT(0);                             \
    }

    using namespace Microsoft::WRL;

    // Represent the DirectML tensor description.
    struct DmlTensorDesc {
        std::vector<UINT> dimensions = {};
        std::vector<UINT> strides = {};
        DML_BUFFER_TENSOR_DESC bufferDesc = {};
    };

    enum NodeType { NonConstantInput, ConstantInput, NonInput };

    // Represent the information of the graph's nodes.
    struct NodeBase {
        virtual ~NodeBase() = default;
        DML_TENSOR_DESC outputTensorDesc = {};
        std::string name = "";
        NodeType type = NodeType::NonInput;
    };

    // Only represent the information of the input nodes.
    struct InputNode final : public NodeBase {
        ~InputNode() override = default;
        // Indicate the index of the graph's input.
        size_t inputIndex = 0;
        void const* buffer = nullptr;
        size_t byteLength = 0;
    };

    // Represent the information of the intermediate and output nodes.
    struct Node final : public NodeBase {
        ~Node() override = default;
        uint32_t nodeIndex = 0;
        uint32_t outputNodeIndex = 0;
    };

    // Describe a graph of DirectML operators used to compile a combined, optimized operator.
    class GraphBuilder {
      public:
        GraphBuilder(ComPtr<IDMLDevice> device) : mDevice(device) {
        }

        void AddInputEdge(const DML_INPUT_GRAPH_EDGE_DESC& inputEdgeDesc) {
            mInputEdgesDesc.push_back(std::move(inputEdgeDesc));
            mInputEdges.push_back({DML_GRAPH_EDGE_TYPE_INPUT, &mInputEdgesDesc.back()});
        };

        void AddIntermediateEdge(const DML_INTERMEDIATE_GRAPH_EDGE_DESC& intermediateEdgeDesc) {
            mIntermediateEdgesDesc.push_back(std::move(intermediateEdgeDesc));
            mIntermediateEdges.push_back(
                {DML_GRAPH_EDGE_TYPE_INTERMEDIATE, &mIntermediateEdgesDesc.back()});
        };

        void AddOutputEdge(const DML_OUTPUT_GRAPH_EDGE_DESC& outputEdgeDesc) {
            mOutputEdgesDesc.push_back(std::move(outputEdgeDesc));
            mOutputEdges.push_back({DML_GRAPH_EDGE_TYPE_OUTPUT, &mOutputEdgesDesc.back()});
        };

        size_t NodeCount() {
            return mIntermediateNodes.size();
        };

        // Create IDMLOperator for the dml graph. Notice that this method will update the graph's
        // node count.
        void CreateOperator(DML_OPERATOR_TYPE type, const void* desc) {
            ComPtr<IDMLOperator> dmlOperator;
            DML_OPERATOR_DESC dmlOperatorDesc = {};
            dmlOperatorDesc.Type = type;
            dmlOperatorDesc.Desc = desc;
            WEBNN_CHECK(mDevice->CreateOperator(&dmlOperatorDesc, IID_PPV_ARGS(&dmlOperator)));
            mIntermediateNodesMap[mIntermediateNodes.size()] = dmlOperator;
            DML_OPERATOR_GRAPH_NODE_DESC nodeDesc;
            nodeDesc.Operator = mIntermediateNodesMap[mIntermediateNodes.size()].Get();
            mIntermediateNodesDesc.push_back(std::move(nodeDesc));
            mIntermediateNodes.push_back(
                {DML_GRAPH_NODE_TYPE_OPERATOR, &mIntermediateNodesDesc.back()});
        }

        std::shared_ptr<NodeBase> CreateNode(const DML_TENSOR_DESC& outputTensorDesc,
                                             uint32_t outputNodeIndex = 0) {
            std::shared_ptr<Node> node(new Node());
            node->outputTensorDesc = outputTensorDesc;
            node->nodeIndex = this->NodeCount() - 1;
            node->outputNodeIndex = outputNodeIndex;
            std::shared_ptr<NodeBase> nodeBase(node);
            return nodeBase;
        }

        // Add an IDMLOperator and the related edges which point to it to the graph
        // description.
        void AddNodes(std::vector<std::shared_ptr<NodeBase>> inputNodes) {
            for (size_t i = 0; i < inputNodes.size(); ++i) {
                switch (inputNodes[i]->type) {
                    case NodeType::ConstantInput:
                    case NodeType::NonConstantInput: {
                        auto inputNode = reinterpret_cast<InputNode*>(inputNodes[i].get());
                        DML_INPUT_GRAPH_EDGE_DESC inputEdgeDesc;
                        inputEdgeDesc.GraphInputIndex = inputNode->inputIndex;
                        inputEdgeDesc.ToNodeIndex = this->NodeCount() - 1;
                        inputEdgeDesc.ToNodeInputIndex = i;
                        this->AddInputEdge(inputEdgeDesc);
                        break;
                    }
                    case NodeType::NonInput: {
                        auto inputNode = reinterpret_cast<Node*>(inputNodes[i].get());
                        DML_INTERMEDIATE_GRAPH_EDGE_DESC intermediateEdgeDesc;
                        intermediateEdgeDesc.FromNodeIndex = inputNode->nodeIndex;
                        intermediateEdgeDesc.FromNodeOutputIndex = inputNode->outputNodeIndex;
                        intermediateEdgeDesc.ToNodeIndex = this->NodeCount() - 1;
                        intermediateEdgeDesc.ToNodeInputIndex = i;
                        this->AddIntermediateEdge(intermediateEdgeDesc);
                        break;
                    }
                    default:
                        dawn::ErrorLog() << "Invalid node type";
                        DAWN_ASSERT(0);
                }
            }
        }

        DML_GRAPH_DESC GetGraphDesc(size_t inputCount, size_t outputCount) {
            DML_GRAPH_DESC graphDesc = {};
            graphDesc.NodeCount = static_cast<UINT>(mIntermediateNodes.size());
            graphDesc.Nodes = mIntermediateNodes.data();
            graphDesc.InputEdgeCount = static_cast<UINT>(mInputEdges.size());
            graphDesc.InputEdges = mInputEdges.data();
            graphDesc.OutputEdgeCount = static_cast<UINT>(mOutputEdges.size());
            graphDesc.OutputEdges = mOutputEdges.data();
            graphDesc.IntermediateEdgeCount = static_cast<UINT>(mIntermediateEdges.size());
            graphDesc.IntermediateEdges = mIntermediateEdges.data();
            graphDesc.InputCount = static_cast<UINT>(inputCount);
            graphDesc.OutputCount = static_cast<UINT>(outputCount);
            return graphDesc;
        };

      private:
        ComPtr<IDMLDevice> mDevice;
        std::vector<DML_GRAPH_NODE_DESC> mIntermediateNodes;
        std::vector<DML_GRAPH_EDGE_DESC> mInputEdges;
        std::vector<DML_GRAPH_EDGE_DESC> mOutputEdges;
        std::vector<DML_GRAPH_EDGE_DESC> mIntermediateEdges;

        // Keep intermediate nodes here to avoid releasing too early.
        std::map<uint32_t, ComPtr<IDMLOperator>> mIntermediateNodesMap;
        // Keep the descriptions of nodes and edges here to avoid releasing too early.
        std::deque<DML_OPERATOR_GRAPH_NODE_DESC> mIntermediateNodesDesc;
        std::deque<DML_INPUT_GRAPH_EDGE_DESC> mInputEdgesDesc;
        std::deque<DML_OUTPUT_GRAPH_EDGE_DESC> mOutputEdgesDesc;
        std::deque<DML_INTERMEDIATE_GRAPH_EDGE_DESC> mIntermediateEdgesDesc;
    };

    inline D3D12_HEAP_PROPERTIES CreateHeapProperties(
        D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT) {
        return {type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
    };

    inline D3D12_RESOURCE_DESC CreateResourceDesc(
        UINT64 width,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        return {D3D12_RESOURCE_DIMENSION_BUFFER, 0,    width, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                D3D12_TEXTURE_LAYOUT_ROW_MAJOR,  flags};
    };

    template <typename T>
    T RoundUpToMultiple(T value, T multiple) {
        static_assert(std::is_integral_v<T>);

        T remainder = value % multiple;
        if (remainder != 0) {
            value += multiple - remainder;
        }

        return value;
    }

    struct CompiledGraph {
        CompiledGraph(const ComPtr<IDMLDevice>& device,
                      const DML_GRAPH_DESC& graphDesc,
                      const std::vector<std::shared_ptr<InputNode>>& inputs,
                      const std::vector<Node>& outputs,
                      DML_EXECUTION_FLAGS flag = DML_EXECUTION_FLAG_NONE)
            : inputNodes(inputs), outputNodes(outputs) {
            ComPtr<IDMLDevice1> device1;
            WEBNN_CHECK(device.Get()->QueryInterface(IID_PPV_ARGS(&device1)));
            WEBNN_CHECK(device1->CompileGraph(&graphDesc, flag, IID_PPV_ARGS(&compiledOperator)));
            IDMLCompiledOperator* compiledOperators[] = {compiledOperator.Get()};
            WEBNN_CHECK(
                device->CreateOperatorInitializer(ARRAYSIZE(compiledOperators), compiledOperators,
                                                  IID_PPV_ARGS(&compiledOperatorInitializer)));
        };

        // IDMLCompiledOperator represents the DirectML graph's output which need to be initialized
        // by IDMLOperatorInitializer.
        ComPtr<IDMLCompiledOperator> compiledOperator;
        ComPtr<IDMLOperatorInitializer> compiledOperatorInitializer;
        std::vector<std::shared_ptr<InputNode>> inputNodes;
        std::vector<Node> outputNodes;
    };

    // An adapter called the "Microsoft Basic Render Driver" is always present. This adapter is a
    // render-only device that has no display outputs.
    bool IsWarpAdapter(IDXGIAdapter1* pAdapter);

    class Device {
      public:
        void Init(DXGI_GPU_PREFERENCE gpuPreference, bool useGpu = true);
        void CreateResourcesForCompiledOperatorInitializer();
        void CreateResourcesForCompiledOperator();
        void InitializeGraph(const CompiledGraph* compiledGraph);
        void ExecuteGraph(const CompiledGraph* compiledGraph,
                          std::unordered_map<std::string, Input> namedInputs,
                          NamedOutputsBase* outputs);
        void BindTemporaryResource(bool bindForInitializer = true);
        void BindPersistentResource(bool bindForInitializer = true);
        void CopyBufferRegion(ComPtr<ID3D12Resource> srcResource,
                              ComPtr<ID3D12Resource> destResource,
                              UINT64 resourceSize,
                              D3D12_RESOURCE_STATES state,
                              bool needBarrierEnd = true);
        void FillUploadResourceAndInputBindings(
            uint64_t uploadResourceSize,
            std::vector<DML_BUFFER_BINDING>& inputBufferBinding,
            const std::vector<std::shared_ptr<InputNode>>& inputNodes,
            std::unordered_map<std::string, Input> namedInputs = {});
        ComPtr<IDMLDevice> GetIDMLDevice() const {
            return mDevice;
        }
        void CloseExecuteResetWait();

      private:
        ComPtr<IDMLDevice> mDevice;
        ComPtr<ID3D12Device> mD3D12Device;
        ComPtr<IDMLCommandRecorder> mCommandRecorder;
        ComPtr<ID3D12CommandQueue> mCommandQueue;
        ComPtr<ID3D12CommandAllocator> mCommandAllocator;
        ComPtr<ID3D12GraphicsCommandList> mCommandList;

        ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
        ComPtr<IDMLBindingTable> mBindingTable;
        DML_BINDING_TABLE_DESC mBindingTableDesc;

        ComPtr<ID3D12Resource> mUploadResource;
        ComPtr<ID3D12Resource> mInputResource;
        ComPtr<ID3D12Resource> mOutputResource;
        ComPtr<ID3D12Resource> mReadBackResource;
        ComPtr<ID3D12Resource> mTemporaryResource;
        ComPtr<ID3D12Resource> mPersistentResource;
        UINT64 mTemporaryResourceSize = 0;
        UINT64 mInitializedTemporaryResourceSize = 0;
        UINT64 mPersistentResourceSize = 0;
        UINT64 mConstantInputsResourceSize = 0;
        uint64_t mNonConstantInputsResourceSize = 0;
        uint64_t mOutputResourceSize = 0;
    };

}  // namespace webnn::native::dml
#endif  // WEBNN_NATIVE_DML_UTILS_H_