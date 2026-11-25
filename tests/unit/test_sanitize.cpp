#include "../../core/sanitize.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using dana::sanitize::sanitize;

namespace {

    int failures = 0;

    void check(bool cond, char const* what) {
        if (!cond) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++failures;
        }
    }

    void test_no_redaction() {
        auto r = sanitize("a perfectly ordinary line with no personal data\n");
        check(!r.redacted, "plain text is not marked redacted");
        check(r.text == "a perfectly ordinary line with no personal data\n", "plain text is unchanged");
    }

    void test_ip_is_redacted() {
        auto r = sanitize("server at 10.0.0.1 answered\n");
        check(r.redacted, "an IP address marks the text redacted");
        check(r.text == "server at IP_ADDRESS answered\n", "the IP is replaced by IP_ADDRESS");
    }

    void test_email_is_redacted() {
        auto r = sanitize("mail someone@example.com now\n");
        check(r.redacted, "an email marks the text redacted");
        check(r.text == "mail EMAIL_ADDRESS now\n", "the email is replaced by EMAIL_ADDRESS");
    }

    void test_project_path_is_shortened_without_redaction() {
        auto r = sanitize("build dir: /home/testuser/Projects/dana/build\n");
        check(!r.redacted, "a path under HOME/Projects is shortened, not marked redacted");
        check(r.text == "build dir: dana/build\n", "the HOME/Projects prefix is dropped");
    }

    void test_home_is_redacted() {
        auto r = sanitize("file /home/testuser/.config/x\n");
        check(r.redacted, "a HOME path marks the text redacted");
        check(r.text == "file HOME/.config/x\n", "HOME replaces the home directory");
    }

    void test_legend_mentions_the_placeholders() {
        std::string_view legend = dana::sanitize::kRedactionLegend;
        check(legend.starts_with("[prompt] "), "the legend carries the harness-visible marker");
        check(legend.contains("IP_ADDRESS"), "the legend lists IP_ADDRESS");
        check(legend.contains("EMAIL_ADDRESS"), "the legend lists EMAIL_ADDRESS");
    }

} // namespace

int main() {
    // The redaction table interpolates $HOME/$USER once per process, so pin them
    // before the first sanitize() call to keep these checks host-independent.
    setenv("HOME", "/home/testuser", 1);
    setenv("USER", "testuser", 1);

    test_no_redaction();
    test_ip_is_redacted();
    test_email_is_redacted();
    test_project_path_is_shortened_without_redaction();
    test_home_is_redacted();
    test_legend_mentions_the_placeholders();

    if (failures != 0) {
        std::fprintf(stderr, "%d sanitize check(s) failed\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
