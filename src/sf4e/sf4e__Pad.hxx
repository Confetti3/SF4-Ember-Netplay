#pragma once

#include "../Dimps/Dimps__Pad.hxx"

namespace sf4e {
	namespace Pad {
		void Install();
        bool MenuInputBlocked();

		struct System : Dimps::Pad::System
		{
			struct Inputs {
				unsigned int mappedOn;
				unsigned int rawOn;
			};

			static const int PLAYBACK_MAX = 60;
			static Inputs playbackData[PLAYBACK_MAX][2];
			static int playbackFrame;
			static void Install();
            void UpdateInputs();

			unsigned int GetButtons_RawOn(int pindex);
			unsigned int GetButtons_MappedOn(int pindex);
		};
	}
}
