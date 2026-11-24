#include "../../tools/tse_screen_review.hpp"

#include <cstdio>
#include <string>

namespace tse = dana::tools::tse;

namespace {

    int failures = 0;

    void check(bool cond, char const* what) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    void test_classify_document() {
        check(tse::classify_screen_text(R"({"results":[],"meta":{}})") == "doc", "results array is a doc");
        check(tse::classify_screen_text(R"({"symbol":"فولاد"})") == "jsonl", "single row is jsonl");
        check(tse::classify_screen_text(R"([{"symbol":"فولاد"}])") == "array", "object array is array");
        check(tse::classify_screen_text("") == "empty", "blank input is empty");
        check(tse::classify_screen_text("Symbol  Price\nفولاد  100") == "table", "table header is table");
        check(tse::classify_screen_text("what to buy tomorrow") == "nl", "prose is natural language");
    }

    void test_rows_to_csv() {
        auto const csv = tse::screen_rows_to_csv(R"({"results":[{"symbol":"فولاد","chg":1.5},{"symbol":"خودرو"}]})");
        check(csv == "symbol,chg\nفولاد,1.5\nخودرو,", "CSV header and missing cell");
        check(tse::screen_rows_to_csv(R"({"results":[]})").empty(), "no columns yields empty CSV");
    }

    void test_snapshot_meta() {
        auto const info = tse::screen_snapshot_meta(
          R"({"results":[{"symbol":"a"},{"symbol":"b"}],"meta":{"screened_at":"1404/09/01","total_universe":700,"args":["--traded"]}})");
        check(info.screened == "1404/09/01", "screened_at");
        check(info.counts == "2 matched / 700 screened", "count line");
        check(info.command == "--traded", "command from args");
    }

    void test_row_count() {
        check(tse::screen_row_count(R"([1,2,3])") == 3, "array row count");
    }

} // namespace

int main() {
    test_classify_document();
    test_rows_to_csv();
    test_snapshot_meta();
    test_row_count();
    if (failures != 0) {
        std::fprintf(stderr, "test_tse_tool: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
}
