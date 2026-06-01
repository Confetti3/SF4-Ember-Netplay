if(NOT QT_CONF_PATH)
  message(FATAL_ERROR "QT_CONF_PATH not set")
endif()
file(WRITE "${QT_CONF_PATH}" "[Paths]\nPrefix=.\nPlugins=plugins\n")
