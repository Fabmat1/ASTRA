// The evolutionary-track tools are always offered, and the adoption change they
// record can be reverted.

#include <doctest.h>

#include "ai/AiRuntime.h"
#include "ai/ToolRegistry.h"
#include "ai_test_support.h"

using namespace astra::ai;

TEST_SUITE("ai")
{

TEST_CASE("evolution tools are registered")
{
    aitest::isolate();
    aitest::NullHost host;
    AiRuntime rt{&host};
    registerJobTools(rt);
    registerEvolutionTools(rt);
    for (const QString& n : {"run_track_fit", "list_track_fits", "set_adopted_track_fit", "delete_track_fit"})
        CHECK_MESSAGE(rt.tools()->find(n) != nullptr, n.toStdString());
    CHECK(rt.tools()->find("delete_track_fit")->risk == Risk::Destructive);
    CHECK(rt.tools()->find("set_adopted_track_fit")->risk == Risk::Modify);
    CHECK(rt.reverter("adopted_track_fit") != nullptr);
}

}  // TEST_SUITE("ai")
