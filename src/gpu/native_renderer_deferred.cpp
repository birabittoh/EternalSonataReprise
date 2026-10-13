// eternalsonata - ReXGlue Recompiled Project
//
// See native_renderer_deferred.h.

#include "native_renderer_deferred.h"

#include <chrono>
#include <cstring>
#include <memory>

#if defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include <rex/logging.h>

#include "big_core_affinity.h"
#include "native_renderer_profile.h"

namespace eternalsonata {

using namespace plume;

std::atomic<uint64_t> g_record_worker_ns{0};

namespace {

inline void CpuRelax() {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#endif
}

// The worker spins this long on an empty ring before sleeping. The guest
// records in bursts a few microseconds apart, and a futex round trip per burst
// would cost more than the spin.
constexpr auto kWorkerSpin = std::chrono::microseconds(50);

// Space is handed back to the producer at least this often, so a full ring
// does not wait for the worker's whole backlog.
constexpr uint64_t kReleaseBytes = 64 * 1024;

// Null for an empty array, which callers turn into a zero count.
template <typename T>
T* CopyArrayStart(uint8_t* tail, const T* items, uint32_t count) {
  if (items == nullptr || count == 0)
    return nullptr;
  std::uninitialized_copy_n(items, count, reinterpret_cast<T*>(tail));
  return reinterpret_cast<T*>(tail);
}

void Unsupported(const char* name) {
  REXLOG_ERROR("native_renderer: {} is not supported on the deferred command list", name);
}

}  // namespace

DeferredCommandList::DeferredCommandList() : ring_(new uint8_t[kRingBytes]) {
  worker_ = std::thread([this] { Run(); });
}

DeferredCommandList::~DeferredCommandList() {
  Drain();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_one();
  worker_.join();
}

uint8_t* DeferredCommandList::Reserve(uint32_t total) {
  uint64_t offset = head_ & kRingMask;
  if (offset + total > kRingBytes) {
    const uint32_t pad = uint32_t(kRingBytes - offset);
    WaitForSpace(uint64_t(pad) + total);
    Header* header = reinterpret_cast<Header*>(ring_.get() + offset);
    header->invoke = nullptr;
    header->size = pad;
    // Published with the packet that follows it.
    head_ += pad;
    offset = 0;
  } else {
    WaitForSpace(total);
  }
  return ring_.get() + offset;
}

void DeferredCommandList::Commit(uint32_t total) {
  head_ += total;
  published_.store(head_, std::memory_order_seq_cst);
  if (worker_asleep_.load(std::memory_order_seq_cst))
    Wake();
}

void DeferredCommandList::WaitForSpace(uint64_t bytes) {
  if (head_ + bytes - consumed_cache_ <= kRingBytes)
    return;
  for (uint32_t spins = 0;; ++spins) {
    consumed_cache_ = consumed_.load(std::memory_order_acquire);
    if (head_ + bytes - consumed_cache_ <= kRingBytes)
      return;
    if (spins < 1024)
      CpuRelax();
    else
      std::this_thread::yield();
  }
}

void DeferredCommandList::Wake() {
  { std::lock_guard<std::mutex> lock(mutex_); }
  wake_.notify_one();
}

void DeferredCommandList::Drain() {
  if (consumed_.load(std::memory_order_acquire) == head_)
    return;
  ProfileZone zone(kPhaseRecordDrain);
  for (uint32_t spins = 0; consumed_.load(std::memory_order_acquire) != head_; ++spins) {
    if (spins < 4096)
      CpuRelax();
    else
      std::this_thread::yield();
  }
  consumed_cache_ = head_;
}

void DeferredCommandList::Run() {
#if defined(__linux__) || defined(__ANDROID__)
  pthread_setname_np(pthread_self(), "PlumeRecord");
#endif
  PinThisThreadToBigCores();

  Target target = nullptr;
  uint64_t tail = 0;
  for (;;) {
    uint64_t available = published_.load(std::memory_order_acquire);
    if (available == tail) {
      const auto spin_until = std::chrono::steady_clock::now() + kWorkerSpin;
      uint32_t spins = 0;
      while (available == tail) {
        CpuRelax();
        available = published_.load(std::memory_order_acquire);
        if ((++spins & 63) == 0 && std::chrono::steady_clock::now() >= spin_until)
          break;
      }
      if (available == tail) {
        std::unique_lock<std::mutex> lock(mutex_);
        worker_asleep_.store(true, std::memory_order_seq_cst);
        wake_.wait(lock, [&] {
          return published_.load(std::memory_order_seq_cst) != tail || stop_;
        });
        worker_asleep_.store(false, std::memory_order_relaxed);
        if (stop_ && published_.load(std::memory_order_acquire) == tail)
          return;
        continue;
      }
    }

    const bool timed = g_profile_zones_enabled.load(std::memory_order_relaxed);
    const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    uint64_t released = tail;
    while (tail != available) {
      Header* header = reinterpret_cast<Header*>(ring_.get() + (tail & kRingMask));
      const uint32_t size = header->size;
      if (header->invoke != nullptr)
        header->invoke(header + 1, target);
      tail += size;
      if (tail - released >= kReleaseBytes) {
        consumed_.store(tail, std::memory_order_release);
        released = tail;
      }
    }
    consumed_.store(tail, std::memory_order_release);
    if (timed) {
      g_record_worker_ns.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count()),
          std::memory_order_relaxed);
    }
  }
}

void DeferredCommandList::Open(RenderCommandList* list) {
  Push([list](Target& target) {
    target = list;
    target->begin();
  });
}

void DeferredCommandList::begin() {
  Push([](Target& target) { target->begin(); });
}

void DeferredCommandList::end() {
  Push([](Target& target) { target->end(); });
}

void DeferredCommandList::barriers(RenderBarrierStages stages,
                                   const RenderBufferBarrier* bufferBarriers,
                                   uint32_t bufferBarriersCount,
                                   const RenderTextureBarrier* textureBarriers,
                                   uint32_t textureBarriersCount) {
  const uint64_t buffer_bytes = Align16(uint64_t(bufferBarriersCount) * sizeof(RenderBufferBarrier));
  Push(buffer_bytes + uint64_t(textureBarriersCount) * sizeof(RenderTextureBarrier),
       [&](uint8_t* tail) {
         RenderBufferBarrier* buffers = CopyArrayStart(tail, bufferBarriers, bufferBarriersCount);
         RenderTextureBarrier* textures =
             CopyArrayStart(tail + buffer_bytes, textureBarriers, textureBarriersCount);
         const uint32_t buffer_count = buffers ? bufferBarriersCount : 0;
         const uint32_t texture_count = textures ? textureBarriersCount : 0;
         return [stages, buffers, buffer_count, textures, texture_count](Target& target) {
           target->barriers(stages, buffers, buffer_count, textures, texture_count);
         };
       });
}

void DeferredCommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) {
  Push([x, y, z](Target& target) { target->dispatch(x, y, z); });
}

void DeferredCommandList::traceRays(uint32_t, uint32_t, uint32_t, RenderBufferReference,
                                    const RenderShaderBindingGroupsInfo&) {
  Unsupported("traceRays");
}

void DeferredCommandList::drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount,
                                        uint32_t startVertexLocation,
                                        uint32_t startInstanceLocation) {
  Push([=](Target& target) {
    target->drawInstanced(vertexCountPerInstance, instanceCount, startVertexLocation,
                          startInstanceLocation);
  });
}

void DeferredCommandList::drawIndexedInstanced(uint32_t indexCountPerInstance,
                                               uint32_t instanceCount,
                                               uint32_t startIndexLocation,
                                               int32_t baseVertexLocation,
                                               uint32_t startInstanceLocation) {
  Push([=](Target& target) {
    target->drawIndexedInstanced(indexCountPerInstance, instanceCount, startIndexLocation,
                                 baseVertexLocation, startInstanceLocation);
  });
}

void DeferredCommandList::setPipeline(const RenderPipeline* pipeline) {
  Push([pipeline](Target& target) { target->setPipeline(pipeline); });
}

void DeferredCommandList::setComputePipelineLayout(const RenderPipelineLayout* pipelineLayout) {
  Push([pipelineLayout](Target& target) { target->setComputePipelineLayout(pipelineLayout); });
}

// The size has to be explicit: zero means "the whole range", and only the
// pipeline layout knows how big that is.
void DeferredCommandList::setComputePushConstants(uint32_t rangeIndex, const void* data,
                                                  uint32_t offset, uint32_t size) {
  if (size == 0) {
    Unsupported("setComputePushConstants without a size");
    return;
  }
  Push(size, [&](uint8_t* tail) {
    std::memcpy(tail, data, size);
    return [rangeIndex, tail, offset, size](Target& target) {
      target->setComputePushConstants(rangeIndex, tail, offset, size);
    };
  });
}

void DeferredCommandList::setComputeDescriptorSet(RenderDescriptorSet* descriptorSet,
                                                  uint32_t setIndex) {
  Push([descriptorSet, setIndex](Target& target) {
    target->setComputeDescriptorSet(descriptorSet, setIndex);
  });
}

void DeferredCommandList::setGraphicsPipelineLayout(const RenderPipelineLayout* pipelineLayout) {
  Push([pipelineLayout](Target& target) { target->setGraphicsPipelineLayout(pipelineLayout); });
}

void DeferredCommandList::setGraphicsPushConstants(uint32_t rangeIndex, const void* data,
                                                   uint32_t offset, uint32_t size) {
  if (size == 0) {
    Unsupported("setGraphicsPushConstants without a size");
    return;
  }
  Push(size, [&](uint8_t* tail) {
    std::memcpy(tail, data, size);
    return [rangeIndex, tail, offset, size](Target& target) {
      target->setGraphicsPushConstants(rangeIndex, tail, offset, size);
    };
  });
}

void DeferredCommandList::setGraphicsDescriptorSet(RenderDescriptorSet* descriptorSet,
                                                   uint32_t setIndex) {
  Push([descriptorSet, setIndex](Target& target) {
    target->setGraphicsDescriptorSet(descriptorSet, setIndex);
  });
}

void DeferredCommandList::setGraphicsRootDescriptor(RenderBufferReference bufferReference,
                                                    uint32_t rootDescriptorIndex) {
  Push([bufferReference, rootDescriptorIndex](Target& target) {
    target->setGraphicsRootDescriptor(bufferReference, rootDescriptorIndex);
  });
}

void DeferredCommandList::setRaytracingPipelineLayout(const RenderPipelineLayout*) {
  Unsupported("setRaytracingPipelineLayout");
}

void DeferredCommandList::setRaytracingPushConstants(uint32_t, const void*, uint32_t, uint32_t) {
  Unsupported("setRaytracingPushConstants");
}

void DeferredCommandList::setRaytracingDescriptorSet(RenderDescriptorSet*, uint32_t) {
  Unsupported("setRaytracingDescriptorSet");
}

void DeferredCommandList::setIndexBuffer(const RenderIndexBufferView* view) {
  if (view == nullptr) {
    Push([](Target& target) { target->setIndexBuffer(nullptr); });
    return;
  }
  Push([copy = *view](Target& target) { target->setIndexBuffer(&copy); });
}

void DeferredCommandList::setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView* views,
                                           uint32_t viewCount,
                                           const RenderInputSlot* inputSlots) {
  const uint64_t view_bytes = Align16(uint64_t(viewCount) * sizeof(RenderVertexBufferView));
  Push(view_bytes + uint64_t(viewCount) * sizeof(RenderInputSlot), [&](uint8_t* tail) {
    RenderVertexBufferView* view_copy = CopyArrayStart(tail, views, viewCount);
    RenderInputSlot* slot_copy = CopyArrayStart(tail + view_bytes, inputSlots, viewCount);
    return [startSlot, view_copy, viewCount, slot_copy](Target& target) {
      target->setVertexBuffers(startSlot, view_copy, view_copy ? viewCount : 0, slot_copy);
    };
  });
}

void DeferredCommandList::setViewports(const RenderViewport* viewports, uint32_t count) {
  Push(uint64_t(count) * sizeof(RenderViewport), [&](uint8_t* tail) {
    RenderViewport* copy = CopyArrayStart(tail, viewports, count);
    return [copy, count](Target& target) { target->setViewports(copy, count); };
  });
}

void DeferredCommandList::setScissors(const RenderRect* scissorRects, uint32_t count) {
  Push(uint64_t(count) * sizeof(RenderRect), [&](uint8_t* tail) {
    RenderRect* copy = CopyArrayStart(tail, scissorRects, count);
    return [copy, count](Target& target) { target->setScissors(copy, count); };
  });
}

void DeferredCommandList::setFramebuffer(const RenderFramebuffer* framebuffer) {
  Push([framebuffer](Target& target) { target->setFramebuffer(framebuffer); });
}

void DeferredCommandList::setDepthBias(float depthBias, float depthBiasClamp,
                                       float slopeScaledDepthBias) {
  Push([=](Target& target) {
    target->setDepthBias(depthBias, depthBiasClamp, slopeScaledDepthBias);
  });
}

void DeferredCommandList::clearColor(uint32_t attachmentIndex, RenderColor colorValue,
                                     const RenderRect* clearRects, uint32_t clearRectsCount) {
  Push(uint64_t(clearRectsCount) * sizeof(RenderRect), [&](uint8_t* tail) {
    RenderRect* copy = CopyArrayStart(tail, clearRects, clearRectsCount);
    const uint32_t count = copy ? clearRectsCount : 0;
    return [attachmentIndex, colorValue, copy, count](Target& target) {
      target->clearColor(attachmentIndex, colorValue, copy, count);
    };
  });
}

void DeferredCommandList::clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue,
                                            uint32_t stencilValue, const RenderRect* clearRects,
                                            uint32_t clearRectsCount) {
  Push(uint64_t(clearRectsCount) * sizeof(RenderRect), [&](uint8_t* tail) {
    RenderRect* copy = CopyArrayStart(tail, clearRects, clearRectsCount);
    const uint32_t count = copy ? clearRectsCount : 0;
    return [clearDepth, clearStencil, depthValue, stencilValue, copy, count](Target& target) {
      target->clearDepthStencil(clearDepth, clearStencil, depthValue, stencilValue, copy, count);
    };
  });
}

void DeferredCommandList::copyBufferRegion(RenderBufferReference dstBuffer,
                                           RenderBufferReference srcBuffer, uint64_t size) {
  Push([=](Target& target) { target->copyBufferRegion(dstBuffer, srcBuffer, size); });
}

void DeferredCommandList::copyTextureRegion(const RenderTextureCopyLocation& dstLocation,
                                            const RenderTextureCopyLocation& srcLocation,
                                            uint32_t dstX, uint32_t dstY, uint32_t dstZ,
                                            const RenderBox* srcBox) {
  const bool has_box = srcBox != nullptr;
  const RenderBox box = has_box ? *srcBox : RenderBox();
  Push([dstLocation, srcLocation, dstX, dstY, dstZ, has_box, box](Target& target) {
    target->copyTextureRegion(dstLocation, srcLocation, dstX, dstY, dstZ,
                              has_box ? &box : nullptr);
  });
}

void DeferredCommandList::copyBuffer(const RenderBuffer* dstBuffer, const RenderBuffer* srcBuffer) {
  Push([=](Target& target) { target->copyBuffer(dstBuffer, srcBuffer); });
}

void DeferredCommandList::copyTexture(const RenderTexture* dstTexture,
                                      const RenderTexture* srcTexture) {
  Push([=](Target& target) { target->copyTexture(dstTexture, srcTexture); });
}

bool DeferredCommandList::blitTexture(const RenderTexture* dstTexture,
                                      const RenderTexture* srcTexture, bool linear) {
  Push([=](Target& target) { target->blitTexture(dstTexture, srcTexture, linear); });
  return true;
}

void DeferredCommandList::resolveTexture(const RenderTexture* dstTexture,
                                         const RenderTexture* srcTexture) {
  Push([=](Target& target) { target->resolveTexture(dstTexture, srcTexture); });
}

void DeferredCommandList::resolveTextureRegion(const RenderTexture* dstTexture, uint32_t dstX,
                                               uint32_t dstY, const RenderTexture* srcTexture,
                                               const RenderRect* srcRect,
                                               RenderResolveMode resolveMode) {
  const bool has_rect = srcRect != nullptr;
  const RenderRect rect = has_rect ? *srcRect : RenderRect();
  Push([=](Target& target) {
    target->resolveTextureRegion(dstTexture, dstX, dstY, srcTexture, has_rect ? &rect : nullptr,
                                 resolveMode);
  });
}

void DeferredCommandList::buildBottomLevelAS(const RenderAccelerationStructure*,
                                             RenderBufferReference,
                                             const RenderBottomLevelASBuildInfo&) {
  Unsupported("buildBottomLevelAS");
}

void DeferredCommandList::buildTopLevelAS(const RenderAccelerationStructure*,
                                          RenderBufferReference, RenderBufferReference,
                                          const RenderTopLevelASBuildInfo&) {
  Unsupported("buildTopLevelAS");
}

void DeferredCommandList::discardTexture(const RenderTexture* texture) {
  Push([texture](Target& target) { target->discardTexture(texture); });
}

void DeferredCommandList::resetQueryPool(const RenderQueryPool* queryPool,
                                         uint32_t queryFirstIndex, uint32_t queryCount) {
  Push([=](Target& target) { target->resetQueryPool(queryPool, queryFirstIndex, queryCount); });
}

void DeferredCommandList::writeTimestamp(const RenderQueryPool* queryPool, uint32_t queryIndex) {
  Push([=](Target& target) { target->writeTimestamp(queryPool, queryIndex); });
}

void DeferredCommandList::beginQuery(const RenderQueryPool* queryPool, uint32_t queryIndex) {
  Push([=](Target& target) { target->beginQuery(queryPool, queryIndex); });
}

void DeferredCommandList::endQuery(const RenderQueryPool* queryPool, uint32_t queryIndex) {
  Push([=](Target& target) { target->endQuery(queryPool, queryIndex); });
}

}  // namespace eternalsonata
