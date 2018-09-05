// Copyright 2018 The SAF Authors. All Rights Reserved.
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

// Multi-target detection using mobilenet ssd

#include "operator/detectors/caffe_mobilenet_ssd_detector.h"

#include "common/context.h"
#include "model/model_manager.h"
#include "utils/cv_utils.h"
#include "utils/yolo_utils.h"

template <typename Dtype>
bool MobilenetSsdDetector<Dtype>::Init() {
  std::string model_file = model_desc_.GetModelDescPath();
  std::string weights_file = model_desc_.GetModelParamsPath();
  LOG(INFO) << "model_file: " << model_file;
  LOG(INFO) << "weights_file: " << weights_file;
  auto mean_colors = ModelManager::GetInstance().GetMeanColors();
  std::ostringstream mean_colors_stream;
  mean_colors_stream << mean_colors[0] << "," << mean_colors[1] << ","
                     << mean_colors[2];

  std::string labelmap_file = model_desc_.GetLabelFilePath();
  voc_names_ = ReadVocNames(labelmap_file);

  // Set Caffe backend
  int desired_device_number = model_desc_.GetDevice()
                                  ? *(model_desc_.GetDevice())
                                  : Context::GetContext().GetInt(DEVICE_NUMBER);
  LOG(INFO) << "desired_device_number: " << desired_device_number;

  if (desired_device_number == DEVICE_NUMBER_CPU_ONLY) {
    LOG(INFO) << "Use device: " << desired_device_number << "(CPU)";
    caffe::Caffe::set_mode(caffe::Caffe::CPU);
  } else {
#ifdef USE_CUDA
    std::vector<int> gpus;
    GetCUDAGpus(gpus);

    if (desired_device_number < gpus.size()) {
      // Device exists
      LOG(INFO) << "Use GPU with device ID " << desired_device_number;
      caffe::Caffe::SetDevice(desired_device_number);
      caffe::Caffe::set_mode(caffe::Caffe::GPU);
    } else {
      LOG(FATAL) << "No GPU device: " << desired_device_number;
    }
#elif USE_OPENCL
    std::vector<int> gpus;
    int count = caffe::Caffe::EnumerateDevices();

    if (desired_device_number < count) {
      // Device exists
      LOG(INFO) << "Use GPU with device ID " << desired_device_number;
      caffe::Caffe::SetDevice(desired_device_number);
      caffe::Caffe::set_mode(caffe::Caffe::GPU);
    } else {
      LOG(FATAL) << "No GPU device: " << desired_device_number;
    }
#else
    LOG(FATAL) << "Compiled in CPU_ONLY mode but have a device number "
                  "configured rather than -1";
#endif  // USE_CUDA
  }

// Load the network
#ifdef USE_OPENCL
  net_ = std::make_unique<caffe::Net<Dtype>>(model_file, caffe::TEST,
                                             caffe::Caffe::GetDefaultDevice());
#else
  net_ = std::make_unique<caffe::Net<Dtype>>(model_file, caffe::TEST);
#endif  // USE_OPENCL
  net_->CopyTrainedLayersFromBinaryProto(weights_file);

  CHECK_EQ(net_->num_inputs(), 1) << "Network should have exactly one input.";
  CHECK_EQ(net_->num_outputs(), 1) << "Network should have exactly one output.";

  caffe::Blob<Dtype>* input_layer = net_->input_blobs()[0];
  num_channels_ = input_layer->channels();
  CHECK(num_channels_ == 3 || num_channels_ == 1)
      << "Input layer should have 1 or 3 channels.";
  input_geometry_ = cv::Size(input_layer->width(), input_layer->height());
  input_blob_size_ = cv::Size(input_layer->width(), input_layer->height());

  caffe::TransformationParameter transform_param;
  caffe::ResizeParameter* resize_param = transform_param.mutable_resize_param();
  resize_param->set_resize_mode(caffe::ResizeParameter_Resize_mode_WARP);

  transform_param.add_mean_value(127.5);
  transform_param.add_mean_value(127.5);
  transform_param.add_mean_value(127.5);
  transform_param.set_scale(1. / 127.5);

  resize_param->set_width(input_blob_size_.width);
  resize_param->set_height(input_blob_size_.height);
  resize_param->set_prob(1.0);
  resize_param->add_interp_mode(caffe::ResizeParameter_Interp_mode_LINEAR);
#ifdef USE_OPENCL
  data_transformer_ = std::make_unique<caffe::DataTransformer<Dtype>>(
      transform_param, caffe::TEST, caffe::Caffe::GetDefaultDevice());
#else
  data_transformer_ = std::make_unique<caffe::DataTransformer<Dtype>>(
      transform_param, caffe::TEST);
#endif  // USE_OPENCL

  LOG(INFO) << "MobilenetSsdDetector initialized";
  return true;
}

template <typename Dtype>
std::vector<ObjectInfo> MobilenetSsdDetector<Dtype>::Detect(
    const cv::Mat& image) {
  caffe::Blob<Dtype>* input_layer = net_->input_blobs()[0];
  input_layer->Reshape(1, num_channels_, input_geometry_.height,
                       input_geometry_.width);
  // Forward dimension change to all layers
  net_->Reshape();

  cv::Mat img = FixupChannels(image, num_channels_);
  data_transformer_->Transform(img, input_layer);
  net_->Forward();

  caffe::Blob<Dtype>* result_blob = net_->output_blobs()[0];
  const Dtype* result = result_blob->cpu_data();
  const int num_det = result_blob->height();
  std::vector<ObjectInfo> result_object;
  for (int k = 0; k < num_det * 7; k += 7) {
    // format: imgid, classid, confidence, midx, midy, w, h
    ObjectInfo object_info;
    int classid = (int)result[k + 1];
    if (classid >= 0 && classid < (int)voc_names_.size())
      object_info.tag = voc_names_.at(classid);
    int left = (int)(result[k + 3] * img.cols);
    int top = (int)(result[k + 4] * img.rows);
    int right = (int)(result[k + 5] * img.cols);
    int bottom = (int)(result[k + 6] * img.rows);
    cv::Point left_top(left, top);
    cv::Point right_bottom(right, bottom);
    object_info.bbox = cv::Rect(left_top, right_bottom);
    object_info.confidence = result[k + 2];
    result_object.push_back(object_info);
  }

  return result_object;
}

template class MobilenetSsdDetector<float>;
#ifdef USE_ISAAC
template class MobilenetSsdDetector<half>;
#endif  // USE_ISAAC
