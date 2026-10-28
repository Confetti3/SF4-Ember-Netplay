#include "../session/CheckpointTransfer.hxx"
#include "../platform/HelperFrame.hxx"
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
    for (const char* text : {"", "A", "AB", "ABC", "ABCD", "AAA=", "AA==", "A-_B", "____", "AAEC_f7_", "AB\x80" "C", "ab+/"})
        for (std::size_t maximum : {std::size_t(0), std::size_t(1), std::size_t(2), std::size_t(3), std::size_t(6), std::size_t(64)})
            compare(text, maximum);
    Check(accepted > 1000 && rejected > 1000);
}

// Raw-body helper frames: the split gives back the header and body as they
// went in, a JSON payload passes through, and malformed ones are refused.
static void RawBodyFrames() {
    using namespace sf4e::platform::helper_frame;
    const std::string header = "{\"type\":\"checkpoint_chunk\",\"offset\":0}";
    const std::string body("\0\1{\"\xff", 5);
    const auto payload = EncodeRawBody(header, body.data(), body.size());
    Check(payload.size() == RawPrefix + header.size() + body.size() && payload[0] == RawBody);
    std::size_t headerSize = 0;
    Check(SplitPayload(payload, headerSize) == PayloadKind::Raw && headerSize == header.size());
    Check(payload.compare(RawPrefix, headerSize, header) == 0 && payload.substr(RawPrefix + headerSize) == body);
    Check(SplitPayload("{\"type\":\"status\"}", headerSize) == PayloadKind::Json);
    // Nothing to send: no body, a header that is not an object, or past the bound.
    Check(EncodeRawBody(header, body.data(), 0).empty());
    Check(EncodeRawBody("[1]", body.data(), body.size()).empty());
    const std::string huge(MaxPayload, 'x');
    Check(EncodeRawBody(header, huge.data(), huge.size()).empty());
    for (const auto& bad : {std::string(1, RawBody), payload.substr(0, RawPrefix + header.size()),
        payload.substr(0, 3), std::string("\0\0\0\0\2[]x", 8), std::string("\0\xff\xff\xff\xff{}x", 8)})
        Check(SplitPayload(bad, headerSize) == PayloadKind::Invalid);
    // The authentication answer names the IPC version; a bare SF4N is 0.
    std::uint16_t version = 7;
    Check(ReadAuthenticationAnswer(AuthenticationAnswer(), version) && version == IpcVersion);
    Check(ReadAuthenticationAnswer("SF4N", version) && version == 0);
    Check(!ReadAuthenticationAnswer("SF4X", version) && !ReadAuthenticationAnswer(std::string("SF4N\0", 5), version));
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
        RawBodyFrames();
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
        // A raw-body chunk carries its bytes as they are, under the same
        // identity, offset and size bounds; its header has no data field.
        receiver.Reset(); Check(receiver.Begin(begin,30000));
        auto rawChunk=id.Envelope("checkpoint_chunk"); rawChunk["offset"]=0;
        const std::string tooBig(CheckpointChunk+1,'x');
        Check(!receiver.Chunk(rawChunk,std::string_view(tooBig)));
        Check(!receiver.Chunk(rawChunk,std::string_view()));
        Check(!receiver.Chunk(stale,std::string_view(body.data(),CheckpointChunk)));
        for (std::size_t offset=0;offset<body.size();offset+=CheckpointChunk) {
            rawChunk["offset"]=offset;
            const std::string_view piece(body.data()+offset,(std::min)(CheckpointChunk,body.size()-offset));
            Check(receiver.Chunk(rawChunk,piece)); Check(!receiver.Chunk(rawChunk,piece));
        }
        Check(receiver.End(end) && receiver.Bytes()==body);
        rawChunk["offset"]=CheckpointChunk;
        const auto rawReplay=receiver.CompleteReplayAcknowledgment(rawChunk,"checkpoint_chunk",
            std::string_view(body.data()+CheckpointChunk,CheckpointChunk));
        Check(rawReplay && *rawReplay==2*CheckpointChunk);
        const std::string other(CheckpointChunk,'y');
        Check(!receiver.CompleteReplayAcknowledgment(rawChunk,"checkpoint_chunk",std::string_view(other)));
        // The leader's own proposal is adopted as a complete body with no
        // transfer, and only while no other body is held.
        Check(!receiver.Adopt(id,body,40000));
        receiver.Reset();
        Check(!receiver.Adopt(id,body.substr(1),40000));
        auto unnumbered=id; unnumbered.transfer=0; Check(!receiver.Adopt(unnumbered,body,40000));
        Check(receiver.Adopt(id,body,40000));
        Check(receiver.Active() && receiver.Complete() && receiver.Bytes()==body && !receiver.Expired(80000));
        const auto committed=id.Envelope("checkpoint_committed_local");
        Check(receiver.Identity().Matches(committed) && receiver.Identity().digest==id.digest);
        std::cout<<"Checkpoint digest, bounded transfer, raw-body chunks, adopted proposals, stale identity, and atomic import regressions passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
