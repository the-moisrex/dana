#include "file_scope.hpp"
#include <system_error>
#include <utility>

namespace prompt {

std::string file_key(std::filesystem::path const& path) {
    std::error_code ec;
    auto const canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) return canonical.string();
    auto const absolute = std::filesystem::absolute(path, ec);
    if (!ec) return absolute.lexically_normal().string();
    return path.lexically_normal().string();
}

file_scope::file_scope(facts const& prior) {
    auto const value = prior.get(keys::files);
    if (!value) return;

    std::size_t start = 0;
    while (start <= value->size()) {
        std::size_t const end = value->find('\n', start);
        std::string_view const piece = value->substr(start, end == std::string_view::npos ? end : end - start);
        if (!piece.empty()) {
            std::string key(piece);
            if (set_.insert(key).second) order_.push_back(std::move(key));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
}

bool file_scope::seen(std::filesystem::path const& path) const { return set_.contains(file_key(path)); }

void file_scope::mark(std::filesystem::path const& path) {
    auto key = file_key(path);
    if (set_.insert(key).second) order_.push_back(std::move(key));
}

std::string file_scope::fact() const {
    std::string out;
    for (auto const& key : order_) {
        out += key;
        out += '\n';
    }
    return out;
}

} // namespace prompt
