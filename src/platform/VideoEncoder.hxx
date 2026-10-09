#pragma once

#include <memory>
#include <string>

// Ember's own video file: H.264 and AAC in an .mp4 through Media Foundation's
// sink writer, which takes the graphics card's encoder (NVENC, Quick Sync,
// AMF) when its driver offers one and Microsoft's software encoder otherwise.
// The encoder is asked for on the card Windows calls high performance (the
// discrete one of two), and where that card has none, Windows' own pick is
// taken as before. A picture past 4096x2160, which those H.264 encoders
// decline, is HEVC.
// The picture is whatever the caller hands over, frame by frame; the sound is
// one process's output, captured through WASAPI process loopback (Windows 10
// 2004 and later; without it the file has no sound). That capture comes
// after the process's volume in the Windows mixer, which is divided out again:
// a game turned down there is at full volume in the file. A muted one is silent.
//
// Not for the game's own process: the encoder keeps about 65 frames in system
// memory at any time (measured with NVIDIA's: 130 MB and 97 bytes a pixel,
// so 330 MB at 1080p and 940 at 4K; Microsoft's software one takes 950 at
// 1080p), which a 32-bit game does not have. VideoLink.hxx runs it in a
// process of its own. Past that queue, a picture that finds the encoder
// behind is dropped rather than kept, so the memory has a ceiling.
namespace sf4e { namespace platform { namespace video {

// One file from Begin to End, kept by whoever encodes it (the encoder's end
// of the link, VideoLinkServe.cxx). Used from one thread; the sound has a
// thread of its own, which End joins.
class Encoder {
public:
	Encoder();
	// Closes a file still open, as End does.
	~Encoder();
	Encoder(const Encoder&) = delete;
	Encoder& operator=(const Encoder&) = delete;

	// Opens the file and starts capturing the sound of the process soundPid.
	// Width and height are even.
	bool Begin(const std::wstring& file, unsigned width, unsigned height, unsigned long soundPid);
	// One NV12 picture (BT.709, 16 to 235) drawn at the Clock time at
	// (VideoLinkProtocol.hxx): height rows of width luma bytes, then height / 2
	// rows of width chroma bytes.
	void Frame(const void* luma, int lumaPitch, const void* chroma, int chromaPitch, long long at);
	// Closes the file. True when it holds at least one picture.
	bool End();
	// What was opened and how it closed, for the caller's log.
	const std::string& Summary() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} } }
