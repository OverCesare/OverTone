#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "resources.h"
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/statvfs.h>
/* The built-in GPU and physical topology cannot change during the lifetime of
 * this Apple Silicon process. Pressure, active cores, budgets and free space can. */
static ot_resources hardware_identity(void) {
    ot_resources r = {0};
    r.memory_total = NSProcessInfo.processInfo.physicalMemory;
    size_t cpu_size = sizeof r.cpu_name;
    if (sysctlbyname("machdep.cpu.brand_string", r.cpu_name, &cpu_size, NULL, 0))
        r.cpu_name[0] = 0;
    r.cpu_name[sizeof r.cpu_name - 1] = 0;
    size_t build_size = sizeof r.system_build;
    if (sysctlbyname("kern.osversion", r.system_build, &build_size, NULL, 0))
        r.system_build[0] = 0;
    r.system_build[sizeof r.system_build - 1] = 0;
    unsigned levels = 0;
    size_t size = sizeof levels;
    if (!sysctlbyname("hw.nperflevels", &levels, &size, NULL, 0) && levels < 16) {
        for (unsigned i = 0; i < levels; i++) {
            char key[80], name[80] = {0};
            unsigned cores = 0;
            snprintf(key, sizeof key, "hw.perflevel%u.name", i);
            size = sizeof name;
            if (sysctlbyname(key, name, &size, NULL, 0))
                continue;
            if (strcasecmp(name, "Performance") && strcasecmp(name, "Super"))
                continue;
            snprintf(key, sizeof key, "hw.perflevel%u.physicalcpu", i);
            size = sizeof cores;
            if (!sysctlbyname(key, &cores, &size, NULL, 0))
                r.performance_cores += cores;
            if (!strcasecmp(name, "Super"))
                r.interactive_cores = cores;
        }
    }
    return r;
}
ot_resources ot_resources_sample(const char *profile) {
    static dispatch_once_t once;
    static ot_resources identity;
    static id<MTLDevice> device;
    dispatch_once(&once, ^{
        identity = hardware_identity();
        device = MTLCreateSystemDefaultDevice();
    });
    ot_resources r = identity;
    r.cores = (unsigned)NSProcessInfo.processInfo.activeProcessorCount;
    id<MTLDevice> current = device ?: MTLCreateSystemDefaultDevice();
    r.gpu = current != nil;
    r.unified = current.hasUnifiedMemory;
    r.gpu_recommended = current.recommendedMaxWorkingSetSize;
    mach_port_t host = mach_host_self();
    vm_size_t page = 0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_page_size(host, &page) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        /* HOST_VM_INFO64 already includes speculative pages in free_count. Inactive
         * pages are a reclaimability estimate, not a promise of immediately free RAM. */
        r.memory_available = ((uint64_t)vm.free_count + vm.inactive_count) * page;
        if (r.memory_available > r.memory_total)
            r.memory_available = r.memory_total;
        r.memory_used = r.memory_total - r.memory_available;
    }
    mach_port_deallocate(mach_task_self(), host);
    struct statvfs disk;
    if (profile && !statvfs(profile, &disk))
        r.disk_available = (uint64_t)disk.f_bavail * disk.f_frsize;
    int pressure = 0;
    size_t size = sizeof pressure;
    if (!sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &size, NULL, 0))
        r.pressure = pressure >= 4 ? 2 : pressure >= 2 ? 1 : 0;
    return r;
}
