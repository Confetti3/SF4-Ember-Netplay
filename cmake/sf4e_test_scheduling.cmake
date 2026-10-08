# Scheduling for ctest --parallel. A test not named here shares nothing with
# another test: its files, pipes and named objects carry a pid or random name.

# These close a loopback UDP socket and hand its port number to GGPO, which
# binds it a moment later. A process that binds port 0 in between can be given
# the same port, so they never run beside each other.
set(sf4e_udp_port_tests GgpoConfirmedFrame GgpoDisconnectBeforeInput GgpoInputGap
    GgpoOutageTolerance GgpoPendingOutput GgpoSpectatorBacklog
    GgpoSpectatorHandleControl GgpoUdpValidation)
if(TEST HelperRust)
    # The helper binds UDP ports of its own, and its Rust tests release and
    # rebind ports the same way.
    list(APPEND sf4e_udp_port_tests HelperProcess HelperRust)
    # The runtime also opens settings.lock in the player's own settings folder
    # without sharing, as the overlay does, and a second opener fails.
    set_tests_properties(RuntimeBootstrap PROPERTIES RESOURCE_LOCK "loopback_udp_ports;user_settings")
    # Cargo compiles on every core, which starves the render sweeps beside it.
    set_tests_properties(HelperRust EmberRust PROPERTIES PROCESSORS 8)
endif()
set_tests_properties(${sf4e_udp_port_tests} PROPERTIES RESOURCE_LOCK loopback_udp_ports)
set_tests_properties(OverlayReset PROPERTIES RESOURCE_LOCK user_settings)

# Three threads spin while two checks allow a message 50 ms of wall time.
set_tests_properties(OverlayLifecycle PROPERTIES PROCESSORS 3)
set_tests_properties(PresentationSnapshot PROPERTIES PROCESSORS 2)
