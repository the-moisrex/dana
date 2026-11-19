#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace dana::text {

    // The bash `$(...)` rule: trailing newlines are always stripped by command
    // substitution. 15 prompt files carried a private copy of this.
    std::string rstrip_newlines(std::string_view s);

    // Trim leading and trailing newlines — a block boundary. Spaces and tabs are
    // left alone (leading indentation in a code block is content, not padding).
    std::string_view strip_newlines(std::string_view s);

    // The one text-fold rule shared by the chain context, the stdout accumulator,
    // the clipboard builder and the history replay: trim both sides, skip an empty
    // block, separate consecutive blocks with exactly one blank line and terminate
    // the buffer with exactly one newline. Returns whether it added anything.
    bool append_block(std::string& text, std::string_view block);

    // Strip trailing occurrences of `c` (default space).
    std::string rstrip(std::string_view s, char c = ' ');

    // Leading and trailing whitespace.
    std::string strip(std::string_view s);

    // Split on '\n'. With keep_blank=false (awk-like: skips empties) trailing
    // newline does not produce a final empty element.
    std::vector<std::string> split_lines(std::string_view s, bool keep_blank = false);

    // Whitespace-separated words.
    std::vector<std::string> words(std::string_view s);

    // First N complete lines, no trailing newline (0 = all). Unlike
    // sdk/embed.hpp's trim_context, this never appends a newline — it is for raw
    // subprocess output.
    std::string first_n_lines(std::string_view s, std::size_t n);

    // Non-throwing integer parsing. When `s` is not a valid non-negative integer
    // the bash code path it ports produced `fallback` (e.g. bash arithmetic
    // treats a bare word as an unset variable → 0).
    std::size_t  to_size(std::string_view s, std::size_t fallback = 0);
    std::int64_t to_ll(std::string_view s, std::int64_t fallback = 0);

    // Escape every regex metacharacter (for interpolating $HOME/$USER into patterns).
    std::string regex_escape(std::string_view s);

    std::string strip_trailing_newlines(std::string_view text);

    // Trim a --head-capped context the way `sed -n 1,Np` does: a range whose end
    // precedes its start counts as line 1, so a --head of 0 keeps one line while an
    // absent --head adds a single trailing newline.
    std::string trim_head(std::string_view text, std::optional<std::size_t> head_lines);

    bool is_all_digits(std::string_view text);

    // Leading-zero tokens would be read in base 8 by an arithmetic test, where
    // 08-style tokens error out and count as false, so only all-zero values fail.
    bool is_positive_int_token(std::string_view text);

    std::size_t parse_head_value(std::string_view text);

    bool ends_with(std::string_view text, std::string_view suffix);


    // Add: ```text ... ```
    void append_fenced(std::string&               out,
                       std::string_view           payload,
                       std::optional<std::size_t> head_lines,
                       std::string_view           language);

    // Const random-access iterator over a type-erased source of string_views.
    template <typename SV>
    struct [[nodiscard]] erased_sv_iterator {
        using value_type        = SV;
        using difference_type   = std::ptrdiff_t;
        using iterator_concept  = std::random_access_iterator_tag;
        using iterator_category = std::random_access_iterator_tag;
        using size_type         = SV::size_type;

        // "deref(i) -> SV" where i is a byte offset from the source's begin().
        using deref_fn = SV (*)(void const* base, std::ptrdiff_t index) noexcept;

        constexpr erased_sv_iterator() = default;

        constexpr erased_sv_iterator(void const* inp_base, size_type const inp_index, deref_fn const inp_func) noexcept
          : base{inp_base},
            index{inp_index},
            func{inp_func} {}

        constexpr SV operator*() const noexcept {
            return func(base, index);
        }

        constexpr SV operator[](difference_type n) const noexcept {
            return func(base, index + n);
        }

        constexpr erased_sv_iterator& operator++() noexcept {
            ++index;
            return *this;
        }

        constexpr erased_sv_iterator operator++(int) noexcept {
            auto t = *this;
            ++index;
            return t;
        }

        constexpr erased_sv_iterator& operator--() noexcept {
            --index;
            return *this;
        }

        constexpr erased_sv_iterator operator--(int) noexcept {
            auto t = *this;
            --index;
            return t;
        }

        constexpr erased_sv_iterator& operator+=(difference_type n) noexcept {
            index += n;
            return *this;
        }

        constexpr erased_sv_iterator& operator-=(difference_type n) noexcept {
            index -= n;
            return *this;
        }

        friend constexpr erased_sv_iterator operator+(erased_sv_iterator iter, difference_type len) noexcept {
            return iter += len;
        }

        friend constexpr erased_sv_iterator operator+(difference_type len, erased_sv_iterator iter) noexcept {
            return iter += len;
        }

        friend constexpr erased_sv_iterator operator-(erased_sv_iterator iter, difference_type len) noexcept {
            return iter -= len;
        }

        friend constexpr difference_type operator-(erased_sv_iterator const& lhs,
                                                   erased_sv_iterator const& rhs) noexcept {
            return lhs.index - rhs.index;
        }

        friend constexpr bool operator==(erased_sv_iterator const& lhs, erased_sv_iterator const& rhs) noexcept {
            return lhs.index == rhs.index;
        }

        friend constexpr auto operator<=>(erased_sv_iterator const& lhs, erased_sv_iterator const& rhs) noexcept {
            return lhs.index <=> rhs.index;
        }

      private:
        void const* base  = nullptr;
        size_type   index = 0;
        deref_fn    func  = nullptr;
    };

    /**
     * Span of StringViews
     * @tparam CharT
     * @tparam TraitsT
     */
    template <typename CharT = char, typename TraitsT = std::char_traits<CharT>>
    struct [[nodiscard]] basic_string_views {
        using string_view_type = std::basic_string_view<CharT, TraitsT>;
        using string_type      = std::basic_string<CharT, TraitsT>;
        using iterator         = erased_sv_iterator<string_view_type>;
        using sentinel         = iterator;
        using size_type        = string_view_type::size_type;

      private:
        using index_type = std::ptrdiff_t;
        using deref_fn   = iterator::deref_fn;
        using advance_fn = void const* (*)(void const* base, size_type index) noexcept;

        struct [[nodiscard]] operations {
            deref_fn   read;
            advance_fn advance;
        };

        // One function per ELEMENT type, not per container type.
        template <typename T>
        static constexpr operations ops_for{
          .read = +[](void const* base, index_type index) noexcept -> string_view_type {
              return string_view_type{static_cast<T const*>(base)[index]};
          },
          .advance = +[](void const* p, size_type n) noexcept -> void const* {
              return static_cast<T const*>(p) + n;
          },
        };


        void const* base   = nullptr;
        size_type   m_size = 0;
        operations  ops    = ops_for<string_view_type>;

      public:
        constexpr basic_string_views() noexcept = default;

        // Lvalues only: don't silently retain a pointer into a temporary
        // vector. Precondition: size fits in ptrdiff_t and data addresses size
        // elements.
        template <typename R,
                  typename P = decltype(std::data(std::declval<R&>())),
                  typename T = std::remove_cvref_t<std::remove_pointer_t<P>>>
            requires std::is_pointer_v<P> && std::convertible_to<P, T const*> &&
                       std::convertible_to<T const&, string_view_type> &&
                       std::is_nothrow_constructible_v<string_view_type, T const&> &&
                       requires(R& rng) { std::size(rng); }
        constexpr basic_string_views(R& rng)
          noexcept(noexcept(std::data(rng)) && noexcept(static_cast<index_type>(std::size(rng))))
          : base(std::data(rng)),
            m_size(static_cast<index_type>(std::size(rng))),
            ops(ops_for<T>) {}

        template <typename P = iterator, typename T = std::remove_cvref_t<std::remove_pointer_t<P>>>
        constexpr basic_string_views(P const begin, size_type const len) noexcept
          : base(begin),
            m_size(len),
            ops(ops_for<T>) {}

        constexpr basic_string_views(basic_string_views const&)                = default;
        constexpr basic_string_views(basic_string_views&&) noexcept            = default;
        constexpr basic_string_views& operator=(basic_string_views const&)     = default;
        constexpr basic_string_views& operator=(basic_string_views&&) noexcept = default;
        constexpr ~basic_string_views() noexcept                               = default;

        constexpr iterator begin() const noexcept {
            return {base, 0, ops.read};
        }

        constexpr iterator end() const noexcept {
            return {base, m_size, ops.read};
        }

        [[nodiscard]] constexpr size_type size() const noexcept {
            return m_size;
        }

        [[nodiscard]] constexpr bool empty() const noexcept {
            return m_size == 0;
        }

        [[nodiscard]] string_view_type operator[](index_type index) const noexcept {
            return ops.read(base, index);
        }

        [[nodiscard]] string_view_type at(index_type index) const {
            if (index >= static_cast<index_type>(m_size)) [[unlikely]] {
                throw std::out_of_range("basic_string_views::at: index out of range");
            }
            return ops.read(base, index);
        }

        [[nodiscard]] string_view_type back() const noexcept {
            assert(m_size > 0);
            return ops.read(base, m_size - 1);
        }

        [[nodiscard]] string_view_type front() const noexcept {
            assert(m_size > 0);
            return ops.read(base, 0);
        }

        [[nodiscard]] void const* data() const noexcept {
            return base;
        }

        [[nodiscard]] constexpr basic_string_views first(size_type const count) const noexcept {
            assert(count <= m_size);
            auto that   = *this;
            that.m_size = count;
            return that;
        }

        [[nodiscard]] constexpr basic_string_views last(size_type count) const noexcept {
            assert(count <= m_size);
            auto that = *this;
            if (count != m_size) [[likely]] {
                that.base = ops->advance(base, m_size - count);
            }
            that.m_size = count;
            return that;
        }

        template <class F>
        void for_each(F&& func) const {
            if (ops.read == &ops_for<string_view_type>.read) {
                auto const* ptr = static_cast<string_view_type const*>(base);
                for (index_type i = 0; i < m_size; ++i) {
                    func(string_view_type{ptr[i]});
                }
            } else if (ops.read == &ops_for<string_type>.read) {
                auto const* ptr = static_cast<string_type const*>(base);
                for (index_type i = 0; i < m_size; ++i) {
                    func(string_view_type{ptr[i]});
                }
            } else {
                for (index_type i = 0; i < m_size; ++i) {
                    func(ops.read(base, i));
                }
            }
        }
    };

    using string_views = basic_string_views<>;



} // namespace dana::text
