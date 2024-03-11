// MIT License

// Copyright (c) 2024  Miguel Ángel González Santamarta

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cv_bridge/cv_bridge.h>
#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "llama_utils/gpt_params.hpp"
#include "llava_ros/llava_node.hpp"

using namespace llava_ros;
using std::placeholders::_1;
using std::placeholders::_2;

LlavaNode::LlavaNode() : rclcpp::Node("llava_node") {

  // load llama
  gpt_params.load_params(this);
  this->llava = std::make_shared<Llava>(this->get_logger(), gpt_params.params,
                                        gpt_params.debug);

  // generate response action server
  this->goal_handle_ = nullptr;
  this->generate_response_action_server_ =
      rclcpp_action::create_server<GenerateResponse>(
          this, "generate_response",
          std::bind(&LlavaNode::handle_goal, this, _1, _2),
          std::bind(&LlavaNode::handle_cancel, this, _1),
          std::bind(&LlavaNode::handle_accepted, this, _1));

  RCLCPP_INFO(this->get_logger(), "Llava Node started");
}

rclcpp_action::GoalResponse
LlavaNode::handle_goal(const rclcpp_action::GoalUUID &uuid,
                       std::shared_ptr<const GenerateResponse::Goal> goal) {
  (void)uuid;
  (void)goal;

  if (this->goal_handle_ != nullptr && this->goal_handle_->is_active()) {
    return rclcpp_action::GoalResponse::REJECT;
  }

  return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse LlavaNode::handle_cancel(
    const std::shared_ptr<GoalHandleGenerateResponse> goal_handle) {
  (void)goal_handle;
  RCLCPP_INFO(this->get_logger(), "Received request to cancel Llava node");
  this->llava->cancel();
  return rclcpp_action::CancelResponse::ACCEPT;
}

void LlavaNode::handle_accepted(
    const std::shared_ptr<GoalHandleGenerateResponse> goal_handle) {
  this->goal_handle_ = goal_handle;
  std::thread{std::bind(&LlavaNode::execute, this, _1), goal_handle}.detach();
}

void LlavaNode::execute(
    const std::shared_ptr<GoalHandleGenerateResponse> goal_handle) {

  auto result = std::make_shared<GenerateResponse::Result>();

  // get goal data
  std::string prompt = goal_handle->get_goal()->prompt;
  auto image_msg = goal_handle->get_goal()->image;
  bool reset = goal_handle->get_goal()->reset;

  if (this->gpt_params.debug) {
    RCLCPP_INFO(this->get_logger(), "Prompt received:\n%s", prompt.c_str());
  }

  // reset llama
  if (reset) {
    this->llava->reset();
  }

  // update sampling params
  auto sampling_config = goal_handle->get_goal()->sampling_config;
  this->gpt_params.update_sampling_params(sampling_config,
                                          this->llava->get_n_vocab(),
                                          this->llava->get_token_eos());

  // load image
  if (image_msg.data.size() > 0) {

    cv_bridge::CvImagePtr cv_ptr =
        cv_bridge::toCvCopy(image_msg, image_msg.encoding);

    std::vector<uchar> buf;
    cv::imencode(".jpg", cv_ptr->image, buf);
    auto *enc_msg = reinterpret_cast<unsigned char *>(buf.data());
    std::string encoded_image = this->base64_encode(enc_msg, buf.size());

    if (!this->llava->load_image(encoded_image)) {
      this->goal_handle_->abort(result);
      return;
    }

  } else {
    this->llava->free_image();
  }

  // call llava
  auto completion_results = this->llava->generate_response(
      prompt, true, std::bind(&LlavaNode::send_text, this, _1));

  for (auto completion : completion_results) {
    result->response.text.append(this->llava->detokenize({completion.token}));
    result->response.tokens.push_back(completion.token);

    llama_msgs::msg::TokenProbArray probs_msg;
    for (auto prob : completion.probs) {
      llama_msgs::msg::TokenProb aux;
      aux.token = prob.token;
      aux.probability = prob.probability;
      aux.token_text = this->llava->detokenize({prob.token});
      probs_msg.data.push_back(aux);
    }
    result->response.probs.push_back(probs_msg);
  }

  if (rclcpp::ok()) {

    if (this->goal_handle_->is_canceling()) {
      this->goal_handle_->canceled(result);
    } else {
      this->goal_handle_->succeed(result);
    }

    this->goal_handle_ = nullptr;
  }
}

// https://renenyffenegger.ch/notes/development/Base64/Encoding-and-decoding-base-64-with-cpp/
std::string LlavaNode::base64_encode(unsigned char const *bytes_to_encode,
                                     size_t in_len, bool url) {

  static const char *base64_chars[2] = {"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        "abcdefghijklmnopqrstuvwxyz"
                                        "0123456789"
                                        "+/",

                                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                        "abcdefghijklmnopqrstuvwxyz"
                                        "0123456789"
                                        "-_"};

  size_t len_encoded = (in_len + 2) / 3 * 4;

  unsigned char trailing_char = url ? '.' : '=';

  //
  // Choose set of base64 characters. They differ
  // for the last two positions, depending on the url
  // parameter.
  // A bool (as is the parameter url) is guaranteed
  // to evaluate to either 0 or 1 in C++ therefore,
  // the correct character set is chosen by subscripting
  // base64_chars with url.
  //
  const char *base64_chars_ = base64_chars[url];

  std::string ret;
  ret.reserve(len_encoded);

  unsigned int pos = 0;

  while (pos < in_len) {
    ret.push_back(base64_chars_[(bytes_to_encode[pos + 0] & 0xfc) >> 2]);

    if (pos + 1 < in_len) {
      ret.push_back(base64_chars_[((bytes_to_encode[pos + 0] & 0x03) << 4) +
                                  ((bytes_to_encode[pos + 1] & 0xf0) >> 4)]);

      if (pos + 2 < in_len) {
        ret.push_back(base64_chars_[((bytes_to_encode[pos + 1] & 0x0f) << 2) +
                                    ((bytes_to_encode[pos + 2] & 0xc0) >> 6)]);
        ret.push_back(base64_chars_[bytes_to_encode[pos + 2] & 0x3f]);
      } else {
        ret.push_back(base64_chars_[(bytes_to_encode[pos + 1] & 0x0f) << 2]);
        ret.push_back(trailing_char);
      }
    } else {

      ret.push_back(base64_chars_[(bytes_to_encode[pos + 0] & 0x03) << 4]);
      ret.push_back(trailing_char);
      ret.push_back(trailing_char);
    }

    pos += 3;
  }

  return ret;
}

void LlavaNode::send_text(const struct completion_output &completion) {

  if (this->goal_handle_ != nullptr) {
    auto feedback = std::make_shared<GenerateResponse::Feedback>();

    feedback->partial_response.text =
        this->llava->detokenize({completion.token});
    feedback->partial_response.token = completion.token;
    feedback->partial_response.probs.chosen_token = completion.token;

    for (auto prob : completion.probs) {
      llama_msgs::msg::TokenProb aux;
      aux.token = prob.token;
      aux.probability = prob.probability;
      aux.token_text = this->llava->detokenize({prob.token});
      feedback->partial_response.probs.data.push_back(aux);
    }

    this->goal_handle_->publish_feedback(feedback);
  }
}
