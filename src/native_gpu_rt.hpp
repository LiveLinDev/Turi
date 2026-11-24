#pragma once

#include <string>

namespace veloz::native {

namespace gpu_rt_parts {

inline constexpr const char* kGpuHeader = R"c(
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define TZ_CL_CALL __stdcall
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#define TZ_CL_CALL
#endif

typedef struct tz_cl_platform_id* tz_cl_platform_id;
typedef struct tz_cl_device_id* tz_cl_device_id;
typedef struct tz_cl_context* tz_cl_context;
typedef struct tz_cl_command_queue* tz_cl_command_queue;
typedef struct tz_cl_program* tz_cl_program;
typedef struct tz_cl_kernel* tz_cl_kernel;
typedef struct tz_cl_mem* tz_cl_mem;
typedef struct tz_cl_event* tz_cl_event;

typedef int32_t tz_cl_int;
typedef uint32_t tz_cl_uint;
typedef uint64_t tz_cl_ulong;
typedef uint64_t tz_cl_bitfield;
typedef tz_cl_uint tz_cl_bool;

#define TZ_CL_SUCCESS 0
#define TZ_CL_TRUE 1u
#define TZ_CL_FALSE 0u
#define TZ_CL_DEVICE_TYPE_GPU (1ULL << 2)
#define TZ_CL_DEVICE_NAME 0x102Bu
#define TZ_CL_DRIVER_VERSION 0x102Du
#define TZ_CL_DEVICE_MAX_COMPUTE_UNITS 0x1002u
#define TZ_CL_DEVICE_GLOBAL_MEM_SIZE 0x101Fu
#define TZ_CL_DEVICE_LOCAL_MEM_SIZE 0x1023u
#define TZ_CL_QUEUE_PROFILING_ENABLE (1ULL << 1)
#define TZ_CL_MEM_READ_WRITE (1ULL << 0)
#define TZ_CL_MEM_WRITE_ONLY (1ULL << 1)
#define TZ_CL_MEM_READ_ONLY (1ULL << 2)
#define TZ_CL_MEM_ALLOC_HOST_PTR (1ULL << 4)
#define TZ_CL_MEM_COPY_HOST_PTR (1ULL << 5)
#define TZ_CL_MAP_READ (1ULL << 0)
#define TZ_CL_MAP_WRITE (1ULL << 1)
#define TZ_CL_PROGRAM_BINARY_SIZES 0x1165u
#define TZ_CL_PROGRAM_BINARIES 0x1166u
#define TZ_CL_PROGRAM_BUILD_LOG 0x1183u
#define TZ_CL_PROFILING_COMMAND_START 0x1282u
#define TZ_CL_PROFILING_COMMAND_END 0x1283u

#define TZ_GPU_BK_NONE 0
#define TZ_GPU_BK_OPENCL 1
#define TZ_GPU_BK_CUDA 2
#define TZ_GPU_BK_METAL 3

static _Thread_local char tz_gpu_tls_err[512];
static _Thread_local char tz_gpu_tls_name[256];
static _Thread_local char tz_gpu_tls_backend[32];
static _Thread_local char tz_gpu_tls_backends[128];
static _Thread_local int64_t tz_gpu_last_cache_hit = 0;

static inline double tz_gpu_now_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER cur;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cur);
    return (double)cur.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
#endif
}

static inline void tz_gpu_set_err(const void** out_ptr, int64_t* out_len, const char* msg) {
    if (!msg || !*msg) {
        if (out_ptr) *out_ptr = NULL;
        if (out_len) *out_len = 0;
        return;
    }
    size_t n = strlen(msg);
    if (n >= sizeof(tz_gpu_tls_err)) n = sizeof(tz_gpu_tls_err) - 1u;
    memcpy(tz_gpu_tls_err, msg, n);
    tz_gpu_tls_err[n] = '\0';
    if (out_ptr) *out_ptr = tz_gpu_tls_err;
    if (out_len) *out_len = (int64_t)n;
}
)c";

inline constexpr const char* kGpuLoader = R"c(
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clGetPlatformIDs)(tz_cl_uint, tz_cl_platform_id*, tz_cl_uint*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clGetDeviceIDs)(tz_cl_platform_id, tz_cl_bitfield, tz_cl_uint, tz_cl_device_id*, tz_cl_uint*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clGetDeviceInfo)(tz_cl_device_id, tz_cl_uint, size_t, void*, size_t*);
typedef tz_cl_context (TZ_CL_CALL *tz_pfn_clCreateContext)(const intptr_t*, tz_cl_uint, const tz_cl_device_id*, void*, void*, tz_cl_int*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clReleaseContext)(tz_cl_context);
typedef tz_cl_command_queue (TZ_CL_CALL *tz_pfn_clCreateCommandQueue)(tz_cl_context, tz_cl_device_id, tz_cl_bitfield, tz_cl_int*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clReleaseCommandQueue)(tz_cl_command_queue);
typedef tz_cl_program (TZ_CL_CALL *tz_pfn_clCreateProgramWithSource)(tz_cl_context, tz_cl_uint, const char**, const size_t*, tz_cl_int*);
typedef tz_cl_program (TZ_CL_CALL *tz_pfn_clCreateProgramWithBinary)(tz_cl_context, tz_cl_uint, const tz_cl_device_id*, const size_t*, const unsigned char**, tz_cl_int*, tz_cl_int*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clGetProgramInfo)(tz_cl_program, tz_cl_uint, size_t, void*, size_t*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clBuildProgram)(tz_cl_program, tz_cl_uint, const tz_cl_device_id*, const char*, void*, void*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clGetProgramBuildInfo)(tz_cl_program, tz_cl_device_id, tz_cl_uint, size_t, void*, size_t*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clReleaseProgram)(tz_cl_program);
typedef tz_cl_kernel (TZ_CL_CALL *tz_pfn_clCreateKernel)(tz_cl_program, const char*, tz_cl_int*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clReleaseKernel)(tz_cl_kernel);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clSetKernelArg)(tz_cl_kernel, tz_cl_uint, size_t, const void*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clEnqueueNDRangeKernel)(tz_cl_command_queue, tz_cl_kernel, tz_cl_uint, const size_t*, const size_t*, const size_t*, tz_cl_uint, const tz_cl_event*, tz_cl_event*);
typedef tz_cl_mem (TZ_CL_CALL *tz_pfn_clCreateBuffer)(tz_cl_context, tz_cl_bitfield, size_t, void*, tz_cl_int*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clEnqueueWriteBuffer)(tz_cl_command_queue, tz_cl_mem, tz_cl_bool, size_t, size_t, const void*, tz_cl_uint, const tz_cl_event*, tz_cl_event*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clEnqueueReadBuffer)(tz_cl_command_queue, tz_cl_mem, tz_cl_bool, size_t, size_t, void*, tz_cl_uint, const tz_cl_event*, tz_cl_event*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clReleaseMemObject)(tz_cl_mem);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clFinish)(tz_cl_command_queue);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clWaitForEvents)(tz_cl_uint, const tz_cl_event*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clGetEventProfilingInfo)(tz_cl_event, tz_cl_uint, size_t, void*, size_t*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clReleaseEvent)(tz_cl_event);
typedef void* (TZ_CL_CALL *tz_pfn_clEnqueueMapBuffer)(tz_cl_command_queue, tz_cl_mem, tz_cl_bool, tz_cl_bitfield, size_t, size_t, tz_cl_uint, const tz_cl_event*, tz_cl_event*, tz_cl_int*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clEnqueueUnmapMemObject)(tz_cl_command_queue, tz_cl_mem, void*, tz_cl_uint, const tz_cl_event*, tz_cl_event*);
typedef tz_cl_int (TZ_CL_CALL *tz_pfn_clEnqueueCopyBuffer)(tz_cl_command_queue, tz_cl_mem, tz_cl_mem, size_t, size_t, size_t, tz_cl_uint, const tz_cl_event*, tz_cl_event*);

static struct {
    int attempted;
    int ok;
    tz_pfn_clGetPlatformIDs get_platforms;
    tz_pfn_clGetDeviceIDs get_devices;
    tz_pfn_clGetDeviceInfo get_device_info;
    tz_pfn_clCreateContext create_ctx;
    tz_pfn_clReleaseContext release_ctx;
    tz_pfn_clCreateCommandQueue create_queue;
    tz_pfn_clReleaseCommandQueue release_queue;
    tz_pfn_clCreateProgramWithSource create_prog;
    tz_pfn_clCreateProgramWithBinary create_prog_bin;
    tz_pfn_clGetProgramInfo get_prog_info;
    tz_pfn_clBuildProgram build_prog;
    tz_pfn_clGetProgramBuildInfo get_build_info;
    tz_pfn_clReleaseProgram release_prog;
    tz_pfn_clCreateKernel create_kernel;
    tz_pfn_clReleaseKernel release_kernel;
    tz_pfn_clSetKernelArg set_arg;
    tz_pfn_clEnqueueNDRangeKernel enqueue_nd;
    tz_pfn_clCreateBuffer create_buf;
    tz_pfn_clEnqueueWriteBuffer write_buf;
    tz_pfn_clEnqueueReadBuffer read_buf;
    tz_pfn_clReleaseMemObject release_buf;
    tz_pfn_clFinish finish;
    tz_pfn_clWaitForEvents wait_events;
    tz_pfn_clGetEventProfilingInfo prof_info;
    tz_pfn_clReleaseEvent release_event;
    tz_pfn_clEnqueueMapBuffer map_buf;
    tz_pfn_clEnqueueUnmapMemObject unmap_mem;
    tz_pfn_clEnqueueCopyBuffer copy_buf;
} tz_cl = {0};

static inline void* tz_gpu_opt_sym(void* mod, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)mod, name);
#else
    return dlsym(mod, name);
#endif
}

static int tz_gpu_load_api(void) {
    if (tz_cl.attempted) return tz_cl.ok;
    tz_cl.attempted = 1;
#ifdef _WIN32
    HMODULE mod = LoadLibraryA("OpenCL.dll");
    if (!mod) return 0;
#define TZ_LOAD_CL(field, sym) \
    tz_cl.field = (tz_pfn_##sym)(void*)GetProcAddress(mod, #sym); \
    if (!tz_cl.field) return 0;
#else
    void* mod = dlopen("libOpenCL.so.1", RTLD_LAZY);
    if (!mod) mod = dlopen("libOpenCL.so", RTLD_LAZY);
    if (!mod) return 0;
#define TZ_LOAD_CL(field, sym) \
    tz_cl.field = (tz_pfn_##sym)dlsym(mod, #sym); \
    if (!tz_cl.field) return 0;
#endif
    TZ_LOAD_CL(get_platforms, clGetPlatformIDs)
    TZ_LOAD_CL(get_devices, clGetDeviceIDs)
    TZ_LOAD_CL(get_device_info, clGetDeviceInfo)
    TZ_LOAD_CL(create_ctx, clCreateContext)
    TZ_LOAD_CL(release_ctx, clReleaseContext)
    TZ_LOAD_CL(create_queue, clCreateCommandQueue)
    TZ_LOAD_CL(release_queue, clReleaseCommandQueue)
    TZ_LOAD_CL(create_prog, clCreateProgramWithSource)
    tz_cl.create_prog_bin = (tz_pfn_clCreateProgramWithBinary)tz_gpu_opt_sym((void*)mod, "clCreateProgramWithBinary");
    tz_cl.get_prog_info = (tz_pfn_clGetProgramInfo)tz_gpu_opt_sym((void*)mod, "clGetProgramInfo");
    TZ_LOAD_CL(build_prog, clBuildProgram)
    TZ_LOAD_CL(get_build_info, clGetProgramBuildInfo)
    TZ_LOAD_CL(release_prog, clReleaseProgram)
    TZ_LOAD_CL(create_kernel, clCreateKernel)
    TZ_LOAD_CL(release_kernel, clReleaseKernel)
    TZ_LOAD_CL(set_arg, clSetKernelArg)
    TZ_LOAD_CL(enqueue_nd, clEnqueueNDRangeKernel)
    TZ_LOAD_CL(create_buf, clCreateBuffer)
    TZ_LOAD_CL(write_buf, clEnqueueWriteBuffer)
    TZ_LOAD_CL(read_buf, clEnqueueReadBuffer)
    TZ_LOAD_CL(release_buf, clReleaseMemObject)
    TZ_LOAD_CL(finish, clFinish)
    TZ_LOAD_CL(wait_events, clWaitForEvents)
    TZ_LOAD_CL(prof_info, clGetEventProfilingInfo)
    TZ_LOAD_CL(release_event, clReleaseEvent)
    tz_cl.map_buf = (tz_pfn_clEnqueueMapBuffer)tz_gpu_opt_sym((void*)mod, "clEnqueueMapBuffer");
    tz_cl.unmap_mem = (tz_pfn_clEnqueueUnmapMemObject)tz_gpu_opt_sym((void*)mod, "clEnqueueUnmapMemObject");
    tz_cl.copy_buf = (tz_pfn_clEnqueueCopyBuffer)tz_gpu_opt_sym((void*)mod, "clEnqueueCopyBuffer");
#undef TZ_LOAD_CL
    tz_cl.ok = 1;
    return 1;
}

static int tz_gpu_pick_device(tz_cl_device_id* out_dev, char* name_buf, size_t name_cap) {
    if (!tz_gpu_load_api()) return 0;
    tz_cl_uint np = 0;
    if (tz_cl.get_platforms(0, NULL, &np) != TZ_CL_SUCCESS || np == 0u) return 0;
    if (np > 16u) np = 16u;
    tz_cl_platform_id plats[16];
    if (tz_cl.get_platforms(np, plats, NULL) != TZ_CL_SUCCESS) return 0;

    tz_cl_device_id best_dev = NULL;
    tz_cl_uint best_cu = 0;
    tz_cl_ulong best_mem = 0;
    for (tz_cl_uint pi = 0; pi < np; ++pi) {
        tz_cl_uint nd = 0;
        if (tz_cl.get_devices(plats[pi], TZ_CL_DEVICE_TYPE_GPU, 0, NULL, &nd) != TZ_CL_SUCCESS || nd == 0u) continue;
        if (nd > 16u) nd = 16u;
        tz_cl_device_id devs[16];
        if (tz_cl.get_devices(plats[pi], TZ_CL_DEVICE_TYPE_GPU, nd, devs, NULL) != TZ_CL_SUCCESS) continue;
        for (tz_cl_uint di = 0; di < nd; ++di) {
            tz_cl_uint cu = 0;
            tz_cl_ulong mem = 0;
            tz_cl.get_device_info(devs[di], TZ_CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cu), &cu, NULL);
            tz_cl.get_device_info(devs[di], TZ_CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem), &mem, NULL);
            if (!best_dev || cu > best_cu || (cu == best_cu && mem > best_mem)) {
                best_dev = devs[di];
                best_cu = cu;
                best_mem = mem;
            }
        }
    }
    if (!best_dev) return 0;
    if (out_dev) *out_dev = best_dev;
    if (name_buf && name_cap > 0u) {
        name_buf[0] = '\0';
        tz_cl.get_device_info(best_dev, TZ_CL_DEVICE_NAME, name_cap - 1u, name_buf, NULL);
        name_buf[name_cap - 1u] = '\0';
    }
    return 1;
}
)c";

inline constexpr const char* kGpuKernels = R"c(
static const char* tz_gpu_cl_src =
"__kernel void tz_saxpy_f32(float alpha, __global const float* x, __global float* y, ulong n) {\n"
"    ulong gid = get_global_id(0);\n"
"    if (gid < n) {\n"
"        y[gid] = alpha * x[gid] + y[gid];\n"
"    }\n"
"}\n"
"__kernel void tz_relu_f32(__global const float* x, __global float* y, ulong n) {\n"
"    ulong gid = get_global_id(0);\n"
"    if (gid < n) {\n"
"        float v = x[gid];\n"
"        y[gid] = v > 0.0f ? v : 0.0f;\n"
"    }\n"
"}\n"
"__kernel void tz_vec_add_f32(__global const float* a, __global const float* b, __global float* out, ulong n) {\n"
"    ulong gid = get_global_id(0);\n"
"    if (gid < n) {\n"
"        out[gid] = a[gid] + b[gid];\n"
"    }\n"
"}\n"
"__kernel void tz_vec_mul_f32(__global const float* a, __global const float* b, __global float* out, ulong n) {\n"
"    ulong gid = get_global_id(0);\n"
"    if (gid < n) {\n"
"        out[gid] = a[gid] * b[gid];\n"
"    }\n"
"}\n"
"__kernel void tz_saxpy_relu_f32(float alpha, __global const float* x, __global float* y, ulong n) {\n"
"    ulong gid = get_global_id(0);\n"
"    if (gid < n) {\n"
"        float v = alpha * x[gid] + y[gid];\n"
"        y[gid] = v > 0.0f ? v : 0.0f;\n"
"    }\n"
"}\n";
)c";

inline constexpr const char* kGpuKernelsMatmul = R"c(
static const char* tz_gpu_cl_src2 =
"__kernel void tz_matmul_f32(__global const float* a, __global const float* b, __global float* c,\n"
"                            uint m, uint k, uint n) {\n"
"    __local float As[16][16];\n"
"    __local float Bs[16][16];\n"
"    uint tx = get_local_id(0);\n"
"    uint ty = get_local_id(1);\n"
"    uint col = get_group_id(0) * 16u + tx;\n"
"    uint row = get_group_id(1) * 16u + ty;\n"
"    float acc = 0.0f;\n"
"    uint tiles = (k + 15u) / 16u;\n"
"    for (uint t = 0u; t < tiles; ++t) {\n"
"        uint a_col = t * 16u + tx;\n"
"        uint b_row = t * 16u + ty;\n"
"        As[ty][tx] = (row < m && a_col < k) ? a[(ulong)row * (ulong)k + (ulong)a_col] : 0.0f;\n"
"        Bs[ty][tx] = (b_row < k && col < n) ? b[(ulong)b_row * (ulong)n + (ulong)col] : 0.0f;\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"        for (uint p = 0u; p < 16u; ++p) acc += As[ty][p] * Bs[p][tx];\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"    }\n"
"    if (row < m && col < n) c[(ulong)row * (ulong)n + (ulong)col] = acc;\n"
"}\n"
"__kernel void tz_matmul_relu_f32(__global const float* a, __global const float* b, __global float* c,\n"
"                                 uint m, uint k, uint n) {\n"
"    __local float As[16][16];\n"
"    __local float Bs[16][16];\n"
"    uint tx = get_local_id(0);\n"
"    uint ty = get_local_id(1);\n"
"    uint col = get_group_id(0) * 16u + tx;\n"
"    uint row = get_group_id(1) * 16u + ty;\n"
"    float acc = 0.0f;\n"
"    uint tiles = (k + 15u) / 16u;\n"
"    for (uint t = 0u; t < tiles; ++t) {\n"
"        uint a_col = t * 16u + tx;\n"
"        uint b_row = t * 16u + ty;\n"
"        As[ty][tx] = (row < m && a_col < k) ? a[(ulong)row * (ulong)k + (ulong)a_col] : 0.0f;\n"
"        Bs[ty][tx] = (b_row < k && col < n) ? b[(ulong)b_row * (ulong)n + (ulong)col] : 0.0f;\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"        for (uint p = 0u; p < 16u; ++p) acc += As[ty][p] * Bs[p][tx];\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"    }\n"
"    if (row < m && col < n) c[(ulong)row * (ulong)n + (ulong)col] = acc > 0.0f ? acc : 0.0f;\n"
"}\n"
"__kernel void tz_matmul_f32_64(__global const float* a, __global const float* b, __global float* c,\n"
"                               uint m, uint k, uint n) {\n"
"    __local float As[64][65];\n"
"    __local float Bs[64][65];\n"
"    uint lx = get_local_id(0);\n"
"    uint ly = get_local_id(1);\n"
"    uint colBase = get_group_id(0) * 64u;\n"
"    uint rowBase = get_group_id(1) * 64u;\n"
"    float acc[4][4];\n"
"    for (uint r = 0u; r < 4u; ++r) for (uint q = 0u; q < 4u; ++q) acc[r][q] = 0.0f;\n"
"    uint tiles = (k + 63u) / 64u;\n"
"    for (uint t = 0u; t < tiles; ++t) {\n"
"        uint kBase = t * 64u;\n"
"        for (uint r = 0u; r < 4u; ++r) {\n"
"            uint lr = ly + r * 16u;\n"
"            uint gr = rowBase + lr;\n"
"            uint br = kBase + lr;\n"
"            for (uint q = 0u; q < 4u; ++q) {\n"
"                uint lc = lx + q * 16u;\n"
"                uint ac = kBase + lc;\n"
"                uint bc = colBase + lc;\n"
"                As[lr][lc] = (gr < m && ac < k) ? a[(ulong)gr * (ulong)k + (ulong)ac] : 0.0f;\n"
"                Bs[lr][lc] = (br < k && bc < n) ? b[(ulong)br * (ulong)n + (ulong)bc] : 0.0f;\n"
"            }\n"
"        }\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"        for (uint p = 0u; p < 64u; ++p) {\n"
"            float ar[4], bc[4];\n"
"            for (uint r = 0u; r < 4u; ++r) ar[r] = As[ly + r * 16u][p];\n"
"            for (uint q = 0u; q < 4u; ++q) bc[q] = Bs[p][lx + q * 16u];\n"
"            for (uint r = 0u; r < 4u; ++r) for (uint q = 0u; q < 4u; ++q) acc[r][q] += ar[r] * bc[q];\n"
"        }\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"    }\n"
"    for (uint r = 0u; r < 4u; ++r) {\n"
"        uint gr = rowBase + ly + r * 16u;\n"
"        if (gr < m) {\n"
"            for (uint q = 0u; q < 4u; ++q) {\n"
"                uint gc = colBase + lx + q * 16u;\n"
"                if (gc < n) c[(ulong)gr * (ulong)n + (ulong)gc] = acc[r][q];\n"
"            }\n"
"        }\n"
"    }\n"
"}\n"
"__kernel void tz_matmul_relu_f32_64(__global const float* a, __global const float* b, __global float* c,\n"
"                                    uint m, uint k, uint n) {\n"
"    __local float As[64][65];\n"
"    __local float Bs[64][65];\n"
"    uint lx = get_local_id(0);\n"
"    uint ly = get_local_id(1);\n"
"    uint colBase = get_group_id(0) * 64u;\n"
"    uint rowBase = get_group_id(1) * 64u;\n"
"    float acc[4][4];\n"
"    for (uint r = 0u; r < 4u; ++r) for (uint q = 0u; q < 4u; ++q) acc[r][q] = 0.0f;\n"
"    uint tiles = (k + 63u) / 64u;\n"
"    for (uint t = 0u; t < tiles; ++t) {\n"
"        uint kBase = t * 64u;\n"
"        for (uint r = 0u; r < 4u; ++r) {\n"
"            uint lr = ly + r * 16u;\n"
"            uint gr = rowBase + lr;\n"
"            uint br = kBase + lr;\n"
"            for (uint q = 0u; q < 4u; ++q) {\n"
"                uint lc = lx + q * 16u;\n"
"                uint ac = kBase + lc;\n"
"                uint bc = colBase + lc;\n"
"                As[lr][lc] = (gr < m && ac < k) ? a[(ulong)gr * (ulong)k + (ulong)ac] : 0.0f;\n"
"                Bs[lr][lc] = (br < k && bc < n) ? b[(ulong)br * (ulong)n + (ulong)bc] : 0.0f;\n"
"            }\n"
"        }\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"        for (uint p = 0u; p < 64u; ++p) {\n"
"            float ar[4], bc[4];\n"
"            for (uint r = 0u; r < 4u; ++r) ar[r] = As[ly + r * 16u][p];\n"
"            for (uint q = 0u; q < 4u; ++q) bc[q] = Bs[p][lx + q * 16u];\n"
"            for (uint r = 0u; r < 4u; ++r) for (uint q = 0u; q < 4u; ++q) acc[r][q] += ar[r] * bc[q];\n"
"        }\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"    }\n"
"    for (uint r = 0u; r < 4u; ++r) {\n"
"        uint gr = rowBase + ly + r * 16u;\n"
"        if (gr < m) {\n"
"            for (uint q = 0u; q < 4u; ++q) {\n"
"                uint gc = colBase + lx + q * 16u;\n"
"                if (gc < n) {\n"
"                    float v = acc[r][q];\n"
"                    c[(ulong)gr * (ulong)n + (ulong)gc] = v > 0.0f ? v : 0.0f;\n"
"                }\n"
"            }\n"
"        }\n"
"    }\n"
"}\n";
)c";

inline constexpr const char* kGpuCache = R"c(
static uint64_t tz_fnv1a64(uint64_t h, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0u; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void tz_gpu_cache_path(tz_cl_device_id dev, char* out_path, size_t cap) {
    char dname[256];
    char dver[256];
    dname[0] = '\0'; dver[0] = '\0';
    tz_cl.get_device_info(dev, TZ_CL_DEVICE_NAME, sizeof(dname) - 1u, dname, NULL);
    tz_cl.get_device_info(dev, TZ_CL_DRIVER_VERSION, sizeof(dver) - 1u, dver, NULL);
    dname[sizeof(dname) - 1u] = '\0';
    dver[sizeof(dver) - 1u] = '\0';
    uint64_t h = 14695981039346656037ULL;
    h = tz_fnv1a64(h, dname, strlen(dname));
    h = tz_fnv1a64(h, "|", 1u);
    h = tz_fnv1a64(h, dver, strlen(dver));
    h = tz_fnv1a64(h, "|", 1u);
    h = tz_fnv1a64(h, tz_gpu_cl_src, strlen(tz_gpu_cl_src));
    h = tz_fnv1a64(h, tz_gpu_cl_src2, strlen(tz_gpu_cl_src2));
    h = tz_fnv1a64(h, "|v3", 3u);

    char dir[512];
#ifdef _WIN32
    const char* root = getenv("LOCALAPPDATA");
    if (!root || !*root) root = getenv("TEMP");
    if (!root || !*root) root = ".";
    snprintf(dir, sizeof(dir), "%s\\turi-ocl-cache", root);
    CreateDirectoryA(dir, NULL);
    snprintf(out_path, cap, "%s\\turi_ocl_%016llx.bin", dir, (unsigned long long)h);
#else
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) {
        snprintf(dir, sizeof(dir), "%s/turi-ocl-cache", xdg);
        mkdir(dir, 0755);
    } else {
        const char* home = getenv("HOME");
        if (home && *home) {
            char parent[480];
            snprintf(parent, sizeof(parent), "%s/.cache", home);
            mkdir(parent, 0755);
            snprintf(dir, sizeof(dir), "%s/turi-ocl-cache", parent);
            mkdir(dir, 0755);
        } else {
            snprintf(dir, sizeof(dir), "/tmp/turi-ocl-cache");
            mkdir(dir, 0755);
        }
    }
    snprintf(out_path, cap, "%s/turi_ocl_%016llx.bin", dir, (unsigned long long)h);
#endif
}

static tz_cl_program tz_gpu_load_cached_prog(tz_cl_context cl_ctx, tz_cl_device_id dev, const char* path) {
    if (!tz_cl.create_prog_bin) return NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char hdr[14];
    if (fread(hdr, 1u, sizeof(hdr), f) != sizeof(hdr) || memcmp(hdr, "TZCLB1", 6u) != 0) {
        fclose(f);
        return NULL;
    }
    uint64_t sz64 = 0;
    memcpy(&sz64, hdr + 6, sizeof(uint64_t));
    if (sz64 == 0u || sz64 > (64ULL * 1024ULL * 1024ULL)) { fclose(f); return NULL; }
    size_t sz = (size_t)sz64;
    unsigned char* buf = (unsigned char*)malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1u, sz, f) != sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    const unsigned char* ptr = buf;
    tz_cl_int bstat = TZ_CL_SUCCESS, rc = TZ_CL_SUCCESS;
    tz_cl_program p = tz_cl.create_prog_bin(cl_ctx, 1u, &dev, &sz, &ptr, &bstat, &rc);
    free(buf);
    if (!p || rc != TZ_CL_SUCCESS || bstat != TZ_CL_SUCCESS) {
        if (p) tz_cl.release_prog(p);
        return NULL;
    }
    if (tz_cl.build_prog(p, 1u, &dev, "", NULL, NULL) != TZ_CL_SUCCESS) {
        tz_cl.release_prog(p);
        return NULL;
    }
    return p;
}

static void tz_gpu_save_cached_prog(tz_cl_program p, const char* path) {
    if (!tz_cl.get_prog_info) return;
    size_t bsz = 0;
    if (tz_cl.get_prog_info(p, TZ_CL_PROGRAM_BINARY_SIZES, sizeof(bsz), &bsz, NULL) != TZ_CL_SUCCESS ||
        bsz == 0u || bsz > (64u * 1024u * 1024u)) {
        return;
    }
    unsigned char* buf = (unsigned char*)malloc(bsz);
    if (!buf) return;
    unsigned char* ptrs[1] = {buf};
    if (tz_cl.get_prog_info(p, TZ_CL_PROGRAM_BINARIES, sizeof(ptrs), ptrs, NULL) != TZ_CL_SUCCESS) {
        free(buf);
        return;
    }
    char tmp_path[640];
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%lu", path, pid);
    FILE* f = fopen(tmp_path, "wb");
    if (!f) { free(buf); return; }
    unsigned char hdr[14];
    memcpy(hdr, "TZCLB1", 6u);
    uint64_t sz64 = (uint64_t)bsz;
    memcpy(hdr + 6, &sz64, sizeof(uint64_t));
    size_t w1 = fwrite(hdr, 1u, sizeof(hdr), f);
    size_t w2 = fwrite(buf, 1u, bsz, f);
    fclose(f);
    free(buf);
    if (w1 == sizeof(hdr) && w2 == bsz) {
#ifdef _WIN32
        if (!MoveFileExA(tmp_path, path, MOVEFILE_REPLACE_EXISTING)) remove(tmp_path);
#else
        if (rename(tmp_path, path) != 0) remove(tmp_path);
#endif
    } else {
        remove(tmp_path);
    }
}
)c";

inline constexpr const char* kGpuMultiBackend = R"c(
typedef int tz_cu_res;
typedef int tz_cu_dev;
typedef uint64_t tz_cu_ptr;
typedef void* tz_cu_ctx;
typedef void* tz_cu_mod;
typedef void* tz_cu_fn;

#ifndef TZ_CU_CALL
#ifdef _WIN32
#define TZ_CU_CALL __stdcall
#else
#define TZ_CU_CALL
#endif
#endif

typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuInit)(unsigned int);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuDeviceGetCount)(int*);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuDeviceGet)(tz_cu_dev*, int);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuDeviceGetName)(char*, int, tz_cu_dev);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuCtxCreate)(tz_cu_ctx*, unsigned int, tz_cu_dev);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuCtxDestroy)(tz_cu_ctx);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuModuleLoadData)(tz_cu_mod*, const void*);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuModuleUnload)(tz_cu_mod);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuModuleGetFunction)(tz_cu_fn*, tz_cu_mod, const char*);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuMemAlloc)(tz_cu_ptr*, size_t);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuMemFree)(tz_cu_ptr);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuMemcpyHtoD)(tz_cu_ptr, const void*, size_t);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuMemcpyDtoH)(void*, tz_cu_ptr, size_t);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuLaunchKernel)(tz_cu_fn, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, void*, void**, void**);
typedef tz_cu_res (TZ_CU_CALL *tz_pfn_cuCtxSynchronize)(void);

static struct {
    int attempted;
    int ok;
    tz_pfn_cuInit init;
    tz_pfn_cuDeviceGetCount dev_count;
    tz_pfn_cuDeviceGet dev_get;
    tz_pfn_cuDeviceGetName dev_name;
    tz_pfn_cuCtxCreate ctx_create;
    tz_pfn_cuCtxDestroy ctx_destroy;
    tz_pfn_cuModuleLoadData mod_load;
    tz_pfn_cuModuleUnload mod_unload;
    tz_pfn_cuModuleGetFunction mod_fn;
    tz_pfn_cuMemAlloc mem_alloc;
    tz_pfn_cuMemFree mem_free;
    tz_pfn_cuMemcpyHtoD memcpy_h2d;
    tz_pfn_cuMemcpyDtoH memcpy_d2h;
    tz_pfn_cuLaunchKernel launch;
    tz_pfn_cuCtxSynchronize sync;
} tz_cu = {0};

static const char* tz_gpu_ptx_src =
".version 7.0\n"
".target sm_60\n"
".address_size 64\n"
".visible .entry tz_saxpy_f32(.param .f32 p_alpha, .param .u64 p_x, .param .u64 p_y, .param .u64 p_n) {\n"
"  .reg .pred %p<2>; .reg .f32 %f<6>; .reg .u32 %r<5>; .reg .u64 %rd<8>;\n"
"  ld.param.f32 %f1, [p_alpha]; ld.param.u64 %rd1, [p_x]; ld.param.u64 %rd2, [p_y]; ld.param.u64 %rd3, [p_n];\n"
"  mov.u32 %r1, %ctaid.x; mov.u32 %r2, %ntid.x; mov.u32 %r3, %tid.x;\n"
"  mad.lo.u32 %r4, %r1, %r2, %r3; cvt.u64.u32 %rd4, %r4;\n"
"  setp.ge.u64 %p1, %rd4, %rd3; @%p1 bra L_DONE;\n"
"  shl.b64 %rd5, %rd4, 2; add.u64 %rd6, %rd1, %rd5; add.u64 %rd7, %rd2, %rd5;\n"
"  ld.global.f32 %f2, [%rd6]; ld.global.f32 %f3, [%rd7];\n"
"  mul.rn.f32 %f4, %f1, %f2; add.rn.f32 %f5, %f4, %f3;\n"
"  st.global.f32 [%rd7], %f5;\n"
"L_DONE: ret;\n}\n"
".visible .entry tz_saxpy_relu_f32(.param .f32 p_alpha, .param .u64 p_x, .param .u64 p_y, .param .u64 p_n) {\n"
"  .reg .pred %p<2>; .reg .f32 %f<7>; .reg .u32 %r<5>; .reg .u64 %rd<8>;\n"
"  ld.param.f32 %f1, [p_alpha]; ld.param.u64 %rd1, [p_x]; ld.param.u64 %rd2, [p_y]; ld.param.u64 %rd3, [p_n];\n"
"  mov.u32 %r1, %ctaid.x; mov.u32 %r2, %ntid.x; mov.u32 %r3, %tid.x;\n"
"  mad.lo.u32 %r4, %r1, %r2, %r3; cvt.u64.u32 %rd4, %r4;\n"
"  setp.ge.u64 %p1, %rd4, %rd3; @%p1 bra L_DONE;\n"
"  shl.b64 %rd5, %rd4, 2; add.u64 %rd6, %rd1, %rd5; add.u64 %rd7, %rd2, %rd5;\n"
"  ld.global.f32 %f2, [%rd6]; ld.global.f32 %f3, [%rd7];\n"
"  mul.rn.f32 %f4, %f1, %f2; add.rn.f32 %f5, %f4, %f3; max.f32 %f6, %f5, 0f00000000;\n"
"  st.global.f32 [%rd7], %f6;\n"
"L_DONE: ret;\n}\n"
".visible .entry tz_relu_f32(.param .u64 p_x, .param .u64 p_y, .param .u64 p_n) {\n"
"  .reg .pred %p<2>; .reg .f32 %f<3>; .reg .u32 %r<5>; .reg .u64 %rd<8>;\n"
"  ld.param.u64 %rd1, [p_x]; ld.param.u64 %rd2, [p_y]; ld.param.u64 %rd3, [p_n];\n"
"  mov.u32 %r1, %ctaid.x; mov.u32 %r2, %ntid.x; mov.u32 %r3, %tid.x;\n"
"  mad.lo.u32 %r4, %r1, %r2, %r3; cvt.u64.u32 %rd4, %r4;\n"
"  setp.ge.u64 %p1, %rd4, %rd3; @%p1 bra L_DONE;\n"
"  shl.b64 %rd5, %rd4, 2; add.u64 %rd6, %rd1, %rd5; add.u64 %rd7, %rd2, %rd5;\n"
"  ld.global.f32 %f1, [%rd6]; max.f32 %f2, %f1, 0f00000000;\n"
"  st.global.f32 [%rd7], %f2;\n"
"L_DONE: ret;\n}\n"
".visible .entry tz_vec_add_f32(.param .u64 p_a, .param .u64 p_b, .param .u64 p_o, .param .u64 p_n) {\n"
"  .reg .pred %p<2>; .reg .f32 %f<4>; .reg .u32 %r<5>; .reg .u64 %rd<10>;\n"
"  ld.param.u64 %rd1, [p_a]; ld.param.u64 %rd2, [p_b]; ld.param.u64 %rd3, [p_o]; ld.param.u64 %rd4, [p_n];\n"
"  mov.u32 %r1, %ctaid.x; mov.u32 %r2, %ntid.x; mov.u32 %r3, %tid.x;\n"
"  mad.lo.u32 %r4, %r1, %r2, %r3; cvt.u64.u32 %rd5, %r4;\n"
"  setp.ge.u64 %p1, %rd5, %rd4; @%p1 bra L_DONE;\n"
"  shl.b64 %rd6, %rd5, 2; add.u64 %rd7, %rd1, %rd6; add.u64 %rd8, %rd2, %rd6; add.u64 %rd9, %rd3, %rd6;\n"
"  ld.global.f32 %f1, [%rd7]; ld.global.f32 %f2, [%rd8]; add.rn.f32 %f3, %f1, %f2;\n"
"  st.global.f32 [%rd9], %f3;\n"
"L_DONE: ret;\n}\n"
".visible .entry tz_vec_mul_f32(.param .u64 p_a, .param .u64 p_b, .param .u64 p_o, .param .u64 p_n) {\n"
"  .reg .pred %p<2>; .reg .f32 %f<4>; .reg .u32 %r<5>; .reg .u64 %rd<10>;\n"
"  ld.param.u64 %rd1, [p_a]; ld.param.u64 %rd2, [p_b]; ld.param.u64 %rd3, [p_o]; ld.param.u64 %rd4, [p_n];\n"
"  mov.u32 %r1, %ctaid.x; mov.u32 %r2, %ntid.x; mov.u32 %r3, %tid.x;\n"
"  mad.lo.u32 %r4, %r1, %r2, %r3; cvt.u64.u32 %rd5, %r4;\n"
"  setp.ge.u64 %p1, %rd5, %rd4; @%p1 bra L_DONE;\n"
"  shl.b64 %rd6, %rd5, 2; add.u64 %rd7, %rd1, %rd6; add.u64 %rd8, %rd2, %rd6; add.u64 %rd9, %rd3, %rd6;\n"
"  ld.global.f32 %f1, [%rd7]; ld.global.f32 %f2, [%rd8]; mul.rn.f32 %f3, %f1, %f2;\n"
"  st.global.f32 [%rd9], %f3;\n"
"L_DONE: ret;\n}\n"
".visible .entry tz_matmul_f32(.param .u64 p_a, .param .u64 p_b, .param .u64 p_c, .param .u32 p_m, .param .u32 p_k, .param .u32 p_n) {\n"
"  .reg .pred %p<4>; .reg .f32 %f<5>; .reg .u32 %r<12>; .reg .u64 %rd<14>;\n"
"  ld.param.u64 %rd1, [p_a]; ld.param.u64 %rd2, [p_b]; ld.param.u64 %rd3, [p_c];\n"
"  ld.param.u32 %r1, [p_m]; ld.param.u32 %r2, [p_k]; ld.param.u32 %r3, [p_n];\n"
"  mov.u32 %r4, %ctaid.x; mov.u32 %r5, %ntid.x; mov.u32 %r6, %tid.x; mad.lo.u32 %r7, %r4, %r5, %r6;\n"
"  mov.u32 %r8, %ctaid.y; mov.u32 %r9, %ntid.y; mov.u32 %r10, %tid.y; mad.lo.u32 %r11, %r8, %r9, %r10;\n"
"  setp.ge.u32 %p1, %r11, %r1; setp.ge.u32 %p2, %r7, %r3; or.pred %p1, %p1, %p2; @%p1 bra L_DONE;\n"
"  mov.f32 %f1, 0f00000000; mov.u32 %r4, 0;\n"
"L_KLOOP: setp.ge.u32 %p3, %r4, %r2; @%p3 bra L_KEND;\n"
"  mad.wide.u32 %rd4, %r11, %r2, 0; cvt.u64.u32 %rd5, %r4; add.u64 %rd4, %rd4, %rd5; shl.b64 %rd4, %rd4, 2; add.u64 %rd4, %rd1, %rd4;\n"
"  mad.wide.u32 %rd6, %r4, %r3, 0; cvt.u64.u32 %rd7, %r7; add.u64 %rd6, %rd6, %rd7; shl.b64 %rd6, %rd6, 2; add.u64 %rd6, %rd2, %rd6;\n"
"  ld.global.f32 %f2, [%rd4]; ld.global.f32 %f3, [%rd6]; mul.rn.f32 %f4, %f2, %f3; add.rn.f32 %f1, %f1, %f4;\n"
"  add.u32 %r4, %r4, 1; bra L_KLOOP;\n"
"L_KEND: mad.wide.u32 %rd8, %r11, %r3, 0; cvt.u64.u32 %rd9, %r7; add.u64 %rd8, %rd8, %rd9; shl.b64 %rd8, %rd8, 2; add.u64 %rd8, %rd3, %rd8;\n"
"  st.global.f32 [%rd8], %f1;\n"
"L_DONE: ret;\n}\n"
".visible .entry tz_matmul_relu_f32(.param .u64 p_a, .param .u64 p_b, .param .u64 p_c, .param .u32 p_m, .param .u32 p_k, .param .u32 p_n) {\n"
"  .reg .pred %p<4>; .reg .f32 %f<6>; .reg .u32 %r<12>; .reg .u64 %rd<14>;\n"
"  ld.param.u64 %rd1, [p_a]; ld.param.u64 %rd2, [p_b]; ld.param.u64 %rd3, [p_c];\n"
"  ld.param.u32 %r1, [p_m]; ld.param.u32 %r2, [p_k]; ld.param.u32 %r3, [p_n];\n"
"  mov.u32 %r4, %ctaid.x; mov.u32 %r5, %ntid.x; mov.u32 %r6, %tid.x; mad.lo.u32 %r7, %r4, %r5, %r6;\n"
"  mov.u32 %r8, %ctaid.y; mov.u32 %r9, %ntid.y; mov.u32 %r10, %tid.y; mad.lo.u32 %r11, %r8, %r9, %r10;\n"
"  setp.ge.u32 %p1, %r11, %r1; setp.ge.u32 %p2, %r7, %r3; or.pred %p1, %p1, %p2; @%p1 bra L_DONE;\n"
"  mov.f32 %f1, 0f00000000; mov.u32 %r4, 0;\n"
"L_KLOOP: setp.ge.u32 %p3, %r4, %r2; @%p3 bra L_KEND;\n"
"  mad.wide.u32 %rd4, %r11, %r2, 0; cvt.u64.u32 %rd5, %r4; add.u64 %rd4, %rd4, %rd5; shl.b64 %rd4, %rd4, 2; add.u64 %rd4, %rd1, %rd4;\n"
"  mad.wide.u32 %rd6, %r4, %r3, 0; cvt.u64.u32 %rd7, %r7; add.u64 %rd6, %rd6, %rd7; shl.b64 %rd6, %rd6, 2; add.u64 %rd6, %rd2, %rd6;\n"
"  ld.global.f32 %f2, [%rd4]; ld.global.f32 %f3, [%rd6]; mul.rn.f32 %f4, %f2, %f3; add.rn.f32 %f1, %f1, %f4;\n"
"  add.u32 %r4, %r4, 1; bra L_KLOOP;\n"
"L_KEND: mad.wide.u32 %rd8, %r11, %r3, 0; cvt.u64.u32 %rd9, %r7; add.u64 %rd8, %rd8, %rd9; shl.b64 %rd8, %rd8, 2; add.u64 %rd8, %rd3, %rd8;\n"
"  max.f32 %f5, %f1, 0f00000000; st.global.f32 [%rd8], %f5;\n"
"L_DONE: ret;\n}\n";

static int tz_gpu_cu_load(void) {
    if (tz_cu.attempted) return tz_cu.ok;
    tz_cu.attempted = 1;
#ifdef _WIN32
    HMODULE mod = LoadLibraryA("nvcuda.dll");
    if (!mod) return 0;
#define TZ_LOAD_CU(field, sym) tz_cu.field = (tz_pfn_##sym)tz_gpu_opt_sym((void*)mod, #sym); if (!tz_cu.field) return 0;
#else
    void* mod = dlopen("libcuda.so.1", RTLD_LAZY);
    if (!mod) mod = dlopen("libcuda.so", RTLD_LAZY);
    if (!mod) return 0;
#define TZ_LOAD_CU(field, sym) tz_cu.field = (tz_pfn_##sym)dlsym(mod, #sym); if (!tz_cu.field) return 0;
#endif
    tz_cu.init = (tz_pfn_cuInit)tz_gpu_opt_sym((void*)mod, "cuInit");
    if (!tz_cu.init) return 0;
    TZ_LOAD_CU(dev_count, cuDeviceGetCount)
    TZ_LOAD_CU(dev_get, cuDeviceGet)
    TZ_LOAD_CU(dev_name, cuDeviceGetName)
    tz_cu.ctx_create = (tz_pfn_cuCtxCreate)tz_gpu_opt_sym((void*)mod, "cuCtxCreate_v2");
    if (!tz_cu.ctx_create) tz_cu.ctx_create = (tz_pfn_cuCtxCreate)tz_gpu_opt_sym((void*)mod, "cuCtxCreate");
    tz_cu.ctx_destroy = (tz_pfn_cuCtxDestroy)tz_gpu_opt_sym((void*)mod, "cuCtxDestroy_v2");
    if (!tz_cu.ctx_destroy) tz_cu.ctx_destroy = (tz_pfn_cuCtxDestroy)tz_gpu_opt_sym((void*)mod, "cuCtxDestroy");
    TZ_LOAD_CU(mod_load, cuModuleLoadData)
    TZ_LOAD_CU(mod_unload, cuModuleUnload)
    TZ_LOAD_CU(mod_fn, cuModuleGetFunction)
    tz_cu.mem_alloc = (tz_pfn_cuMemAlloc)tz_gpu_opt_sym((void*)mod, "cuMemAlloc_v2");
    tz_cu.mem_free = (tz_pfn_cuMemFree)tz_gpu_opt_sym((void*)mod, "cuMemFree_v2");
    tz_cu.memcpy_h2d = (tz_pfn_cuMemcpyHtoD)tz_gpu_opt_sym((void*)mod, "cuMemcpyHtoD_v2");
    tz_cu.memcpy_d2h = (tz_pfn_cuMemcpyDtoH)tz_gpu_opt_sym((void*)mod, "cuMemcpyDtoH_v2");
    TZ_LOAD_CU(launch, cuLaunchKernel)
    TZ_LOAD_CU(sync, cuCtxSynchronize)
#undef TZ_LOAD_CU
    if (!tz_cu.ctx_create || !tz_cu.ctx_destroy || !tz_cu.mem_alloc || !tz_cu.mem_free ||
        !tz_cu.memcpy_h2d || !tz_cu.memcpy_d2h) return 0;
    if (tz_cu.init(0u) != 0) return 0;
    tz_cu.ok = 1;
    return 1;
}

static int tz_gpu_cu_available(char* name_buf, size_t name_cap) {
    if (!tz_gpu_cu_load()) return 0;
    int cnt = 0;
    if (tz_cu.dev_count(&cnt) != 0 || cnt <= 0) return 0;
    tz_cu_dev dev = 0;
    if (tz_cu.dev_get(&dev, 0) != 0) return 0;
    if (name_buf && name_cap > 0u) {
        name_buf[0] = '\0';
        tz_cu.dev_name(name_buf, (int)(name_cap - 1u), dev);
        name_buf[name_cap - 1u] = '\0';
    }
    return 1;
}

static int tz_gpu_mtl_available(void) {
#ifdef __APPLE__
    static int attempted = 0, ok = 0;
    if (attempted) return ok;
    attempted = 1;
    void* m = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY | RTLD_LOCAL);
    if (m) ok = (dlsym(m, "MTLCreateSystemDefaultDevice") != NULL) ? 1 : 0;
    return ok;
#else
    return 0;
#endif
}

static int tz_gpu_cl_available(void) {
    tz_cl_device_id dev = NULL;
    return tz_gpu_pick_device(&dev, NULL, 0u) ? 1 : 0;
}

static int tz_gpu_pick_backend_kind(void) {
    const char* req = getenv("TURI_GPU_BACKEND");
    if (req && strcmp(req, "cuda") == 0) return tz_gpu_cu_available(NULL, 0u) ? TZ_GPU_BK_CUDA : TZ_GPU_BK_NONE;
    if (req && strcmp(req, "opencl") == 0) return tz_gpu_cl_available() ? TZ_GPU_BK_OPENCL : TZ_GPU_BK_NONE;
    if (req && strcmp(req, "metal") == 0) return tz_gpu_mtl_available() ? TZ_GPU_BK_METAL : TZ_GPU_BK_NONE;
    if (tz_gpu_cu_available(NULL, 0u)) return TZ_GPU_BK_CUDA;
    if (tz_gpu_cl_available()) return TZ_GPU_BK_OPENCL;
    if (tz_gpu_mtl_available()) return TZ_GPU_BK_METAL;
    return TZ_GPU_BK_NONE;
}
)c";

inline constexpr const char* kGpuApi = R"c(
typedef struct tz_gpu_ctx {
    int backend_kind;
    tz_cl_device_id dev;
    tz_cl_context cl_ctx;
    tz_cl_command_queue queue;
    tz_cl_program prog;
    tz_cl_kernel k_saxpy;
    tz_cl_kernel k_saxpy_relu;
    tz_cl_kernel k_matmul;
    tz_cl_kernel k_matmul64;
    tz_cl_kernel k_matmul_relu;
    tz_cl_kernel k_matmul64_relu;
    tz_cl_kernel k_relu;
    tz_cl_kernel k_vec_add;
    tz_cl_kernel k_vec_mul;
    tz_cl_mem stg_mem;
    size_t stg_cap;
    tz_cl_ulong local_mem;
    tz_cu_ctx cu_ctx;
    tz_cu_mod cu_mod;
    tz_cu_fn cu_k_saxpy;
    tz_cu_fn cu_k_saxpy_relu;
    tz_cu_fn cu_k_relu;
    tz_cu_fn cu_k_vec_add;
    tz_cu_fn cu_k_vec_mul;
    tz_cu_fn cu_k_matmul;
    tz_cu_fn cu_k_matmul_relu;
} tz_gpu_ctx;

typedef struct tz_gpu_buf {
    int backend_kind;
    tz_cl_mem mem;
    tz_cu_ptr cu_ptr;
    size_t count;
} tz_gpu_buf;

static double tz_gpu_ev_ms(tz_cl_event ev) {
    if (!ev) return 0.0;
    tz_cl_ulong t0 = 0, t1 = 0;
    tz_cl.prof_info(ev, TZ_CL_PROFILING_COMMAND_START, sizeof(t0), &t0, NULL);
    tz_cl.prof_info(ev, TZ_CL_PROFILING_COMMAND_END, sizeof(t1), &t1, NULL);
    tz_cl.release_event(ev);
    return (t1 > t0) ? (double)(t1 - t0) * 1e-6 : 0.0;
}

int32_t tz_gpu_available(void) {
    return tz_gpu_pick_backend_kind() != TZ_GPU_BK_NONE ? 1 : 0;
}

int64_t tz_gpu_cache_hit(void) {
    return tz_gpu_last_cache_hit;
}

const uint8_t* tz_gpu_backend_name(int64_t* out_len) {
    int bk = tz_gpu_pick_backend_kind();
    const char* nm = "none";
    if (bk == TZ_GPU_BK_CUDA) nm = "cuda";
    else if (bk == TZ_GPU_BK_OPENCL) nm = "opencl";
    else if (bk == TZ_GPU_BK_METAL) nm = "metal";
    snprintf(tz_gpu_tls_backend, sizeof(tz_gpu_tls_backend), "%s", nm);
    if (out_len) *out_len = (int64_t)strlen(tz_gpu_tls_backend);
    return (const uint8_t*)tz_gpu_tls_backend;
}

const uint8_t* tz_gpu_available_backends(int64_t* out_len) {
    tz_gpu_tls_backends[0] = '\0';
    int n = 0;
    if (tz_gpu_cu_available(NULL, 0u)) {
        n += snprintf(tz_gpu_tls_backends + n, sizeof(tz_gpu_tls_backends) - (size_t)n, "cuda");
    }
    if (tz_gpu_cl_available()) {
        n += snprintf(tz_gpu_tls_backends + n, sizeof(tz_gpu_tls_backends) - (size_t)n, "%sopencl", n ? "," : "");
    }
    if (tz_gpu_mtl_available()) {
        n += snprintf(tz_gpu_tls_backends + n, sizeof(tz_gpu_tls_backends) - (size_t)n, "%smetal", n ? "," : "");
    }
    if (n == 0) snprintf(tz_gpu_tls_backends, sizeof(tz_gpu_tls_backends), "none");
    if (out_len) *out_len = (int64_t)strlen(tz_gpu_tls_backends);
    return (const uint8_t*)tz_gpu_tls_backends;
}

const uint8_t* tz_gpu_device_name(int64_t* out_len) {
    tz_gpu_tls_name[0] = '\0';
    int bk = tz_gpu_pick_backend_kind();
    if (bk == TZ_GPU_BK_CUDA) {
        if (!tz_gpu_cu_available(tz_gpu_tls_name, sizeof(tz_gpu_tls_name))) tz_gpu_tls_name[0] = '\0';
    } else if (bk == TZ_GPU_BK_OPENCL) {
        tz_cl_device_id dev = NULL;
        if (!tz_gpu_pick_device(&dev, tz_gpu_tls_name, sizeof(tz_gpu_tls_name))) tz_gpu_tls_name[0] = '\0';
    } else if (bk == TZ_GPU_BK_METAL) {
        snprintf(tz_gpu_tls_name, sizeof(tz_gpu_tls_name), "Apple Metal GPU");
    }
    if (out_len) *out_len = (int64_t)strlen(tz_gpu_tls_name);
    return (const uint8_t*)tz_gpu_tls_name;
}

void tz_gpu_free(void* cp) {
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    if (!c) return;
    if (c->backend_kind == TZ_GPU_BK_CUDA) {
        if (c->cu_mod && tz_cu.mod_unload) tz_cu.mod_unload(c->cu_mod);
        if (c->cu_ctx && tz_cu.ctx_destroy) tz_cu.ctx_destroy(c->cu_ctx);
        free(c);
        return;
    }
    if (c->stg_mem) tz_cl.release_buf(c->stg_mem);
    if (c->k_saxpy) tz_cl.release_kernel(c->k_saxpy);
    if (c->k_saxpy_relu) tz_cl.release_kernel(c->k_saxpy_relu);
    if (c->k_matmul) tz_cl.release_kernel(c->k_matmul);
    if (c->k_matmul64) tz_cl.release_kernel(c->k_matmul64);
    if (c->k_matmul_relu) tz_cl.release_kernel(c->k_matmul_relu);
    if (c->k_matmul64_relu) tz_cl.release_kernel(c->k_matmul64_relu);
    if (c->k_relu) tz_cl.release_kernel(c->k_relu);
    if (c->k_vec_add) tz_cl.release_kernel(c->k_vec_add);
    if (c->k_vec_mul) tz_cl.release_kernel(c->k_vec_mul);
    if (c->prog) tz_cl.release_prog(c->prog);
    if (c->queue) tz_cl.release_queue(c->queue);
    if (c->cl_ctx) tz_cl.release_ctx(c->cl_ctx);
    free(c);
}

void* tz_gpu_init(const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    tz_gpu_last_cache_hit = 0;
    int bk = tz_gpu_pick_backend_kind();
    if (bk == TZ_GPU_BK_CUDA) {
        if (!tz_gpu_cu_available(tz_gpu_tls_name, sizeof(tz_gpu_tls_name))) {
            tz_gpu_set_err(err_ptr, err_len, "No se encontró dispositivo CUDA disponible");
            return NULL;
        }
        tz_cu_dev cdev = 0;
        if (tz_cu.dev_get(&cdev, 0) != 0) {
            tz_gpu_set_err(err_ptr, err_len, "Error en cuDeviceGet");
            return NULL;
        }
        tz_gpu_ctx* c = (tz_gpu_ctx*)calloc(1, sizeof(tz_gpu_ctx));
        if (!c) { tz_gpu_set_err(err_ptr, err_len, "Sin memoria para contexto CUDA"); return NULL; }
        c->backend_kind = TZ_GPU_BK_CUDA;
        if (tz_cu.ctx_create(&c->cu_ctx, 0u, cdev) != 0 || !c->cu_ctx) {
            tz_gpu_free(c);
            tz_gpu_set_err(err_ptr, err_len, "Error en cuCtxCreate");
            return NULL;
        }
        if (tz_cu.mod_load(&c->cu_mod, tz_gpu_ptx_src) != 0 || !c->cu_mod) {
            tz_gpu_free(c);
            tz_gpu_set_err(err_ptr, err_len, "Error en cuModuleLoadData (PTX)");
            return NULL;
        }
        if (tz_cu.mod_fn(&c->cu_k_saxpy, c->cu_mod, "tz_saxpy_f32") != 0 ||
            tz_cu.mod_fn(&c->cu_k_saxpy_relu, c->cu_mod, "tz_saxpy_relu_f32") != 0 ||
            tz_cu.mod_fn(&c->cu_k_relu, c->cu_mod, "tz_relu_f32") != 0 ||
            tz_cu.mod_fn(&c->cu_k_vec_add, c->cu_mod, "tz_vec_add_f32") != 0 ||
            tz_cu.mod_fn(&c->cu_k_vec_mul, c->cu_mod, "tz_vec_mul_f32") != 0 ||
            tz_cu.mod_fn(&c->cu_k_matmul, c->cu_mod, "tz_matmul_f32") != 0 ||
            tz_cu.mod_fn(&c->cu_k_matmul_relu, c->cu_mod, "tz_matmul_relu_f32") != 0) {
            tz_gpu_free(c);
            tz_gpu_set_err(err_ptr, err_len, "Error resolviendo funciones PTX CUDA");
            return NULL;
        }
        return c;
    }
    if (bk == TZ_GPU_BK_METAL) {
        tz_gpu_set_err(err_ptr, err_len, "Backend Metal detectado; ejecución directa aún experimental — usa OpenCL/Auto");
        return NULL;
    }
    tz_cl_device_id dev = NULL;
    if (bk != TZ_GPU_BK_OPENCL || !tz_gpu_pick_device(&dev, tz_gpu_tls_name, sizeof(tz_gpu_tls_name))) {
        tz_gpu_set_err(err_ptr, err_len, "No se encontró dispositivo GPU OpenCL/CUDA disponible");
        return NULL;
    }
    tz_gpu_ctx* c = (tz_gpu_ctx*)calloc(1, sizeof(tz_gpu_ctx));
    if (!c) { tz_gpu_set_err(err_ptr, err_len, "Sin memoria para contexto GPU"); return NULL; }
    c->backend_kind = TZ_GPU_BK_OPENCL;
    c->dev = dev;
    tz_cl.get_device_info(dev, TZ_CL_DEVICE_LOCAL_MEM_SIZE, sizeof(c->local_mem), &c->local_mem, NULL);
    tz_cl_int rc = TZ_CL_SUCCESS;
    c->cl_ctx = tz_cl.create_ctx(NULL, 1u, &dev, NULL, NULL, &rc);
    if (!c->cl_ctx || rc != TZ_CL_SUCCESS) {
        tz_gpu_free(c);
        tz_gpu_set_err(err_ptr, err_len, "Error en clCreateContext");
        return NULL;
    }
    c->queue = tz_cl.create_queue(c->cl_ctx, dev, TZ_CL_QUEUE_PROFILING_ENABLE, &rc);
    if (!c->queue || rc != TZ_CL_SUCCESS) {
        tz_gpu_free(c);
        tz_gpu_set_err(err_ptr, err_len, "Error en clCreateCommandQueue");
        return NULL;
    }
    char cache_path[580];
    tz_gpu_cache_path(dev, cache_path, sizeof(cache_path));
    c->prog = tz_gpu_load_cached_prog(c->cl_ctx, dev, cache_path);
    if (c->prog) {
        tz_gpu_last_cache_hit = 1;
    } else {
        const char* srcs[2] = {tz_gpu_cl_src, tz_gpu_cl_src2};
        size_t slens[2] = {strlen(srcs[0]), strlen(srcs[1])};
        c->prog = tz_cl.create_prog(c->cl_ctx, 2u, srcs, slens, &rc);
        if (!c->prog || rc != TZ_CL_SUCCESS) {
            tz_gpu_free(c);
            tz_gpu_set_err(err_ptr, err_len, "Error en clCreateProgramWithSource");
            return NULL;
        }
        rc = tz_cl.build_prog(c->prog, 1u, &dev, "", NULL, NULL);
        if (rc != TZ_CL_SUCCESS) {
            char log[384];
            size_t lw = 0;
            tz_cl.get_build_info(c->prog, dev, TZ_CL_PROGRAM_BUILD_LOG, sizeof(log) - 1u, log, &lw);
            log[lw < sizeof(log) ? lw : sizeof(log) - 1u] = '\0';
            tz_gpu_set_err(err_ptr, err_len, log[0] ? log : "Error compilando programa OpenCL");
            tz_gpu_free(c);
            return NULL;
        }
        tz_gpu_save_cached_prog(c->prog, cache_path);
    }
    c->k_saxpy = tz_cl.create_kernel(c->prog, "tz_saxpy_f32", &rc);
    if (!c->k_saxpy || rc != TZ_CL_SUCCESS) {
        tz_gpu_free(c);
        tz_gpu_set_err(err_ptr, err_len, "Error creando kernel tz_saxpy_f32");
        return NULL;
    }
    c->k_matmul = tz_cl.create_kernel(c->prog, "tz_matmul_f32", &rc);
    if (!c->k_matmul || rc != TZ_CL_SUCCESS) {
        tz_gpu_free(c);
        tz_gpu_set_err(err_ptr, err_len, "Error creando kernel tz_matmul_f32");
        return NULL;
    }
    tz_cl_int rce = TZ_CL_SUCCESS;
    c->k_saxpy_relu = tz_cl.create_kernel(c->prog, "tz_saxpy_relu_f32", &rce);
    if (rce != TZ_CL_SUCCESS && c->k_saxpy_relu) { tz_cl.release_kernel(c->k_saxpy_relu); c->k_saxpy_relu = NULL; }
    rce = TZ_CL_SUCCESS;
    c->k_matmul_relu = tz_cl.create_kernel(c->prog, "tz_matmul_relu_f32", &rce);
    if (rce != TZ_CL_SUCCESS && c->k_matmul_relu) { tz_cl.release_kernel(c->k_matmul_relu); c->k_matmul_relu = NULL; }
    if (c->local_mem >= 40960u) {
        tz_cl_int rc64 = TZ_CL_SUCCESS;
        c->k_matmul64 = tz_cl.create_kernel(c->prog, "tz_matmul_f32_64", &rc64);
        if (rc64 != TZ_CL_SUCCESS && c->k_matmul64) { tz_cl.release_kernel(c->k_matmul64); c->k_matmul64 = NULL; }
        rc64 = TZ_CL_SUCCESS;
        c->k_matmul64_relu = tz_cl.create_kernel(c->prog, "tz_matmul_relu_f32_64", &rc64);
        if (rc64 != TZ_CL_SUCCESS && c->k_matmul64_relu) { tz_cl.release_kernel(c->k_matmul64_relu); c->k_matmul64_relu = NULL; }
    }
    rce = TZ_CL_SUCCESS;
    c->k_relu = tz_cl.create_kernel(c->prog, "tz_relu_f32", &rce);
    if (rce != TZ_CL_SUCCESS && c->k_relu) { tz_cl.release_kernel(c->k_relu); c->k_relu = NULL; }
    rce = TZ_CL_SUCCESS;
    c->k_vec_add = tz_cl.create_kernel(c->prog, "tz_vec_add_f32", &rce);
    if (rce != TZ_CL_SUCCESS && c->k_vec_add) { tz_cl.release_kernel(c->k_vec_add); c->k_vec_add = NULL; }
    rce = TZ_CL_SUCCESS;
    c->k_vec_mul = tz_cl.create_kernel(c->prog, "tz_vec_mul_f32", &rce);
    if (rce != TZ_CL_SUCCESS && c->k_vec_mul) { tz_cl.release_kernel(c->k_vec_mul); c->k_vec_mul = NULL; }
    return c;
}

static int32_t tz_gpu_launch_saxpy_kern(tz_gpu_ctx* c, tz_cl_kernel kern, float alpha, tz_cl_mem mx, tz_cl_mem my,
                                        uint64_t nu, tz_cl_event* out_ev) {
    if (!kern) return 0;
    size_t local = 256u;
    size_t global = (size_t)(((nu + local - 1u) / local) * local);
    tz_cl_int rc = TZ_CL_SUCCESS;
    rc |= tz_cl.set_arg(kern, 0u, sizeof(float), &alpha);
    rc |= tz_cl.set_arg(kern, 1u, sizeof(tz_cl_mem), &mx);
    rc |= tz_cl.set_arg(kern, 2u, sizeof(tz_cl_mem), &my);
    rc |= tz_cl.set_arg(kern, 3u, sizeof(tz_cl_ulong), &nu);
    if (rc != TZ_CL_SUCCESS) return 0;
    return tz_cl.enqueue_nd(c->queue, kern, 1u, NULL, &global, &local, 0u, NULL, out_ev) == TZ_CL_SUCCESS;
}

static int32_t tz_gpu_launch_saxpy(tz_gpu_ctx* c, float alpha, tz_cl_mem mx, tz_cl_mem my,
                                   uint64_t nu, tz_cl_event* out_ev) {
    return tz_gpu_launch_saxpy_kern(c, c->k_saxpy, alpha, mx, my, nu, out_ev);
}

static int32_t tz_gpu_launch_matmul_kern(tz_gpu_ctx* c, tz_cl_kernel k64, tz_cl_kernel k16,
                                         tz_cl_mem ma, tz_cl_mem mb, tz_cl_mem mc,
                                         uint32_t m, uint32_t k, uint32_t n, tz_cl_event* out_ev) {
    size_t local[2] = {16u, 16u};
    if (k64 && m >= 32u && k >= 32u && n >= 32u) {
        size_t g64[2] = {((size_t)n + 63u) / 64u * 16u, ((size_t)m + 63u) / 64u * 16u};
        tz_cl_int rc64 = TZ_CL_SUCCESS;
        rc64 |= tz_cl.set_arg(k64, 0u, sizeof(tz_cl_mem), &ma);
        rc64 |= tz_cl.set_arg(k64, 1u, sizeof(tz_cl_mem), &mb);
        rc64 |= tz_cl.set_arg(k64, 2u, sizeof(tz_cl_mem), &mc);
        rc64 |= tz_cl.set_arg(k64, 3u, sizeof(uint32_t), &m);
        rc64 |= tz_cl.set_arg(k64, 4u, sizeof(uint32_t), &k);
        rc64 |= tz_cl.set_arg(k64, 5u, sizeof(uint32_t), &n);
        if (rc64 == TZ_CL_SUCCESS &&
            tz_cl.enqueue_nd(c->queue, k64, 2u, NULL, g64, local, 0u, NULL, out_ev) == TZ_CL_SUCCESS) {
            return 1;
        }
    }
    if (!k16) return 0;
    size_t global[2] = {((size_t)n + 15u) / 16u * 16u, ((size_t)m + 15u) / 16u * 16u};
    tz_cl_int rc = TZ_CL_SUCCESS;
    rc |= tz_cl.set_arg(k16, 0u, sizeof(tz_cl_mem), &ma);
    rc |= tz_cl.set_arg(k16, 1u, sizeof(tz_cl_mem), &mb);
    rc |= tz_cl.set_arg(k16, 2u, sizeof(tz_cl_mem), &mc);
    rc |= tz_cl.set_arg(k16, 3u, sizeof(uint32_t), &m);
    rc |= tz_cl.set_arg(k16, 4u, sizeof(uint32_t), &k);
    rc |= tz_cl.set_arg(k16, 5u, sizeof(uint32_t), &n);
    if (rc != TZ_CL_SUCCESS) return 0;
    return tz_cl.enqueue_nd(c->queue, k16, 2u, NULL, global, local, 0u, NULL, out_ev) == TZ_CL_SUCCESS;
}

static int32_t tz_gpu_launch_matmul(tz_gpu_ctx* c, tz_cl_mem ma, tz_cl_mem mb, tz_cl_mem mc,
                                    uint32_t m, uint32_t k, uint32_t n, tz_cl_event* out_ev) {
    return tz_gpu_launch_matmul_kern(c, c->k_matmul64, c->k_matmul, ma, mb, mc, m, k, n, out_ev);
}
)c";

inline constexpr const char* kGpuApiOps = R"c(
static int tz_gpu_cu_launch_1d(tz_cu_fn fn, uint64_t nu, void** params) {
    if (!fn) return 0;
    unsigned bx = 256u;
    unsigned gx = (unsigned)((nu + bx - 1u) / bx);
    if (gx == 0u) gx = 1u;
    if (tz_cu.launch(fn, gx, 1u, 1u, bx, 1u, 1u, 0u, NULL, params, NULL) != 0) return 0;
    return tz_cu.sync() == 0;
}

static int32_t tz_gpu_vec_binary(tz_gpu_ctx* c, tz_cl_kernel kern, tz_cu_fn cu_fn,
                                 const float* a, const float* b, float* out,
                                 int64_t n, double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                                 const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    if (!c || !a || !b || !out || n < 0 || (uint64_t)n > SIZE_MAX / sizeof(float)) {
        tz_gpu_set_err(err_ptr, err_len, "Argumentos inválidos en operación vectorial GPU");
        return 0;
    }
    if (out_h2d_ms) *out_h2d_ms = 0.0;
    if (out_kern_ms) *out_kern_ms = 0.0;
    if (out_d2h_ms) *out_d2h_ms = 0.0;
    if (n == 0) return 1;
    size_t bytes = (size_t)n * sizeof(float);
    uint64_t nu = (uint64_t)n;
    if (c->backend_kind == TZ_GPU_BK_CUDA) {
        if (!cu_fn) { tz_gpu_set_err(err_ptr, err_len, "Kernel CUDA no disponible"); return 0; }
        tz_cu_ptr da = 0, db = 0, dout = 0;
        double t0 = tz_gpu_now_ms();
        if (tz_cu.mem_alloc(&da, bytes) != 0 || tz_cu.mem_alloc(&db, bytes) != 0 || tz_cu.mem_alloc(&dout, bytes) != 0 ||
            tz_cu.memcpy_h2d(da, a, bytes) != 0 || tz_cu.memcpy_h2d(db, b, bytes) != 0) {
            if (da) tz_cu.mem_free(da);
            if (db) tz_cu.mem_free(db);
            if (dout) tz_cu.mem_free(dout);
            tz_gpu_set_err(err_ptr, err_len, "Fallo H2D CUDA vectorial");
            return 0;
        }
        double t1 = tz_gpu_now_ms();
        void* params[4] = {&da, &db, &dout, &nu};
        int ok = tz_gpu_cu_launch_1d(cu_fn, nu, params);
        double t2 = tz_gpu_now_ms();
        if (ok && tz_cu.memcpy_d2h(out, dout, bytes) != 0) ok = 0;
        double t3 = tz_gpu_now_ms();
        tz_cu.mem_free(da); tz_cu.mem_free(db); tz_cu.mem_free(dout);
        if (out_h2d_ms) *out_h2d_ms = t1 - t0;
        if (out_kern_ms) *out_kern_ms = t2 - t1;
        if (out_d2h_ms) *out_d2h_ms = t3 - t2;
        if (!ok) tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando operación CUDA");
        return ok;
    }
    if (!kern) { tz_gpu_set_err(err_ptr, err_len, "Kernel OpenCL no disponible"); return 0; }
    tz_cl_int r1 = 0, r2 = 0, r3 = 0;
    tz_cl_mem ma = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_READ_ONLY, bytes, NULL, &r1);
    tz_cl_mem mb = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_READ_ONLY, bytes, NULL, &r2);
    tz_cl_mem mo = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_WRITE_ONLY, bytes, NULL, &r3);
    if (!ma || !mb || !mo || r1 || r2 || r3) {
        if (ma) tz_cl.release_buf(ma);
        if (mb) tz_cl.release_buf(mb);
        if (mo) tz_cl.release_buf(mo);
        tz_gpu_set_err(err_ptr, err_len, "Error creando buffers OpenCL vectoriales");
        return 0;
    }
    size_t local = 256u;
    size_t global = (size_t)(((nu + local - 1u) / local) * local);
    tz_cl_int s = 0;
    s |= tz_cl.set_arg(kern, 0u, sizeof(tz_cl_mem), &ma);
    s |= tz_cl.set_arg(kern, 1u, sizeof(tz_cl_mem), &mb);
    s |= tz_cl.set_arg(kern, 2u, sizeof(tz_cl_mem), &mo);
    s |= tz_cl.set_arg(kern, 3u, sizeof(tz_cl_ulong), &nu);
    tz_cl_event e1 = NULL, e2 = NULL, ek = NULL, e3 = NULL;
    int ok = 1;
    if (s != TZ_CL_SUCCESS ||
        tz_cl.write_buf(c->queue, ma, TZ_CL_FALSE, 0u, bytes, a, 0u, NULL, &e1) != TZ_CL_SUCCESS ||
        tz_cl.write_buf(c->queue, mb, TZ_CL_FALSE, 0u, bytes, b, 0u, NULL, &e2) != TZ_CL_SUCCESS ||
        tz_cl.enqueue_nd(c->queue, kern, 1u, NULL, &global, &local, 0u, NULL, &ek) != TZ_CL_SUCCESS ||
        tz_cl.read_buf(c->queue, mo, TZ_CL_TRUE, 0u, bytes, out, 0u, NULL, &e3) != TZ_CL_SUCCESS) {
        ok = 0;
        tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando operación vectorial OpenCL");
    }
    tz_cl.finish(c->queue);
    if (out_h2d_ms) *out_h2d_ms = tz_gpu_ev_ms(e1) + tz_gpu_ev_ms(e2);
    if (out_kern_ms) *out_kern_ms = tz_gpu_ev_ms(ek);
    if (out_d2h_ms) *out_d2h_ms = tz_gpu_ev_ms(e3);
    tz_cl.release_buf(ma); tz_cl.release_buf(mb); tz_cl.release_buf(mo);
    return ok;
}

int32_t tz_gpu_relu_f32(void* cp, const float* x, float* y, int64_t n,
                        double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                        const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    if (!c || !x || !y || n < 0 || (uint64_t)n > SIZE_MAX / sizeof(float)) {
        tz_gpu_set_err(err_ptr, err_len, "Argumentos inválidos en tz_gpu_relu_f32");
        return 0;
    }
    if (out_h2d_ms) *out_h2d_ms = 0.0;
    if (out_kern_ms) *out_kern_ms = 0.0;
    if (out_d2h_ms) *out_d2h_ms = 0.0;
    if (n == 0) return 1;
    size_t bytes = (size_t)n * sizeof(float);
    uint64_t nu = (uint64_t)n;
    if (c->backend_kind == TZ_GPU_BK_CUDA) {
        tz_cu_ptr dx = 0, dy = 0;
        double t0 = tz_gpu_now_ms();
        if (tz_cu.mem_alloc(&dx, bytes) != 0 || tz_cu.mem_alloc(&dy, bytes) != 0 ||
            tz_cu.memcpy_h2d(dx, x, bytes) != 0) {
            if (dx) tz_cu.mem_free(dx);
            if (dy) tz_cu.mem_free(dy);
            tz_gpu_set_err(err_ptr, err_len, "Fallo H2D CUDA relu");
            return 0;
        }
        double t1 = tz_gpu_now_ms();
        void* params[3] = {&dx, &dy, &nu};
        int ok = tz_gpu_cu_launch_1d(c->cu_k_relu, nu, params);
        double t2 = tz_gpu_now_ms();
        if (ok && tz_cu.memcpy_d2h(y, dy, bytes) != 0) ok = 0;
        double t3 = tz_gpu_now_ms();
        tz_cu.mem_free(dx); tz_cu.mem_free(dy);
        if (out_h2d_ms) *out_h2d_ms = t1 - t0;
        if (out_kern_ms) *out_kern_ms = t2 - t1;
        if (out_d2h_ms) *out_d2h_ms = t3 - t2;
        if (!ok) tz_gpu_set_err(err_ptr, err_len, "Fallo relu CUDA");
        return ok;
    }
    if (!c->k_relu) { tz_gpu_set_err(err_ptr, err_len, "Kernel relu no disponible"); return 0; }
    tz_cl_int r1 = 0, r2 = 0;
    tz_cl_mem mx = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_READ_ONLY, bytes, NULL, &r1);
    tz_cl_mem my = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_WRITE_ONLY, bytes, NULL, &r2);
    if (!mx || !my || r1 || r2) {
        if (mx) tz_cl.release_buf(mx);
        if (my) tz_cl.release_buf(my);
        tz_gpu_set_err(err_ptr, err_len, "Error creando buffers OpenCL en relu");
        return 0;
    }
    size_t local = 256u;
    size_t global = (size_t)(((nu + local - 1u) / local) * local);
    tz_cl_int s = tz_cl.set_arg(c->k_relu, 0u, sizeof(tz_cl_mem), &mx) |
                  tz_cl.set_arg(c->k_relu, 1u, sizeof(tz_cl_mem), &my) |
                  tz_cl.set_arg(c->k_relu, 2u, sizeof(tz_cl_ulong), &nu);
    tz_cl_event e1 = NULL, ek = NULL, e2 = NULL;
    int ok = 1;
    if (s != TZ_CL_SUCCESS ||
        tz_cl.write_buf(c->queue, mx, TZ_CL_FALSE, 0u, bytes, x, 0u, NULL, &e1) != TZ_CL_SUCCESS ||
        tz_cl.enqueue_nd(c->queue, c->k_relu, 1u, NULL, &global, &local, 0u, NULL, &ek) != TZ_CL_SUCCESS ||
        tz_cl.read_buf(c->queue, my, TZ_CL_TRUE, 0u, bytes, y, 0u, NULL, &e2) != TZ_CL_SUCCESS) {
        ok = 0;
        tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando relu OpenCL");
    }
    tz_cl.finish(c->queue);
    if (out_h2d_ms) *out_h2d_ms = tz_gpu_ev_ms(e1);
    if (out_kern_ms) *out_kern_ms = tz_gpu_ev_ms(ek);
    if (out_d2h_ms) *out_d2h_ms = tz_gpu_ev_ms(e2);
    tz_cl.release_buf(mx); tz_cl.release_buf(my);
    return ok;
}

int32_t tz_gpu_vec_add_f32(void* cp, const float* a, const float* b, float* out, int64_t n,
                           double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                           const void** err_ptr, int64_t* err_len) {
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    return tz_gpu_vec_binary(c, c ? c->k_vec_add : NULL, c ? c->cu_k_vec_add : NULL,
                             a, b, out, n, out_h2d_ms, out_kern_ms, out_d2h_ms, err_ptr, err_len);
}

int32_t tz_gpu_vec_mul_f32(void* cp, const float* a, const float* b, float* out, int64_t n,
                           double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                           const void** err_ptr, int64_t* err_len) {
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    return tz_gpu_vec_binary(c, c ? c->k_vec_mul : NULL, c ? c->cu_k_vec_mul : NULL,
                             a, b, out, n, out_h2d_ms, out_kern_ms, out_d2h_ms, err_ptr, err_len);
}
)c";

inline constexpr const char* kGpuApiBody = R"c(
static int tz_gpu_cu_launch_matmul(tz_cu_fn fn, tz_cu_ptr a, tz_cu_ptr b, tz_cu_ptr c,
                                   uint32_t m, uint32_t k, uint32_t n) {
    if (!fn) return 0;
    unsigned gx = ((unsigned)n + 15u) / 16u;
    unsigned gy = ((unsigned)m + 15u) / 16u;
    if (gx == 0u) gx = 1u;
    if (gy == 0u) gy = 1u;
    void* params[6] = {&a, &b, &c, &m, &k, &n};
    if (tz_cu.launch(fn, gx, gy, 1u, 16u, 16u, 1u, 0u, NULL, params, NULL) != 0) return 0;
    return tz_cu.sync() == 0;
}

static int32_t tz_gpu_saxpy_common(void* cp, float alpha, const float* x, float* y, int64_t n,
                                   int fused_relu, double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                                   const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    if (!c || !x || !y || n < 0 || (uint64_t)n > SIZE_MAX / sizeof(float)) {
        tz_gpu_set_err(err_ptr, err_len, "Argumentos inválidos en saxpy GPU");
        return 0;
    }
    if (out_h2d_ms) *out_h2d_ms = 0.0;
    if (out_kern_ms) *out_kern_ms = 0.0;
    if (out_d2h_ms) *out_d2h_ms = 0.0;
    if (n == 0) return 1;

    size_t bytes = (size_t)n * sizeof(float);
    uint64_t nu = (uint64_t)n;
    if (c->backend_kind == TZ_GPU_BK_CUDA) {
        tz_cu_fn fn = fused_relu ? c->cu_k_saxpy_relu : c->cu_k_saxpy;
        tz_cu_ptr dx = 0, dy = 0;
        double t0 = tz_gpu_now_ms();
        if (tz_cu.mem_alloc(&dx, bytes) != 0 || tz_cu.mem_alloc(&dy, bytes) != 0 ||
            tz_cu.memcpy_h2d(dx, x, bytes) != 0 || tz_cu.memcpy_h2d(dy, y, bytes) != 0) {
            if (dx) tz_cu.mem_free(dx);
            if (dy) tz_cu.mem_free(dy);
            tz_gpu_set_err(err_ptr, err_len, "Error H2D CUDA en saxpy");
            return 0;
        }
        double t1 = tz_gpu_now_ms();
        void* params[4] = {&alpha, &dx, &dy, &nu};
        int ok = tz_gpu_cu_launch_1d(fn, nu, params);
        double t2 = tz_gpu_now_ms();
        if (ok && tz_cu.memcpy_d2h(y, dy, bytes) != 0) ok = 0;
        double t3 = tz_gpu_now_ms();
        tz_cu.mem_free(dx); tz_cu.mem_free(dy);
        if (out_h2d_ms) *out_h2d_ms = t1 - t0;
        if (out_kern_ms) *out_kern_ms = t2 - t1;
        if (out_d2h_ms) *out_d2h_ms = t3 - t2;
        if (!ok) tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando saxpy CUDA");
        return ok;
    }

    tz_cl_kernel kern = fused_relu ? c->k_saxpy_relu : c->k_saxpy;
    if (!kern) {
        tz_gpu_set_err(err_ptr, err_len, "Kernel saxpy OpenCL no disponible");
        return 0;
    }
    tz_cl_int rc1 = TZ_CL_SUCCESS, rc2 = TZ_CL_SUCCESS;
    tz_cl_mem mx = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_READ_ONLY, bytes, NULL, &rc1);
    tz_cl_mem my = tz_cl.create_buf(c->cl_ctx, TZ_CL_MEM_READ_WRITE, bytes, NULL, &rc2);
    if (!mx || !my || rc1 != TZ_CL_SUCCESS || rc2 != TZ_CL_SUCCESS) {
        if (mx) tz_cl.release_buf(mx);
        if (my) tz_cl.release_buf(my);
        tz_gpu_set_err(err_ptr, err_len, "Error creando buffers OpenCL en saxpy");
        return 0;
    }
    tz_cl_event ev_hx = NULL, ev_hy = NULL, ev_k = NULL, ev_d2h = NULL;
    int ok = 1;
    if (tz_cl.write_buf(c->queue, mx, TZ_CL_FALSE, 0u, bytes, x, 0u, NULL, &ev_hx) != TZ_CL_SUCCESS ||
        tz_cl.write_buf(c->queue, my, TZ_CL_FALSE, 0u, bytes, y, 0u, NULL, &ev_hy) != TZ_CL_SUCCESS ||
        !tz_gpu_launch_saxpy_kern(c, kern, alpha, mx, my, nu, &ev_k) ||
        tz_cl.read_buf(c->queue, my, TZ_CL_TRUE, 0u, bytes, y, 0u, NULL, &ev_d2h) != TZ_CL_SUCCESS) {
        ok = 0;
        tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando saxpy OpenCL");
    }
    tz_cl.finish(c->queue);
    double h2d = tz_gpu_ev_ms(ev_hx) + tz_gpu_ev_ms(ev_hy);
    double kern_ms = tz_gpu_ev_ms(ev_k);
    double d2h = tz_gpu_ev_ms(ev_d2h);
    if (out_h2d_ms) *out_h2d_ms = h2d;
    if (out_kern_ms) *out_kern_ms = kern_ms;
    if (out_d2h_ms) *out_d2h_ms = d2h;
    tz_cl.release_buf(mx);
    tz_cl.release_buf(my);
    return ok;
}

int32_t tz_gpu_saxpy_f32(void* cp, float alpha, const float* x, float* y, int64_t n,
                         double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                         const void** err_ptr, int64_t* err_len) {
    return tz_gpu_saxpy_common(cp, alpha, x, y, n, 0, out_h2d_ms, out_kern_ms, out_d2h_ms, err_ptr, err_len);
}

int32_t tz_gpu_saxpy_relu_f32(void* cp, float alpha, const float* x, float* y, int64_t n,
                              double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                              const void** err_ptr, int64_t* err_len) {
    return tz_gpu_saxpy_common(cp, alpha, x, y, n, 1, out_h2d_ms, out_kern_ms, out_d2h_ms, err_ptr, err_len);
}

int32_t tz_gpu_matmul_f32(void* cp, const float* a, const float* b, float* c_out,
                          int64_t m, int64_t k, int64_t n,
                          double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                          const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    if (!ctx || !a || !b || !c_out || m <= 0 || k <= 0 || n <= 0 ||
        m > 0xFFFFFFFFLL || k > 0xFFFFFFFFLL || n > 0xFFFFFFFFLL ||
        (uint64_t)m > (SIZE_MAX / sizeof(float)) / (uint64_t)k ||
        (uint64_t)k > (SIZE_MAX / sizeof(float)) / (uint64_t)n ||
        (uint64_t)m > (SIZE_MAX / sizeof(float)) / (uint64_t)n) {
        tz_gpu_set_err(err_ptr, err_len, "Dimensiones o punteros inválidos en tz_gpu_matmul_f32");
        return 0;
    }
    size_t ba = (size_t)m * (size_t)k * sizeof(float);
    size_t bb = (size_t)k * (size_t)n * sizeof(float);
    size_t bc = (size_t)m * (size_t)n * sizeof(float);
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        tz_cu_ptr da = 0, db = 0, dc = 0;
        double t0 = tz_gpu_now_ms();
        if (tz_cu.mem_alloc(&da, ba) != 0 || tz_cu.mem_alloc(&db, bb) != 0 || tz_cu.mem_alloc(&dc, bc) != 0 ||
            tz_cu.memcpy_h2d(da, a, ba) != 0 || tz_cu.memcpy_h2d(db, b, bb) != 0) {
            if (da) tz_cu.mem_free(da);
            if (db) tz_cu.mem_free(db);
            if (dc) tz_cu.mem_free(dc);
            tz_gpu_set_err(err_ptr, err_len, "Error H2D CUDA en matmul");
            return 0;
        }
        double t1 = tz_gpu_now_ms();
        int ok = tz_gpu_cu_launch_matmul(ctx->cu_k_matmul, da, db, dc, (uint32_t)m, (uint32_t)k, (uint32_t)n);
        double t2 = tz_gpu_now_ms();
        if (ok && tz_cu.memcpy_d2h(c_out, dc, bc) != 0) ok = 0;
        double t3 = tz_gpu_now_ms();
        tz_cu.mem_free(da); tz_cu.mem_free(db); tz_cu.mem_free(dc);
        if (out_h2d_ms) *out_h2d_ms = t1 - t0;
        if (out_kern_ms) *out_kern_ms = t2 - t1;
        if (out_d2h_ms) *out_d2h_ms = t3 - t2;
        if (!ok) tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando matmul CUDA");
        return ok;
    }

    tz_cl_int r1 = 0, r2 = 0, r3 = 0;
    tz_cl_mem ma = tz_cl.create_buf(ctx->cl_ctx, TZ_CL_MEM_READ_ONLY, ba, NULL, &r1);
    tz_cl_mem mb = tz_cl.create_buf(ctx->cl_ctx, TZ_CL_MEM_READ_ONLY, bb, NULL, &r2);
    tz_cl_mem mc = tz_cl.create_buf(ctx->cl_ctx, TZ_CL_MEM_WRITE_ONLY, bc, NULL, &r3);
    if (!ma || !mb || !mc || r1 || r2 || r3) {
        if (ma) tz_cl.release_buf(ma);
        if (mb) tz_cl.release_buf(mb);
        if (mc) tz_cl.release_buf(mc);
        tz_gpu_set_err(err_ptr, err_len, "Error creando buffers OpenCL en matmul");
        return 0;
    }
    tz_cl_event e1 = NULL, e2 = NULL, ek = NULL, e3 = NULL;
    int ok = 1;
    if (tz_cl.write_buf(ctx->queue, ma, TZ_CL_FALSE, 0u, ba, a, 0u, NULL, &e1) != TZ_CL_SUCCESS ||
        tz_cl.write_buf(ctx->queue, mb, TZ_CL_FALSE, 0u, bb, b, 0u, NULL, &e2) != TZ_CL_SUCCESS ||
        !tz_gpu_launch_matmul(ctx, ma, mb, mc, (uint32_t)m, (uint32_t)k, (uint32_t)n, &ek) ||
        tz_cl.read_buf(ctx->queue, mc, TZ_CL_TRUE, 0u, bc, c_out, 0u, NULL, &e3) != TZ_CL_SUCCESS) {
        ok = 0;
        tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando matmul OpenCL");
    }
    tz_cl.finish(ctx->queue);
    double h2d = tz_gpu_ev_ms(e1) + tz_gpu_ev_ms(e2);
    double kern = tz_gpu_ev_ms(ek);
    double d2h = tz_gpu_ev_ms(e3);
    if (out_h2d_ms) *out_h2d_ms = h2d;
    if (out_kern_ms) *out_kern_ms = kern;
    if (out_d2h_ms) *out_d2h_ms = d2h;
    tz_cl.release_buf(ma);
    tz_cl.release_buf(mb);
    tz_cl.release_buf(mc);
    return ok;
}

void* tz_gpu_buf_alloc(void* cp, int64_t count) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    if (!ctx || count <= 0 || (uint64_t)count > SIZE_MAX / sizeof(float)) return NULL;
    tz_gpu_buf* b = (tz_gpu_buf*)calloc(1, sizeof(tz_gpu_buf));
    if (!b) return NULL;
    b->backend_kind = ctx->backend_kind;
    b->count = (size_t)count;
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        if (tz_cu.mem_alloc(&b->cu_ptr, (size_t)count * sizeof(float)) != 0 || !b->cu_ptr) {
            free(b);
            return NULL;
        }
        return b;
    }
    tz_cl_int rc = TZ_CL_SUCCESS;
    tz_cl_mem mem = tz_cl.create_buf(ctx->cl_ctx, TZ_CL_MEM_READ_WRITE, (size_t)count * sizeof(float), NULL, &rc);
    if (!mem || rc != TZ_CL_SUCCESS) { if (mem) tz_cl.release_buf(mem); free(b); return NULL; }
    b->mem = mem;
    return b;
}

void tz_gpu_buf_free(void* bp) {
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!b) return;
    if (b->backend_kind == TZ_GPU_BK_CUDA) {
        if (b->cu_ptr && tz_cu.mem_free) tz_cu.mem_free(b->cu_ptr);
    } else if (b->mem) {
        tz_cl.release_buf(b->mem);
    }
    free(b);
}

int32_t tz_gpu_buf_upload(void* cp, void* bp, const float* src, int64_t count, double* out_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!ctx || !b || !src || count <= 0 || (size_t)count > b->count || ctx->backend_kind != b->backend_kind) return 0;
    size_t bytes = (size_t)count * sizeof(float);
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        double t0 = tz_gpu_now_ms();
        if (tz_cu.memcpy_h2d(b->cu_ptr, src, bytes) != 0) return 0;
        double t1 = tz_gpu_now_ms();
        if (out_ms) *out_ms = t1 - t0;
        return 1;
    }
    tz_cl_event ev = NULL;
    if (tz_cl.write_buf(ctx->queue, b->mem, TZ_CL_TRUE, 0u, bytes, src, 0u, NULL, &ev) != TZ_CL_SUCCESS) {
        if (ev) tz_cl.release_event(ev);
        return 0;
    }
    double ms = tz_gpu_ev_ms(ev);
    if (out_ms) *out_ms = ms;
    return 1;
}

int32_t tz_gpu_buf_download(void* cp, void* bp, float* dst, int64_t count, double* out_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!ctx || !b || !dst || count <= 0 || (size_t)count > b->count || ctx->backend_kind != b->backend_kind) return 0;
    size_t bytes = (size_t)count * sizeof(float);
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        double t0 = tz_gpu_now_ms();
        if (tz_cu.memcpy_d2h(dst, b->cu_ptr, bytes) != 0) return 0;
        double t1 = tz_gpu_now_ms();
        if (out_ms) *out_ms = t1 - t0;
        return 1;
    }
    tz_cl_event ev = NULL;
    if (tz_cl.read_buf(ctx->queue, b->mem, TZ_CL_TRUE, 0u, bytes, dst, 0u, NULL, &ev) != TZ_CL_SUCCESS) {
        if (ev) tz_cl.release_event(ev);
        return 0;
    }
    double ms = tz_gpu_ev_ms(ev);
    if (out_ms) *out_ms = ms;
    return 1;
}

static int tz_gpu_ensure_staging(tz_gpu_ctx* ctx, size_t bytes) {
    if (ctx->stg_mem && ctx->stg_cap >= bytes) return 1;
    if (ctx->stg_mem) { tz_cl.release_buf(ctx->stg_mem); ctx->stg_mem = NULL; ctx->stg_cap = 0u; }
    tz_cl_int sr = TZ_CL_SUCCESS;
    ctx->stg_mem = tz_cl.create_buf(ctx->cl_ctx, TZ_CL_MEM_READ_WRITE | TZ_CL_MEM_ALLOC_HOST_PTR, bytes, NULL, &sr);
    if (!ctx->stg_mem || sr != TZ_CL_SUCCESS) {
        if (ctx->stg_mem) { tz_cl.release_buf(ctx->stg_mem); ctx->stg_mem = NULL; }
        return 0;
    }
    ctx->stg_cap = bytes;
    return 1;
}

int32_t tz_gpu_buf_upload_pinned(void* cp, void* bp, const float* src, int64_t count, double* out_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!ctx || !b || !src || count <= 0 || (size_t)count > b->count || ctx->backend_kind != b->backend_kind) return 0;
    if (ctx->backend_kind == TZ_GPU_BK_CUDA || !tz_cl.map_buf || !tz_cl.unmap_mem || !tz_cl.copy_buf) {
        return tz_gpu_buf_upload(cp, bp, src, count, out_ms);
    }
    size_t bytes = (size_t)count * sizeof(float);
    if (!tz_gpu_ensure_staging(ctx, bytes)) {
        return tz_gpu_buf_upload(cp, bp, src, count, out_ms);
    }
    double t0 = tz_gpu_now_ms();
    tz_cl_int mr = TZ_CL_SUCCESS;
    void* mapped = tz_cl.map_buf(ctx->queue, ctx->stg_mem, TZ_CL_TRUE, TZ_CL_MAP_WRITE, 0u, bytes, 0u, NULL, NULL, &mr);
    if (!mapped || mr != TZ_CL_SUCCESS) {
        return tz_gpu_buf_upload(cp, bp, src, count, out_ms);
    }
    memcpy(mapped, src, bytes);
    tz_cl.unmap_mem(ctx->queue, ctx->stg_mem, mapped, 0u, NULL, NULL);
    tz_cl_event ev = NULL;
    if (tz_cl.copy_buf(ctx->queue, ctx->stg_mem, b->mem, 0u, 0u, bytes, 0u, NULL, &ev) != TZ_CL_SUCCESS) {
        if (ev) tz_cl.release_event(ev);
        return tz_gpu_buf_upload(cp, bp, src, count, out_ms);
    }
    tz_cl.finish(ctx->queue);
    if (ev) tz_cl.release_event(ev);
    double t1 = tz_gpu_now_ms();
    if (out_ms) *out_ms = t1 - t0;
    return 1;
}

int32_t tz_gpu_buf_download_pinned(void* cp, void* bp, float* dst, int64_t count, double* out_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!ctx || !b || !dst || count <= 0 || (size_t)count > b->count || ctx->backend_kind != b->backend_kind) return 0;
    if (ctx->backend_kind == TZ_GPU_BK_CUDA || !tz_cl.map_buf || !tz_cl.unmap_mem || !tz_cl.copy_buf) {
        return tz_gpu_buf_download(cp, bp, dst, count, out_ms);
    }
    size_t bytes = (size_t)count * sizeof(float);
    if (!tz_gpu_ensure_staging(ctx, bytes)) {
        return tz_gpu_buf_download(cp, bp, dst, count, out_ms);
    }
    double t0 = tz_gpu_now_ms();
    tz_cl_event ev = NULL;
    if (tz_cl.copy_buf(ctx->queue, b->mem, ctx->stg_mem, 0u, 0u, bytes, 0u, NULL, &ev) != TZ_CL_SUCCESS) {
        if (ev) tz_cl.release_event(ev);
        return tz_gpu_buf_download(cp, bp, dst, count, out_ms);
    }
    tz_cl_int mr = TZ_CL_SUCCESS;
    void* mapped = tz_cl.map_buf(ctx->queue, ctx->stg_mem, TZ_CL_TRUE, TZ_CL_MAP_READ, 0u, bytes, ev ? 1u : 0u, ev ? &ev : NULL, NULL, &mr);
    if (ev) tz_cl.release_event(ev);
    if (!mapped || mr != TZ_CL_SUCCESS) {
        return tz_gpu_buf_download(cp, bp, dst, count, out_ms);
    }
    memcpy(dst, mapped, bytes);
    tz_cl.unmap_mem(ctx->queue, ctx->stg_mem, mapped, 0u, NULL, NULL);
    tz_cl.finish(ctx->queue);
    double t1 = tz_gpu_now_ms();
    if (out_ms) *out_ms = t1 - t0;
    return 1;
}

static int32_t tz_gpu_saxpy_resident_common(void* cp, float alpha, void* bx_p, void* by_p, int64_t n,
                                            int fused_relu, double* out_kern_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* bx = (tz_gpu_buf*)bx_p;
    tz_gpu_buf* by = (tz_gpu_buf*)by_p;
    if (!ctx || !bx || !by || n <= 0 || (size_t)n > bx->count || (size_t)n > by->count ||
        ctx->backend_kind != bx->backend_kind || ctx->backend_kind != by->backend_kind) return 0;
    uint64_t nu = (uint64_t)n;
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        tz_cu_fn fn = fused_relu ? ctx->cu_k_saxpy_relu : ctx->cu_k_saxpy;
        void* params[4] = {&alpha, &bx->cu_ptr, &by->cu_ptr, &nu};
        double t0 = tz_gpu_now_ms();
        int ok = tz_gpu_cu_launch_1d(fn, nu, params);
        double t1 = tz_gpu_now_ms();
        if (ok && out_kern_ms) *out_kern_ms = t1 - t0;
        return ok;
    }
    tz_cl_kernel kern = fused_relu ? ctx->k_saxpy_relu : ctx->k_saxpy;
    tz_cl_event ek = NULL;
    if (!tz_gpu_launch_saxpy_kern(ctx, kern, alpha, bx->mem, by->mem, nu, &ek)) {
        if (ek) tz_cl.release_event(ek);
        return 0;
    }
    tz_cl.wait_events(1u, &ek);
    double ms = tz_gpu_ev_ms(ek);
    if (out_kern_ms) *out_kern_ms = ms;
    return 1;
}

int32_t tz_gpu_saxpy_resident_f32(void* cp, float alpha, void* bx_p, void* by_p, int64_t n, double* out_kern_ms) {
    return tz_gpu_saxpy_resident_common(cp, alpha, bx_p, by_p, n, 0, out_kern_ms);
}

int32_t tz_gpu_saxpy_relu_resident_f32(void* cp, float alpha, void* bx_p, void* by_p, int64_t n, double* out_kern_ms) {
    return tz_gpu_saxpy_resident_common(cp, alpha, bx_p, by_p, n, 1, out_kern_ms);
}

static int32_t tz_gpu_matmul_resident_common(void* cp, void* ba_p, void* bb_p, void* bc_p,
                                             int64_t m, int64_t k, int64_t n, int fused_relu, double* out_kern_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* ba = (tz_gpu_buf*)ba_p;
    tz_gpu_buf* bb = (tz_gpu_buf*)bb_p;
    tz_gpu_buf* bc = (tz_gpu_buf*)bc_p;
    if (!ctx || !ba || !bb || !bc || m <= 0 || k <= 0 || n <= 0 ||
        m > 0xFFFFFFFFLL || k > 0xFFFFFFFFLL || n > 0xFFFFFFFFLL ||
        (uint64_t)m > ba->count / (uint64_t)k ||
        (uint64_t)k > bb->count / (uint64_t)n ||
        (uint64_t)m > bc->count / (uint64_t)n ||
        ctx->backend_kind != ba->backend_kind || ctx->backend_kind != bb->backend_kind ||
        ctx->backend_kind != bc->backend_kind) {
        return 0;
    }
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        tz_cu_fn fn = fused_relu ? ctx->cu_k_matmul_relu : ctx->cu_k_matmul;
        double t0 = tz_gpu_now_ms();
        int ok = tz_gpu_cu_launch_matmul(fn, ba->cu_ptr, bb->cu_ptr, bc->cu_ptr, (uint32_t)m, (uint32_t)k, (uint32_t)n);
        double t1 = tz_gpu_now_ms();
        if (ok && out_kern_ms) *out_kern_ms = t1 - t0;
        return ok;
    }
    tz_cl_kernel k64 = fused_relu ? ctx->k_matmul64_relu : ctx->k_matmul64;
    tz_cl_kernel k16 = fused_relu ? ctx->k_matmul_relu : ctx->k_matmul;
    tz_cl_event ek = NULL;
    if (!tz_gpu_launch_matmul_kern(ctx, k64, k16, ba->mem, bb->mem, bc->mem, (uint32_t)m, (uint32_t)k, (uint32_t)n, &ek)) {
        if (ek) tz_cl.release_event(ek);
        return 0;
    }
    tz_cl.wait_events(1u, &ek);
    double ms = tz_gpu_ev_ms(ek);
    if (out_kern_ms) *out_kern_ms = ms;
    return 1;
}

int32_t tz_gpu_matmul_resident_f32(void* cp, void* ba_p, void* bb_p, void* bc_p,
                                   int64_t m, int64_t k, int64_t n, double* out_kern_ms) {
    return tz_gpu_matmul_resident_common(cp, ba_p, bb_p, bc_p, m, k, n, 0, out_kern_ms);
}

int32_t tz_gpu_matmul_relu_resident_f32(void* cp, void* ba_p, void* bb_p, void* bc_p,
                                        int64_t m, int64_t k, int64_t n, double* out_kern_ms) {
    return tz_gpu_matmul_resident_common(cp, ba_p, bb_p, bc_p, m, k, n, 1, out_kern_ms);
}

int32_t tz_gpu_relu_resident_f32(void* cp, void* bx_p, void* by_p, int64_t n, double* out_kern_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* bx = (tz_gpu_buf*)bx_p;
    tz_gpu_buf* by = (tz_gpu_buf*)by_p;
    if (!ctx || !bx || !by || n <= 0 || (size_t)n > bx->count || (size_t)n > by->count ||
        ctx->backend_kind != bx->backend_kind || ctx->backend_kind != by->backend_kind) return 0;
    uint64_t nu = (uint64_t)n;
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        void* params[3] = {&bx->cu_ptr, &by->cu_ptr, &nu};
        double t0 = tz_gpu_now_ms();
        int ok = tz_gpu_cu_launch_1d(ctx->cu_k_relu, nu, params);
        double t1 = tz_gpu_now_ms();
        if (ok && out_kern_ms) *out_kern_ms = t1 - t0;
        return ok;
    }
    if (!ctx->k_relu) return 0;
    size_t local = 256u;
    size_t global = (size_t)(((nu + local - 1u) / local) * local);
    tz_cl_int s = tz_cl.set_arg(ctx->k_relu, 0u, sizeof(tz_cl_mem), &bx->mem) |
                  tz_cl.set_arg(ctx->k_relu, 1u, sizeof(tz_cl_mem), &by->mem) |
                  tz_cl.set_arg(ctx->k_relu, 2u, sizeof(tz_cl_ulong), &nu);
    tz_cl_event ek = NULL;
    if (s != TZ_CL_SUCCESS ||
        tz_cl.enqueue_nd(ctx->queue, ctx->k_relu, 1u, NULL, &global, &local, 0u, NULL, &ek) != TZ_CL_SUCCESS) {
        if (ek) tz_cl.release_event(ek);
        return 0;
    }
    tz_cl.wait_events(1u, &ek);
    double ms = tz_gpu_ev_ms(ek);
    if (out_kern_ms) *out_kern_ms = ms;
    return 1;
}

int32_t tz_gpu_vec_add_resident_f32(void* cp, void* ba_p, void* bb_p, void* bo_p, int64_t n, double* out_kern_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* ba = (tz_gpu_buf*)ba_p;
    tz_gpu_buf* bb = (tz_gpu_buf*)bb_p;
    tz_gpu_buf* bo = (tz_gpu_buf*)bo_p;
    if (!ctx || !ba || !bb || !bo || n <= 0 || (size_t)n > ba->count ||
        (size_t)n > bb->count || (size_t)n > bo->count ||
        ctx->backend_kind != ba->backend_kind || ctx->backend_kind != bb->backend_kind ||
        ctx->backend_kind != bo->backend_kind) return 0;
    uint64_t nu = (uint64_t)n;
    if (ctx->backend_kind == TZ_GPU_BK_CUDA) {
        void* params[4] = {&ba->cu_ptr, &bb->cu_ptr, &bo->cu_ptr, &nu};
        double t0 = tz_gpu_now_ms();
        int ok = tz_gpu_cu_launch_1d(ctx->cu_k_vec_add, nu, params);
        double t1 = tz_gpu_now_ms();
        if (ok && out_kern_ms) *out_kern_ms = t1 - t0;
        return ok;
    }
    if (!ctx->k_vec_add) return 0;
    size_t local = 256u;
    size_t global = (size_t)(((nu + local - 1u) / local) * local);
    tz_cl_int s = tz_cl.set_arg(ctx->k_vec_add, 0u, sizeof(tz_cl_mem), &ba->mem) |
                  tz_cl.set_arg(ctx->k_vec_add, 1u, sizeof(tz_cl_mem), &bb->mem) |
                  tz_cl.set_arg(ctx->k_vec_add, 2u, sizeof(tz_cl_mem), &bo->mem) |
                  tz_cl.set_arg(ctx->k_vec_add, 3u, sizeof(tz_cl_ulong), &nu);
    tz_cl_event ek = NULL;
    if (s != TZ_CL_SUCCESS ||
        tz_cl.enqueue_nd(ctx->queue, ctx->k_vec_add, 1u, NULL, &global, &local, 0u, NULL, &ek) != TZ_CL_SUCCESS) {
        if (ek) tz_cl.release_event(ek);
        return 0;
    }
    tz_cl.wait_events(1u, &ek);
    double ms = tz_gpu_ev_ms(ek);
    if (out_kern_ms) *out_kern_ms = ms;
    return 1;
}
)c";

} // namespace gpu_rt_parts

inline std::string turiGpuRuntimeC() {
    std::string out;
    out.reserve(49152);
    out += gpu_rt_parts::kGpuHeader;
    out += gpu_rt_parts::kGpuLoader;
    out += gpu_rt_parts::kGpuKernels;
    out += gpu_rt_parts::kGpuKernelsMatmul;
    out += gpu_rt_parts::kGpuCache;
    out += gpu_rt_parts::kGpuMultiBackend;
    out += gpu_rt_parts::kGpuApi;
    out += gpu_rt_parts::kGpuApiOps;
    out += gpu_rt_parts::kGpuApiBody;
    return out;
}

} // namespace veloz::native
