#pragma once

namespace sf4e { namespace netplay {
// Ember's own speech for its menus (platform/Speech.hxx), off until the
// player turns it on. rate is the voice's, -10 to 10 around its normal pace;
// volume is percent in steps of ten. details: a row's description is read by
// itself once the player stays on it, not only when asked for (F2). hints: its
// button hint is read with it. position: each row is followed by its place in
// the list ("3 of 12"). chime: a notice that opens by itself plays a short
// chime first, at chimeVolume percent in steps of ten. introHeard: speech
// has told the player how it works. describeUsed: how often the player has
// asked for a description, up to three. remind: a row with a description
// names the accessibility button after its name: Auto until describeUsed
// reaches three, On always, Off never; tick: past that, a soft tick does.
struct SpeechPreferences {
    enum Remind { Auto, Always, Never };
    bool enabled = false;
    int rate = 0;
    int volume = 100;
    bool details = false, hints = false, position = false, chime = true;
    int chimeVolume = 60;
    bool introHeard = false, tick = true;
    int describeUsed = 0, remind = Auto;
    // Whether a row with a description names the accessibility button now.
    bool Reminds() const { return remind == Always || (remind == Auto && describeUsed < 3); }
    bool operator==(const SpeechPreferences& other) const {
        return enabled == other.enabled && rate == other.rate && volume == other.volume && details == other.details && hints == other.hints &&
            position == other.position && chime == other.chime && chimeVolume == other.chimeVolume && introHeard == other.introHeard &&
            tick == other.tick && describeUsed == other.describeUsed && remind == other.remind;
    }
    bool Valid() const {
        return rate >= -10 && rate <= 10 && volume >= 10 && volume <= 100 && volume % 10 == 0 && chimeVolume >= 10 && chimeVolume <= 100 && chimeVolume % 10 == 0 &&
            describeUsed >= 0 && describeUsed <= 3 && remind >= Auto && remind <= Never;
    }
};
} }
