#include <cassert>
#include <cuda_runtime.h>

#include "scope/init/init.hpp"
#include "scope/init/flags.hpp"

#include "args.hpp"

#define NAME "Comm/Duplex/Memcpy/GPUGPU"

#define OR_SKIP(stmt, msg) \
  if (PRINT_IF_ERROR(stmt)) { \
    state.SkipWithError(msg); \
    return; \
  }

static void Comm_Duplex_Memcpy_GPUGPU(benchmark::State &state) {

  if (!has_cuda) {
    state.SkipWithError(NAME " no CUDA device found");
    return;
  }

  if (num_gpus() < 2) {
    state.SkipWithError(NAME " requires at least 2 GPUs");
    return;
  }
  assert(FLAG(cuda_device_ids).size() >= 2);
  const int gpu0 = FLAG(cuda_device_ids)[0];
  const int gpu1 = FLAG(cuda_device_ids)[1];
  if (gpu0 == gpu1) {
    state.SkipWithError(NAME " requires two different GPUs");
    return;
  }


  const auto bytes = 1ULL << static_cast<size_t>(state.range(0));


  OR_SKIP(utils::cuda_reset_device(gpu0), NAME " failed to reset CUDA device");
  OR_SKIP(utils::cuda_reset_device(gpu1), NAME " failed to reset CUDA device");

  // There are two copies, one gpu0 -> gpu1, one gpu1 -> gpu0

  // Create One stream per copy
  cudaStream_t stream1, stream2;
  std::vector<cudaStream_t> streams = {stream1, stream2};
  OR_SKIP(cudaStreamCreate(&streams[0]), NAME "failed to create stream");
  OR_SKIP(cudaStreamCreate(&streams[1]), NAME "failed to create stream");


  // Start and stop events for each copy
  cudaEvent_t start1, start2, stop1, stop2;
  std::vector<cudaEvent_t> starts = {start1, start2};
  std::vector<cudaEvent_t> stops = {stop1, stop2};
  OR_SKIP(cudaEventCreate(&starts[0]), NAME " failed to create event");
  OR_SKIP(cudaEventCreate(&starts[1]), NAME " failed to create event");
  OR_SKIP(cudaEventCreate(&stops[0] ), NAME " failed to create event");
  OR_SKIP(cudaEventCreate(&stops[1] ), NAME " failed to create event");

  // Source and destination for each copy
  std::vector<char *> srcs, dsts;

  // create a source and destination allocation for first copy
  

  // allocate on gpu0 and enable peer access
  char *ptr;
  OR_SKIP(cudaSetDevice(gpu0), NAME "failed to set device");
  OR_SKIP(cudaMalloc(&ptr, bytes), NAME " failed to perform cudaMalloc");
  srcs.push_back(ptr);
  OR_SKIP(cudaMemset(ptr, 0, bytes), NAME " failed to perform src cudaMemset");
  cudaError_t err = cudaDeviceEnablePeerAccess(gpu1, 0);
  if (cudaSuccess != err && cudaErrorPeerAccessAlreadyEnabled != err) {
    state.SkipWithError(NAME " failed to ensure peer access");
    return;
  }

  // allocate on gpu1 and enable peer access
  OR_SKIP(cudaSetDevice(gpu1), NAME "failed to set device");
  OR_SKIP(cudaMalloc(&ptr, bytes), NAME " failed to perform cudaMalloc");
  OR_SKIP(cudaMemset(ptr, 0, bytes), NAME " failed to perform src cudaMemset");
  dsts.push_back(ptr);
  err = cudaDeviceEnablePeerAccess(gpu0, 0);
  if (cudaSuccess != err && cudaErrorPeerAccessAlreadyEnabled != err) {
    state.SkipWithError(NAME " failed to ensure peer access");
    return;
  }

  // create a source and destination for second copy
  OR_SKIP(cudaSetDevice(gpu1), NAME " failed to set device");
  OR_SKIP(cudaMalloc(&ptr, bytes), NAME " failed to perform cudaMalloc");
  OR_SKIP(cudaMemset(ptr, 0, bytes), NAME " failed to perform src cudaMemset");
  srcs.push_back(ptr);

  OR_SKIP(cudaSetDevice(gpu1), NAME " failed to set device");
  OR_SKIP(cudaMalloc(&ptr, bytes), NAME " failed to perform cudaMalloc");
  OR_SKIP(cudaMemset(ptr, 0, bytes), NAME " failed to perform src cudaMemset");
  dsts.push_back(ptr);


  assert(starts.size() == stops.size());
  assert(streams.size() == starts.size());
  assert(srcs.size() == dsts.size());
  assert(streams.size() == srcs.size());

  for (auto _ : state) {

    // Start all copies
    for (size_t i = 0; i < streams.size(); ++i) {
      auto start = starts[i];
      auto stop = stops[i];
      auto stream = streams[i];
      auto src = srcs[i];
      auto dst = dsts[i];
      OR_SKIP(cudaEventRecord(start, stream), NAME " failed to record start event");
      OR_SKIP(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDeviceToDevice, stream), NAME " failed to start cudaMemcpyAsync");
      OR_SKIP(cudaEventRecord(stop, stream), NAME " failed to record stop event");
    }

    // Wait for all copies to finish
    for (auto s : stops) {
      OR_SKIP(cudaEventSynchronize(s), NAME " failed to synchronize");
    }

    // Find the longest time between any start and stop
    float maxMillis = 0;
    for (const auto start : starts) {
      for (const auto stop : stops) {
        float millis;
        OR_SKIP(cudaEventElapsedTime(&millis, start, stop), NAME " failed to compute elapsed tiume");
        maxMillis = std::max(millis, maxMillis);
      }
    }
    state.SetIterationTime(maxMillis / 1000);
  }
  state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(bytes) * 2);
  state.counters["bytes"] = bytes;
  state.counters["gpu0"] = gpu0;
  state.counters["gpu1"] = gpu1;

  float stopSum = 0;
  float startSum = 0;
  for ( const auto stream : streams ){

        float startTime1, startTime2, stopTime1, stopTime2;
        OR_SKIP(cudaEventElapsedTime(&startTime1, starts[0], starts[1]), NAME " failed to compare start times");
        OR_SKIP(cudaEventElapsedTime(&startTime2, starts[1], starts[0]), NAME " failed to compare start times");
        OR_SKIP(cudaEventElapsedTime(&stopTime1, stops[0],  stops[1]),  NAME " failed to compare stop times");
        OR_SKIP(cudaEventElapsedTime(&stopTime2, stops[1],  stops[0]),  NAME " failed to compare stop times");
        startSum += std::max(startTime1, startTime2);
        stopSum += std::max(stopTime1, stopTime2);
  }

  state.counters["avg_start_spread"] = startSum/state.iterations();
  state.counters["avg_stop_spread"] = stopSum/state.iterations();

  for (auto src : srcs) {
    cudaFree(src);
  }
  for (auto dst : dsts) {
    cudaFree(dst);
  }
}

BENCHMARK(Comm_Duplex_Memcpy_GPUGPU)->SMALL_ARGS()->UseManualTime();
