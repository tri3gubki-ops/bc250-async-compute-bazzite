/* BC-250 / GFX1013 async-compute soak test.
 *
 * The short functional test (bc250-ace-test) proves the ACE queues work for
 * one-shot submissions. This one targets what the BC-250 *kernel* patches were
 * actually about, none of which a short test can reach:
 *
 *   - PASID TLB invalidation routed through KIQ (kernel patch 1 and 3):
 *     needs sustained load, many VM map/unmap cycles, and more than one
 *     process holding a VMID. We churn buffers periodically and expect a
 *     second process (vkcube) to be rendering at the same time.
 *   - GFXOFF while compute is idle (kernel patch 2): needs real idle gaps
 *     between bursts so power gating actually engages, then work again.
 *
 * Every round submits to all ACE queues plus the graphics queue at once and
 * verifies every result bit-exactly. Progress is fsynced so a hard hang still
 * says which round and which queue died.
 *
 * Usage: bc250-ace-soak [seconds]   (default 300)
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>

#include "ace_comp_spv.h"

#define MAX_Q 8

static FILE *progress;
static int failures;

static void note(const char *fmt, ...)
{
   va_list ap;
   char line[512];
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);
   printf("%s\n", line);
   fflush(stdout);
   if (progress) {
      fprintf(progress, "%s\n", line);
      fflush(progress);
      fsync(fileno(progress));
   }
}

#define CHECK(expr)                                                                                \
   do {                                                                                            \
      VkResult _r = (expr);                                                                        \
      if (_r != VK_SUCCESS) {                                                                      \
         note("FATAL %s:%d %s -> VkResult %d", __FILE__, __LINE__, #expr, (int)_r);                \
         exit(2);                                                                                  \
      }                                                                                            \
   } while (0)

static uint32_t expected(uint32_t i, uint32_t seed)
{
   uint32_t v = i ^ seed;
   for (int k = 0; k < 32; ++k)
      v = v * 1664525u + 1013904223u;
   return v;
}

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long read_long(const char *path, long fallback)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return fallback;
   long v = fallback;
   if (fscanf(f, "%ld", &v) != 1)
      v = fallback;
   fclose(f);
   return v;
}

/* hwmon indices are discovered once so the sampler stays cheap. */
static char gpu_hw[64], cpu_hw[64];

static void find_hwmon(void)
{
   for (int i = 0; i < 16; i++) {
      char p[96], name[64] = {0};
      snprintf(p, sizeof(p), "/sys/class/hwmon/hwmon%d/name", i);
      FILE *f = fopen(p, "r");
      if (!f)
         continue;
      if (fscanf(f, "%63s", name) == 1) {
         if (!strcmp(name, "amdgpu"))
            snprintf(gpu_hw, sizeof(gpu_hw), "/sys/class/hwmon/hwmon%d", i);
         else if (!strcmp(name, "k10temp"))
            snprintf(cpu_hw, sizeof(cpu_hw), "/sys/class/hwmon/hwmon%d", i);
      }
      fclose(f);
   }
}

static void sample(char *out, size_t n)
{
   char p[128];
   long gt = 0, ct = 0, pw = 0, fq = 0, mv = 0;
   if (*gpu_hw) {
      snprintf(p, sizeof(p), "%s/temp1_input", gpu_hw);   gt = read_long(p, 0);
      snprintf(p, sizeof(p), "%s/power1_average", gpu_hw); pw = read_long(p, 0);
      snprintf(p, sizeof(p), "%s/freq1_input", gpu_hw);    fq = read_long(p, 0);
      snprintf(p, sizeof(p), "%s/in0_input", gpu_hw);      mv = read_long(p, 0);
   }
   if (*cpu_hw) {
      snprintf(p, sizeof(p), "%s/temp1_input", cpu_hw);    ct = read_long(p, 0);
   }
   snprintf(out, n, "gpu %ld.%ldC %ldMHz %ldmV  apu %ld.%ldW  cpu %ld.%ldC", gt / 1000, (gt % 1000) / 100,
            fq / 1000000, mv, pw / 1000000, (pw % 1000000) / 100000, ct / 1000, (ct % 1000) / 100);
}

static long gpu_temp_mc(void)
{
   char p[128];
   if (!*gpu_hw)
      return 0;
   snprintf(p, sizeof(p), "%s/temp1_input", gpu_hw);
   return read_long(p, 0);
}

static long cpu_temp_mc(void)
{
   char p[128];
   if (!*cpu_hw)
      return 0;
   snprintf(p, sizeof(p), "%s/temp1_input", cpu_hw);
   return read_long(p, 0);
}

struct ctx {
   VkInstance instance;
   VkPhysicalDevice phys;
   VkDevice dev;
   VkPhysicalDeviceMemoryProperties mem;
   uint32_t gfx_family, ace_family, ace_count, nq;
   VkQueue q[MAX_Q];
   VkCommandPool pool[MAX_Q];
   const char *qname[MAX_Q];
};

static uint32_t find_mem(struct ctx *c, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < c->mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (c->mem.memoryTypes[i].propertyFlags & want) == want)
         return i;
   note("FATAL no memory type for 0x%x", want);
   exit(2);
}

struct buf {
   VkBuffer handle;
   VkDeviceMemory memory;
   void *mapped;
   VkDeviceSize size;
};

static struct buf make_buf(struct ctx *c, VkDeviceSize size)
{
   struct buf b = {.size = size};
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = size,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   CHECK(vkCreateBuffer(c->dev, &bi, NULL, &b.handle));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->dev, b.handle, &req);
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = find_mem(c, req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
   };
   CHECK(vkAllocateMemory(c->dev, &ai, NULL, &b.memory));
   CHECK(vkBindBufferMemory(c->dev, b.handle, b.memory, 0));
   CHECK(vkMapMemory(c->dev, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
   return b;
}

static void free_buf(struct ctx *c, struct buf *b)
{
   if (!b->handle)
      return;
   vkUnmapMemory(c->dev, b->memory);
   vkDestroyBuffer(c->dev, b->handle, NULL);
   vkFreeMemory(c->dev, b->memory, NULL);
   memset(b, 0, sizeof(*b));
}

struct pipe {
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout layout;
   VkPipeline pipeline;
   VkDescriptorPool desc_pool;
};

static struct pipe make_pipe(struct ctx *c)
{
   struct pipe p = {0};
   VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .descriptorCount = 1,
                                           .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo sli = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 1,
                                          .pBindings = &binding};
   CHECK(vkCreateDescriptorSetLayout(c->dev, &sli, NULL, &p.set_layout));
   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &p.set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pcr};
   CHECK(vkCreatePipelineLayout(c->dev, &pli, NULL, &p.layout));
   VkShaderModule module;
   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof(ace_comp_spv),
                                   .pCode = ace_comp_spv};
   CHECK(vkCreateShaderModule(c->dev, &smi, NULL, &module));
   VkComputePipelineCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = module,
                                                .pName = "main"},
                                      .layout = p.layout};
   CHECK(vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p.pipeline));
   vkDestroyShaderModule(c->dev, module, NULL);
   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 256};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                     .maxSets = 256,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &ps};
   CHECK(vkCreateDescriptorPool(c->dev, &dpi, NULL, &p.desc_pool));
   return p;
}

int main(int argc, char **argv)
{
   double duration = argc > 1 ? atof(argv[1]) : 300.0;
   progress = fopen("bc250-ace-soak.progress", "w");
   find_hwmon();
   note("BC-250 ACE soak: %.0f s, verifying every result bit-exactly", duration);

   struct ctx c = {0};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "bc250-ace-soak",
                            .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   CHECK(vkCreateInstance(&ii, NULL, &c.instance));

   uint32_t ndev = 0;
   CHECK(vkEnumeratePhysicalDevices(c.instance, &ndev, NULL));
   VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
   CHECK(vkEnumeratePhysicalDevices(c.instance, &ndev, devs));
   VkPhysicalDeviceProperties props;
   int found = 0;
   for (uint32_t i = 0; i < ndev && !found; i++) {
      vkGetPhysicalDeviceProperties(devs[i], &props);
      if (props.vendorID == 0x1002 && props.deviceID == 0x13fe) {
         c.phys = devs[i];
         found = 1;
      }
   }
   if (!found) {
      note("FATAL BC-250 not found");
      return 2;
   }

   uint32_t nfam = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nfam, NULL);
   VkQueueFamilyProperties *fams = calloc(nfam, sizeof(*fams));
   vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nfam, fams);
   int gfx = -1, ace = -1;
   for (uint32_t i = 0; i < nfam; i++) {
      int has_c = !!(fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT);
      int has_g = !!(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT);
      if (gfx < 0 && has_g)
         gfx = (int)i;
      if (ace < 0 && has_c && !has_g)
         ace = (int)i;
   }
   if (ace < 0) {
      note("RESULT no dedicated compute family -- nothing to soak");
      return 1;
   }
   c.gfx_family = (uint32_t)gfx;
   c.ace_family = (uint32_t)ace;
   c.ace_count = fams[ace].queueCount > MAX_Q - 1 ? MAX_Q - 1 : fams[ace].queueCount;
   note("graphics family %u, ACE family %u with %u queues", c.gfx_family, c.ace_family, c.ace_count);

   float prio[MAX_Q];
   for (int i = 0; i < MAX_Q; i++)
      prio[i] = 1.0f;
   VkDeviceQueueCreateInfo qi[2] = {
      {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
       .queueFamilyIndex = c.gfx_family,
       .queueCount = 1,
       .pQueuePriorities = prio},
      {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
       .queueFamilyIndex = c.ace_family,
       .queueCount = c.ace_count,
       .pQueuePriorities = prio},
   };
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .queueCreateInfoCount = 2,
                            .pQueueCreateInfos = qi};
   CHECK(vkCreateDevice(c.phys, &di, NULL, &c.dev));
   vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);

   /* queue 0 is the graphics ring doing compute, 1..n are the ACE rings */
   vkGetDeviceQueue(c.dev, c.gfx_family, 0, &c.q[0]);
   c.qname[0] = "gfx";
   for (uint32_t i = 0; i < c.ace_count; i++) {
      vkGetDeviceQueue(c.dev, c.ace_family, i, &c.q[1 + i]);
      c.qname[1 + i] = "ace";
   }
   c.nq = 1 + c.ace_count;
   for (uint32_t i = 0; i < c.nq; i++) {
      VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = i == 0 ? c.gfx_family : c.ace_family};
      CHECK(vkCreateCommandPool(c.dev, &pi, NULL, &c.pool[i]));
   }

   struct pipe p = make_pipe(&c);

   const uint32_t n = 1u << 18; /* 256k words per queue per round */
   struct buf bufs[MAX_Q] = {0};
   VkDescriptorSet sets[MAX_Q] = {0};
   for (uint32_t i = 0; i < c.nq; i++)
      bufs[i] = make_buf(&c, (VkDeviceSize)n * 4);

   double start = now_s(), last_report = start, last_idle = start;
   unsigned long round = 0, verified_words = 0, churns = 0, idles = 0;
   char s[160];
   sample(s, sizeof(s));
   note("start: %s", s);

   while (now_s() - start < duration && !failures) {
      /* Periodically tear down and rebuild the buffers. Every cycle forces VM
       * unmap/map and therefore TLB invalidation, which is the path the BC-250
       * kernel patches rewrote. */
      if (round && round % 16 == 0) {
         for (uint32_t i = 0; i < c.nq; i++) {
            vkFreeDescriptorSets(c.dev, p.desc_pool, 1, &sets[i]);
            sets[i] = VK_NULL_HANDLE;
            free_buf(&c, &bufs[i]);
            bufs[i] = make_buf(&c, (VkDeviceSize)n * 4);
         }
         churns++;
      }

      VkCommandBuffer cmds[MAX_Q];
      VkFence fences[MAX_Q];
      uint32_t seeds[MAX_Q];

      for (uint32_t i = 0; i < c.nq; i++) {
         if (!sets[i]) {
            VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = p.desc_pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &p.set_layout};
            CHECK(vkAllocateDescriptorSets(c.dev, &dai, &sets[i]));
            VkDescriptorBufferInfo dbi = {.buffer = bufs[i].handle, .range = VK_WHOLE_SIZE};
            VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = sets[i],
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &dbi};
            vkUpdateDescriptorSets(c.dev, 1, &w, 0, NULL);
         }
         memset(bufs[i].mapped, 0xcd, bufs[i].size);
         seeds[i] = (uint32_t)(round * 977 + i * 31 + 1);

         VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = c.pool[i],
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
         CHECK(vkAllocateCommandBuffers(c.dev, &cai, &cmds[i]));
         VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
         CHECK(vkBeginCommandBuffer(cmds[i], &cbi));
         vkCmdBindPipeline(cmds[i], VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
         vkCmdBindDescriptorSets(cmds[i], VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &sets[i], 0, NULL);
         uint32_t push[2] = {n, seeds[i]};
         vkCmdPushConstants(cmds[i], p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
         vkCmdDispatch(cmds[i], (n + 255) / 256, 1, 1);
         VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
         vkCmdPipelineBarrier(cmds[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                              &mb, 0, NULL, 0, NULL);
         CHECK(vkEndCommandBuffer(cmds[i]));
      }

      /* All queues in flight simultaneously: this is the contention we want. */
      for (uint32_t i = 0; i < c.nq; i++) {
         VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
         CHECK(vkCreateFence(c.dev, &fi, NULL, &fences[i]));
         VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &cmds[i]};
         VkResult r = vkQueueSubmit(c.q[i], 1, &si, fences[i]);
         if (r != VK_SUCCESS) {
            note("FAIL round %lu queue %u (%s): vkQueueSubmit -> %d", round, i, c.qname[i], (int)r);
            failures++;
         }
      }

      VkResult wr = vkWaitForFences(c.dev, c.nq, fences, VK_TRUE, 30ull * 1000000000ull);
      if (wr != VK_SUCCESS) {
         note("FAIL round %lu: vkWaitForFences -> %d (%s)", round, (int)wr,
              wr == VK_TIMEOUT ? "timeout: a ring stopped retiring" : "device lost: GPU hang");
         failures++;
      } else {
         for (uint32_t i = 0; i < c.nq; i++) {
            const uint32_t *got = bufs[i].mapped;
            for (uint32_t k = 0; k < n; k++) {
               if (got[k] != expected(k, seeds[i])) {
                  note("FAIL round %lu queue %u (%s): word %u = 0x%08x want 0x%08x", round, i, c.qname[i], k,
                       got[k], expected(k, seeds[i]));
                  failures++;
                  break;
               }
            }
            verified_words += n;
         }
      }

      for (uint32_t i = 0; i < c.nq; i++) {
         vkDestroyFence(c.dev, fences[i], NULL);
         vkFreeCommandBuffers(c.dev, c.pool[i], 1, &cmds[i]);
      }
      round++;

      /* Thermal guard: the profile limit is 85 C and the governor should throttle
       * first, so anything past 88 C means stop rather than push. */
      long gt = gpu_temp_mc(), ct = cpu_temp_mc();
      if (gt > 88000 || ct > 88000) {
         note("STOP thermal guard: gpu %ld.%ldC cpu %ld.%ldC", gt / 1000, (gt % 1000) / 100, ct / 1000,
              (ct % 1000) / 100);
         break;
      }

      double t = now_s();
      if (t - last_report >= 15.0) {
         sample(s, sizeof(s));
         note("t+%3.0fs round %lu  verified %lu Mword  churns %lu  idles %lu  |  %s", t - start, round,
              verified_words / 1000000, churns, idles, s);
         last_report = t;
      }
      /* Real idle gap so GFXOFF can engage between bursts. */
      if (t - last_idle >= 30.0) {
         note("      idle gap 5 s (letting GFXOFF engage)");
         sleep(5);
         idles++;
         last_idle = now_s();
      }
   }

   vkDeviceWaitIdle(c.dev);
   sample(s, sizeof(s));
   note("end: %s", s);
   note("rounds %lu, verified %lu Mword, buffer churns %lu, idle gaps %lu", round, verified_words / 1000000,
        churns, idles);
   note(failures ? "RESULT FAIL (%d failures)" : "RESULT PASS (async compute stable under sustained load)",
        failures);

   for (uint32_t i = 0; i < c.nq; i++) {
      free_buf(&c, &bufs[i]);
      vkDestroyCommandPool(c.dev, c.pool[i], NULL);
   }
   vkDestroyDescriptorPool(c.dev, p.desc_pool, NULL);
   vkDestroyPipeline(c.dev, p.pipeline, NULL);
   vkDestroyPipelineLayout(c.dev, p.layout, NULL);
   vkDestroyDescriptorSetLayout(c.dev, p.set_layout, NULL);
   vkDestroyDevice(c.dev, NULL);
   vkDestroyInstance(c.instance, NULL);
   return failures ? 1 : 0;
}
