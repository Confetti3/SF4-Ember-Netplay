#pragma once

#include <windows.h>

namespace Dimps {
	namespace Pad {
		void Locate(HMODULE peRoot);
		
		enum PadType {
			PADTYPE_RAWINPUT = 1,
			PADTYPE_XINPUT = 3,
            PADTYPE_DIRECTINPUT = 4,
		};

		// Game thread, main menu or offline training; never assigns a side.
		bool ReadController(int deviceType, int deviceIndex, unsigned int& held, unsigned int* physicalButtons = nullptr,
                            unsigned int* selectPhysical = nullptr, unsigned int* backPhysical = nullptr);
		struct System {
			static const int BUTTON_MAPPING_FIGHT;
			static const int BUTTON_MAPPING_MENU;

			struct PlayerEntry {
				BYTE pad[80];

				static int* DeviceType(PlayerEntry* e);
				static int* DeviceIndex(PlayerEntry* e);
				static int* AssignedController(PlayerEntry* e);
			};

			typedef struct __publicMethods {
				void (System::* UpdateInputs)();
				unsigned int(System::* GetButtons_RawOn)(int pindex);
				unsigned int(System::* GetButtons_MappedOn)(int pindex);
				unsigned int(System::* GetButtons_RawRising)(int pindex);
				unsigned int(System::* GetButtons_RawFalling)(int pindex);
				unsigned int(System::* GetButtons_RawRisingWithRepeat)(int pindex);
				int(System::* GetAllDeviceCount)();
				int(System::* GetOKDeviceCount)();
				char*(System::* GetDeviceName)(int dindex);
				int (System::* GetDeviceIndexForPlayer)(int pindex);
				int (System::* GetDeviceTypeForPlayer)(int pindex);
				int (System::* GetAssigmentStatusForPlayer)(int pindex);
				void (System::* AssociatePlayerAndGamepad)(int side, int deviceIndex);
				void (System::* SetSideHasAssignedController)(int side, int clearIfFalse);
				void (System::* SetDeviceTypeForPlayer)(int side, int deviceType);
				void (System::* SetActiveButtonMapping)(int buttonMapping);
				int (System::* CaptureNextMatchingPadToSide)(int side, DWORD inputMask, int unk3);
			} __publicMethods;

			typedef struct __staticMethods {
				System* (*GetSingleton)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		struct System_RawInput {
			typedef struct __publicMethods {
				unsigned int(System_RawInput::* SetDeviceInUse)(int deviceIdx, int bInUse);
			} __publicMethods;

			typedef struct __staticMethods {
				System_RawInput* (*GetSingleton)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		struct System_XInput {
			typedef struct __publicMethods {
                int(System_XInput::* GetDeviceCount)();
                int(System_XInput::* GetDeviceStatus)(int index);
                char*(System_XInput::* GetDeviceName)(int index);
                unsigned int(System_XInput::* GetButtonsOn)(int index);
				unsigned int(System_XInput::* SetDeviceInUse)(int deviceIdx, int bInUse);
			} __publicMethods;

			typedef struct __staticMethods {
				System_XInput* (*GetSingleton)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};
	}
}
