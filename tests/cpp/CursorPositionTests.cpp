// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"
#include <set>

namespace {
using ReferenceStarts = std::set<std::pair<std::string, lsp::Position>>;

ReferenceStarts referenceStarts(ServerHarness& server, const Cursor& cursor) {
    auto references = server.getDocReferences(lsp::ReferenceParams{
        .context = {.includeDeclaration = true},
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

TEST_CASE("Cursor at identifier end supports definitions references and highlights") {
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
    CHECK(referenceStarts(server, end) == expected);

    auto highlights = end.getHighlights();
    REQUIRE(highlights.size() == expected.size());
    ReferenceStarts highlighted;
    for (const auto& highlight : highlights)
        highlighted.emplace(document.m_uri.str(), highlight.range.start);
    CHECK(highlighted == expected);
}
