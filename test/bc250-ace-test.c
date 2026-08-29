/* BC-250 / GFX1013 async-compute (ACE) queue test.
 *
 * Purpose: decide whether the dedicated compute queue family that a patched
 * RADV exposes on GFX1013 actually works on a stock kernel, or whether the
 * BC-250 kernel patches are required.
 *
 * What it exercises, in the order the upstream patch says things break:
 *   1. vkCmdFillBuffer on the ACE queue   -- RADV meta shader path
 *   2. vkCmdCopyBuffer on the ACE queue   -- RADV meta shader path
 *   3. plain compute dispatch on the ACE queue, verified bit-exact
 *   4. a large dispatch, to hit the threadgroup-dimension workaround
 *   5. the same work on all ACE queues at once, then repeated, to catch
 *      lifecycle bugs that only show after a queue has been reused
 *
 * Every step is flushed to stdout and to a progress file before it runs, so a
 * GPU hang still tells us exactly which operation wedged.
 *
 * Build: see build-test.sh. Run with VK_DRIVER_FILES pointing at the patched ICD.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>

#include "ace_comp_spv.h"

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
         note("FATAL %s:%d %s -> VkResult %d", __FILE__, __LINE__, #expr, (int)_r);                 \
         exit(2);                                                                                  \
      }                                                                                            \
   } while (0)

/* Mirror of the shader, so expected values are computed independently. */
static uint32_t expected(uint32_t i, uint32_t seed)
{
   uint32_t v = i ^ seed;
   for (int k = 0; k < 32; ++k)
      v = v * 1664525u + 1013904223u;
   return v;
}

struct ctx {
   VkInstance instance;
   VkPhysicalDevice phys;
   VkDevice dev;
   uint32_t ace_family;
   uint32_t ace_count;
   VkQueue queues[8];
   VkCommandPool pool;
   VkPhysicalDeviceMemoryProperties mem;
};

static uint32_t find_mem(struct ctx *c, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < c->mem.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (c->mem.memoryTypes[i].propertyFlags & want) == want)
         return i;
   note("FATAL no memory type for flags 0x%x", want);
   exit(2);
}

struct buf {
   VkBuffer handle;
   VkDeviceMemory memory;
   void *mapped;
   VkDeviceSize size;
};

static struct buf make_buf(struct ctx *c, VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
   struct buf b = {.size = size};
   VkBufferCreateInfo bi = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   CHECK(vkCreateBuffer(c->dev, &bi, NULL, &b.handle));

   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->dev, b.handle, &req);
   VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = find_mem(c, req.memoryTypeBits, want),
   };
   CHECK(vkAllocateMemory(c->dev, &ai, NULL, &b.memory));
   CHECK(vkBindBufferMemory(c->dev, b.handle, b.memory, 0));
   if (host)
      CHECK(vkMapMemory(c->dev, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
   return b;
}

static void free_buf(struct ctx *c, struct buf *b)
{
   if (b->mapped)
      vkUnmapMemory(c->dev, b->memory);
   vkDestroyBuffer(c->dev, b->handle, NULL);
   vkFreeMemory(c->dev, b->memory, NULL);
}

static VkCommandBuffer begin_cmd(struct ctx *c)
{
   VkCommandBuffer cmd;
   VkCommandBufferAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   CHECK(vkAllocateCommandBuffers(c->dev, &ai, &cmd));
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   CHECK(vkBeginCommandBuffer(cmd, &bi));
   return cmd;
}

/* Submit and wait with a fence, so a hang shows up as a timeout rather than
 * a process stuck forever in the driver. */
static int submit_wait(struct ctx *c, VkQueue q, VkCommandBuffer cmd, uint64_t timeout_ns, const char *what)
{
   CHECK(vkEndCommandBuffer(cmd));
   VkFence fence;
   VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   CHECK(vkCreateFence(c->dev, &fi, NULL, &fence));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(q, 1, &si, fence));
   VkResult r = vkWaitForFences(c->dev, 1, &fence, VK_TRUE, timeout_ns);
   vkDestroyFence(c->dev, fence, NULL);
   vkFreeCommandBuffers(c->dev, c->pool, 1, &cmd);
   if (r == VK_TIMEOUT) {
      note("FAIL  %s: fence timed out -- the ACE queue did not retire the work", what);
      failures++;
      return 0;
    }
   if (r != VK_SUCCESS) {
      note("FAIL  %s: vkWaitForFences -> %d (device lost means a GPU hang)", what, (int)r);
      failures++;
      return 0;
   }
   return 1;
}

static void barrier_to_host(VkCommandBuffer cmd)
{
   VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* ---- test 1: vkCmdFillBuffer on the ACE queue (RADV meta path) ---- */
static void test_fill(struct ctx *c)
{
   const uint32_t n = 1u << 16;
   const uint32_t pattern = 0xa5c3f00du;
   note("RUN   1/5 vkCmdFillBuffer on ACE queue, %u words", n);
   struct buf b = make_buf(c, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
   memset(b.mapped, 0, b.size);

   VkCommandBuffer cmd = begin_cmd(c);
   vkCmdFillBuffer(cmd, b.handle, 0, b.size, pattern);
   barrier_to_host(cmd);
   if (submit_wait(c, c->queues[0], cmd, 10ull * 1000000000ull, "fill")) {
      uint32_t bad = 0, first = 0;
      const uint32_t *p = b.mapped;
      for (uint32_t i = 0; i < n; i++)
         if (p[i] != pattern) {
            if (!bad)
               first = i;
            bad++;
         }
      if (bad)
         note("FAIL  1/5 fill: %u/%u words wrong, first at %u (got 0x%08x)", bad, n, first, p[first]), failures++;
      else
         note("PASS  1/5 fill");
   }
   free_buf(c, &b);
}

/* ---- test 2: vkCmdCopyBuffer on the ACE queue (RADV meta path) ---- */
static void test_copy(struct ctx *c)
{
   const uint32_t n = 1u << 16;
   note("RUN   2/5 vkCmdCopyBuffer on ACE queue, %u words", n);
   struct buf src = make_buf(c, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
   struct buf dst = make_buf(c, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
   uint32_t *s = src.mapped;
   for (uint32_t i = 0; i < n; i++)
      s[i] = expected(i, 0x1234u);
   memset(dst.mapped, 0, dst.size);

   VkCommandBuffer cmd = begin_cmd(c);
   VkBufferCopy region = {.size = src.size};
   vkCmdCopyBuffer(cmd, src.handle, dst.handle, 1, &region);
   barrier_to_host(cmd);
   if (submit_wait(c, c->queues[0], cmd, 10ull * 1000000000ull, "copy")) {
      if (memcmp(src.mapped, dst.mapped, src.size)) {
         uint32_t *d = dst.mapped, bad = 0, first = 0;
         for (uint32_t i = 0; i < n; i++)
            if (d[i] != s[i]) {
               if (!bad)
                  first = i;
               bad++;
            }
         note("FAIL  2/5 copy: %u/%u words wrong, first at %u", bad, n, first);
         failures++;
      } else {
         note("PASS  2/5 copy");
      }
   }
   free_buf(c, &src);
   free_buf(c, &dst);
}

/* ---- compute pipeline shared by tests 3-5 ---- */
struct pipe {
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout layout;
   VkPipeline pipeline;
   VkDescriptorPool desc_pool;
};

static struct pipe make_pipe(struct ctx *c)
{
   struct pipe p = {0};
   VkDescriptorSetLayoutBinding binding = {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
   };
   VkDescriptorSetLayoutCreateInfo sli = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding,
   };
   CHECK(vkCreateDescriptorSetLayout(c->dev, &sli, NULL, &p.set_layout));

   VkPushConstantRange pcr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 8};
   VkPipelineLayoutCreateInfo pli = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &p.set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &pcr,
   };
   CHECK(vkCreatePipelineLayout(c->dev, &pli, NULL, &p.layout));

   VkShaderModule module;
   VkShaderModuleCreateInfo smi = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(ace_comp_spv),
      .pCode = (const uint32_t *)ace_comp_spv,
   };
   CHECK(vkCreateShaderModule(c->dev, &smi, NULL, &module));

   VkComputePipelineCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main"},
      .layout = p.layout,
   };
   CHECK(vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p.pipeline));
   vkDestroyShaderModule(c->dev, module, NULL);

   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 64};
   VkDescriptorPoolCreateInfo dpi = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 64,
      .poolSizeCount = 1,
      .pPoolSizes = &ps,
   };
   CHECK(vkCreateDescriptorPool(c->dev, &dpi, NULL, &p.desc_pool));
   return p;
}

static VkDescriptorSet bind_set(struct ctx *c, struct pipe *p, struct buf *b)
{
   VkDescriptorSet set;
   VkDescriptorSetAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = p->desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &p->set_layout,
   };
   CHECK(vkAllocateDescriptorSets(c->dev, &ai, &set));
   VkDescriptorBufferInfo bi = {.buffer = b->handle, .range = VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &bi,
   };
   vkUpdateDescriptorSets(c->dev, 1, &w, 0, NULL);
   return set;
}

static int dispatch_and_verify(struct ctx *c, struct pipe *p, VkQueue q, uint32_t n, uint32_t seed,
                               const char *what)
{
   struct buf b = make_buf(c, (VkDeviceSize)n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
   memset(b.mapped, 0xcd, b.size);
   VkDescriptorSet set = bind_set(c, p, &b);

   VkCommandBuffer cmd = begin_cmd(c);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
   uint32_t push[2] = {n, seed};
   vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
   vkCmdDispatch(cmd, (n + 255) / 256, 1, 1);
   barrier_to_host(cmd);

   int ok = 0;
   if (submit_wait(c, q, cmd, 30ull * 1000000000ull, what)) {
      const uint32_t *got = b.mapped;
      uint32_t bad = 0, first = 0;
      for (uint32_t i = 0; i < n; i++)
         if (got[i] != expected(i, seed)) {
            if (!bad)
               first = i;
            bad++;
         }
      if (bad) {
         note("FAIL  %s: %u/%u words wrong, first at %u (got 0x%08x want 0x%08x)", what, bad, n, first,
              got[first], expected(first, seed));
         failures++;
      } else {
         ok = 1;
      }
   }
   free_buf(c, &b);
   return ok;
}

int main(void)
{
   progress = fopen("bc250-ace-test.progress", "w");
   note("BC-250 ACE (async compute) queue test");

   struct ctx c = {0};
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "bc250-ace-test",
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
      note("FATAL BC-250 (1002:13fe) not found among %u Vulkan devices", ndev);
      return 2;
   }
   note("device: %s (driver %u.%u.%u)", props.deviceName, VK_VERSION_MAJOR(props.driverVersion),
        VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));

   uint32_t nfam = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nfam, NULL);
   VkQueueFamilyProperties *fams = calloc(nfam, sizeof(*fams));
   vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nfam, fams);

   int ace = -1;
   for (uint32_t i = 0; i < nfam; i++) {
      note("  family %u: count=%u flags=0x%x%s", i, fams[i].queueCount, fams[i].queueFlags,
           (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
              ? "  <- dedicated compute (ACE)"
              : "");
      if (ace < 0 && (fams[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         ace = (int)i;
   }
   if (ace < 0) {
      note("RESULT no dedicated compute queue family -- this driver does not expose async compute");
      return 1;
   }
   c.ace_family = (uint32_t)ace;
   c.ace_count = fams[ace].queueCount > 8 ? 8 : fams[ace].queueCount;
   note("using ACE family %u with %u queues", c.ace_family, c.ace_count);

   float prio[8];
   for (uint32_t i = 0; i < c.ace_count; i++)
      prio[i] = 1.0f;
   VkDeviceQueueCreateInfo qi = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c.ace_family,
      .queueCount = c.ace_count,
      .pQueuePriorities = prio,
   };
   VkDeviceCreateInfo di = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qi,
   };
   CHECK(vkCreateDevice(c.phys, &di, NULL, &c.dev));
   for (uint32_t i = 0; i < c.ace_count; i++)
      vkGetDeviceQueue(c.dev, c.ace_family, i, &c.queues[i]);
   vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem);

   VkCommandPoolCreateInfo pi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c.ace_family,
   };
   CHECK(vkCreateCommandPool(c.dev, &pi, NULL, &c.pool));

   test_fill(&c);
   test_copy(&c);

   struct pipe p = make_pipe(&c);

   note("RUN   3/5 compute dispatch on ACE queue 0, 65536 elements");
   if (dispatch_and_verify(&c, &p, c.queues[0], 1u << 16, 0xdeadbeefu, "3/5 dispatch"))
      note("PASS  3/5 dispatch");

   note("RUN   4/5 large dispatch on ACE queue 0, 4194304 elements (threadgroup workaround path)");
   if (dispatch_and_verify(&c, &p, c.queues[0], 1u << 22, 0x5a5a5a5au, "4/5 large dispatch"))
      note("PASS  4/5 large dispatch");

   note("RUN   5/5 all %u ACE queues, 3 rounds (queue lifecycle / reuse)", c.ace_count);
   int all_ok = 1;
   for (int round = 0; round < 3; round++) {
      for (uint32_t q = 0; q < c.ace_count; q++) {
         char what[64];
         snprintf(what, sizeof(what), "5/5 round %d queue %u", round, q);
         if (!dispatch_and_verify(&c, &p, c.queues[q], 1u << 18, 0x1000u + round * 16 + q, what))
            all_ok = 0;
      }
      note("      round %d done", round);
   }
   if (all_ok)
      note("PASS  5/5 all queues, all rounds");

   vkDestroyDescriptorPool(c.dev, p.desc_pool, NULL);
   vkDestroyPipeline(c.dev, p.pipeline, NULL);
   vkDestroyPipelineLayout(c.dev, p.layout, NULL);
   vkDestroyDescriptorSetLayout(c.dev, p.set_layout, NULL);
   vkDestroyCommandPool(c.dev, c.pool, NULL);
   vkDestroyDevice(c.dev, NULL);
   vkDestroyInstance(c.instance, NULL);

   note(failures ? "RESULT FAIL (%d failing checks)" : "RESULT PASS (async compute usable)", failures);
   return failures ? 1 : 0;
}
