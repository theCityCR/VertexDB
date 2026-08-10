#include "VertexDB/common/string_pattern.hpp"

#include <regex>
#include <stdexcept>

namespace VertexDB {
namespace {

[[nodiscard]] bool matchLikeRec(std::string_view text, std::string_view pattern) {
    while (!pattern.empty()) {
        if (pattern.front() == '%') {
            pattern.remove_prefix(1);
            if (pattern.empty()) {
                return true;
            }
            for (std::size_t i = 0; i <= text.size(); ++i) {
                if (matchLikeRec(text.substr(i), pattern)) {
                    return true;
                }
            }
            return false;
        }
        if (text.empty()) {
            return false;
        }
        if (pattern.front() != '_' && pattern.front() != text.front()) {
            return false;
        }
        pattern.remove_prefix(1);
        text.remove_prefix(1);
    }
    return text.empty();
}

[[nodiscard]] bool isWildcardFree(std::string_view text) {
    return text.find('%') == std::string_view::npos && text.find('_') == std::string_view::npos;
}

} // namespace

bool matchLikePattern(std::string_view text, std::string_view pattern) {
    return matchLikeRec(text, pattern);
}

bool matchRegexPattern(std::string_view text, std::string_view pattern) {
    try {
        const std::regex re{std::string{pattern},
                            std::regex_constants::ECMAScript | std::regex_constants::optimize};
        return std::regex_search(text.begin(), text.end(), re);
    } catch (const std::regex_error &ex) {
        throw std::runtime_error(std::string{"invalid regex pattern: "} + ex.what());
    }
}

std::optional<std::string> likePrefixLiteral(std::string_view pattern) {
    if (pattern.empty() || pattern.back() != '%') {
        return std::nullopt;
    }
    const auto literal = pattern.substr(0, pattern.size() - 1);
    if (literal.empty() || !isWildcardFree(literal)) {
        return std::nullopt;
    }
    return std::string{literal};
}

std::optional<std::string> likeContainsLiteral(std::string_view pattern) {
    if (pattern.size() < 3 || pattern.front() != '%' || pattern.back() != '%') {
        return std::nullopt;
    }
    const auto literal = pattern.substr(1, pattern.size() - 2);
    if (literal.empty() || !isWildcardFree(literal)) {
        return std::nullopt;
    }
    return std::string{literal};
}

std::vector<std::string> extractTrigrams(std::string_view text) {
    std::vector<std::string> grams;
    if (text.size() < 3) {
        if (!text.empty()) {
            grams.emplace_back(text);
        }
        return grams;
    }
    grams.reserve(text.size() - 2);
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        grams.emplace_back(text.substr(i, 3));
    }
    // Include the final window when length >= 3 (already covered by loop for i+2 < size;
    // last trigram is at i = size-3, and i+2 = size-1 < size — good).
    return grams;
}

} // namespace VertexDB
