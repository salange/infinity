#pragma once
// Bound GPU work without changing geometry, shading, target resolution or AA.
// A chunk ends a render pass, stores its attachments, submits and waits. The
// next chunk loads exactly those attachments and restores all drawing state.
#include "gpu.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
namespace cb {
class RenderBatch {
public:
  static constexpr std::uint64_t triangle_budget = 250000;
  RenderBatch(Gpu &gpu, WGPUCommandEncoder &encoder, bool diagnostic)
      : gpu_(gpu), encoder_(encoder), diagnostic_(diagnostic) {}
  ~RenderBatch() {
    if (active_) {
      wgpuRenderPassEncoderEnd(active_);
      wgpuRenderPassEncoderRelease(active_);
    }
    if (encoder_) {
      wgpuCommandEncoderRelease(encoder_);
      encoder_ = nullptr;
    }
  }
  void flush(const std::string &label) {
    if (!encoder_)
      throw GpuUnavailable(gpu_.failure_message());
    WGPUCommandBufferDescriptor descriptor{};
    WGPUCommandBuffer command = wgpuCommandEncoderFinish(encoder_, &descriptor);
    wgpuCommandEncoderRelease(encoder_);
    encoder_ = nullptr;
    if (diagnostic_) {
      std::fprintf(stderr, "[gpu-stage] begin %s triangles=%llu\n",
                   label.c_str(), (unsigned long long)triangles_);
      std::fflush(stderr);
    }
    const auto begin = std::chrono::steady_clock::now();
    const bool ok = gpu_.submit(command, true);
    wgpuCommandBufferRelease(command);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - begin)
                          .count();
    if (diagnostic_ || ms > 1000) {
      std::fprintf(stderr, "[gpu-stage] %s %s %.2f ms\n",
                   ok ? "done" : "failed", label.c_str(), ms);
      std::fflush(stderr);
    }
    if (!ok)
      throw GpuUnavailable(gpu_.failure_message());
    WGPUCommandEncoderDescriptor next{};
    encoder_ = wgpuDeviceCreateCommandEncoder(gpu_.device, &next);
  }
  WGPURenderPassEncoder begin(const WGPURenderPassDescriptor &descriptor,
                              const std::string &label) {
    // Flush ancillary SSAO/Hi-Z work before starting the next geometry stage.
    flush(label + "/prior");
    label_ = label;
    chunk_ = 0;
    triangles_ = 0;
    descriptor_ = descriptor;
    if (descriptor.colorAttachmentCount) {
      color_ = descriptor.colorAttachments[0];
      descriptor_.colorAttachments = &color_;
    }
    if (descriptor.depthStencilAttachment) {
      depth_ = *descriptor.depthStencilAttachment;
      descriptor_.depthStencilAttachment = &depth_;
    }
    groups_ = {};
    pipeline_ = nullptr;
    vertices_ = nullptr;
    indices_ = nullptr;
    active_ = wgpuCommandEncoderBeginRenderPass(encoder_, &descriptor_);
    return active_;
  }
  void end(WGPURenderPassEncoder &pass) {
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    pass = nullptr;
    active_ = nullptr;
    flush(label_ + "/" + std::to_string(chunk_));
    triangles_ = 0;
  }
  void pipeline(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline) {
    if (pass == active_)
      pipeline_ = pipeline;
    wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  }
  void group(WGPURenderPassEncoder pass, std::uint32_t slot,
             WGPUBindGroup group, std::uint32_t count,
             const std::uint32_t *offsets) {
    if (pass == active_ && slot < groups_.size()) {
      auto &s = groups_[slot];
      s.group = group;
      s.offsets.clear();
      if (count)
        s.offsets.assign(offsets, offsets + count);
    }
    wgpuRenderPassEncoderSetBindGroup(pass, slot, group, count, offsets);
  }
  void vertex(WGPURenderPassEncoder pass, std::uint32_t slot, WGPUBuffer buffer,
              std::uint64_t offset, std::uint64_t size) {
    if (pass == active_) {
      vertices_ = buffer;
      vertex_slot_ = slot;
      vertex_offset_ = offset;
      vertex_size_ = size;
    }
    wgpuRenderPassEncoderSetVertexBuffer(pass, slot, buffer, offset, size);
  }
  void index(WGPURenderPassEncoder pass, WGPUBuffer buffer,
             WGPUIndexFormat format, std::uint64_t offset, std::uint64_t size) {
    if (pass == active_) {
      indices_ = buffer;
      index_format_ = format;
      index_offset_ = offset;
      index_size_ = size;
    }
    wgpuRenderPassEncoderSetIndexBuffer(pass, buffer, format, offset, size);
  }
  void reserve(WGPURenderPassEncoder &pass, std::uint64_t triangles) {
    if (pass != active_)
      return;
    if (triangles_ && triangles_ + triangles > triangle_budget) {
      wgpuRenderPassEncoderEnd(pass);
      wgpuRenderPassEncoderRelease(pass);
      active_ = nullptr;
      flush(label_ + "/" + std::to_string(chunk_++));
      triangles_ = 0;
      if (descriptor_.colorAttachmentCount)
        color_.loadOp = WGPULoadOp_Load;
      if (descriptor_.depthStencilAttachment)
        depth_.depthLoadOp = WGPULoadOp_Load;
      active_ = wgpuCommandEncoderBeginRenderPass(encoder_, &descriptor_);
      pass = active_;
      if (pipeline_)
        wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
      for (std::uint32_t i = 0; i < groups_.size(); ++i)
        if (groups_[i].group)
          wgpuRenderPassEncoderSetBindGroup(pass, i, groups_[i].group,
                                            groups_[i].offsets.size(),
                                            groups_[i].offsets.data());
      if (vertices_)
        wgpuRenderPassEncoderSetVertexBuffer(pass, vertex_slot_, vertices_,
                                             vertex_offset_, vertex_size_);
      if (indices_)
        wgpuRenderPassEncoderSetIndexBuffer(pass, indices_, index_format_,
                                            index_offset_, index_size_);
    }
    triangles_ += triangles;
  }
  void draw(WGPURenderPassEncoder &pass, std::uint32_t count,
            std::uint32_t instances, std::uint32_t first, std::int32_t base,
            std::uint32_t first_instance) {
    const std::uint32_t max_indices = std::uint32_t(triangle_budget * 3);
    if (!count || !instances)
      return;
    if (count <= max_indices) {
      const std::uint32_t step = std::max(1u, max_indices / count);
      for (std::uint32_t i = 0; i < instances;) {
        const auto n = std::min(step, instances - i);
        reserve(pass, std::uint64_t(count) * n / 3);
        wgpuRenderPassEncoderDrawIndexed(pass, count, n, first, base,
                                         first_instance + i);
        i += n;
      }
    } else {
      for (std::uint32_t i = 0; i < instances; ++i)
        for (std::uint32_t j = 0; j < count;) {
          const auto n = std::min(max_indices, count - j);
          reserve(pass, n / 3);
          wgpuRenderPassEncoderDrawIndexed(pass, n, 1, first + j, base,
                                           first_instance + i);
          j += n;
        }
    }
  }
  void indirect(WGPURenderPassEncoder &pass, WGPUBuffer args,
                std::uint64_t offset, std::uint64_t triangles) {
    reserve(pass, triangles);
    wgpuRenderPassEncoderDrawIndexedIndirect(pass, args, offset);
  }

private:
  struct Group {
    WGPUBindGroup group{};
    std::vector<std::uint32_t> offsets;
  };
  Gpu &gpu_;
  WGPUCommandEncoder &encoder_;
  bool diagnostic_{};
  WGPURenderPassEncoder active_{};
  WGPURenderPassDescriptor descriptor_{};
  WGPURenderPassColorAttachment color_{};
  WGPURenderPassDepthStencilAttachment depth_{};
  std::string label_;
  std::uint32_t chunk_{};
  std::uint64_t triangles_{};
  WGPURenderPipeline pipeline_{};
  std::array<Group, 2> groups_{};
  WGPUBuffer vertices_{}, indices_{};
  std::uint32_t vertex_slot_{};
  WGPUIndexFormat index_format_{};
  std::uint64_t vertex_offset_{}, vertex_size_{}, index_offset_{},
      index_size_{};
};
} // namespace cb
