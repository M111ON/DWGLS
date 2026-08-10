/*
 * bench/vulkan_bw_pull.c — Vulkan bandwidth puller on real .gcube data
 *
 * Mirrors gpu_jet_puller.cu (CUDA) but on Vulkan — device driver of this
 * machine (GTX 1050 Ti) benchmarks faster on Vulkan than CUDA per user's
 * repeated testing. XOR checksum kernel (lesson #16) prevents compiler
 * from eliminating loads; measures REAL device read bandwidth.
 *
 * Flow:  load .gcube → stage → device buffer → dispatch XOR kernel
 *        → report GB/s of device-side reads (the actual pull rate).
 *
 * Build (MSYS2):  gcc -O2 -I/c/msys64/mingw64/include -L/c/msys64/mingw64/lib \
 *                 -o build/vulkan_bw_pull bench/vulkan_bw_pull.c -lvulkan-1
 * Shader: glslangValidator (WSL) → build/vulkan_pull.spv
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <vulkan/vulkan.h>

#define CHECK_VK(call) do { VkResult _r_ = (call); if (_r_ != VK_SUCCESS) { \
    fprintf(stderr, "VK ERROR %d at %s:%d (%s)\n", _r_, __FILE__, __LINE__, #call); \
    return -1; } } while (0)

static double now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* read whole file into memory (simple; .gcube up to ~700MB) */
static uint8_t *load_file(const char *path, size_t *sz_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { fprintf(stderr, "short read\n"); free(buf); return NULL; }
    *sz_out = (size_t)sz;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vulkan_bw_pull <file.bin>\n"); return 2; }
    size_t fsz = 0;
    uint8_t *src = load_file(argv[1], &fsz);
    if (!src) return 1;
    printf("Vulkan BW Pull — file %s (%u MB)\n", argv[1], (unsigned)(fsz >> 20));

    uint32_t CHUNK = 1024;           /* bytes per workgroup (XOR-validated config) */
    uint32_t n_chunks = (uint32_t)(fsz / CHUNK);
    printf("  chunks=%u x %uB\n", n_chunks, CHUNK);

    /* ── instance ── */
    VkInstance inst; VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    CHECK_VK(vkCreateInstance(&ici, NULL, &inst));

    /* ── physical device: pick first with compute ── */
    uint32_t ndev = 0; vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    if (!ndev) { fprintf(stderr, "no Vulkan device\n"); return 1; }
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props[8];
    VkPhysicalDevice pdevs[8]; vkEnumeratePhysicalDevices(inst, &ndev, pdevs);
    if (ndev > 8) ndev = 8;
    for (uint32_t i = 0; i < ndev; i++) {
        vkGetPhysicalDeviceProperties(pdevs[i], &props[i]);
        /* prefer discrete GPU */
        if (props[i].deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && pdev == VK_NULL_HANDLE)
            pdev = pdevs[i];
    }
    if (pdev == VK_NULL_HANDLE) pdev = pdevs[0];
    VkPhysicalDeviceProperties psel; vkGetPhysicalDeviceProperties(pdev, &psel);
    printf("  device  : %s (%s)\n", psel.deviceName,
           psel.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "integrated");

    /* ── queue family with compute ── */
    uint32_t nqf = 0; vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nqf, NULL);
    VkQueueFamilyProperties qfp[16]; if (nqf > 16) nqf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nqf, qfp);
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++)
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = i; break; }
    if (qfam == UINT32_MAX) { fprintf(stderr, "no compute queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &dqci };
    VkDevice dev; CHECK_VK(vkCreateDevice(pdev, &dci, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, qfam, 0, &q);

    /* ── find host-visible memory (staging + source copy) ── */
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    uint32_t m_host = UINT32_MAX, m_dev = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryType mt = mp.memoryTypes[i];
        if (m_host == UINT32_MAX && (mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            m_host = i;
        if (m_dev == UINT32_MAX && (mt.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            m_dev = i;
    }

    /* ── buffers ── */
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = fsz, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer srcBuf, dstBuf;
    VkDeviceMemory srcMem, dstMem;
    uint32_t sums_bytes = n_chunks * 4;

    CHECK_VK(vkCreateBuffer(dev, &bci, NULL, &srcBuf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, srcBuf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = m_dev };
    CHECK_VK(vkAllocateMemory(dev, &mai, NULL, &srcMem));
    CHECK_VK(vkBindBufferMemory(dev, srcBuf, srcMem, 0));

    /* staging (host-visible) */
    VkBuffer stageBuf; VkDeviceMemory stageMem;
    VkBufferCreateInfo sbci = bci; sbci.size = fsz;
    sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    CHECK_VK(vkCreateBuffer(dev, &sbci, NULL, &stageBuf));
    vkGetBufferMemoryRequirements(dev, stageBuf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = m_host;
    CHECK_VK(vkAllocateMemory(dev, &mai, NULL, &stageMem));
    CHECK_VK(vkBindBufferMemory(dev, stageBuf, stageMem, 0));

    /* dst (sums) host-visible */
    VkBufferCreateInfo dbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sums_bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK_VK(vkCreateBuffer(dev, &dbci, NULL, &dstBuf));
    vkGetBufferMemoryRequirements(dev, dstBuf, &mr);
    mai.allocationSize = mr.size; mai.memoryTypeIndex = m_host;
    CHECK_VK(vkAllocateMemory(dev, &mai, NULL, &dstMem));
    CHECK_VK(vkBindBufferMemory(dev, dstBuf, dstMem, 0));

    /* ── upload: host→stage→device, timed ── */
    void *map; CHECK_VK(vkMapMemory(dev, stageMem, 0, fsz, 0, &map));
    double t0 = now_ms();
    memcpy(map, src, fsz);
    vkUnmapMemory(dev, stageMem);
    double t1 = now_ms();
    printf("  host copy: %.1f ms (%.1f MB/s)\n", t1 - t0, fsz / 1048576.0 / ((t1 - t0) / 1000.0));

    /* command pool / buffers */
    VkCommandPool cp; VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfam };
    CHECK_VK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBuffer cb; VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK_VK(vkAllocateCommandBuffers(dev, &cai, &cb));

    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK_VK(vkBeginCommandBuffer(cb, &cbbi));
    VkBufferCopy bc = { .srcOffset = 0, .dstOffset = 0, .size = fsz };
    vkCmdCopyBuffer(cb, stageBuf, srcBuf, 1, &bc);
    VkBufferMemoryBarrier bar0 = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = srcBuf, .offset = 0, .size = fsz };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 1, &bar0, 0, NULL);
    CHECK_VK(vkEndCommandBuffer(cb));

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    t0 = now_ms();
    CHECK_VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK_VK(vkDeviceWaitIdle(dev));
    t1 = now_ms();
    printf("  upload   : %.1f ms (%.1f MB/s) [stage→device]\n", t1 - t0, fsz / 1048576.0 / ((t1 - t0) / 1000.0));

    /* ── shader module ── */
    FILE *spv = fopen("build/vulkan_pull.spv", "rb");
    if (!spv) { fprintf(stderr, "missing build/vulkan_pull.spv — compile with glslangValidator first\n"); return 1; }
    fseek(spv, 0, SEEK_END); long n = ftell(spv); fseek(spv, 0, SEEK_SET);
    uint32_t *code = (uint32_t *)malloc((size_t)n); fread(code, 1, (size_t)n, spv); fclose(spv);
    VkShaderModule sm; VkShaderModuleCreateInfo smci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)n, .pCode = code };
    CHECK_VK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    free(code);

    /* descriptor set */
    VkDescriptorSetLayoutBinding binds[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl; CHECK_VK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 4 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl; CHECK_VK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));

    VkPipelineShaderStageCreateInfo psci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" };
    VkComputePipelineCreateInfo cpci2 = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = psci, .layout = pl };
    VkPipeline pipe; CHECK_VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci2, NULL, &pipe));

    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool dp; CHECK_VK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds; CHECK_VK(vkAllocateDescriptorSets(dev, &dsai, &ds));
    VkDescriptorBufferInfo dbi0 = { .buffer = srcBuf, .offset = 0, .range = fsz };
    VkDescriptorBufferInfo dbi1 = { .buffer = dstBuf, .offset = 0, .range = sums_bytes };
    VkWriteDescriptorSet wds[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi0 },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi1 } };
    vkUpdateDescriptorSets(dev, 2, wds, 0, NULL);

    /* ── dispatch: 5 passes, timing each ── */
    uint32_t chunk_uints = CHUNK / 4; /* 256 */
    printf("  dispatch: %u workgroups x 256 threads, %u B per wg\n", n_chunks, CHUNK);

    double best = 1e18;
    VkCommandBufferBeginInfo cbb2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    for (int pass = 0; pass < 5; pass++) {
        CHECK_VK(vkBeginCommandBuffer(cb, &cbb2));
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &chunk_uints);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdDispatch(cb, n_chunks, 1, 1);
        CHECK_VK(vkEndCommandBuffer(cb));
        VkSubmitInfo si2 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        t0 = now_ms();
        CHECK_VK(vkQueueSubmit(q, 1, &si2, VK_NULL_HANDLE));
        CHECK_VK(vkDeviceWaitIdle(dev));
        t1 = now_ms();
        double dt = t1 - t0;
        if (dt < best) best = dt;
        double gbps = fsz / 1048576.0 / (dt / 1000.0) / 1024.0;
        printf("  pass %d: %.1f ms  = %.2f GB/s\n", pass, dt, gbps);
    }
    printf("  BEST    : %.2f ms  = %.2f GB/s (device read, XOR kernel)\n",
           best, fsz / 1048576.0 / (best / 1000.0) / 1024.0);
    printf("  effective: pulls %u chunks / pass @ 1024B\n", n_chunks);

    /* read back sums to prove the reads happened (checksum sanity) */
    void *smap; CHECK_VK(vkMapMemory(dev, dstMem, 0, sums_bytes, 0, &smap));
    volatile uint32_t *sums = (volatile uint32_t *)smap;
    /* CPU reference: XOR of first 4 chunks (1024B = 256 uints each) */
    uint32_t ref[4] = {0};
    for (int c = 0; c < 4; c++)
        for (uint32_t i = 0; i < chunk_uints; i++)
            ref[c] ^= ((const uint32_t *)src)[c * chunk_uints + i];
    printf("  GPU sums[0..3] = %08X %08X %08X %08X\n",
           (unsigned)sums[0], (unsigned)sums[1], (unsigned)sums[2], (unsigned)sums[3]);
    printf("  CPU ref [0..3] = %08X %08X %08X %08X %s\n",
           ref[0], ref[1], ref[2], ref[3],
           memcmp((void*)sums, ref, 16) == 0 ? "MATCH ✓ (data verified)" : "MISMATCH ✗");
    vkUnmapMemory(dev, dstMem);

    /* cleanup */
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyBuffer(dev, srcBuf, NULL);   vkFreeMemory(dev, srcMem, NULL);
    vkDestroyBuffer(dev, stageBuf, NULL); vkFreeMemory(dev, stageMem, NULL);
    vkDestroyBuffer(dev, dstBuf, NULL);   vkFreeMemory(dev, dstMem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    free(src);
    return 0;
}