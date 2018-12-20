#include <cuda_runtime.h>

#include "scope/init/init.hpp"
#include "scope/utils/utils.hpp"
#include "scope/init/flags.hpp"

#include "init/flags.hpp"
#include "utils/numa.hpp"
#include "init/numa.hpp"
#include "utils/cache_control.hpp"

#include "args.hpp"

#define NAME "Comm_ZeroCopy_GPUToGPU"

#define OR_SKIP(stmt) \
  if (PRINT_IF_ERROR(stmt)) { \
    state.SkipWithError(NAME); \
    return; \
}

typedef enum {
  READ,
  WRITE,
} AccessType;

static std::string to_string(const AccessType &a) {
  if (a == READ) {
    return "_Read";
  } else {
    return "_Write";
  }
}

template <typename write_t>
__global__ void gpu_write(write_t *ptr, const size_t bytes) {
  const size_t gx = blockIdx.x * blockDim.x + threadIdx.x;
  const size_t num_elems = bytes / sizeof(write_t);

  for (size_t i = gx; i < num_elems; i += gridDim.x * blockDim.x) {
    ptr[i] = 0;
  }
}


template <typename read_t>
__global__ void gpu_read(const read_t *ptr, const size_t bytes) {
  const size_t gx = blockIdx.x * blockDim.x + threadIdx.x;
  const size_t num_elems = bytes / sizeof(read_t);

  __shared__ int32_t s[256];
  int32_t t;

  for (size_t i = gx; i < num_elems; i += gridDim.x * blockDim.x) {
    t += ptr[i];
  }
  s[threadIdx.x] = t;
  (void) s[threadIdx.x];
}


auto Comm_ZeroCopy_GPUToGPU = [](benchmark::State &state, const int gpu0, const int gpu1, const AccessType access_type) {

  LOG(debug, "Entered {}", NAME);
  if (!has_cuda) {
    state.SkipWithError(NAME " no CUDA device found");
    return;
  }

  const size_t pageSize = page_size();
  const auto bytes   = 1ULL << static_cast<size_t>(state.range(0));
  void *ptr = nullptr;

  OR_SKIP(utils::cuda_reset_device(gpu0));
  OR_SKIP(utils::cuda_reset_device(gpu1));

  OR_SKIP(cudaSetDevice(gpu0));
  { \
    cudaError_t err = cudaDeviceEnablePeerAccess(gpu1, 0); \
    if (cudaErrorPeerAccessAlreadyEnabled != err) { \
      OR_SKIP(err); \
    } \
  }
  OR_SKIP(cudaSetDevice(gpu1));
  {
    cudaError_t err = cudaDeviceEnablePeerAccess(gpu0, 0);
    if (cudaErrorPeerAccessAlreadyEnabled != err) {
      OR_SKIP(err);
    }
  }

  if (READ == access_type) {
    OR_SKIP(cudaSetDevice(gpu0));
    OR_SKIP(cudaMalloc(&ptr, bytes));
    OR_SKIP(cudaMemset(ptr, 0, bytes));
    OR_SKIP(cudaSetDevice(gpu1));
  } else {
    OR_SKIP(cudaSetDevice(gpu1)); \
    OR_SKIP(cudaMalloc(&ptr, bytes)); \
    OR_SKIP(cudaMemset(ptr, 0, bytes)); \
    OR_SKIP(cudaSetDevice(gpu0)); \
  }
  defer(cudaFree(ptr));

  cudaEvent_t start, stop;
  OR_SKIP(cudaEventCreate(&start));
  OR_SKIP(cudaEventCreate(&stop));
  defer(cudaEventDestroy(start));
  defer(cudaEventDestroy(stop));

  for (auto _ : state) {
    OR_SKIP(cudaEventRecord(start));
    if (READ == access_type) {
      // READ: gpu1 reads from gpu0 (gpu0 is src, gpu1 is dst)
      gpu_read<int32_t><<<256, 256>>>(static_cast<int32_t*>(ptr), bytes);
    } else {
      // WRITE: gpu0 writes to gpu1 (gpu0 is src, gpu1 is dst)
      gpu_write<int32_t><<<256, 256>>>(static_cast<int32_t*>(ptr), bytes);
    }
    OR_SKIP(cudaEventRecord(stop));
    OR_SKIP(cudaEventSynchronize(stop));

    float millis;
    OR_SKIP(cudaEventElapsedTime(&millis, start, stop));
    state.SetIterationTime(millis / 1000);
  }

  state.SetBytesProcessed(int64_t(state.iterations()) * int64_t(bytes));
  state.counters["bytes"] = bytes;
  state.counters["gpu0"] = gpu0;
  state.counters["gpu1"] = gpu1;
};

static void registerer() {

  LOG(debug, "Registering {} benchmarks", NAME);
  std::string name;
  for (auto workload : {READ, WRITE}) {
      for (auto src_gpu : unique_cuda_device_ids()) {
        for (auto dst_gpu : unique_cuda_device_ids()) {
          if (src_gpu < dst_gpu) {

            int s2d = false;
            int d2s = false;
            if (!PRINT_IF_ERROR(cudaDeviceCanAccessPeer(&s2d, src_gpu, dst_gpu))
             && !PRINT_IF_ERROR(cudaDeviceCanAccessPeer(&d2s, dst_gpu, src_gpu))) {
              if (s2d && d2s) {
                std::string name(NAME);
                name += to_string(workload)
                    + "/" + std::to_string(src_gpu)
                    + "/" + std::to_string(dst_gpu);
                benchmark::RegisterBenchmark(name.c_str(), Comm_ZeroCopy_GPUToGPU,
                src_gpu, dst_gpu, workload)->ARGS()->UseManualTime();
              } else {
                LOG(debug, "{} can't run on devices {} {}: peer access not available", NAME, src_gpu, dst_gpu);
              }
            }
          }
        }
      }
  }
}

SCOPE_REGISTER_AFTER_INIT(registerer, NAME);