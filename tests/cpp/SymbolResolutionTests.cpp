// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"
#include <set>

// The fixture holds the same module in two sibling projects (proj_a, proj_b), and a cell both
// in its own dependency and vendored inside another one.

namespace {

const std::vector<std::string> allSources{"./.../*.sv"};

URI fixtureUri(std::string_view relativePath) {
    return URI::fromFile(fs::current_path() / relativePath);
}

std::vector<URI> definitionUris(const Cursor& cursor) {
    std::vector<URI> uris;
    for (const auto& definition : Cursor(cursor).getDefinitions())
        uris.push_back(definition.targetUri);
    return uris;
}

std::set<std::string> referenceUris(ServerHarness& server, const Cursor& cursor) {
    auto references = server.getDocReferences(lsp::ReferenceParams{
        .context = {.includeDeclaration = true},
        .textDocument = {.uri = cursor.getUri()},
        .position = cursor.getPosition(),
    });
    REQUIRE(references.has_value());
    std::set<std::string> uris;
    for (const auto& reference : *references)
        uris.insert(reference.uri.str());
    return uris;
}

} // namespace

TEST_CASE("Symbol resolution - nearest declaration ranks first without a build",
          "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});
    REQUIRE(server.m_indexer.getFilesForSymbol("SymResLeaf").size() == 2);

    auto leafA = fixtureUri("proj_a/leaf.sv");
    auto leafB = fixtureUri("proj_b/leaf.sv");

    // Both declarations stay available, the one next to the referencing file first.
    auto topA = server.openFile("proj_a/top.sv");
    CHECK(definitionUris(topA.before("SymResLeaf first")) == std::vector{leafA, leafB});
    auto topB = server.openFile("proj_b/top.sv");
    CHECK(definitionUris(topB.before("SymResLeaf inst")) == std::vector{leafB, leafA});

    // Analysis of each file uses that same first choice, so ports resolve consistently.
    CHECK(definitionUris(topA.after("first(.")) == std::vector{leafA});
    CHECK(definitionUris(topB.after("inst(.")) == std::vector{leafB});
}

TEST_CASE("Symbol resolution - shallowest declaration ranks first among equally distant ones",
          "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});
    REQUIRE(server.m_indexer.getFilesForSymbol("SymResCell").size() == 2);

    auto cell = fixtureUri("deps/cells/cell.sv");
    auto vendored = fixtureUri("deps/other/vendor/cells/cell.sv");

    auto user = server.openFile("rtl/user.sv");
    CHECK(definitionUris(user.before("SymResCell cell_inst")) == std::vector{cell, vendored});
    CHECK(definitionUris(user.after("cell_inst(.")) == std::vector{cell});
}

TEST_CASE("Symbol resolution - indexer ranks declarations for a referrer", "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});
    auto& indexer = server.m_indexer;
    auto root = fs::current_path();

    auto leafA = root / "proj_a/leaf.sv";
    auto leafB = root / "proj_b/leaf.sv";
    CHECK(indexer.getFilesForSymbol("SymResLeaf", root / "proj_a/top.sv") ==
          std::vector{leafA, leafB});
    CHECK(indexer.getFilesForSymbol("SymResLeaf", root / "proj_b/top.sv") ==
          std::vector{leafB, leafA});
    // Without a closer declaration the order is still deterministic.
    CHECK(indexer.getFilesForSymbol("SymResLeaf", root / "rtl/user.sv") ==
          std::vector{leafA, leafB});
    CHECK(indexer.getFilesForSymbol("SymResLeaf", {}) == std::vector{leafA, leafB});

    auto best = indexer.getBestSymbolLoc("SymResCell", root / "rtl/user.sv");
    REQUIRE(best);
    CHECK(*best->uri == root / "deps/cells/cell.sv");
    // A file inside the vendoring dependency resolves to its own copy.
    best = indexer.getBestSymbolLoc("SymResCell", root / "deps/other/src/other.sv");
    REQUIRE(best);
    CHECK(*best->uri == root / "deps/other/vendor/cells/cell.sv");

    CHECK(!indexer.getBestSymbolLoc("SymResMissing", root / "rtl/user.sv"));
    CHECK(indexer.getFilesForSymbol("SymResMissing", root / "rtl/user.sv").empty());
}

TEST_CASE("Symbol resolution - selected build decides for the files it compiles",
          "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});

    auto leafA = fixtureUri("proj_a/leaf.sv");
    auto leafB = fixtureUri("proj_b/leaf.sv");

    // The build compiles proj_a/top.sv against the declaration in the other project.
    server.setBuildFile("far.f");
    auto topA = server.openFile("proj_a/top.sv");
    auto use = topA.before("SymResLeaf first");
    CHECK(definitionUris(use) == std::vector{leafB});
    CHECK(definitionUris(topA.after("first(.")) == std::vector{leafB});

    auto references = referenceUris(server, use);
    CHECK(references.contains(leafB.str()));
    CHECK(references.contains(topA.m_uri.str()));
    CHECK(!references.contains(leafA.str()));

    // A file outside the build keeps the ranked declarations.
    auto extra = server.openFile("proj_a/extra.sv");
    CHECK(definitionUris(extra.before("SymResLeaf inst")) == std::vector{leafA, leafB});
    CHECK(definitionUris(extra.after("inst(.")) == std::vector{leafA});

    // So does every file once no build is selected.
    server.setBuildFile("");
    CHECK(definitionUris(use) == std::vector{leafA, leafB});
    CHECK(definitionUris(topA.after("first(.")) == std::vector{leafA});
}

TEST_CASE("Symbol resolution - selected build resolves names missing from the index",
          "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{
        .indexGlobs = {std::vector<std::string>{"proj_a/top.sv", "proj_a/extra.sv"}},
    });
    REQUIRE(server.m_indexer.getFilesForSymbol("SymResLeaf").empty());
    server.setBuildFile("far.f");

    auto leafB = fixtureUri("proj_b/leaf.sv");
    auto topA = server.openFile("proj_a/top.sv");
    CHECK(definitionUris(topA.before("SymResLeaf first")) == std::vector{leafB});
    CHECK(definitionUris(topA.after("first(.")) == std::vector{leafB});

    // The build's declaration is also the only one known for a file outside the build.
    auto extra = server.openFile("proj_a/extra.sv");
    CHECK(definitionUris(extra.before("SymResLeaf inst")) == std::vector{leafB});
}

namespace {

// The fixture's scoped.sv declares its own nested SymResLeaf.
void checkNestedLeaf(DocumentHandle& scoped) {
    auto use = scoped.before("SymResLeaf local_instance");
    auto definitions = use.getDefinitions();
    REQUIRE(definitions.size() == 1);
    CHECK(definitions.front().targetUri == scoped.m_uri);
    CHECK(definitions.front().targetSelectionRange.start ==
          scoped.before("SymResLeaf #").getPosition());

    auto hover = scoped.getHoverAt(use.m_offset);
    REQUIRE(hover);
    auto content = rfl::get<lsp::MarkupContent>(hover->contents).value;
    CHECK(content.find("SCOPED_VALUE") != std::string::npos);
    CHECK(content.find("input logic a") == std::string::npos);
}

} // namespace

TEST_CASE("Symbol resolution - nested module scope wins inside the selected build",
          "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});
    server.setBuildFile("scoped.f");

    auto scoped = server.openFile("scoped.sv");
    checkNestedLeaf(scoped);

    // Before saving, the build still owns the previous buffer of this file.
    scoped.insert(0, "\n\n");
    scoped.ensureSynced();
    checkNestedLeaf(scoped);
}

TEST_CASE("Symbol resolution - nested module scope wins outside the selected build",
          "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});
    server.setBuildFile("far.f");

    auto scoped = server.openFile("scoped.sv");
    checkNestedLeaf(scoped);
}

TEST_CASE("Symbol resolution - build choice survives edits and saves", "[symbol-resolution]") {
    ServerHarness server("symbol_resolution");
    server.loadConfig(Config{.indexGlobs = {allSources}});
    server.setBuildFile("far.f");
    auto topA = server.openFile("proj_a/top.sv");
    auto leafB = server.openFile("proj_b/leaf.sv");

    auto checkDefinition = [&](std::string_view step) {
        INFO(step);
        auto definitions = topA.before("SymResLeaf first").getDefinitions();
        REQUIRE(definitions.size() == 1);
        CHECK(definitions.front().targetUri == leafB.m_uri);
        CHECK(definitions.front().targetSelectionRange.start ==
              leafB.after("module ").getPosition());
    };
    checkDefinition("initial");

    // Editing the use creates a new source buffer before the design is recompiled on save.
    topA.insert(0, "\n\n");
    topA.ensureSynced();
    checkDefinition("use edited");
    topA.save();
    checkDefinition("use saved");

    // The target location follows edits of the declaring file, saved or not.
    leafB.insert(0, "\n\n\n");
    leafB.ensureSynced();
    checkDefinition("declaration edited");
    leafB.save();
    checkDefinition("declaration saved");
}
