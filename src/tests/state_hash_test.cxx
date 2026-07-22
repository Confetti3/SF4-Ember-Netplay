// Pure unit tests for the canonical semantic state hasher.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/sf4e__StateHash.hxx"

using namespace sf4e::statehash;

static int g_failures = 0;

#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
			g_failures++;                                                    \
		}                                                                    \
	} while (0)

// A stand-in for engine state with padding and a pointer, mirroring the
// hazards the encoder must be independent of.
struct FakeState {
	uint8_t a;
	// 3 bytes padding here
	uint32_t b;
	void* processLocalPointer; // must never enter the hash
	float pos;
	uint16_t fixFrac;
	int16_t fixInt;
};

static uint64_t HashFake(const FakeState& s) {
	Hasher h;
	h.U8(s.a);
	h.U32(s.b);
	// processLocalPointer intentionally excluded
	h.F32(s.pos);
	h.Fixed(s.fixFrac, s.fixInt);
	return h.Value();
}

static void TestDeterminism() {
	FakeState s = { 7, 1234, (void*)0xdeadbeef, 1.5f, 100, -3 };
	CHECK(HashFake(s) == HashFake(s));
}

static void TestFieldSensitivity() {
	FakeState s = { 7, 1234, 0, 1.5f, 100, -3 };
	FakeState t = s;
	t.b = 1235;
	CHECK(HashFake(s) != HashFake(t));

	t = s;
	t.pos = 1.5000001f;
	CHECK(HashFake(s) != HashFake(t));

	t = s;
	t.fixInt = -4;
	CHECK(HashFake(s) != HashFake(t));
}

static void TestPaddingIndependence() {
	// Two buffers with different garbage in padding bytes but identical
	// semantic fields must hash identically.
	unsigned char raw1[sizeof(FakeState)];
	unsigned char raw2[sizeof(FakeState)];
	memset(raw1, 0x00, sizeof(raw1));
	memset(raw2, 0xff, sizeof(raw2));
	FakeState* s1 = (FakeState*)raw1;
	FakeState* s2 = (FakeState*)raw2;
	s1->a = 9;      s2->a = 9;
	s1->b = 42;     s2->b = 42;
	s1->processLocalPointer = (void*)0x1111;
	s2->processLocalPointer = (void*)0x2222;
	s1->pos = 3.25f; s2->pos = 3.25f;
	s1->fixFrac = 5; s2->fixFrac = 5;
	s1->fixInt = -1; s2->fixInt = -1;
	CHECK(HashFake(*s1) == HashFake(*s2));
}

static void TestAddressIndependence() {
	// The same semantic state at two different heap addresses hashes equal.
	FakeState* a = (FakeState*)malloc(sizeof(FakeState));
	FakeState* b = (FakeState*)malloc(sizeof(FakeState));
	memset(a, 0, sizeof(*a));
	memset(b, 0, sizeof(*b));
	a->b = 77; b->b = 77;
	a->processLocalPointer = a; // self-referencing pointers differ
	b->processLocalPointer = b;
	CHECK(HashFake(*a) == HashFake(*b));
	free(a);
	free(b);
}

static void TestCanonicalByteEncoding() {
	// U32 must equal feeding its little-endian bytes through U8: the wire
	// encoding is defined, not host-dependent.
	Hasher a;
	a.U32(0x01020304u);
	Hasher b;
	b.U8(0x04); b.U8(0x03); b.U8(0x02); b.U8(0x01);
	CHECK(a.Value() == b.Value());

	Hasher c;
	c.U16(0xBEEF);
	Hasher d;
	d.U8(0xEF); d.U8(0xBE);
	CHECK(c.Value() == d.Value());

	Hasher e;
	e.U64(0x1122334455667788ULL);
	Hasher f;
	f.U32(0x55667788u); f.U32(0x11223344u);
	CHECK(e.Value() == f.Value());

	// Fixed = U16(frac) then I16(int), two's complement.
	Hasher g1;
	g1.Fixed(0x0005, -1);
	Hasher g2;
	g2.U16(0x0005); g2.U16(0xFFFF);
	CHECK(g1.Value() == g2.Value());
}

static void TestKnownVector() {
	// FNV-1a 64 reference: empty = offset basis; "a" = 0xaf63dc4c8601ec8c.
	Hasher e;
	CHECK(e.Value() == 14695981039346656037ULL);
	Hasher a;
	a.U8('a');
	CHECK(a.Value() == 0xaf63dc4c8601ec8cULL);
}

static void TestFloatBitPattern() {
	// +0.0 and -0.0 differ in bit pattern and therefore in hash — a real
	// determinism difference must not be masked by numeric comparison.
	Hasher p, n;
	p.F32(0.0f);
	n.F32(-0.0f);
	CHECK(p.Value() != n.Value());
}

static void TestOrderSensitivity() {
	Hasher ab, ba;
	ab.U8(1); ab.U8(2);
	ba.U8(2); ba.U8(1);
	CHECK(ab.Value() != ba.Value());
}

int main() {
	TestDeterminism();
	TestFieldSensitivity();
	TestPaddingIndependence();
	TestAddressIndependence();
	TestCanonicalByteEncoding();
	TestKnownVector();
	TestFloatBitPattern();
	TestOrderSensitivity();

	if (g_failures) {
		printf("%d failure(s)\n", g_failures);
		return 1;
	}
	printf("state_hash_test: all tests passed\n");
	return 0;
}
