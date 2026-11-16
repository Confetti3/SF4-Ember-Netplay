#pragma once
#include <cstdint>

namespace sf4e { namespace netplay {

// A value whose every change is counted. It is read through Get() and changed
// only through Edit(), which counts before it hands out the reference, so a
// writer cannot change it without the revision moving. What shows the value
// (the published snapshot, its throttle) compares the revision rather than
// listing the fields it read.
template<class T>
class Revisioned {
public:
    const T& Get() const { return value_; }
    T& Edit() { ++revision_; return value_; }
    std::uint64_t Revision() const { return revision_; }
private:
    T value_{};
    std::uint64_t revision_ = 0;
};

} }
