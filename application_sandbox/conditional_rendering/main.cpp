// Copyright 2020 Google Inc.
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

#include <chrono>

#include "application_sandbox/sample_application_framework/sample_application.h"
#include "mathfu/matrix.h"
#include "mathfu/vector.h"
#include "support/entry/entry.h"
#include "vulkan_helpers/buffer_frame_data.h"
#include "vulkan_helpers/helper_functions.h"
#include "vulkan_helpers/vulkan_application.h"
#include "vulkan_helpers/vulkan_model.h"

using Mat44 = mathfu::Matrix<float, 4, 4>;
using Vector4 = mathfu::Vector<float, 4>;

namespace cube_model {
#include "cube.obj.h"
}
const auto& cube_data = cube_model::model;

uint32_t cube_vertex_shader[] =
#include "cube.vert.spv"
    ;

uint32_t cube_fragment_shader[] =
#include "cube.frag.spv"
    ;

uint32_t compute_shader[] =
#include "cube.comp.spv"
    ;

struct ConditionalRenderingFrameData {
  containers::unique_ptr<vulkan::VkCommandBuffer> command_buffer_;
  containers::unique_ptr<vulkan::VkFramebuffer> framebuffer_;
  containers::unique_ptr<vulkan::DescriptorSet> cube_descriptor_set_;
  containers::unique_ptr<vulkan::DescriptorSet> compute_descriptor_set_;
  containers::unique_ptr<vulkan::VkBufferView> dispatch_data_buffer_view_;
};

VkPhysicalDeviceConditionalRenderingFeaturesEXT kConditionalRenderingFeatures =
    {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,
     nullptr, VK_TRUE, VK_FALSE};

// This creates an application with 16MB of image memory, and defaults
// for host, and device buffer sizes.
class ConditionalRenderingSample
    : public sample_application::Sample<ConditionalRenderingFrameData> {
 public:
  ConditionalRenderingSample(const entry::EntryData* data)
      : data_(data),
        Sample<ConditionalRenderingFrameData>(
            data->allocator(), data, 1, 512, 1, 1,
            sample_application::SampleOptions().AddDeviceExtensionStructure(
                &kConditionalRenderingFeatures),
            {0}, {}, {VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME}),
        cube_(data->allocator(), data->logger(), cube_data) {}
  virtual void InitializeApplicationData(
      vulkan::VkCommandBuffer* initialization_buffer,
      size_t num_swapchain_images) override {
    cube_.InitializeData(app(), initialization_buffer);

    cube_descriptor_set_layouts_[0] = {
        0,                                  // binding
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  // descriptorType
        1,                                  // descriptorCount
        VK_SHADER_STAGE_VERTEX_BIT,         // stageFlags
        nullptr                             // pImmutableSamplers
    };
    cube_descriptor_set_layouts_[1] = {
        1,                                  // binding
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,  // descriptorType
        1,                                  // descriptorCount
        VK_SHADER_STAGE_VERTEX_BIT,         // stageFlags
        nullptr                             // pImmutableSamplers
    };
    cube_descriptor_set_layouts_[2] = {
        2,                                        // binding
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,  // descriptorType
        1,                                        // descriptorCount
        VK_SHADER_STAGE_FRAGMENT_BIT,             // stageFlags
        nullptr                                   // pImmutableSamplers
    };

    pipeline_layout_ = containers::make_unique<vulkan::PipelineLayout>(
        data_->allocator(),
        app()->CreatePipelineLayout(
            {{cube_descriptor_set_layouts_[0], cube_descriptor_set_layouts_[1],
              cube_descriptor_set_layouts_[2]}}));

    VkAttachmentReference color_attachment = {
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    render_pass_ = containers::make_unique<vulkan::VkRenderPass>(
        data_->allocator(),
        app()->CreateRenderPass(
            {{
                0,                                         // flags
                render_format(),                           // format
                num_samples(),                             // samples
                VK_ATTACHMENT_LOAD_OP_LOAD,                // loadOp
                VK_ATTACHMENT_STORE_OP_STORE,              // storeOp
                VK_ATTACHMENT_LOAD_OP_DONT_CARE,           // stenilLoadOp
                VK_ATTACHMENT_STORE_OP_DONT_CARE,          // stenilStoreOp
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,  // initialLayout
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL   // finalLayout
            }},  // AttachmentDescriptions
            {{
                0,                                // flags
                VK_PIPELINE_BIND_POINT_GRAPHICS,  // pipelineBindPoint
                0,                                // inputAttachmentCount
                nullptr,                          // pInputAttachments
                1,                                // colorAttachmentCount
                &color_attachment,                // colorAttachment
                nullptr,                          // pResolveAttachments
                nullptr,                          // pDepthStencilAttachment
                0,                                // preserveAttachmentCount
                nullptr                           // pPreserveAttachments
            }},                                   // SubpassDescriptions
            {}                                    // SubpassDependencies
            ));

    cube_pipeline_ = containers::make_unique<vulkan::VulkanGraphicsPipeline>(
        data_->allocator(), app()->CreateGraphicsPipeline(
                                pipeline_layout_.get(), render_pass_.get(), 0));
    cube_pipeline_->AddShader(VK_SHADER_STAGE_VERTEX_BIT, "main",
                              cube_vertex_shader);
    cube_pipeline_->AddShader(VK_SHADER_STAGE_FRAGMENT_BIT, "main",
                              cube_fragment_shader);
    cube_pipeline_->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    cube_pipeline_->SetInputStreams(&cube_);
    cube_pipeline_->SetViewport(viewport());
    cube_pipeline_->SetScissor(scissor());
    cube_pipeline_->SetSamples(num_samples());
    cube_pipeline_->AddAttachment();
    cube_pipeline_->Commit();

    camera_data_ = containers::make_unique<vulkan::BufferFrameData<CameraData>>(
        data_->allocator(), app(), num_swapchain_images,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    model_data_ = containers::make_unique<vulkan::BufferFrameData<ModelData>>(
        data_->allocator(), app(), num_swapchain_images,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    conditional_data_ = containers::make_unique<
        vulkan::BufferFrameData<ConditionalRenderingData>>(
        data_->allocator(), app(), num_swapchain_images,
        VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT);

    dispatch_data_ =
        containers::make_unique<vulkan::BufferFrameData<DispatchData>>(
            data_->allocator(), app(), num_swapchain_images,
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    float aspect =
        (float)app()->swapchain().width() / (float)app()->swapchain().height();
    camera_data_->data().projection_matrix =
        Mat44::FromScaleVector(mathfu::Vector<float, 3>{1.0f, -1.0f, 1.0f}) *
        Mat44::Perspective(1.5708f, aspect, 0.1f, 100.0f);

    model_data_->data().transform = Mat44::FromTranslationVector(
        mathfu::Vector<float, 3>{0.0f, 0.0f, -3.0f});

    dispatch_data_->data().value = 0.0;

    conditional_data_->data().condition = 1;

    compute_descriptor_set_layout_ = {
        0,                                  // binding
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,  // descriptorType
        1,                                  // descriptorCount
        VK_SHADER_STAGE_COMPUTE_BIT,        // stageFlags
        nullptr                             // pImmutableSamplers
    };
    compute_pipeline_layout_ = containers::make_unique<vulkan::PipelineLayout>(
        data_->allocator(),
        app()->CreatePipelineLayout({{compute_descriptor_set_layout_}}));
    compute_pipeline_ = containers::make_unique<vulkan::VulkanComputePipeline>(
        data_->allocator(),
        app()->CreateComputePipeline(
            compute_pipeline_layout_.get(),
            VkShaderModuleCreateInfo{
                VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                sizeof(compute_shader), compute_shader},
            "main"));
  }

  virtual void InitializeFrameData(
      ConditionalRenderingFrameData* frame_data,
      vulkan::VkCommandBuffer* initialization_buffer,
      size_t frame_index) override {
    frame_data->command_buffer_ =
        containers::make_unique<vulkan::VkCommandBuffer>(
            data_->allocator(), app()->GetCommandBuffer());

    frame_data->cube_descriptor_set_ =
        containers::make_unique<vulkan::DescriptorSet>(
            data_->allocator(),
            app()->AllocateDescriptorSet({cube_descriptor_set_layouts_[0],
                                          cube_descriptor_set_layouts_[1],
                                          cube_descriptor_set_layouts_[2]}));

    VkBufferViewCreateInfo dispatch_data_buffer_view_create_info{
        VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,          // sType
        nullptr,                                            // pNext
        0,                                                  // flags
        dispatch_data_->get_buffer(),                       // buffer
        VK_FORMAT_R32_SFLOAT,                               // format
        dispatch_data_->get_offset_for_frame(frame_index),  // offset
        dispatch_data_->aligned_data_size(),                // range
    };
    ::VkBufferView raw_buf_view;
    LOG_ASSERT(==, data_->logger(), VK_SUCCESS,
               app()->device()->vkCreateBufferView(
                   app()->device(), &dispatch_data_buffer_view_create_info,
                   nullptr, &raw_buf_view));
    frame_data->dispatch_data_buffer_view_ =
        containers::make_unique<vulkan::VkBufferView>(
            data_->allocator(),
            vulkan::VkBufferView(raw_buf_view, nullptr, &app()->device()));

    // Allocate the descriptors for the compute pipeline
    frame_data->compute_descriptor_set_ =
        containers::make_unique<vulkan::DescriptorSet>(
            data_->allocator(),
            app()->AllocateDescriptorSet({compute_descriptor_set_layout_}));

    VkDescriptorBufferInfo buffer_infos[3] = {
        {
            camera_data_->get_buffer(),                       // buffer
            camera_data_->get_offset_for_frame(frame_index),  // offset
            camera_data_->size(),                             // range
        },
        {
            model_data_->get_buffer(),                       // buffer
            model_data_->get_offset_for_frame(frame_index),  // offset
            model_data_->size(),                             // range
        },
        {
            dispatch_data_->get_buffer(),                       // buffer
            dispatch_data_->get_offset_for_frame(frame_index),  // offset
            dispatch_data_->size(),                             // range
        },
    };

    VkWriteDescriptorSet write[3] = {
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
            nullptr,                                 // pNext
            *frame_data->cube_descriptor_set_,       // dstSet
            0,                                       // dstbinding
            0,                                       // dstArrayElement
            2,                                       // descriptorCount
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,       // descriptorType
            nullptr,                                 // pImageInfo
            buffer_infos,                            // pBufferInfo
            nullptr,                                 // pTexelBufferView
        },
        // Render pass: Dispatch buffer
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,   // sType
            nullptr,                                  // pNext
            *frame_data->cube_descriptor_set_,        // dstSet
            2,                                        // dstbinding
            0,                                        // dstArrayElement
            1,                                        // descriptorCount
            VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,  // descriptorType
            nullptr,                                  // pImageInfo
            nullptr,                                  // pBufferInfo
            &frame_data->dispatch_data_buffer_view_
                 ->get_raw_object()  // pTexelBufferView
        },
        // Compute pass: Dispatch buffer
        {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,  // sType
            nullptr,                                 // pNext
            *frame_data->compute_descriptor_set_,    // dstSet
            0,                                       // dstbinding
            0,                                       // dstArrayElement
            1,                                       // descriptorCount
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,       // descriptorType
            nullptr,                                 // pImageInfo
            &buffer_infos[2],                        // pBufferInfo
            nullptr                                  // pTexelBufferView
        },
    };

    app()->device()->vkUpdateDescriptorSets(app()->device(), 3, write, 0,
                                            nullptr);

    ::VkImageView raw_view = color_view(frame_data);

    // Create a framebuffer with depth and image attachments
    VkFramebufferCreateInfo framebuffer_create_info{
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,  // sType
        nullptr,                                    // pNext
        0,                                          // flags
        *render_pass_,                              // renderPass
        1,                                          // attachmentCount
        &raw_view,                                  // attachments
        app()->swapchain().width(),                 // width
        app()->swapchain().height(),                // height
        1                                           // layers
    };

    ::VkFramebuffer raw_framebuffer;
    app()->device()->vkCreateFramebuffer(
        app()->device(), &framebuffer_create_info, nullptr, &raw_framebuffer);
    frame_data->framebuffer_ = containers::make_unique<vulkan::VkFramebuffer>(
        data_->allocator(),
        vulkan::VkFramebuffer(raw_framebuffer, nullptr, &app()->device()));

    (*frame_data->command_buffer_)
        ->vkBeginCommandBuffer((*frame_data->command_buffer_),
                               &sample_application::kBeginCommandBuffer);
    vulkan::VkCommandBuffer& cmdBuffer = (*frame_data->command_buffer_);

    VkRenderPassBeginInfo pass_begin = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,  // sType
        nullptr,                                   // pNext
        *render_pass_,                             // renderPass
        *frame_data->framebuffer_,                 // framebuffer
        {{0, 0},
         {app()->swapchain().width(),
          app()->swapchain().height()}},  // renderArea
        0,                                // clearValueCount
        nullptr                           // clears
    };

    // Two colorful cubes with a blue background
    VkConditionalRenderingBeginInfoEXT conditional_begin1 = {
        VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,  // sType
        nullptr,                                                 // pNext
        conditional_data_->get_buffer(),                         // buffer
        conditional_data_->get_offset_for_frame(frame_index),    // offset
        0,                                                       // flags
    };

    // Single black cube with a pink background
    VkConditionalRenderingBeginInfoEXT conditional_begin2 = conditional_begin1;
    conditional_begin2.flags = VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;

    VkBufferMemoryBarrier to_use_in_comp{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_HOST_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        dispatch_data_->get_buffer(),
        dispatch_data_->get_offset_for_frame(frame_index),
        dispatch_data_->aligned_data_size()};
    VkBufferMemoryBarrier to_use_in_frag{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        nullptr,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        dispatch_data_->get_buffer(),
        dispatch_data_->get_offset_for_frame(frame_index),
        dispatch_data_->aligned_data_size()};

    cmdBuffer->vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                    nullptr, 1, &to_use_in_comp, 0, nullptr);

    cmdBuffer->vkCmdBeginConditionalRenderingEXT(cmdBuffer,
                                                 &conditional_begin1);
    cmdBuffer->vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 *compute_pipeline_);
    cmdBuffer->vkCmdBindDescriptorSets(
        cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        ::VkPipelineLayout(*compute_pipeline_layout_), 0, 1,
        &frame_data->compute_descriptor_set_->raw_set(), 0, nullptr);
    cmdBuffer->vkCmdDispatch(cmdBuffer, 1, 1, 1);
    cmdBuffer->vkCmdEndConditionalRenderingEXT(cmdBuffer);

    cmdBuffer->vkCmdPipelineBarrier(cmdBuffer,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                    nullptr, 1, &to_use_in_frag, 0, nullptr);

    cmdBuffer->vkCmdBeginRenderPass(cmdBuffer, &pass_begin,
                                    VK_SUBPASS_CONTENTS_INLINE);

    cmdBuffer->vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 *cube_pipeline_);
    cmdBuffer->vkCmdBindDescriptorSets(
        cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
        ::VkPipelineLayout(*pipeline_layout_), 0, 1,
        &frame_data->cube_descriptor_set_->raw_set(), 0, nullptr);

    VkClearColorValue clear_color{0.0f, 1.0f, 1.0f, 1.0f};
    VkClearAttachment attachment = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0,
        clear_color,
    };
    VkClearRect rect = {
        {
            {0, 0},
            {app()->swapchain().width(), app()->swapchain().height()},
        },
        0,  // baseArrayLayer
        1,  // layerCount
    };

    cmdBuffer->vkCmdBeginConditionalRenderingEXT(cmdBuffer,
                                                 &conditional_begin1);
    cmdBuffer->vkCmdClearAttachments(cmdBuffer, 1, &attachment, 1, &rect);
    cube_.DrawInstanced(&cmdBuffer, 2);
    cmdBuffer->vkCmdEndConditionalRenderingEXT(cmdBuffer);

    cmdBuffer->vkCmdBeginConditionalRenderingEXT(cmdBuffer,
                                                 &conditional_begin2);
    VkClearColorValue clear_color2{1.0f, 0.0f, 1.0f, 1.0f};
    attachment.clearValue.color = clear_color2;
    cmdBuffer->vkCmdClearAttachments(cmdBuffer, 1, &attachment, 1, &rect);
    cube_.Draw(&cmdBuffer);
    cmdBuffer->vkCmdEndConditionalRenderingEXT(cmdBuffer);

    cmdBuffer->vkCmdEndRenderPass(cmdBuffer);

    (*frame_data->command_buffer_)
        ->vkEndCommandBuffer(*frame_data->command_buffer_);
  }

  virtual void Update(float time_since_last_render) override {
    frames_since_last_notify_ += 1;
    model_data_->data().transform =
        model_data_->data().transform *
        Mat44::FromRotationMatrix(
            Mat44::RotationX(3.14f * time_since_last_render) *
            Mat44::RotationY(3.14f * time_since_last_render * 0.5f));

    conditional_data_->data().condition =
        (frames_since_last_notify_ % 120) < 60;
    // Reset alpha value to 0.
    dispatch_data_->data().value = 0;
  }

  virtual void Render(vulkan::VkQueue* queue, size_t frame_index,
                      ConditionalRenderingFrameData* frame_data) override {
    // Update our uniform buffers.
    camera_data_->UpdateBuffer(queue, frame_index);
    model_data_->UpdateBuffer(queue, frame_index);
    conditional_data_->UpdateBuffer(queue, frame_index);
    // Force update for the compute shader buffer, since it is
    // updated by the GPU.
    bool forceUpdate = true;
    dispatch_data_->UpdateBuffer(queue, frame_index, 0, forceUpdate);

    VkSubmitInfo init_submit_info{
        VK_STRUCTURE_TYPE_SUBMIT_INFO,  // sType
        nullptr,                        // pNext
        0,                              // waitSemaphoreCount
        nullptr,                        // pWaitSemaphores
        nullptr,                        // pWaitDstStageMask,
        1,                              // commandBufferCount
        &(frame_data->command_buffer_->get_command_buffer()),
        0,       // signalSemaphoreCount
        nullptr  // pSignalSemaphores
    };

    app()->render_queue()->vkQueueSubmit(app()->render_queue(), 1,
                                         &init_submit_info,
                                         static_cast<VkFence>(VK_NULL_HANDLE));
  }

 private:
  struct CameraData {
    Mat44 projection_matrix;
  };

  struct ModelData {
    Mat44 transform;
  };

  struct DispatchData {
    float value;
  };

  struct ConditionalRenderingData {
    uint32_t condition;
  };

  const entry::EntryData* data_;
  containers::unique_ptr<vulkan::PipelineLayout> pipeline_layout_;
  containers::unique_ptr<vulkan::VulkanGraphicsPipeline> cube_pipeline_;
  containers::unique_ptr<vulkan::VkRenderPass> render_pass_;
  containers::unique_ptr<vulkan::PipelineLayout> compute_pipeline_layout_;
  containers::unique_ptr<vulkan::VulkanComputePipeline> compute_pipeline_;
  VkDescriptorSetLayoutBinding compute_descriptor_set_layout_;
  VkDescriptorSetLayoutBinding cube_descriptor_set_layouts_[3];
  vulkan::VulkanModel cube_;

  containers::unique_ptr<vulkan::BufferFrameData<CameraData>> camera_data_;
  containers::unique_ptr<vulkan::BufferFrameData<ModelData>> model_data_;
  containers::unique_ptr<vulkan::BufferFrameData<DispatchData>> dispatch_data_;
  containers::unique_ptr<vulkan::BufferFrameData<ConditionalRenderingData>>
      conditional_data_;
  uint32_t frames_since_last_notify_ = 0;
};

int main_entry(const entry::EntryData* data) {
  data->logger()->LogInfo("Application Startup");
  ConditionalRenderingSample sample(data);
  sample.Initialize();

  while (!sample.should_exit() && !data->WindowClosing()) {
    sample.ProcessFrame();
  }
  sample.WaitIdle();

  data->logger()->LogInfo("Application Shutdown");
  return 0;
}
