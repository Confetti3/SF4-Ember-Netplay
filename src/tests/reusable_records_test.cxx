#include "../common/ReusableRecords.hxx"

#include <vector>

#include "test_support.hxx"

using sf4e::ReusableRecords;

struct PoolRecord {
	int manager = 0;
	std::vector<int> entries;
};

int main() {
	// A-003: the sound pool records are rebuilt every save. After the first
	// save, a reset and rebuild must reuse the same inner storage.
	ReusableRecords<PoolRecord> records;
	for (int manager = 1; manager <= 3; manager++) {
		auto& record = records.Next();
		record.manager = manager;
		record.entries.assign(40, manager);
	}
	CHECK(records.size() == 3);
	const int* storage = records[1].entries.data();

	records.Reset();
	CHECK(records.empty());
	CHECK(records.begin() == records.end());
	auto& reused = records.Next();
	reused.entries.clear();
	CHECK(reused.entries.capacity() >= 40);
	auto& second = records.Next();
	second.entries.clear();
	second.entries.assign(40, 9);
	CHECK(second.entries.data() == storage); // No reallocation.

	// Only live records are visible; the third, spare record from the
	// larger save is not.
	CHECK(records.size() == 2);
	int visited = 0;
	for (auto& record : records) {
		(void)record;
		visited++;
	}
	CHECK(visited == 2);
	return 0;
}
