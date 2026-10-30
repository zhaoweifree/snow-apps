// SPDX-License-Identifier: GPL-3.0-only
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/wechat_qrcode.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<uchar> readModel(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "the pinned QR model must be available without a network");
    std::vector<uchar> bytes{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    require(!bytes.empty(), "the pinned QR model must not be empty");
    return bytes;
}

void forwardModel(const std::vector<uchar>& definition, const std::vector<uchar>& weights,
                  int inputSize, bool superResolution) {
    auto network = cv::dnn::readNetFromCaffe(definition, weights);
    network.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    network.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
    const int shape[] = {1, 1, inputSize, inputSize};
    network.setInput(cv::Mat::zeros(4, shape, CV_32F));
    const auto outputs = network.getUnconnectedOutLayersNames();
    require(outputs.size() == 1, "each pinned QR graph must have one final output");
    network.registerOutput("fixture_output", network.getLayerId(outputs.front()), 0);
    const cv::Mat output = network.forward("fixture_output");
    require(!output.empty() && cv::checkRange(output),
            "every pinned QR network layer must execute and produce finite output");
    if (superResolution) {
        require(output.dims == 4 && output.size[2] > inputSize && output.size[3] > inputSize,
                "the super-resolution graph must still enlarge its input");
    }
}

void verifyMinimalRegistry() {
    for (const char* type : {"Identity", "BatchNorm", "Concat", "Convolution", "Crop",
                             "Deconvolution", "DetectionOutput", "Eltwise", "Flatten", "Permute",
                             "Pooling", "PriorBox", "ReLU", "Reshape", "Scale", "Softmax"}) {
        require(cv::dnn::LayerFactory::isLayerRegistered(type),
                "the QR profile must retain its model and Net output layer constructors");
    }
    for (const char* type :
         {"Reduce", "LSTM", "GRU", "Attention", "NaryEltwise", "Scatter", "ScatterND"}) {
        require(!cv::dnn::LayerFactory::isLayerRegistered(type),
                "the QR profile must not retain unrelated DNN constructor registrations");
    }
}
} // namespace

int main(int argc, char** argv) {
    require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--minimal-registry"),
            "usage: qr_dnn_model_tests MODELS_DIRECTORY [--minimal-registry]");
    const std::filesystem::path directory(argv[1]);
    const auto detectorDefinition = readModel(directory / "detect.prototxt");
    const auto detectorWeights = readModel(directory / "detect.caffemodel");
    const auto scaleDefinition = readModel(directory / "sr.prototxt");
    const auto scaleWeights = readModel(directory / "sr.caffemodel");
    if (argc == 3)
        verifyMinimalRegistry();

    // Exercise both graphs directly: ordinary QR samples need not invoke super-resolution.
    forwardModel(detectorDefinition, detectorWeights, 384, false);
    forwardModel(scaleDefinition, scaleWeights, 224, true);

    constexpr auto payload = "https://snowshot.example/qr-model-profile";
    cv::Mat modules;
    cv::QRCodeEncoder::create()->encode(payload, modules);
    cv::copyMakeBorder(modules, modules, 4, 4, 4, 4, cv::BORDER_CONSTANT, cv::Scalar(255));
    cv::Mat image;
    cv::resize(modules, image, cv::Size(), 6, 6, cv::INTER_NEAREST);
    cv::wechat_qrcode::WeChatQRCode detector(detectorDefinition, detectorWeights, scaleDefinition,
                                             scaleWeights);
    for (int rotation = 0; rotation < 4; ++rotation) {
        const auto decoded = detector.detectAndDecode(image);
        require(std::find(decoded.begin(), decoded.end(), payload) != decoded.end(),
                "the pinned QR models must decode every right-angle orientation");
        cv::rotate(image, image, cv::ROTATE_90_CLOCKWISE);
    }
    return EXIT_SUCCESS;
}
