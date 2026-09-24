#pragma once

// Records rebuilt on every rollback save (ledger A-003). Reset() keeps every
// element, and each element keeps its own heap capacity, so a record that
// holds vectors stops allocating once a battle has warmed up. std::vector
// clear() would destroy those inner vectors and reallocate them next save.
//
// Only the first size() elements are live; begin()/end() and operator[]
// cover just those, so spare elements from an earlier, larger save are never
// read. Next() hands back a spare element with its old contents: the caller
// overwrites all of it.

#include <cstddef>
#include <vector>

namespace sf4e {

template <class T>
class ReusableRecords {
public:
	void Reserve(std::size_t count) { items_.reserve(count); }
	void Reset() { size_ = 0; }
	T& Next() {
		if (size_ == items_.size()) items_.emplace_back();
		return items_[size_++];
	}
	std::size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }
	T& operator[](std::size_t index) { return items_[index]; }
	T* begin() { return items_.data(); }
	T* end() { return items_.data() + size_; }

private:
	std::vector<T> items_;
	std::size_t size_ = 0;
};

} // namespace sf4e
