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

#include "examples/ResNet/ResNet.h"
#include "src/tests/WebnnTest.h"
#include <iostream>
#include <fstream>
class ResNetNchwTests : public WebnnTest {
  public:
    void TestResNetNchw(const std::string& inputFile,
                        const std::string& expectedFile,
                        bool fused = true) {
        ResNet resnet;
        resnet.mFused = fused;
        const std::string nchwPath =
            "node/third_party/webnn-polyfill/test-data/models/resnet50v2_nchw/";
        resnet.mWeightsPath = nchwPath + "weights/";
        const ml::GraphBuilder builder = ml::CreateGraphBuilder(GetContext());
        ml::Operand output = resnet.LoadNCHW(builder, false);
        ml::Graph graph = utils::Build(builder, {{"output", output}});
        const cnpy::NpyArray inputNpy = cnpy::npy_load(nchwPath + "test_data_set/" + inputFile);
        const std::vector<float> inputData = inputNpy.as_vec<float>();
        std::vector<float> result(900000,3.14159);
        utils::Compute(graph, {{"input", inputData}}, {{"output", result}});
        // const cnpy::NpyArray outputNpy = cnpy::npy_load(nchwPath + "test_data_set/" +
        // expectedFile); EXPECT_TRUE(utils::CheckValue(result, outputNpy.as_vec<float>()));
        // std::streambuf *psbuf, *backup;
        // std::ofstream file;
        // file.open("conv000.txt");
        // backup = std::cout.rdbuf();
        // psbuf = file.rdbuf();

        // std::cout.rdbuf(psbuf);  //将cout输出重定向到文件
        // for (size_t i = 0; i < result.size(); i++) {
        //     std::cout << result[i] << std::endl;
        // }
        // std::cout.rdbuf(backup);  //恢复cout输出重定向到终端
        // file.close();
    }
};

TEST_F(ResNetNchwTests, NchwTest0) {
    TestResNetNchw("0/input_0.npy", "0/output_0.npy", false);
}

TEST_F(ResNetNchwTests, NchwTest1) {
    TestResNetNchw("1/input_0.npy", "1/output_0.npy", false);
}

TEST_F(ResNetNchwTests, NchwTest2) {
    TestResNetNchw("2/input_0.npy", "2/output_0.npy", false);
}

TEST_F(ResNetNchwTests, FusedNchwTest0) {
    TestResNetNchw("0/input_0.npy", "0/output_0.npy");
}

TEST_F(ResNetNchwTests, FusedNchwTest1) {
    TestResNetNchw("1/input_0.npy", "1/output_0.npy");
}

TEST_F(ResNetNchwTests, FusedNchwTest2) {
    TestResNetNchw("2/input_0.npy", "2/output_0.npy");
}
