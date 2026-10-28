// Regression test for the NVRHI garbage-collection contract:
//
// NVRHI command lists hold their upload chunks (sized to the largest
// CPU->GPU upload of the frame) alive through the queue's lifetime
// tracker, and the app is responsible for calling
// IDevice::runGarbageCollection() to release them once the GPU work has
// finished (nvrhi.h: "Execute runGarbageCollection frequently to poll
// the GPU and release resources when possible"). Without it, every frame
// permanently leaks one upload chunk - for video layers, the entire
// decoded frame (a measured ~frame bytes per rendered frame).
//
// This test uploads a 1 MB texture once per command list over a long run
// with runGarbageCollection() each frame and asserts that the process
// working set stays bounded (a steady state of a few chunks, not one
// chunk per frame).

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "mediamax/render/VulkanContext.h"

using namespace mediamax::render;

namespace
{

unsigned long long workingSetBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        return static_cast<unsigned long long>(pmc.WorkingSetSize);
    }
#endif
    return 0;
}

} // namespace

TEST_CASE("NVRHI upload memory stays bounded when runGarbageCollection is called each frame")
{
    VulkanContext context;
    std::string error;
    VulkanContextDesc desc;
    desc.appName = "MediaMax Upload Memory Test";
    if (!VulkanContext::create(desc, context, error))
    {
        DOCTEST_MESSAGE("skipping: no usable GPU (" << error << ")");
        return;
    }
    auto dev = context.nvrhiDevice();
    DOCTEST_REQUIRE(dev != nullptr);

    nvrhi::TextureDesc td;
    td.setWidth(512).setHeight(512).setFormat(nvrhi::Format::RGBA8_UNORM)
        .setDimension(nvrhi::TextureDimension::Texture2D)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
    auto tex = dev->createTexture(td);
    DOCTEST_REQUIRE(tex != nullptr);
    std::vector<std::uint8_t> data(512u * 512u * 4u, 0xAB);

    // Warm up (fill the command-buffer/chunk pools to their steady state),
    // then measure growth over the remaining run. Without
    // runGarbageCollection() this grows by ~1 MB per iteration (one chunk
    // per frame); with it, growth stays at a few MB at most.
    const int kWarmUp = 100;
    const int kTotal = 600;
    for (int i = 0; i < kWarmUp; ++i)
    {
        auto cmd = dev->createCommandList();
        cmd->open();
        cmd->writeTexture(tex.Get(), 0, 0, data.data(), 0, 0);
        cmd->close();
        nvrhi::ICommandList* p = cmd.Get();
        dev->executeCommandLists(&p, 1);
        dev->waitForIdle();
        dev->runGarbageCollection();
    }

    const unsigned long long baseline = workingSetBytes();
    for (int i = kWarmUp; i < kTotal; ++i)
    {
        auto cmd = dev->createCommandList();
        cmd->open();
        cmd->writeTexture(tex.Get(), 0, 0, data.data(), 0, 0);
        cmd->close();
        nvrhi::ICommandList* p = cmd.Get();
        dev->executeCommandLists(&p, 1);
        dev->waitForIdle();
        dev->runGarbageCollection();
    }
    dev->runGarbageCollection();

    const unsigned long long finalWs = workingSetBytes();
    const long long growthMb =
        static_cast<long long>((std::max(baseline, finalWs) - std::min(baseline, finalWs)) /
                               (1024ull * 1024ull));
    CHECK_MESSAGE(growthMb < 64,
                  "working set grew " << growthMb << " MB over " << (kTotal - kWarmUp)
                                      << " x 1MB uploads (expected < 64 MB; a leak of one "
                                         "chunk per frame would be ~500 MB)");
}
