#pragma once

#include <string>

namespace veloz::native {

namespace gpu_rt_parts {

inline constexpr const char* kGpuHeader = R"c(
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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
#define TZ_CL_MEM_COPY_HOST_PTR (1ULL << 5)
#define TZ_CL_PROGRAM_BINARY_SIZES 0x1165u
#define TZ_CL_PROGRAM_BINARIES 0x1166u
#define TZ_CL_PROGRAM_BUILD_LOG 0x1183u
#define TZ_CL_PROFILING_COMMAND_START 0x1282u
#define TZ_CL_PROFILING_COMMAND_END 0x1283u

static _Thread_local char tz_gpu_tls_err[512];
static _Thread_local char tz_gpu_tls_name[256];
static _Thread_local int64_t tz_gpu_last_cache_hit = 0;

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
} tz_cl = {0};

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
    tz_cl.create_prog_bin = (tz_pfn_clCreateProgramWithBinary)(void*)
#ifdef _WIN32
        GetProcAddress(mod, "clCreateProgramWithBinary");
#else
        dlsym(mod, "clCreateProgramWithBinary");
#endif
    tz_cl.get_prog_info = (tz_pfn_clGetProgramInfo)(void*)
#ifdef _WIN32
        GetProcAddress(mod, "clGetProgramInfo");
#else
        dlsym(mod, "clGetProgramInfo");
#endif
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
    h = tz_fnv1a64(h, "|v2", 3u);

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

inline constexpr const char* kGpuApi = R"c(
typedef struct tz_gpu_ctx {
    tz_cl_device_id dev;
    tz_cl_context cl_ctx;
    tz_cl_command_queue queue;
    tz_cl_program prog;
    tz_cl_kernel k_saxpy;
    tz_cl_kernel k_matmul;
    tz_cl_kernel k_matmul64;
    tz_cl_kernel k_relu;
    tz_cl_kernel k_vec_add;
    tz_cl_kernel k_vec_mul;
    tz_cl_ulong local_mem;
} tz_gpu_ctx;

typedef struct tz_gpu_buf {
    tz_cl_mem mem;
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
    tz_cl_device_id dev = NULL;
    return tz_gpu_pick_device(&dev, NULL, 0u) ? 1 : 0;
}

int64_t tz_gpu_cache_hit(void) {
    return tz_gpu_last_cache_hit;
}

const uint8_t* tz_gpu_device_name(int64_t* out_len) {
    tz_cl_device_id dev = NULL;
    tz_gpu_tls_name[0] = '\0';
    if (!tz_gpu_pick_device(&dev, tz_gpu_tls_name, sizeof(tz_gpu_tls_name))) {
        if (out_len) *out_len = 0;
        return (const uint8_t*)"";
    }
    if (out_len) *out_len = (int64_t)strlen(tz_gpu_tls_name);
    return (const uint8_t*)tz_gpu_tls_name;
}

void tz_gpu_free(void* cp) {
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    if (!c) return;
    if (c->k_saxpy) tz_cl.release_kernel(c->k_saxpy);
    if (c->k_matmul) tz_cl.release_kernel(c->k_matmul);
    if (c->k_matmul64) tz_cl.release_kernel(c->k_matmul64);
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
    tz_cl_device_id dev = NULL;
    if (!tz_gpu_pick_device(&dev, tz_gpu_tls_name, sizeof(tz_gpu_tls_name))) {
        tz_gpu_set_err(err_ptr, err_len, "No se encontró dispositivo GPU OpenCL disponible");
        return NULL;
    }
    tz_gpu_ctx* c = (tz_gpu_ctx*)calloc(1, sizeof(tz_gpu_ctx));
    if (!c) { tz_gpu_set_err(err_ptr, err_len, "Sin memoria para contexto GPU"); return NULL; }
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
    if (c->local_mem >= 40960u) {
        tz_cl_int rc64 = TZ_CL_SUCCESS;
        c->k_matmul64 = tz_cl.create_kernel(c->prog, "tz_matmul_f32_64", &rc64);
        if (rc64 != TZ_CL_SUCCESS && c->k_matmul64) { tz_cl.release_kernel(c->k_matmul64); c->k_matmul64 = NULL; }
    }
    tz_cl_int rce = TZ_CL_SUCCESS;
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

static int32_t tz_gpu_launch_saxpy(tz_gpu_ctx* c, float alpha, tz_cl_mem mx, tz_cl_mem my,
                                   uint64_t nu, tz_cl_event* out_ev) {
    size_t local = 256u;
    size_t global = (size_t)(((nu + local - 1u) / local) * local);
    tz_cl_int rc = TZ_CL_SUCCESS;
    rc |= tz_cl.set_arg(c->k_saxpy, 0u, sizeof(float), &alpha);
    rc |= tz_cl.set_arg(c->k_saxpy, 1u, sizeof(tz_cl_mem), &mx);
    rc |= tz_cl.set_arg(c->k_saxpy, 2u, sizeof(tz_cl_mem), &my);
    rc |= tz_cl.set_arg(c->k_saxpy, 3u, sizeof(tz_cl_ulong), &nu);
    if (rc != TZ_CL_SUCCESS) return 0;
    return tz_cl.enqueue_nd(c->queue, c->k_saxpy, 1u, NULL, &global, &local, 0u, NULL, out_ev) == TZ_CL_SUCCESS;
}

static int32_t tz_gpu_launch_matmul(tz_gpu_ctx* c, tz_cl_mem ma, tz_cl_mem mb, tz_cl_mem mc,
                                    uint32_t m, uint32_t k, uint32_t n, tz_cl_event* out_ev) {
    size_t local[2] = {16u, 16u};
    if (c->k_matmul64 && m >= 32u && k >= 32u && n >= 32u) {
        size_t g64[2] = {((size_t)n + 63u) / 64u * 16u, ((size_t)m + 63u) / 64u * 16u};
        tz_cl_int rc64 = TZ_CL_SUCCESS;
        rc64 |= tz_cl.set_arg(c->k_matmul64, 0u, sizeof(tz_cl_mem), &ma);
        rc64 |= tz_cl.set_arg(c->k_matmul64, 1u, sizeof(tz_cl_mem), &mb);
        rc64 |= tz_cl.set_arg(c->k_matmul64, 2u, sizeof(tz_cl_mem), &mc);
        rc64 |= tz_cl.set_arg(c->k_matmul64, 3u, sizeof(uint32_t), &m);
        rc64 |= tz_cl.set_arg(c->k_matmul64, 4u, sizeof(uint32_t), &k);
        rc64 |= tz_cl.set_arg(c->k_matmul64, 5u, sizeof(uint32_t), &n);
        if (rc64 == TZ_CL_SUCCESS &&
            tz_cl.enqueue_nd(c->queue, c->k_matmul64, 2u, NULL, g64, local, 0u, NULL, out_ev) == TZ_CL_SUCCESS) {
            return 1;
        }
    }
    size_t global[2] = {((size_t)n + 15u) / 16u * 16u, ((size_t)m + 15u) / 16u * 16u};
    tz_cl_int rc = TZ_CL_SUCCESS;
    rc |= tz_cl.set_arg(c->k_matmul, 0u, sizeof(tz_cl_mem), &ma);
    rc |= tz_cl.set_arg(c->k_matmul, 1u, sizeof(tz_cl_mem), &mb);
    rc |= tz_cl.set_arg(c->k_matmul, 2u, sizeof(tz_cl_mem), &mc);
    rc |= tz_cl.set_arg(c->k_matmul, 3u, sizeof(uint32_t), &m);
    rc |= tz_cl.set_arg(c->k_matmul, 4u, sizeof(uint32_t), &k);
    rc |= tz_cl.set_arg(c->k_matmul, 5u, sizeof(uint32_t), &n);
    if (rc != TZ_CL_SUCCESS) return 0;
    return tz_cl.enqueue_nd(c->queue, c->k_matmul, 2u, NULL, global, local, 0u, NULL, out_ev) == TZ_CL_SUCCESS;
}
)c";

inline constexpr const char* kGpuApiOps = R"c(
static int32_t tz_gpu_vec_binary(tz_gpu_ctx* c, tz_cl_kernel kern, const float* a, const float* b, float* out,
                                 int64_t n, double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                                 const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    if (!c || !kern || !a || !b || !out || n < 0 || (uint64_t)n > SIZE_MAX / sizeof(float)) {
        tz_gpu_set_err(err_ptr, err_len, "Argumentos inválidos en operación vectorial GPU");
        return 0;
    }
    if (out_h2d_ms) *out_h2d_ms = 0.0;
    if (out_kern_ms) *out_kern_ms = 0.0;
    if (out_d2h_ms) *out_d2h_ms = 0.0;
    if (n == 0) return 1;
    size_t bytes = (size_t)n * sizeof(float);
    uint64_t nu = (uint64_t)n;
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
    if (!c || !c->k_relu || !x || !y || n < 0 || (uint64_t)n > SIZE_MAX / sizeof(float)) {
        tz_gpu_set_err(err_ptr, err_len, "Argumentos inválidos en tz_gpu_relu_f32");
        return 0;
    }
    if (out_h2d_ms) *out_h2d_ms = 0.0;
    if (out_kern_ms) *out_kern_ms = 0.0;
    if (out_d2h_ms) *out_d2h_ms = 0.0;
    if (n == 0) return 1;
    size_t bytes = (size_t)n * sizeof(float);
    uint64_t nu = (uint64_t)n;
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
    return tz_gpu_vec_binary(c, c ? c->k_vec_add : NULL, a, b, out, n, out_h2d_ms, out_kern_ms, out_d2h_ms, err_ptr, err_len);
}

int32_t tz_gpu_vec_mul_f32(void* cp, const float* a, const float* b, float* out, int64_t n,
                           double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                           const void** err_ptr, int64_t* err_len) {
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    return tz_gpu_vec_binary(c, c ? c->k_vec_mul : NULL, a, b, out, n, out_h2d_ms, out_kern_ms, out_d2h_ms, err_ptr, err_len);
}
)c";

inline constexpr const char* kGpuApiBody = R"c(

int32_t tz_gpu_saxpy_f32(void* cp, float alpha, const float* x, float* y, int64_t n,
                         double* out_h2d_ms, double* out_kern_ms, double* out_d2h_ms,
                         const void** err_ptr, int64_t* err_len) {
    tz_gpu_set_err(err_ptr, err_len, NULL);
    tz_gpu_ctx* c = (tz_gpu_ctx*)cp;
    if (!c || !x || !y || n < 0 || (uint64_t)n > SIZE_MAX / sizeof(float)) {
        tz_gpu_set_err(err_ptr, err_len, "Argumentos inválidos en tz_gpu_saxpy_f32");
        return 0;
    }
    if (out_h2d_ms) *out_h2d_ms = 0.0;
    if (out_kern_ms) *out_kern_ms = 0.0;
    if (out_d2h_ms) *out_d2h_ms = 0.0;
    if (n == 0) return 1;

    size_t bytes = (size_t)n * sizeof(float);
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
        !tz_gpu_launch_saxpy(c, alpha, mx, my, (uint64_t)n, &ev_k) ||
        tz_cl.read_buf(c->queue, my, TZ_CL_TRUE, 0u, bytes, y, 0u, NULL, &ev_d2h) != TZ_CL_SUCCESS) {
        ok = 0;
        tz_gpu_set_err(err_ptr, err_len, "Fallo ejecutando saxpy OpenCL");
    }
    tz_cl.finish(c->queue);
    double h2d = tz_gpu_ev_ms(ev_hx) + tz_gpu_ev_ms(ev_hy);
    double kern = tz_gpu_ev_ms(ev_k);
    double d2h = tz_gpu_ev_ms(ev_d2h);
    if (out_h2d_ms) *out_h2d_ms = h2d;
    if (out_kern_ms) *out_kern_ms = kern;
    if (out_d2h_ms) *out_d2h_ms = d2h;
    tz_cl.release_buf(mx);
    tz_cl.release_buf(my);
    return ok;
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
    tz_cl_int rc = TZ_CL_SUCCESS;
    tz_cl_mem mem = tz_cl.create_buf(ctx->cl_ctx, TZ_CL_MEM_READ_WRITE, (size_t)count * sizeof(float), NULL, &rc);
    if (!mem || rc != TZ_CL_SUCCESS) { if (mem) tz_cl.release_buf(mem); return NULL; }
    tz_gpu_buf* b = (tz_gpu_buf*)malloc(sizeof(tz_gpu_buf));
    if (!b) { tz_cl.release_buf(mem); return NULL; }
    b->mem = mem;
    b->count = (size_t)count;
    return b;
}

void tz_gpu_buf_free(void* bp) {
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!b) return;
    if (b->mem) tz_cl.release_buf(b->mem);
    free(b);
}

int32_t tz_gpu_buf_upload(void* cp, void* bp, const float* src, int64_t count, double* out_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* b = (tz_gpu_buf*)bp;
    if (!ctx || !b || !src || count <= 0 || (size_t)count > b->count) return 0;
    tz_cl_event ev = NULL;
    if (tz_cl.write_buf(ctx->queue, b->mem, TZ_CL_TRUE, 0u, (size_t)count * sizeof(float), src, 0u, NULL, &ev) != TZ_CL_SUCCESS) {
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
    if (!ctx || !b || !dst || count <= 0 || (size_t)count > b->count) return 0;
    tz_cl_event ev = NULL;
    if (tz_cl.read_buf(ctx->queue, b->mem, TZ_CL_TRUE, 0u, (size_t)count * sizeof(float), dst, 0u, NULL, &ev) != TZ_CL_SUCCESS) {
        if (ev) tz_cl.release_event(ev);
        return 0;
    }
    double ms = tz_gpu_ev_ms(ev);
    if (out_ms) *out_ms = ms;
    return 1;
}

int32_t tz_gpu_saxpy_resident_f32(void* cp, float alpha, void* bx_p, void* by_p, int64_t n, double* out_kern_ms) {
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* bx = (tz_gpu_buf*)bx_p;
    tz_gpu_buf* by = (tz_gpu_buf*)by_p;
    if (!ctx || !bx || !by || n <= 0 || (size_t)n > bx->count || (size_t)n > by->count) return 0;
    tz_cl_event ek = NULL;
    if (!tz_gpu_launch_saxpy(ctx, alpha, bx->mem, by->mem, (uint64_t)n, &ek)) {
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
    tz_gpu_ctx* ctx = (tz_gpu_ctx*)cp;
    tz_gpu_buf* ba = (tz_gpu_buf*)ba_p;
    tz_gpu_buf* bb = (tz_gpu_buf*)bb_p;
    tz_gpu_buf* bc = (tz_gpu_buf*)bc_p;
    if (!ctx || !ba || !bb || !bc || m <= 0 || k <= 0 || n <= 0 ||
        m > 0xFFFFFFFFLL || k > 0xFFFFFFFFLL || n > 0xFFFFFFFFLL ||
        (uint64_t)m > ba->count / (uint64_t)k ||
        (uint64_t)k > bb->count / (uint64_t)n ||
        (uint64_t)m > bc->count / (uint64_t)n) {
        return 0;
    }
    tz_cl_event ek = NULL;
    if (!tz_gpu_launch_matmul(ctx, ba->mem, bb->mem, bc->mem, (uint32_t)m, (uint32_t)k, (uint32_t)n, &ek)) {
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
    out.reserve(32768);
    out += gpu_rt_parts::kGpuHeader;
    out += gpu_rt_parts::kGpuLoader;
    out += gpu_rt_parts::kGpuKernels;
    out += gpu_rt_parts::kGpuKernelsMatmul;
    out += gpu_rt_parts::kGpuCache;
    out += gpu_rt_parts::kGpuApi;
    out += gpu_rt_parts::kGpuApiOps;
    out += gpu_rt_parts::kGpuApiBody;
    return out;
}

} // namespace veloz::native
