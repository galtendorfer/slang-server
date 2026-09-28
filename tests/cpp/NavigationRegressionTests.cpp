// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"
#include <set>

namespace {
using ReferenceStarts = std::set<std::pair<std::string, lsp::Position>>;

ReferenceStarts referenceStarts(ServerHarness& server, const Cursor& cursor,
                                bool includeDeclaration) {
    auto references = server.getDocReferences(lsp::ReferenceParams{
        .context = {.includeDeclaration = includeDeclaration},
        .textDocument = {.uri = cursor.getUri()},
        .position = cursor.getPosition(),
    });
    REQUIRE(references.has_value());
    ReferenceStarts starts;
    for (const auto& reference : *references)
        starts.emplace(reference.uri.str(), reference.range.start);
    CHECK(starts.size() == references->size());
    return starts;
}
} // namespace

TEST_CASE("Navigation - Active build selects module definition and reference declaration",
          "[navigation-regression]") {
    ServerHarness server("navigation_regression");
    server.loadConfig(Config{
        .indexGlobs = {std::vector<std::string>{"selected.sv", "duplicate.sv", "top.sv"}},
    });
    REQUIRE(server.m_indexer.getFilesForSymbol("NavigationRegressionALU").size() == 2);

    auto top = server.openFile("top.sv");
    auto use = top.before("NavigationRegressionALU first");

    // With no selected build, both workspace definitions are valid candidates.
    REQUIRE(use.getDefinitions().size() == 2);

    server.setBuildFile("design.f");
    auto selected = server.openFile("selected.sv");
    auto declaration = selected.after("module ");
    ReferenceStarts uses{
        {top.m_uri.str(), use.getPosition()},
        {top.m_uri.str(), top.before("NavigationRegressionALU second").getPosition()},
    };

    for (auto cursor : {use, top.after("    NavigationRegressionALU")}) {
        CAPTURE(cursor.m_offset);
        auto definitions = cursor.getDefinitions();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions.front().targetUri == selected.m_uri);
        CHECK(definitions.front().targetSelectionRange.start == declaration.getPosition());

        CHECK(referenceStarts(server, cursor, false) == uses);
        auto withDeclaration = uses;
        withDeclaration.emplace(selected.m_uri.str(), declaration.getPosition());
        CHECK(referenceStarts(server, cursor, true) == withDeclaration);
    }

    // Returning to explore mode must restore the upstream multiple-definition behavior.
    server.setBuildFile("");
    REQUIRE(use.getDefinitions().size() == 2);
}

TEST_CASE("Navigation - Active build resolves a module missing from the index",
          "[navigation-regression]") {
    ServerHarness server("navigation_regression");
    server.loadConfig(Config{
        .indexGlobs = {std::vector<std::string>{"./.../*.sv.disabled"}},
    });
    REQUIRE(server.m_indexer.getFilesForSymbol("NavigationRegressionALU").empty());
    server.setBuildFile("design.f");

    auto top = server.openFile("top.sv");
    for (auto cursor :
         {top.before("NavigationRegressionALU first"), top.after("    NavigationRegressionALU")}) {
        auto definitions = cursor.getDefinitions();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions.front().targetUri == URI::fromFile(fs::current_path() / "selected.sv"));
        CHECK(definitions.front().targetSelectionRange.start == lsp::Position{0, 7});
    }
}

TEST_CASE("Navigation - Active build respects nested module scope", "[navigation-regression]") {
    ServerHarness server("navigation_regression");
    server.loadConfig(Config{.indexGlobs = {std::vector<std::string>{"*.sv"}}});
    server.setBuildFile("scoped.f");

    auto scoped = server.openFile("scoped.sv");
    auto use = scoped.before("NavigationRegressionALU local_instance");
    auto definitions = use.getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == scoped.m_uri);
    CHECK(definitions.front().targetSelectionRange.start ==
          scoped.before("NavigationRegressionALU #").getPosition());

    auto hover = scoped.getHoverAt(use.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("SCOPED_VALUE") != std::string::npos);
    CHECK(content.find("input logic a") == std::string::npos);

    // Before saving, the build still owns the previous buffer. Local source locations
    // must come from the current document rather than that stale definition.
    scoped.insert(0, "\n\n");
    scoped.ensureSynced();
    definitions = scoped.before("NavigationRegressionALU local_instance").getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == scoped.m_uri);
    CHECK(definitions.front().targetSelectionRange.start ==
          scoped.before("NavigationRegressionALU #").getPosition());
}

TEST_CASE("Navigation - Active build leaves unrelated nested definitions intact",
          "[navigation-regression]") {
    ServerHarness server("navigation_regression");
    server.loadConfig(Config{.indexGlobs = {std::vector<std::string>{"*.sv"}}});
    server.setBuildFile("design.f");

    // This document is outside design.f and its local NavigationRegressionALU must retain its
    // lexical meaning.
    auto scoped = server.openFile("scoped.sv");
    auto use = scoped.before("NavigationRegressionALU local_instance");
    auto definitions = use.getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == scoped.m_uri);
    CHECK(definitions.front().targetSelectionRange.start ==
          scoped.before("NavigationRegressionALU #").getPosition());

    auto hover = scoped.getHoverAt(use.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("SCOPED_VALUE") != std::string::npos);
}

TEST_CASE("Navigation - Build selection survives edits and refreshes saved target locations",
          "[navigation-regression]") {
    ServerHarness server("navigation_regression");
    server.loadConfig(Config{
        .indexGlobs = {std::vector<std::string>{"selected.sv", "duplicate.sv", "top.sv"}},
    });
    server.setBuildFile("design.f");
    auto top = server.openFile("top.sv");
    auto selected = server.openFile("selected.sv");

    auto checkDefinition = [&] {
        auto definitions = top.after("    NavigationRegressionALU").getDefinitions();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions.front().targetUri == selected.m_uri);
        CHECK(definitions.front().targetSelectionRange.start ==
              selected.after("module ").getPosition());
    };
    checkDefinition();

    // Editing the use creates a new source buffer before the design is recompiled on save.
    top.insert(0, "\n\n");
    top.ensureSynced();
    checkDefinition();
    top.save();
    checkDefinition();

    // A refreshed design must stop using locations from the earlier target buffer.
    selected.insert(0, "\n\n\n");
    selected.save();
    checkDefinition();
    ReferenceStarts expected{
        {selected.m_uri.str(), selected.after("module ").getPosition()},
        {top.m_uri.str(), top.before("NavigationRegressionALU first").getPosition()},
        {top.m_uri.str(), top.before("NavigationRegressionALU second").getPosition()},
    };
    CHECK(referenceStarts(server, top.after("    NavigationRegressionALU"), true) == expected);
}

TEST_CASE("Navigation - Identifier end supports definitions references and highlights",
          "[navigation-regression]") {
    ServerHarness server;
    auto document = server.openFile("identifier_end.sv", R"(
module top;
    logic value;
    logic copied;
    assign copied = value;
    initial value = 1'b0;
endmodule
)");
    auto declaration = document.before("value;");
    auto read = document.after("assign copied = ");
    auto write = document.after("initial ");
    auto end = document.after("assign copied = value");

    auto definitions = end.getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == document.m_uri);
    CHECK(definitions.front().targetSelectionRange.start == declaration.getPosition());

    ReferenceStarts expected{
        {document.m_uri.str(), declaration.getPosition()},
        {document.m_uri.str(), read.getPosition()},
        {document.m_uri.str(), write.getPosition()},
    };
    CHECK(referenceStarts(server, end, true) == expected);

    auto highlights = end.getHighlights();
    REQUIRE(highlights.size() == expected.size());
    ReferenceStarts highlighted;
    for (const auto& highlight : highlights)
        highlighted.emplace(document.m_uri.str(), highlight.range.start);
    CHECK(highlighted == expected);
}
