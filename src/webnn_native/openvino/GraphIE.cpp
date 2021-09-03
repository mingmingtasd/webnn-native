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

#include "webnn_native/openvino/GraphIE.h"

#include <algorithm>
#include <vector>

#include "common/Assert.h"
#include "common/Log.h"
#include "webnn_native/ErrorData.h"
#include "webnn_native/NamedInputs.h"
#include "webnn_native/NamedOperands.h"
#include "webnn_native/NamedOutputs.h"
#include "webnn_native/openvino/ErrorIE.h"

#define WEBNN_ASSERT(condition, message) \
    do {                                 \
        dawn::ErrorLog() << message;     \
        DAWN_ASSERT(condition);          \
    } while (0)

namespace webnn_native { namespace ie {

    namespace {
        MaybeError TensorDesc(OperandDescriptor const* desc, tensor_desc_t& tensorDesc) {
            // Inference Engine C API only support rank 8 with defination of dimensions_t
            // https://github.com/openvinotoolkit/openvino/blob/master/inference-engine/ie_bridges/c/include/c_api/ie_c_api.h#L132.
            if (desc->dimensionsCount > 8) {
                return DAWN_INTERNAL_ERROR("Inference Engine C API only support rank 8.");
            }
            for (size_t i = 0; i < desc->dimensionsCount; ++i) {
                if (desc->dimensions[i] < 0) {
                    return DAWN_INTERNAL_ERROR("dynamic shape isn't supported now.");
                }
                tensorDesc.dims.dims[i] = desc->dimensions[i];
            }
            tensorDesc.dims.ranks = desc->dimensionsCount;
            switch (desc->type) {
                case ml::OperandType::Float32:
                    tensorDesc.precision = precision_e::FP32;
                    break;
                case ml::OperandType::Int32:
                    tensorDesc.precision = precision_e::I32;
                    break;
                case ml::OperandType::Float16:
                    tensorDesc.precision = precision_e::FP16;
                    break;
                case ml::OperandType::Uint32:
                    tensorDesc.precision = precision_e::U32;
                    break;
                default:
                    UNREACHABLE();
            }
            tensorDesc.layout = layout_e::ANY;
            return {};
        }

        MaybeError GetConstantData(std::unordered_set<const OperandBase*>& constantSet,
                                   Ref<OperandBase>& constant,
                                   std::vector<float>& data,
                                   std::vector<size_t>& dimensions) {
            if (constantSet.find(constant.Get()) == constantSet.end()) {
                return DAWN_INTERNAL_ERROR("The input is not a constant");
            }

            const op::Constant* minConstant =
                reinterpret_cast<const op::Constant*>(constant->Operator());
            const float* value = static_cast<const float*>(minConstant->GetBuffer());
            const int32_t* dims = minConstant->GetOperandDescriptor()->dimensions;
            uint32_t dimensionsCount = minConstant->GetOperandDescriptor()->dimensionsCount;
            if (dimensionsCount == 0) {
                data.push_back(*value);
                return {};
            }
            size_t size = 1;
            for (size_t i = 0; i < dimensionsCount; ++i) {
                if (dims[i] < 0) {
                    return DAWN_INTERNAL_ERROR("OpenVINO can't support the type.");
                }
                dimensions.push_back(dims[i]);
                size *= dims[i];
            }
            data.assign(value, value + size);
            return {};
        }

        template <typename TYPE>
        ngraph_node_t* AddConstantWithGraph(precision_e type,
                                            std::vector<size_t> shape,
                                            std::vector<TYPE> values) {
            ie_blob_t* blob;
            tensor_desc_t tensorDesc;
            tensorDesc.precision = type;
            tensorDesc.layout = layout_e::ANY;
            for (size_t i = 0; i < shape.size(); ++i) {
                tensorDesc.dims.dims[i] = shape[i];
            }
            tensorDesc.dims.ranks = shape.size();
            // just wrap data to ie_blob_t pointer without allocating of new memory, the wrapped
            // data will be copied after creating ngraph::op::Constant node.
            IEStatusCode status = ie_blob_make_memory_from_preallocated(
                &tensorDesc, values.data(), values.size() * sizeof(TYPE), &blob);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "Failed to make memory from preallocated.";
                return nullptr;
            }
            ngraph_node_t* constantNode = nullptr;
            status = ngraph_constant(&tensorDesc, blob, &constantNode);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "Failed to add ngraph constant.";
                return nullptr;
            }
            return constantNode;
        }

        IEStatusCode AddActivationNode(const ngraph_node_t* inputNode,
                                       OperatorBase* activation,
                                       ngraph_node_t** activationNode) {
            IEStatusCode status = IEStatusCode::OK;
            if (activation == nullptr) {
                *activationNode = const_cast<ngraph_node_t*>(inputNode);
                return status;
            }
            switch (activation->GetFusedOperator()) {
                // Workaround(mingming): Currently we implement Relu6 operator by clamp. For
                // case OperatorType::Clamp, we added a clamp node in GraphBuilder
                // directly to ensure that we can find the min and max operands from the graph.
                case FusedOperator::Clamp:
                    *activationNode = const_cast<ngraph_node_t*>(inputNode);
                    break;
                case FusedOperator::Relu:
                    status = ngraph_relu(inputNode, activationNode);
                    break;
                case FusedOperator::Sigmoid:
                    status = ngraph_sigmoid(inputNode, activationNode);
                    break;
                case FusedOperator::LeakyRelu: {
                    auto leakyRelu = reinterpret_cast<const op::LeakyRelu*>(activation);
                    const ngraph_node_t* constantNode = AddConstantWithGraph<float>(
                        precision_e::FP32, {1}, {leakyRelu->GetAlpha()});
                    status = ngraph_leaky_relu(inputNode, constantNode, activationNode);
                    break;
                }
                default:
                    WEBNN_ASSERT(0, "The OperatorType isn't supported.");
            }
            return status;
        }

        // Transpose NHWC <=> NCHW.
        ngraph_node_t* TransposeInputLayout(const ngraph_node_t* input, bool nhwc_to_nchw) {
            std::vector<int64_t> order =
                nhwc_to_nchw ? std::vector<int64_t>{0, 3, 1, 2} : std::vector<int64_t>{0, 2, 3, 1};
            const ngraph_node_t* orderNode =
                AddConstantWithGraph<int64_t>(precision_e::I64, {order.size()}, order);
            ngraph_node_t* transposeNode = nullptr;
            IEStatusCode status = ngraph_transpose(input, orderNode, &transposeNode);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "Failed to transpose input layout";
            }
            return transposeNode;
        }

        // hwio => oihw or ohwi => oihw
        ngraph_node_t* TransposeFilterLayout(const ngraph_node_t* node,
                                             ml::FilterOperandLayout layout) {
            if (layout == ml::FilterOperandLayout::Oihw) {
                return const_cast<ngraph_node_t*>(node);
            }

            std::vector<int64_t> order;
            switch (layout) {
                case ml::FilterOperandLayout::Hwio:
                    order = std::vector<int64_t>{3, 2, 0, 1};
                    break;
                case ml::FilterOperandLayout::Ohwi:
                    order = std::vector<int64_t>{0, 3, 1, 2};
                    break;
                case ml::FilterOperandLayout::Ihwo:
                    order = std::vector<int64_t>{3, 0, 1, 2};
                    break;
                default:
                    WEBNN_ASSERT(0, "The filter layout isn't supported.");
                    break;
            }

            const ngraph_node_t* orderNode =
                AddConstantWithGraph<int64_t>(precision_e::I64, {order.size()}, order);
            ngraph_node_t* transposeNode = nullptr;
            IEStatusCode status = ngraph_transpose(node, orderNode, &transposeNode);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "Failed to transpose filter layout.";
            }
            return transposeNode;
        }

        IEStatusCode MatMul(const ngraph_node_t* primaryNode,
                            const ngraph_node_t* secondaryNode,
                            ngraph_node_t** matMulNode) {
            IEStatusCode status = IEStatusCode::OK;
            dimensions_t primaryShape;
            ngraph_get_shape(primaryNode, &primaryShape);
            if (primaryShape.ranks == 1) {
                std::vector<size_t> newShape = {1, primaryShape.dims[0]};
                auto newShapeNode =
                    AddConstantWithGraph<uint64_t>(precision_e::U64, {newShape.size()}, newShape);
                status = ngraph_reshape(primaryNode, newShapeNode,
                                        const_cast<ngraph_node_t**>(&primaryNode));
            }
            dimensions_t secondaryShape;
            ngraph_get_shape(secondaryNode, &secondaryShape);
            if (secondaryShape.ranks == 1) {
                std::vector<size_t> newShape = {secondaryShape.dims[0], 1};
                auto newShapeNode =
                    AddConstantWithGraph<uint64_t>(precision_e::U64, {newShape.size()}, newShape);
                status = ngraph_reshape(secondaryNode, newShapeNode,
                                        const_cast<ngraph_node_t**>(&secondaryNode));
            }
            status = ngraph_mat_mul(primaryNode, secondaryNode, matMulNode);
            if (primaryShape.ranks == 1 && secondaryShape.ranks == 1) {
                auto newShapeNode = AddConstantWithGraph<uint64_t>(precision_e::U64, {}, {1});
                status = ngraph_reshape(*matMulNode, newShapeNode, matMulNode);
            }
            return status;
        }
    }  // namespace

    Graph::Graph(Context* context)
        : GraphBase(context), mInferEngineNetwork(nullptr), mInferEngineRequest(nullptr) {
        mInferEngineCore = context->InferenceEngineCore();
    }

    Graph::~Graph() {
        if (mInferEngineNetwork) {
            ie_network_free(&mInferEngineNetwork);
        }
        if (mInferEngineRequest) {
            ie_infer_request_free(&mInferEngineRequest);
        }
        for (auto node : mGraphNodeMap) {
            ngraph_node_free(const_cast<ngraph_node_t**>(&node.second));
        }
    }

    MaybeError Graph::AddConstant(const op::Constant* constant) {
        tensor_desc_t tensorDesc;
        DAWN_TRY(TensorDesc(constant->GetOperandDescriptor(), tensorDesc));
        ie_blob_t* blob;
        // just wrap data to ie_blob_t pointer without allocating of new memory.
        IEStatusCode status = ie_blob_make_memory_from_preallocated(
            &tensorDesc, const_cast<void*>(constant->GetBuffer()), constant->GetByteLength(),
            &blob);
        DAWN_TRY(CheckStatusCode(status, "IE blob make memory"));
        ngraph_node_t* ngraphConstant;
        status = ngraph_constant(&tensorDesc, blob, &ngraphConstant);
        DAWN_TRY(CheckStatusCode(status, "ngraph add constant"));
        mGraphNodeMap[constant->PrimaryOutput()] = ngraphConstant;
        mConstantSet.insert(constant->PrimaryOutput());
        return {};
    }

    MaybeError Graph::AddInput(const op::Input* input) {
        tensor_desc_t tensorDesc;
        DAWN_TRY(TensorDesc(input->GetOperandDescriptor(), tensorDesc));
        ngraph_node_t* graphInput;
        IEStatusCode status = ngraph_input(&tensorDesc, &graphInput);
        DAWN_TRY(CheckStatusCode(status, "ngraph add input"));
        mGraphInputs.push_back(graphInput);
        mGraphNodeMap[input->PrimaryOutput()] = graphInput;
        mInputIdMap[input->GetName()] = mGraphInputs.size() - 1;
        return {};
    }

    MaybeError Graph::AddOutput(const std::string& name, const OperandBase* output) {
        ngraph_node_t* graphOutput;
        IEStatusCode status = ngraph_output(mGraphNodeMap[output], &graphOutput);
        DAWN_TRY(CheckStatusCode(status, "ngraph add output"));
        mGraphOutputs.push_back(graphOutput);
        char* originalName;
        ngraph_get_name(mGraphNodeMap[output], &originalName);
        mOutputNameMap[name] = std::string(originalName);
        ie_network_name_free(&originalName);
        return {};
    }

    MaybeError Graph::AddInstanceNorm(const op::InstanceNorm* instanceNorm) {
        auto inputs = instanceNorm->Inputs();
        // input
        std::vector<int64_t> axes({2, 3});
        auto axesNode = AddConstantWithGraph<int64_t>(precision_e::I64, {axes.size()}, axes);
        auto options = instanceNorm->GetOptions();
        auto input = mGraphNodeMap[inputs[0].Get()];
        if (options->layout == ml::InputOperandLayout::Nhwc) {
            input = TransposeInputLayout(input, true);
        }
        ngraph_node_t* meanNode = nullptr;
        IEStatusCode status = ngraph_reduce_mean(input, axesNode, true, &meanNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph reduce mean"));
        ngraph_node_t* subNode = nullptr;
        status = ngraph_sub(input, meanNode, &subNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph sub"));
        auto constantNode = AddConstantWithGraph<float>(precision_e::FP32, {}, {2});
        ngraph_node_t* powerNode = nullptr;
        status = ngraph_power(subNode, constantNode, &powerNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph power"));
        ngraph_node_t* varianceNode = nullptr;
        status = ngraph_reduce_mean(powerNode, axesNode, true, &varianceNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph reduce mean"));
        // options->epsilon
        constantNode = AddConstantWithGraph<float>(precision_e::FP32, {}, {options->epsilon});
        ngraph_node_t* addNode = nullptr;
        status = ngraph_add(varianceNode, constantNode, &addNode);
        constantNode = AddConstantWithGraph<float>(precision_e::FP32, {}, {0.5});
        status = ngraph_power(addNode, constantNode, &powerNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph power"));
        ngraph_node_t* divNode = nullptr;
        status = ngraph_divide(subNode, powerNode, &divNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph div"));

        // scale
        dimensions_t shape;
        ngraph_get_shape(input, &shape);
        auto channel = shape.dims[1];
        ngraph_node_t* scaleNode = nullptr;
        if (options->scale != nullptr) {
            auto scaleOperand = inputs[1].Get();
            DAWN_ASSERT(mGraphNodeMap.find(scaleOperand) != mGraphNodeMap.end());
            scaleNode = const_cast<ngraph_node_t*>(mGraphNodeMap[scaleOperand]);
        } else {
            std::vector<float> channelVector(channel, 1);
            scaleNode = AddConstantWithGraph<float>(precision_e::FP32, {channelVector.size()},
                                                    channelVector);
        }
        std::vector<int64_t> newShape = {1, -1, 1, 1};
        auto newShapeNode =
            AddConstantWithGraph<int64_t>(precision_e::I64, {newShape.size()}, newShape);
        status = ngraph_reshape(scaleNode, newShapeNode, &scaleNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph reshape"));

        // bias
        ngraph_node_t* biasNode = nullptr;
        if (options->bias != nullptr) {
            size_t biasIndex = options->scale != nullptr ? 2 : 1;
            auto biasOperand = inputs[biasIndex].Get();
            DAWN_ASSERT(mGraphNodeMap.find(biasOperand) != mGraphNodeMap.end());
            biasNode = const_cast<ngraph_node_t*>(mGraphNodeMap[biasOperand]);
        } else {
            std::vector<float> channelVector(channel, 0);
            biasNode = AddConstantWithGraph<float>(precision_e::FP32, {channelVector.size()},
                                                   channelVector);
        }
        status = ngraph_reshape(biasNode, newShapeNode, &biasNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph reshape"));

        // input multiply scale, add bias.
        ngraph_node_t* instanceNormNode = nullptr;
        status = ngraph_mul(scaleNode, divNode, &instanceNormNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph mul"));
        status = ngraph_add(instanceNormNode, biasNode, &instanceNormNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph add"));

        if (options->layout == ml::InputOperandLayout::Nhwc) {
            instanceNormNode = TransposeInputLayout(instanceNormNode, false);
        }
        mGraphNodeMap[instanceNorm->PrimaryOutput()] = instanceNormNode;

        return {};
    }

    MaybeError Graph::AddBatchNorm(const op::BatchNorm* batchNorm) {
        auto inputs = batchNorm->Inputs();
        DAWN_ASSERT(inputs.size() == 3 || inputs.size() == 4 || inputs.size() == 5);
        // When input is a 4-D tensor of the "nchw" or "nhwc" layout, options.axis should be set to
        // 1 or 3 respectively.
        auto inputNode = mGraphNodeMap[inputs[0].Get()];
        auto options = batchNorm->GetOptions();
        bool nhwc = options->axis == 3;
        if (nhwc) {
            inputNode = TransposeInputLayout(inputNode, true);
        }
        dimensions_t dimensions;
        ngraph_get_shape(inputNode, &dimensions);
        auto channel = dimensions.dims[1];
        auto meanNode = mGraphNodeMap[inputs[1].Get()];
        auto varianceNode = mGraphNodeMap[inputs[2].Get()];
        ngraph_node_t* scaleNode = nullptr;
        if (options->scale != nullptr) {
            scaleNode = const_cast<ngraph_node_t*>(mGraphNodeMap[inputs[3].Get()]);
        } else {
            std::vector<float> scale(channel, 1);
            scaleNode = AddConstantWithGraph<float>(precision_e::FP32, {channel}, scale);
        }
        ngraph_node_t* biasNode = nullptr;
        if (options->bias != nullptr) {
            size_t biasIndex = options->scale != nullptr ? 4 : 3;
            biasNode = const_cast<ngraph_node_t*>(mGraphNodeMap[inputs[biasIndex].Get()]);
        } else {
            std::vector<float> bias(channel, 0);
            biasNode = AddConstantWithGraph<float>(precision_e::FP32, {channel}, bias);
        }
        ngraph_node_t* batchNormNode;
        IEStatusCode status =
            ngraph_batch_norm_inference(inputNode, scaleNode, biasNode, meanNode, varianceNode,
                                        options->epsilon, &batchNormNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph batch norm inference"));
        ngraph_node_t* activationNode;
        status = AddActivationNode(batchNormNode, options->activation, &activationNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph activation"));
        if (nhwc) {
            activationNode = TransposeInputLayout(activationNode, false);
        }
        mGraphNodeMap[batchNorm->PrimaryOutput()] = activationNode;
        return {};
    }

    MaybeError Graph::AddBinary(const op::Binary* binary) {
        auto inputs = binary->Inputs();
        auto primaryNode = mGraphNodeMap[inputs[0].Get()];
        auto secondaryNode = mGraphNodeMap[inputs[1].Get()];
        ngraph_node_t* binaryNode = nullptr;
        IEStatusCode status = IEStatusCode::OK;
        switch (binary->GetType()) {
            case op::BinaryOpType::kAdd:
                status = ngraph_add(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kMul:
                status = ngraph_mul(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kSub:
                status = ngraph_sub(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kMatMul:
                status = MatMul(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kDiv:
                status = ngraph_divide(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kMax:
                status = ngraph_max(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kMin:
                status = ngraph_min(primaryNode, secondaryNode, &binaryNode);
                break;
            case op::BinaryOpType::kPower:
                status = ngraph_power(primaryNode, secondaryNode, &binaryNode);
                break;
            default:
                DAWN_ASSERT(0);
        }
        DAWN_TRY(CheckStatusCode(status, "ngraph add binary"));
        mGraphNodeMap[binary->PrimaryOutput()] = binaryNode;
        return {};
    }

    MaybeError Graph::AddClamp(const op::Clamp* clamp) {
        auto inputs = clamp->Inputs();
        auto options = clamp->GetOptions();
        std::vector<float> minValue;
        std::vector<size_t> minDimensions;
        if (options->minValue != nullptr) {
            DAWN_TRY(GetConstantData(mConstantSet, inputs[1], minValue, minDimensions));
        } else {
            minValue.push_back(std::numeric_limits<float>::lowest());
        }
        std::vector<float> maxValue;
        std::vector<size_t> maxDimensions;
        if (options->maxValue != nullptr) {
            size_t maxIndex = options->minValue != nullptr ? 2 : 1;
            DAWN_TRY(GetConstantData(mConstantSet, inputs[maxIndex], maxValue, maxDimensions));
        } else {
            maxValue.push_back(std::numeric_limits<float>::max());
        }
        // If minValue and maxValue are both scalars with shape {1} or shape {}, use
        // native Clamp to create graph. Otherwise, due to the limitation of Clamp's
        // attributes type, use Maximum and Minimum to implement Clamp as a
        // emulation. Note that this emulation may cause performance decline in
        // OpenVINO.
        ngraph_node_t* clampNode;
        IEStatusCode status;
        auto inputNode = mGraphNodeMap[inputs[0].Get()];
        if ((minDimensions.empty() && maxDimensions.empty()) ||
            (minDimensions.size() * maxDimensions.size() == 1 &&
             minDimensions[0] * maxDimensions[0] == 1)) {
            status = ngraph_clamp(inputNode, minValue[0], maxValue[0], &clampNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph clamp"));
        } else {
            ngraph_node_t* maxNode = const_cast<ngraph_node_t*>(inputNode);
            if (!minValue.empty()) {
                const ngraph_node_t* minConstant =
                    AddConstantWithGraph<float>(precision_e::FP32, minDimensions, minValue);
                status = ngraph_max(inputNode, minConstant, &maxNode);
                DAWN_TRY(CheckStatusCode(status, "ngraph max"));
            }
            clampNode = maxNode;
            if (!maxValue.empty()) {
                const ngraph_node_t* maxConstant =
                    AddConstantWithGraph<float>(precision_e::FP32, maxDimensions, maxValue);
                status = ngraph_min(maxNode, maxConstant, &clampNode);
                DAWN_TRY(CheckStatusCode(status, "ngraph min"));
            }
        }
        mGraphNodeMap[clamp->PrimaryOutput()] = clampNode;
        return {};
    }

    MaybeError Graph::AddConv2d(const op::Conv2d* conv2d) {
        IEStatusCode status;
        auto options = conv2d->GetOptions();
        std::vector<size_t> strides(options->strides, options->strides + options->stridesCount);
        DAWN_ASSERT(strides.size() == 2);
        std::vector<int32_t> padding(options->padding, options->padding + options->paddingCount);
        DAWN_ASSERT(padding.size() == 4);
        std::vector<size_t> dilations(options->dilations,
                                      options->dilations + options->dilationsCount);
        DAWN_ASSERT(strides.size() == 2);
        auto input = mGraphNodeMap[conv2d->Inputs()[0].Get()];
        if (options->inputLayout == ml::InputOperandLayout::Nhwc) {
            input = TransposeInputLayout(input, true);
        }
        auto filterNode = const_cast<ngraph_node_t*>(mGraphNodeMap[conv2d->Inputs()[1].Get()]);
        filterNode = TransposeFilterLayout(filterNode, options->filterLayout);
        ngraph_node_t* conv2dNode;
        dimensions_t filterDims;
        ngraph_get_shape(filterNode, &filterDims);
        if (options->groups > 1) {
            // Insert the groups to the shape of filter as first item.
            std::vector<size_t> filterShape(filterDims.dims, filterDims.dims + filterDims.ranks);
            filterShape.at(0) = filterShape.at(0) / options->groups;
            filterShape.insert(filterShape.begin(), options->groups);
            // Reshape the filter to support groups conv.
            const ngraph_node_t* reshapeNode =
                AddConstantWithGraph<uint64_t>(precision_e::U64, {filterShape.size()}, filterShape);
            status = ngraph_reshape(filterNode, reshapeNode, &filterNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph reshape"));
            status = ngraph_group_convolution(
                input, filterNode, strides.data(), strides.size(), padding.data(), padding.size(),
                dilations.data(), dilations.size(), static_cast<ngraph_auto_pad>(options->autoPad),
                &conv2dNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph group convolution"));
        } else {
            status = ngraph_convolution(
                input, filterNode, strides.data(), strides.size(), padding.data(), padding.size(),
                dilations.data(), dilations.size(), static_cast<ngraph_auto_pad>(options->autoPad),
                &conv2dNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph convolution"));
        }
        if (options->bias != nullptr) {
            ngraph_node_t* biasNode =
                const_cast<ngraph_node_t*>(mGraphNodeMap[conv2d->Inputs()[2].Get()]);
            dimensions_t biasDims;
            ngraph_get_shape(biasNode, &biasDims);
            if (biasDims.ranks != 1 || biasDims.dims[0] != filterDims.dims[0]) {
                return DAWN_INTERNAL_ERROR(
                    "The bias should be 1-D tensor with the shape of [output_channels].");
            }
            // Reshape bias from 1-D to 4-D for NCHW layout.
            const ngraph_node_t* reshapeNode =
                AddConstantWithGraph<int64_t>(precision_e::I64, {4}, {1, -1, 1, 1});
            status = ngraph_reshape(biasNode, reshapeNode, &biasNode);
            status = ngraph_add(conv2dNode, biasNode, &conv2dNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph add"));
        }
        ngraph_node_t* activationNode;
        status = AddActivationNode(conv2dNode, options->activation, &activationNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph activation"));
        if (options->inputLayout == ml::InputOperandLayout::Nhwc) {
            activationNode = TransposeInputLayout(activationNode, false);
        }
        mGraphNodeMap[conv2d->PrimaryOutput()] = activationNode;
        return {};
    }

    MaybeError Graph::AddPad(const op::Pad* pad) {
        auto inputs = pad->Inputs();
        if (mConstantSet.find(inputs[1].Get()) == mConstantSet.end()) {
            return DAWN_INTERNAL_ERROR("The padding is not a constant");
        }
        const op::Constant* padding = reinterpret_cast<const op::Constant*>(inputs[1]->Operator());
        int32_t const* paddingDimensions = padding->GetOperandDescriptor()->dimensions;
        uint32_t inputRank = inputs[0]->Rank();
        uint32_t padCount = padding->GetByteLength() / sizeof(int32_t);
        if (paddingDimensions[1] != 2 || paddingDimensions[0] != static_cast<int32_t>(inputRank)) {
            return DAWN_INTERNAL_ERROR(
                "The padding should has shape [n, 2], where n is the rank of the input tensor");
        }
        const uint32_t* padBuffer = static_cast<const uint32_t*>(padding->GetBuffer());
        std::vector<int32_t> padBegin, padEnd;
        for (size_t i = 0; i < padCount / 2; ++i) {
            padBegin.push_back(padBuffer[2 * i]);
            padEnd.push_back(padBuffer[2 * i + 1]);
        }
        const ngraph_node_t* padBeginNode =
            AddConstantWithGraph<int32_t>(precision_e::I32, {padBegin.size()}, padBegin);
        const ngraph_node_t* padEndNode =
            AddConstantWithGraph<int32_t>(precision_e::I32, {padEnd.size()}, padEnd);
        auto options = pad->GetOptions();
        const ngraph_node_t* padValueNode =
            AddConstantWithGraph<float>(precision_e::FP32, {}, {options->value});
        auto input = mGraphNodeMap[pad->Inputs()[0].Get()];
        ngraph_node_t* padNode;
        IEStatusCode status = ngraph_pad(input, padBeginNode, padEndNode, padValueNode,
                                         static_cast<ngraph_padding_mode>(options->mode), &padNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph pad"));
        mGraphNodeMap[pad->PrimaryOutput()] = padNode;
        return {};
    }

    MaybeError Graph::AddPool2d(const op::Pool2d* pool2d) {
        auto options = pool2d->GetOptions();
        auto input = mGraphNodeMap[pool2d->Inputs()[0].Get()];
        if (options->layout == ml::InputOperandLayout::Nhwc) {
            input = TransposeInputLayout(input, true);
        }
        std::vector<size_t> strides(options->strides, options->strides + options->stridesCount);
        DAWN_ASSERT(strides.size() == 2);
        std::vector<size_t> padding(options->padding, options->padding + options->paddingCount);
        DAWN_ASSERT(padding.size() == 4);
        std::vector<size_t> windowDimensions;
        windowDimensions.reserve(2);
        if (options->windowDimensions == nullptr || options->windowDimensionsCount == 0) {
            dimensions_t inputShape;
            ngraph_get_shape(input, &inputShape);
            if (inputShape.ranks <= 1 || inputShape.ranks > 4)
                return DAWN_INTERNAL_ERROR("The input shape is invaild.");
            size_t height_index = inputShape.ranks == 2 ? 0 : inputShape.ranks == 3 ? 1 : 2;
            windowDimensions.push_back(inputShape.dims[height_index]);
            windowDimensions.push_back(inputShape.dims[height_index + 1]);
        } else {
            windowDimensions.push_back(options->windowDimensions[0]);
            windowDimensions.push_back(options->windowDimensions[1]);
        }
        ngraph_node_t* poolNode = nullptr;
        IEStatusCode status = IEStatusCode::OK;
        switch (pool2d->GetType()) {
            case op::Pool2dType::kAveragePool2d:
                status = ngraph_average_pool(
                    input, strides.data(), strides.size(), padding.data(), padding.size(),
                    windowDimensions.data(), windowDimensions.size(),
                    static_cast<ngraph_auto_pad>(options->autoPad), &poolNode);
                break;
            case op::Pool2dType::kMaxPool2d:
                status = ngraph_max_pool(input, strides.data(), strides.size(), padding.data(),
                                         padding.size(), windowDimensions.data(),
                                         windowDimensions.size(),
                                         static_cast<ngraph_auto_pad>(options->autoPad), &poolNode);
                break;
            default:
                DAWN_ASSERT(0);
        }
        DAWN_TRY(CheckStatusCode(status, "ngraph pool"));
        if (options->layout == ml::InputOperandLayout::Nhwc) {
            poolNode = TransposeInputLayout(poolNode, false);
        }
        mGraphNodeMap[pool2d->PrimaryOutput()] = poolNode;

        return {};
    }

    MaybeError Graph::AddUnary(const op::Unary* unary) {
        auto input = mGraphNodeMap[unary->Inputs()[0].Get()];
        ngraph_node_t* unaryNode = nullptr;
        IEStatusCode status = IEStatusCode::OK;
        if (unary->GetType() == op::UnaryOpType::kRelu) {
            status = ngraph_relu(input, &unaryNode);
        } else if (unary->GetType() == op::UnaryOpType::kLeakyRelu) {
            const op::LeakyRelu* leakyRelu = reinterpret_cast<const op::LeakyRelu*>(unary);
            const ngraph_node_t* constantNode =
                AddConstantWithGraph<float>(precision_e::FP32, {1}, {leakyRelu->GetAlpha()});
            status = ngraph_leaky_relu(input, constantNode, &unaryNode);
        } else if (unary->GetType() == op::UnaryOpType::kSoftmax) {
            status = ngraph_softmax(input, &unaryNode);
        } else if (unary->GetType() == op::UnaryOpType::kSigmoid) {
            status = ngraph_sigmoid(input, &unaryNode);
        } else if (unary->GetType() == op::UnaryOpType::kTanh) {
            status = ngraph_tanh(input, &unaryNode);
        }
        DAWN_TRY(CheckStatusCode(status, "ngraph unary"));
        mGraphNodeMap[unary->PrimaryOutput()] = unaryNode;

        return {};
    }

    MaybeError Graph::AddReduceMean(const op::ReduceMean* reduceMean) {
        auto options = reduceMean->GetOptions();
        std::vector<int64_t> axes(options->axes, options->axes + options->axesCount);
        auto input = mGraphNodeMap[reduceMean->Inputs()[0].Get()];
        const ngraph_node_t* axesNode =
            AddConstantWithGraph<int64_t>(precision_e::I64, {axes.size()}, axes);
        ngraph_node_t* reduceMeanNode;
        IEStatusCode status =
            ngraph_reduce_mean(input, axesNode, options->keepDimensions, &reduceMeanNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph reduce mean"));
        mGraphNodeMap[reduceMean->PrimaryOutput()] = reduceMeanNode;
        return {};
    }

    MaybeError Graph::AddResample(const op::Resample* resample) {
        auto input = mGraphNodeMap[resample->Inputs()[0].Get()];
        dimensions_t inputShape;
        ngraph_get_shape(input, &inputShape);
        // scales.
        std::vector<float> scales;
        auto options = resample->GetOptions();
        if (options->scalesCount == 0) {
            scales.reserve(4);
            for (uint32_t i = 0; i < 4; ++i) {
                scales.push_back(static_cast<float>(options->sizes[i]) /
                                 static_cast<float>(inputShape.dims[i]));
            }
        } else {
            for (uint32_t i = 0; i < options->scalesCount; ++i) {
                scales.push_back(options->scales[i]);
            }
        }
        // Interpolate layer only supports resize on spatial dimensions(height and width) according
        // https://github.com/openvinotoolkit/openvino/blob/master/inference-engine/src/mkldnn_plugin/nodes/mkldnn_interpolate_node.cpp#L2047.
        // "scales[0] * scales[3] == 1" means N and C aren't supported resize.
        // "scales[1] * scales[2] != 1" means H and W need to be resized for NHWC.
        bool transpose = scales[0] * scales[3] == 1 && scales[1] * scales[2] != 1 ? true : false;
        if (transpose) {
            std::vector<float> nchwScales = scales;
            nchwScales[0] = scales[0];
            nchwScales[1] = scales[3];
            nchwScales[2] = scales[1];
            nchwScales[3] = scales[2];
            scales = std::move(nchwScales);
        }
        const ngraph_node_t* scalesNode =
            AddConstantWithGraph<float>(precision_e::FP32, {scales.size()}, scales);

        // sizes.
        std::vector<int32_t> sizes;
        if (options->sizesCount == 0) {
            sizes.reserve(4);
            for (uint32_t i = 0; i < 4; ++i) {
                sizes.push_back(int32_t(inputShape.dims[i] * options->scales[i]));
            }
        } else {
            sizes.assign(options->sizes, options->sizes + options->sizesCount);
        }
        if (transpose) {
            std::vector<int32_t> nchwSizes = sizes;
            nchwSizes[0] = sizes[0];
            nchwSizes[1] = sizes[3];
            nchwSizes[2] = sizes[1];
            nchwSizes[3] = sizes[2];
            sizes = std::move(nchwSizes);
        }
        const ngraph_node_t* sizesNode =
            AddConstantWithGraph<int32_t>(precision_e::I32, {sizes.size()}, sizes);
        // attrs.
        interpolate_attrs_t attrs;
        attrs.mode = static_cast<ngraph_interpolation_mode>(options->mode);
        if (options->sizesCount != 0) {
            attrs.shape_calculation_mode = ngraph_shape_calc_mode::Sizes;
        } else {
            attrs.shape_calculation_mode = ngraph_shape_calc_mode::Scales;
        }
        const ngraph_node_t* axes =
            AddConstantWithGraph<int64_t>(precision_e::I64, {4}, {0, 1, 2, 3});
        ngraph_node_t* resampleNode;
        if (transpose) {
            input = TransposeInputLayout(input, true);
        }
        IEStatusCode status =
            ngraph_interpolate(input, sizesNode, scalesNode, axes, &attrs, &resampleNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph resample"));
        if (transpose) {
            resampleNode = TransposeInputLayout(resampleNode, false);
        }
        mGraphNodeMap[resample->PrimaryOutput()] = resampleNode;
        return {};
    }

    MaybeError Graph::AddReshape(const op::Reshape* reshape) {
        auto newShape = reshape->GetNewShape();
        const ngraph_node_t* constantNode =
            AddConstantWithGraph<int32_t>(precision_e::I32, {newShape.size()}, newShape);
        auto input = mGraphNodeMap[reshape->Inputs()[0].Get()];
        ngraph_node_t* reshapeNode;
        IEStatusCode status = ngraph_reshape(input, constantNode, &reshapeNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph reshape"));
        mGraphNodeMap[reshape->PrimaryOutput()] = reshapeNode;
        return {};
    }

    MaybeError Graph::AddSqueeze(const op::Squeeze* squeeze) {
        auto input = mGraphNodeMap[squeeze->Inputs()[0].Get()];
        std::vector<int32_t> axes = squeeze->GetAxes();
        const ngraph_node_t* constantNode =
            axes.empty() ? nullptr
                         : AddConstantWithGraph<int32_t>(precision_e::I32, {axes.size()}, axes);
        ngraph_node_t* squeezeNode;
        IEStatusCode status = ngraph_squeeze(input, constantNode, &squeezeNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph squeeze"));
        mGraphNodeMap[squeeze->PrimaryOutput()] = squeezeNode;
        return {};
    }

    MaybeError Graph::AddTranspose(const op::Transpose* transpose) {
        auto input = mGraphNodeMap[transpose->Inputs()[0].Get()];
        std::vector<int32_t> permutation = transpose->GetPermutation();
        const ngraph_node_t* constantNode =
            AddConstantWithGraph<int32_t>(precision_e::I32, {permutation.size()}, permutation);
        ngraph_node_t* transposeNode;
        IEStatusCode status = ngraph_transpose(input, constantNode, &transposeNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph transpose"));
        mGraphNodeMap[transpose->PrimaryOutput()] = transposeNode;
        return {};
    }

    MaybeError Graph::AddConcat(const op::Concat* concat) {
        auto inputs = concat->Inputs();
        std::vector<ngraph_node_t*> inputNodes;
        inputNodes.reserve(inputs.size());
        for (auto& input : inputs) {
            inputNodes.push_back(const_cast<ngraph_node_t*>(mGraphNodeMap[input.Get()]));
        }
        ngraph_node_t* concatNode;
        IEStatusCode status =
            ngraph_concat(inputNodes.data(), inputNodes.size(), concat->GetAxis(), &concatNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph concat"));
        mGraphNodeMap[concat->PrimaryOutput()] = concatNode;
        return {};
    }

    MaybeError Graph::AddGemm(const op::Gemm* gemm) {
        auto inputs = gemm->Inputs();
        auto nodeA = const_cast<ngraph_node_t*>(mGraphNodeMap[inputs[0].Get()]);
        dimensions_t inputShape;
        ngraph_get_shape(nodeA, &inputShape);
        std::vector<int64_t> inputOrder;
        inputOrder.reserve(inputShape.ranks);
        for (uint32_t i = 0; i < inputShape.ranks; ++i) {
            inputOrder.push_back(inputShape.ranks - i - 1);
        }
        const ngraph_node_t* orderNode =
            AddConstantWithGraph<int64_t>(precision_e::I64, {inputShape.ranks}, inputOrder);
        auto options = gemm->GetOptions();
        IEStatusCode status;
        if (options->aTranspose) {
            status = ngraph_transpose(nodeA, orderNode, &nodeA);
            DAWN_TRY(CheckStatusCode(status, "ngraph transpose"));
        }
        auto nodeB = const_cast<ngraph_node_t*>(mGraphNodeMap[inputs[1].Get()]);
        if (options->bTranspose) {
            status = ngraph_transpose(nodeB, orderNode, &nodeB);
            DAWN_TRY(CheckStatusCode(status, "ngraph transpose"));
        }
        ngraph_node_t* gemmNode;
        status = ngraph_mat_mul(nodeA, nodeB, &gemmNode);
        DAWN_TRY(CheckStatusCode(status, "ngraph mat mul"));
        const ngraph_node_t* alphaNode =
            AddConstantWithGraph<float>(precision_e::FP32, {}, {options->alpha});
        if (options->alpha != 1) {
            status = ngraph_mul(gemmNode, alphaNode, &gemmNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph mul"));
        }
        if (inputs.size() == 3) {
            auto nodeC = mGraphNodeMap[inputs[2].Get()];
            auto betaNode = AddConstantWithGraph<float>(precision_e::FP32, {}, {options->beta});
            status = ngraph_mul(betaNode, nodeC, &betaNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph mul"));
            status = ngraph_add(gemmNode, betaNode, &gemmNode);
            DAWN_TRY(CheckStatusCode(status, "ngraph add"));
        }
        mGraphNodeMap[gemm->PrimaryOutput()] = gemmNode;
        return {};
    }

    MaybeError Graph::Finish() {
        // IEStatusCode code = IE(ie_model_finish)(mIeModel, &mInferEngineNetwork);
        // DAWN_TRY(CheckStatusCode(code, "IE finish creating model"));
        if (mGraphInputs.empty()) {
            return DAWN_VALIDATION_ERROR("The input must be set.");
        }
        ngraph_function_t* function = nullptr;
        IEStatusCode status =
            create_ngraph_function(&mGraphOutputs[0], mGraphOutputs.size(), &mGraphInputs[0],
                                   mGraphInputs.size(), &function);
        DAWN_TRY(CheckStatusCode(status, "ngraph create function"));
        ie_network_t* network = nullptr;
        status = create_network(function, &network);
        DAWN_TRY(CheckStatusCode(status, "ngraph create network"));
        size_t size = 0;
        status = ie_network_get_outputs_number(network, &size);
        for (size_t i = 0; i < size; ++i) {
            char* name;
            status = ie_network_get_output_name(network, i, &name);
            mOriginalNameMap[std::string(name)] = i;
            ie_network_name_free(&name);
        }
        transpose_sinking(function);
        ie_network_free(&network);
        status = create_network(function, &mInferEngineNetwork);
        DAWN_TRY(CheckStatusCode(status, "ngraph create network"));
        return {};
    }

    MaybeError Graph::CompileImpl() {
        ml::DevicePreference devicePreference = GetContext()->GetContextOptions().devicePreference;
        const char* deviceName = devicePreference == ml::DevicePreference::Gpu ? "GPU" : "CPU";

        ie_config_t config = {NULL, NULL, NULL};
        ie_executable_network_t* executableNetwork;
        IEStatusCode status = ie_core_load_network(mInferEngineCore, mInferEngineNetwork,
                                                   deviceName, &config, &executableNetwork);
        DAWN_TRY(CheckStatusCode(status, "IE load network"));
        status = ie_exec_network_create_infer_request(executableNetwork, &mInferEngineRequest);
        DAWN_TRY(CheckStatusCode(status, "IE create infer request"));
        ie_exec_network_free(&executableNetwork);
        return {};
    }

    MLComputeGraphStatus Graph::ComputeImpl(NamedInputsBase* inputs, NamedOutputsBase* outputs) {
        auto namedInputs = inputs->GetRecords();
        for (auto& input : mInputIdMap) {
            // All the inputs must be set.
            if (namedInputs.find(input.first) == namedInputs.end()) {
                dawn::ErrorLog() << "The input isn't set";
                return MLComputeGraphStatus_Error;
            }
            ie_blob_t* blob;
            char* inputName = nullptr;
            IEStatusCode status =
                ie_network_get_input_name(mInferEngineNetwork, input.second, &inputName);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "IE Failed to ie_network_get_input_name";
                return MLComputeGraphStatus_Error;
            }
            status = ie_infer_request_get_blob(mInferEngineRequest, inputName, &blob);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "IE Failed to ie_infer_request_get_blob";
                return MLComputeGraphStatus_Error;
            }
            ie_blob_buffer_t buffer;
            status = ie_blob_get_buffer(blob, &buffer);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "IE Failed to ie_blob_get_buffer";
                return MLComputeGraphStatus_Error;
            }
            auto& resource = namedInputs[input.first]->resource;
            memcpy(buffer.buffer, static_cast<int8_t*>(resource.buffer) + resource.byteOffset,
                   resource.byteLength);
        }

        // Compute the compiled model.
        IEStatusCode code = ie_infer_request_infer(mInferEngineRequest);
        if (code != IEStatusCode::OK) {
            dawn::ErrorLog() << "IE Failed to compute model";
            return MLComputeGraphStatus_Error;
        }

        // Get Data from nGraph with output.
        for (auto namedOutput : outputs->GetRecords()) {
            const ArrayBufferView* output = namedOutput.second;
            DAWN_ASSERT(output->buffer != nullptr && output->byteLength != 0);
            // Get output id with friendly name.
            auto originalName = mOutputNameMap[namedOutput.first];
            if (mOriginalNameMap.find(originalName) == mOriginalNameMap.end()) {
                dawn::ErrorLog() << "IE Failed to compute model";
                return MLComputeGraphStatus_Error;
            }
            char* sinkingName;
            IEStatusCode status = ie_network_get_output_name(
                mInferEngineNetwork, mOriginalNameMap[originalName], &sinkingName);
            ie_blob_t* outputBlob;
            status = ie_infer_request_get_blob(mInferEngineRequest, sinkingName, &outputBlob);
            if (status != IEStatusCode::OK) {
                dawn::ErrorLog() << "IE Failed to ie_infer_request_get_blob";
                return MLComputeGraphStatus_Error;
            }
            ie_blob_buffer_t outputBuffer;
            status = ie_blob_get_cbuffer(outputBlob, &outputBuffer);
            int bufferLength;
            status = ie_blob_byte_size(outputBlob, &bufferLength);
            dawn::ErrorLog()<<"bufferLength===="<<bufferLength;
            bufferLength = std::min(static_cast<size_t>(bufferLength), output->byteLength);
            if (output->byteLength >= static_cast<size_t>(bufferLength)) {
                dawn::ErrorLog()<<"output->byteLength===="<<output->byteLength;
                memcpy(static_cast<int8_t*>(output->buffer) + output->byteOffset,
                       outputBuffer.cbuffer, bufferLength);
            }
        }

        return MLComputeGraphStatus_Success;
    }
}}  // namespace webnn_native::ie
