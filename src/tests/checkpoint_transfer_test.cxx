#include "../session/CheckpointTransfer.hxx"
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace sf4e::coordination;
static void Check(bool value) { if (!value) throw std::runtime_error("checkpoint transfer regression"); }

// The decoder as it was before the table: a linear search of the alphabet per
// character. The table decoder must give the same bytes and the same verdict
// for every input.
static bool ReferenceDecode(const std::string& text, std::string& bytes, std::size_t maximum) {
    if (text.size() > (maximum * 4 + 2) / 3 || text.size() % 4 == 1) return false;
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string decoded; decoded.reserve(text.size() * 3 / 4);
    std::uint32_t bits = 0; unsigned count = 0;
    for (char value : text) {
        const auto index = alphabet.find(value);
        if (index == std::string::npos) return false;
        bits = (bits << 6) | static_cast<std::uint32_t>(index); count += 6;
        if (count >= 8) {
            count -= 8;
            if (decoded.size() >= maximum) return false;
            decoded += static_cast<char>((bits >> count) & 255);
        }
    }
    if (count && (bits & ((1u << count) - 1))) return false;
    bytes = std::move(decoded); return true;
}

static void CompareDecoders() {
    std::mt19937 random(20261026);
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned accepted = 0, rejected = 0;
    const auto compare = [&](const std::string& text, std::size_t maximum) {
        std::string expected = "prior", actual = "prior";
        const bool reference = ReferenceDecode(text, expected, maximum);
        Check(Decode(text, actual, maximum) == reference && actual == expected);
        // Appending keeps what was there and adds exactly the same bytes; a
        // rejection leaves it untouched.
        std::string appended = "prefix";
        Check(DecodeAppend(text, appended, maximum) == reference);
        Check(appended == (reference ? "prefix" + expected : std::string("prefix")));
        if (reference) ++accepted; else ++rejected;
    };
    for (int round = 0; round < 4000; ++round) {
        std::string bytes(random() % 64, '\0');
        for (auto& byte : bytes) byte = static_cast<char>(random() & 255);
        const auto text = Encode(bytes);
        Check(Encode(bytes.data(), bytes.size()) == text);
        const std::size_t maximum = random() % 3 == 0 ? bytes.size() - (bytes.empty() ? 0 : random() % 2) : 64;
        compare(text, maximum);
        // Malformed: a character changed to any byte, a character dropped or
        // added, and trailing bits set.
        if (!text.empty()) {
            auto changed = text; changed[random() % changed.size()] = static_cast<char>(random() & 255); compare(changed, 64);
            auto shorter = text; shorter.erase(random() % shorter.size(), 1); compare(shorter, 64);
            auto trailing = text; trailing.back() = alphabet[random() % 64]; compare(trailing, 64);
        }
        auto longer = text; longer.insert(random() % (longer.size() + 1), 1, alphabet[random() % 64]); compare(longer, 64);
    }
    for (const char* text : {"", "A", "AB", "ABC", "ABCD", "AAA=", "AA==", "A-_B", "____", "AAEC_f7_", "AB\x80C", "ab+/"})
        for (std::size_t maximum : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(6), std::size_t(64)})
            compare(text, maximum);
    Check(accepted > 1000 && rejected > 1000);
}
int main() {
    try {
        Check(Sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        std::string decoded;
        Check(Decode("AAEC_f7_",decoded,6) && decoded==std::string("\0\1\2\xfd\xfe\xff",6));
        Check(!Decode("AB",decoded,4));
        Check(!Decode("A",decoded,4));
        Check(!Decode("AAA=",decoded,4));
        Check(!Decode("AAAA",decoded,2));
        CompareDecoders();
        const std::string body(MaximumCheckpoint,'x');
        TransferIdentity id; id.room[0]=1; id.epoch=1; id.transfer=7; id.term=3;
        id.baseRevision=5; id.revision=6; id.length=body.size(); id.digest=Sha256(body);
        auto begin=id.Envelope("checkpoint_begin"); begin["length"]=id.length; begin["digest"]=id.digest;
        CheckpointReceiver receiver;
        Check(receiver.Begin(begin,100));
        auto end=id.Envelope("checkpoint_end"); end["length"]=id.length; end["digest"]=id.digest;
        Check(!receiver.End(end));
        auto chunk=id.Envelope("checkpoint_chunk"); chunk["offset"]=0; chunk["data"]=Encode(body.substr(0,CheckpointChunk));
        auto stale=chunk; stale["term"]=2; Check(!receiver.Chunk(stale));
        auto wrongRoom=chunk; wrongRoom["room"][0]=9; Check(!receiver.Chunk(wrongRoom));
        Check(receiver.Offset()==0);
        Check(receiver.Chunk(chunk));
        Check(receiver.Offset()==CheckpointChunk);
        // The sender retries a timed-out transfer from offset zero using the
        // same immutable identity. Restart that exact incomplete transfer; a
        // different transfer cannot displace the retained body.
        auto differentBegin=begin;differentBegin["transfer"]=8;
        Check(!receiver.Begin(differentBegin,101));
        Check(receiver.Offset()==CheckpointChunk);
        Check(receiver.Begin(begin,102));
        Check(receiver.Offset()==0&&!receiver.Complete());
        for (std::size_t offset=0;offset<body.size();offset+=CheckpointChunk) {
            chunk["offset"]=offset; chunk["data"]=Encode(body.substr(offset,CheckpointChunk));
            Check(receiver.Chunk(chunk)); Check(!receiver.Chunk(chunk));
        }
        auto badDigest=end; badDigest["digest"]=std::string(64,'0'); Check(!receiver.End(badDigest));
        Check(!receiver.Complete()); Check(receiver.End(end));
        Check(receiver.Complete() && receiver.Bytes()==body);
		// Once End validates the complete digest, retain this one bounded body
		// until its authenticated committed marker is consumed. The helper has
		// advanced its exported revision before that marker reaches native IPC, so
		// expiring here would create a permanent watch/applied revision gap.
		Check(!receiver.Expired(15102));
        // A complete body may be pinned until its committed marker can enter a
        // bounded staging queue. A duplicate begin must not destroy it.
        Check(!receiver.Begin(begin,103));
        Check(receiver.Complete()&&receiver.Bytes()==body);
        // A retained complete body can meet a sender retry without violating
        // its fresh four-chunk credit window. Begin carries no credit. Each
        // replayed chunk proves its exact retained range and acknowledges only
        // that boundary; End proves the full immutable identity.
        Check(!receiver.CompleteReplayAcknowledgment(begin,"checkpoint_begin"));
        for (std::size_t offset=0;offset<5*CheckpointChunk;offset+=CheckpointChunk) {
            chunk["offset"]=offset; chunk["data"]=Encode(body.substr(offset,CheckpointChunk));
            const auto acknowledged=receiver.CompleteReplayAcknowledgment(chunk,"checkpoint_chunk");
            Check(acknowledged&&*acknowledged==offset+CheckpointChunk);
        }
        auto corrupted=chunk;
        corrupted["offset"]=0; corrupted["data"]=Encode(std::string(CheckpointChunk,'y'));
        Check(!receiver.CompleteReplayAcknowledgment(corrupted,"checkpoint_chunk"));
        const auto finalAcknowledgment=receiver.CompleteReplayAcknowledgment(end,"checkpoint_end");
        Check(finalAcknowledgment&&*finalAcknowledgment==body.size());
        Check(!receiver.End(end));
        receiver.Reset(); Check(!receiver.Complete() && receiver.Bytes().empty());
        auto tooLarge=begin; tooLarge["length"]=MaximumCheckpoint+1; Check(!receiver.Begin(tooLarge,0));
        Check(receiver.Begin(begin,100)); Check(!receiver.Expired(15099)); Check(receiver.Expired(15100));
        receiver.Reset(); Check(receiver.Begin(begin,20000));
        chunk["offset"]=0; chunk["data"]=Encode(std::string(CheckpointChunk+1,'x')); Check(!receiver.Chunk(chunk));
        std::cout<<"Checkpoint digest, bounded transfer, stale identity, and atomic import regressions passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
