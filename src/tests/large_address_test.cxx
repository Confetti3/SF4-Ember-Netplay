#include "../launcher/LargeAddress.hxx"

#include "test_support.hxx"
using sf4e::launcher::LargeAddress;
using sf4e::launcher::LargeAddressExecutable;
using sf4e::launcher::MarkLargeAddressAware;

namespace {

// The smallest image the check accepts: DOS header, PE signature, COFF header
// and the optional-header magic, with SSFIV.exe's own values.
std::vector<unsigned char> Image(unsigned machine = 0x014c, unsigned magic = 0x010b, unsigned characteristics = 0x0103) {
	std::vector<unsigned char> image(0x200, 0);
	image[0] = 'M'; image[1] = 'Z';
	image[0x3c] = 0x80;
	image[0x80] = 'P'; image[0x81] = 'E';
	image[0x84] = machine & 0xff; image[0x85] = machine >> 8;
	image[0x96] = characteristics & 0xff; image[0x97] = characteristics >> 8;
	image[0x98] = magic & 0xff; image[0x99] = magic >> 8;
	return image;
}

std::vector<unsigned char> Read(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void Write(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
	std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}

int main() {
	// Only the one bit changes.
	auto image = Image();
	const auto before = image;
	CHECK(MarkLargeAddressAware(image) == LargeAddress::Set);
	CHECK(image[0x96] == 0x23 && image[0x97] == 0x01);
	image[0x96] = before[0x96];
	CHECK(image == before);

	auto flagged = Image(0x014c, 0x010b, 0x0123);
	const auto flaggedBefore = flagged;
	CHECK(MarkLargeAddressAware(flagged) == LargeAddress::AlreadySet && flagged == flaggedBefore);

	// A 64-bit image, a PE32+ optional header, and text are left alone.
	for (auto other : {Image(0x8664), Image(0x014c, 0x020b), std::vector<unsigned char>{'M', 'Z', 'x'}, std::vector<unsigned char>(0x200, 'A')}) {
		const auto otherBefore = other;
		CHECK(MarkLargeAddressAware(other) == LargeAddress::NotPe32 && other == otherBefore);
	}
	// A header offset that points past the end of the file.
	auto truncated = Image();
	truncated[0x3c] = 0xf0; truncated[0x3d] = 0x01;
	CHECK(MarkLargeAddressAware(truncated) == LargeAddress::NotPe32);
	truncated[0x3c] = 0xff; truncated[0x3d] = 0xff; truncated[0x3e] = 0xff; truncated[0x3f] = 0xff;
	CHECK(MarkLargeAddressAware(truncated) == LargeAddress::NotPe32);

	const auto folder = std::filesystem::temp_directory_path() / "sf4e-large-address-test";
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	const auto original = folder / "SSFIV.exe";
	const auto copy = folder / sf4e::launcher::kLargeAddressExecutableName;
	Write(original, before);

	// The copy carries the flag and the original is not written to.
	CHECK(LargeAddressExecutable(folder.wstring(), original.wstring()) == copy.wstring());
	CHECK(Read(original) == before);
	auto expected = before;
	expected[0x96] |= 0x20;
	CHECK(Read(copy) == expected);
	CHECK(!std::filesystem::exists(folder / (std::wstring(sf4e::launcher::kLargeAddressExecutableName) + L".part")));

	// A game update changes the original; the copy follows it.
	auto updated = before;
	updated[0x1ff] = 0x7f;
	Write(original, updated);
	CHECK(LargeAddressExecutable(folder.wstring(), original.wstring()) == copy.wstring());
	updated[0x96] |= 0x20;
	CHECK(Read(copy) == updated);

	// Nothing to do, or nothing safe to do: start the original.
	Write(original, flaggedBefore);
	CHECK(LargeAddressExecutable(folder.wstring(), original.wstring()) == original.wstring());
	Write(original, Image(0x8664));
	CHECK(LargeAddressExecutable(folder.wstring(), original.wstring()) == original.wstring());
	CHECK(LargeAddressExecutable(folder.wstring(), (folder / "missing.exe").wstring()) == (folder / "missing.exe").wstring());
	// A folder the copy cannot be written to.
	Write(original, before);
	CHECK(LargeAddressExecutable((folder / "no-such-folder").wstring(), original.wstring()) == original.wstring());

	std::filesystem::remove_all(folder);
	return 0;
}
