// Pyxis unit test — RenderGraph smoke (M1).
//
// Plan §9.2 / §41 M1: verify RenderGraph::AddPass + Execute both work
// without a real GPU. The Profiler is constructed CPU-only
// (`Profiler{nullptr}`) — Plan §18.7 defines that as the supported
// no-GPU profiler. Each fake pass increments a counter so we can assert
// the order matches AddPass registration and that every pass got
// Execute()d exactly once.

#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

// The RenderGraph + IRenderPass live in Private/. Including them here
// is the unit-test exception (per the §35 "tests link the same DLLs the
// application uses" rule + the gtest-only EHsc allowance). The tests
// project gets the include directory via the same target's
// pyxis_renderer Public/ + Private/ fileset.
#include "RenderGraph/IRenderPass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/RenderGraph.h"

#include <atomic>
#include <memory>

namespace {

// Tiny no-GPU IRenderPass that just counts how many times it ran and
// remembers the last execution order index.
class CountingPass final : public pyxis::IRenderPass {
public:
    CountingPass(std::string_view name,
                 std::atomic<int>& globalCounter,
                 int& lastOrder,
                 int& runCount)
        : _name(name), _global(globalCounter), _lastOrder(lastOrder), _runCount(runCount) {}

    std::string_view Name() const override { return _name; }
    void Execute(nvrhi::ICommandList* /*commandList*/, const pyxis::PassContext& /*ctx*/) override {
        _lastOrder = _global.fetch_add(1);
        ++_runCount;
    }

private:
    std::string_view  _name;
    std::atomic<int>& _global;
    int&              _lastOrder;
    int&              _runCount;
};

}  // namespace

TEST(RenderGraphSmoke, EmptyGraphExecuteIsNoOp) {
    pyxis::Profiler profiler{ nullptr };  // CPU-only — §18.7
    pyxis::RenderGraph graph{ /*device=*/nullptr, &profiler };
    const pyxis::PassContext ctx{};
    graph.Execute(/*commandList=*/nullptr, ctx);
    SUCCEED();
}

TEST(RenderGraphSmoke, AddPassExecutesInRegistrationOrder) {
    pyxis::Profiler    profiler{ nullptr };
    pyxis::RenderGraph graph{ /*device=*/nullptr, &profiler };

    std::atomic<int> globalCounter{ 0 };
    int orderA = -1, orderB = -1, orderC = -1;
    int runsA  = 0,  runsB  = 0,  runsC  = 0;

    graph.AddPass(std::make_unique<CountingPass>("pass.A", globalCounter, orderA, runsA));
    graph.AddPass(std::make_unique<CountingPass>("pass.B", globalCounter, orderB, runsB));
    graph.AddPass(std::make_unique<CountingPass>("pass.C", globalCounter, orderC, runsC));

    const pyxis::PassContext ctx{};
    graph.Execute(nullptr, ctx);

    EXPECT_EQ(runsA, 1);
    EXPECT_EQ(runsB, 1);
    EXPECT_EQ(runsC, 1);
    EXPECT_EQ(orderA, 0);
    EXPECT_EQ(orderB, 1);
    EXPECT_EQ(orderC, 2);
}

TEST(RenderGraphSmoke, ExecuteIsRepeatable) {
    pyxis::Profiler    profiler{ nullptr };
    pyxis::RenderGraph graph{ /*device=*/nullptr, &profiler };

    std::atomic<int> globalCounter{ 0 };
    int order = -1;
    int runs  = 0;
    graph.AddPass(std::make_unique<CountingPass>("pass.X", globalCounter, order, runs));

    const pyxis::PassContext ctx{};
    graph.Execute(nullptr, ctx);
    graph.Execute(nullptr, ctx);
    graph.Execute(nullptr, ctx);

    EXPECT_EQ(runs, 3);
}

TEST(RenderGraphSmoke, NullPassIsRejectedQuietly) {
    pyxis::Profiler    profiler{ nullptr };
    pyxis::RenderGraph graph{ /*device=*/nullptr, &profiler };

    graph.AddPass(nullptr);  // §30.6: silent rejection, no crash
    const pyxis::PassContext ctx{};
    graph.Execute(nullptr, ctx);
    SUCCEED();
}
