# The runtime's sources (sf4e_runtime), relative to src/: the one list SF4
# Ember's build and the x64 library build (cmake/ember-x64-probe) both read,
# so the two cannot drift. The runtime reaches its game only through
# sf4e__GameAdapter.hxx and sf4e__RuntimeEvents.hxx.
set(SF4E_RUNTIME_SOURCES
    sf4e/sf4e__NetplayFacade.hxx
    sf4e/sf4e__NetplayFacade.cxx
    sf4e/sf4e__NetplayRuntime.hxx
    sf4e/sf4e__NetplayRuntime.cxx
    sf4e/sf4e__NetplayRuntime__AutoDelay.cxx
    sf4e/sf4e__NetplayRuntime__Input.cxx
    sf4e/sf4e__NetplayRuntime__Match.cxx
    sf4e/sf4e__NetplayRuntime__Room.cxx
    sf4e/sf4e__NetplayRuntime__Tournament.cxx
    sf4e/sf4e__NetplayRuntime__PublicRooms.cxx
    sf4e/sf4e__NetplayRuntime__Rankings.cxx
    sf4e/sf4e__NetplayRuntime__RandomFighter.cxx
    sf4e/sf4e__NetplayRuntime__Stages.cxx
    sf4e/sf4e__NetplayRuntime__Status.cxx
    sf4e/sf4e__NetplayPresentation.cxx
    sf4e/sf4e__RuntimeBridge.hxx
    sf4e/sf4e__RuntimeBridge.cxx
    sf4e/sf4e__RuntimeSession.hxx
    sf4e/sf4e__RuntimeSession.cxx
    sf4e/sf4e__RuntimeTick.hxx
    sf4e/sf4e__RuntimeTick.cxx
    sf4e/sf4e__ShellHost.hxx
    sf4e/sf4e__ShellHost.cxx
    sf4e/sf4e__GameAdapter.hxx
    sf4e/sf4e__RuntimeEvents.hxx)
