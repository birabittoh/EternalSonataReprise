// eternalsonata - ReXGlue Recompiled Project
//
// A command list that records into a ring and replays on a worker thread onto
// the real one, so the driver's recording cost leaves the guest thread.
//
// Everything that reads guest memory has already happened by the time a call
// reaches this: a packet holds only Plume arguments, with the arrays they point
// at copied. Objects named by pointer have to outlive the replay, which frame
// slot retirement already guarantees, since nothing recorded is freed before
// its frame's fence.
//
// Vulkan only. Its blitTexture always succeeds, which is what lets this one
// answer without waiting for the replay.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#include <plume_render_interface.h>

namespace eternalsonata {

class DeferredCommandList final : public plume::RenderCommandList {
 public:
  DeferredCommandList();
  ~DeferredCommandList() override;

  // Points every later packet at `target` and begins it.
  void Open(plume::RenderCommandList* target);

  // Returns once the worker has replayed everything recorded so far. Required
  // before the real list is ended, submitted or reused.
  void Drain();

  void begin() override;
  void end() override;
  void barriers(plume::RenderBarrierStages stages, const plume::RenderBufferBarrier* bufferBarriers,
                uint32_t bufferBarriersCount, const plume::RenderTextureBarrier* textureBarriers,
                uint32_t textureBarriersCount) override;
  void dispatch(uint32_t x, uint32_t y, uint32_t z) override;
  void traceRays(uint32_t width, uint32_t height, uint32_t depth,
                 plume::RenderBufferReference shaderBindingTable,
                 const plume::RenderShaderBindingGroupsInfo& shaderBindingGroupsInfo) override;
  void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                     uint32_t startVertexLocation, uint32_t startInstanceLocation) override;
  void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount,
                            uint32_t startIndexLocation, int32_t baseVertexLocation,
                            uint32_t startInstanceLocation) override;
  void setPipeline(const plume::RenderPipeline* pipeline) override;
  void setComputePipelineLayout(const plume::RenderPipelineLayout* pipelineLayout) override;
  void setComputePushConstants(uint32_t rangeIndex, const void* data, uint32_t offset,
                               uint32_t size) override;
  void setComputeDescriptorSet(plume::RenderDescriptorSet* descriptorSet,
                               uint32_t setIndex) override;
  void setGraphicsPipelineLayout(const plume::RenderPipelineLayout* pipelineLayout) override;
  void setGraphicsPushConstants(uint32_t rangeIndex, const void* data, uint32_t offset,
                                uint32_t size) override;
  void setGraphicsDescriptorSet(plume::RenderDescriptorSet* descriptorSet,
                                uint32_t setIndex) override;
  void setGraphicsRootDescriptor(plume::RenderBufferReference bufferReference,
                                 uint32_t rootDescriptorIndex) override;
  void setRaytracingPipelineLayout(const plume::RenderPipelineLayout* pipelineLayout) override;
  void setRaytracingPushConstants(uint32_t rangeIndex, const void* data, uint32_t offset,
                                  uint32_t size) override;
  void setRaytracingDescriptorSet(plume::RenderDescriptorSet* descriptorSet,
                                  uint32_t setIndex) override;
  void setIndexBuffer(const plume::RenderIndexBufferView* view) override;
  void setVertexBuffers(uint32_t startSlot, const plume::RenderVertexBufferView* views,
                        uint32_t viewCount, const plume::RenderInputSlot* inputSlots) override;
  void setViewports(const plume::RenderViewport* viewports, uint32_t count) override;
  void setScissors(const plume::RenderRect* scissorRects, uint32_t count) override;
  void setFramebuffer(const plume::RenderFramebuffer* framebuffer) override;
  void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override;
  void setStencilReference(uint32_t stencilReference) override;
  void clearColor(uint32_t attachmentIndex, plume::RenderColor colorValue,
                  const plume::RenderRect* clearRects, uint32_t clearRectsCount) override;
  void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue,
                         uint32_t stencilValue, const plume::RenderRect* clearRects,
                         uint32_t clearRectsCount) override;
  void copyBufferRegion(plume::RenderBufferReference dstBuffer,
                        plume::RenderBufferReference srcBuffer, uint64_t size) override;
  void copyTextureRegion(const plume::RenderTextureCopyLocation& dstLocation,
                         const plume::RenderTextureCopyLocation& srcLocation, uint32_t dstX,
                         uint32_t dstY, uint32_t dstZ, const plume::RenderBox* srcBox) override;
  void copyBuffer(const plume::RenderBuffer* dstBuffer,
                  const plume::RenderBuffer* srcBuffer) override;
  void copyTexture(const plume::RenderTexture* dstTexture,
                   const plume::RenderTexture* srcTexture) override;
  bool blitTexture(const plume::RenderTexture* dstTexture, const plume::RenderTexture* srcTexture,
                   bool linear) override;
  void resolveTexture(const plume::RenderTexture* dstTexture,
                      const plume::RenderTexture* srcTexture) override;
  void resolveTextureRegion(const plume::RenderTexture* dstTexture, uint32_t dstX, uint32_t dstY,
                            const plume::RenderTexture* srcTexture,
                            const plume::RenderRect* srcRect,
                            plume::RenderResolveMode resolveMode) override;
  void buildBottomLevelAS(const plume::RenderAccelerationStructure* dstAccelerationStructure,
                          plume::RenderBufferReference scratchBuffer,
                          const plume::RenderBottomLevelASBuildInfo& buildInfo) override;
  void buildTopLevelAS(const plume::RenderAccelerationStructure* dstAccelerationStructure,
                       plume::RenderBufferReference scratchBuffer,
                       plume::RenderBufferReference instancesBuffer,
                       const plume::RenderTopLevelASBuildInfo& buildInfo) override;
  void discardTexture(const plume::RenderTexture* texture) override;
  void resetQueryPool(const plume::RenderQueryPool* queryPool, uint32_t queryFirstIndex,
                      uint32_t queryCount) override;
  void writeTimestamp(const plume::RenderQueryPool* queryPool, uint32_t queryIndex) override;
  void beginQuery(const plume::RenderQueryPool* queryPool, uint32_t queryIndex) override;
  void endQuery(const plume::RenderQueryPool* queryPool, uint32_t queryIndex) override;

 private:
  using Target = plume::RenderCommandList*;

  struct alignas(16) Header {
    // Null for the padding that skips to the start of the ring.
    void (*invoke)(void* body, Target& target);
    uint32_t size;
  };

  static constexpr uint64_t kRingBytes = 8ull << 20;
  static constexpr uint64_t kRingMask = kRingBytes - 1;

  static constexpr uint32_t Align16(uint64_t bytes) { return uint32_t((bytes + 15) & ~15ull); }

  template <typename F>
  static void Invoke(void* body, Target& target) {
    F* f = static_cast<F*>(body);
    (*f)(target);
    f->~F();
  }

  // `make` receives where `tail_bytes` of copied arrays go and returns the
  // callable the worker runs.
  template <typename Make>
  void Push(uint64_t tail_bytes, Make&& make) {
    using F = decltype(make(static_cast<uint8_t*>(nullptr)));
    static_assert(alignof(F) <= 16, "packet bodies are 16 byte aligned");
    const uint32_t body = Align16(sizeof(F));
    const uint32_t total = uint32_t(sizeof(Header)) + body + Align16(tail_bytes);
    uint8_t* packet = Reserve(total);
    new (packet + sizeof(Header)) F(make(packet + sizeof(Header) + body));
    Header* header = reinterpret_cast<Header*>(packet);
    header->invoke = &Invoke<F>;
    header->size = total;
    Commit(total);
  }

  template <typename F>
  void Push(F&& f) {
    Push(0, [&](uint8_t*) { return std::forward<F>(f); });
  }

  uint8_t* Reserve(uint32_t total);
  void Commit(uint32_t total);
  void WaitForSpace(uint64_t bytes);
  void Wake();
  void WakeIfAsleep();
  void Run();

  std::unique_ptr<uint8_t[]> ring_;

  // Producer side, guest thread only.
  uint64_t head_ = 0;
  uint64_t consumed_cache_ = 0;
  // Head when the worker was last seen awake or woken.
  uint64_t unwoken_from_ = 0;

  alignas(64) std::atomic<uint64_t> published_{0};
  alignas(64) std::atomic<uint64_t> consumed_{0};
  alignas(64) std::atomic<bool> worker_asleep_{false};

  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace eternalsonata
