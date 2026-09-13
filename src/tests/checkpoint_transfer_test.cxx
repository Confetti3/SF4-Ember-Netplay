#include "../session/CheckpointTransfer.hxx"
#include <iostream>
#include <stdexcept>
using namespace sf4e::coordination;
static void Check(bool value) { if (!value) throw std::runtime_error("checkpoint transfer regression"); }
int main() {
    try {
        Check(Sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        std::string decoded;
        Check(Decode("AAEC_f7_",decoded,6) && decoded==std::string("\0\1\2\xfd\xfe\xff",6));
        Check(!Decode("AB",decoded,4));
        Check(!Decode("A",decoded,4));
        Check(!Decode("AAA=",decoded,4));
        Check(!Decode("AAAA",decoded,2));
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
