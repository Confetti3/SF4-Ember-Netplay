#pragma once
#include "../common/ProductIdentity.hxx"
#include <cstddef>
#include <string_view>

namespace sf4e { namespace platform {
// The product's outbound links (ember::ProductIdentity::links), in the order Help & About lists them
// and Home's header draws them. The mark a link wears is LinkMark; Ko-fi has no logo in the icon set,
// so its mark is the cup (LinkMark::Coffee).
using ember::Link;
using ember::LinkMark;

// The running product's table as a range: empty for a product that names none.
class LinkTable {
public:
    explicit LinkTable(const Link* first) : first_(first) {
        while (first_ && first_[count_].id) ++count_;
    }
    const Link* begin() const { return first_; }
    const Link* end() const { return first_ + count_; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const Link& operator[](std::size_t i) const { return first_[i]; }
    const Link& back() const { return first_[count_ - 1]; }
private:
    const Link* first_;
    std::size_t count_ = 0;
};
inline LinkTable Links() { return LinkTable(ember::Product().links); }
// The link named `id`, or null.
inline const Link* FindLink(std::string_view id) {
    for (const Link& link : Links()) if (id == link.id) return &link;
    return nullptr;
}
} }
