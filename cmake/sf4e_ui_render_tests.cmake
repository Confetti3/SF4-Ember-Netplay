# UiRenderTest draws every screen for each locale at each window size. As one
# process the English sweep took three to nine minutes and the translations
# fourteen, past their timeout. Each sweep is cut into shards that CTest can run
# side by side. Shard K of N takes every Nth locale and size configuration
# starting at K, so the shards share none and between them miss none, and each
# prints how many of the whole sweep it drew.
set(SF4E_UI_RENDER_SHARDS 4)
math(EXPR sf4e_ui_render_last_shard "${SF4E_UI_RENDER_SHARDS} - 1")
foreach(shard RANGE ${sf4e_ui_render_last_shard})
    # English and the padded pseudo locale at every size.
    add_test(NAME UiRender.${shard} COMMAND UiRenderTest)
    set_tests_properties(UiRender.${shard} PROPERTIES TIMEOUT 300
        ENVIRONMENT "SF4E_UI_RENDER_LOCALES=:${shard}/${SF4E_UI_RENDER_SHARDS}")
    # Each translation at three sizes.
    add_test(NAME UiRenderLocales.${shard} COMMAND UiRenderTest)
    set_tests_properties(UiRenderLocales.${shard} PROPERTIES TIMEOUT 600
        ENVIRONMENT "SF4E_UI_RENDER_LOCALES=translations:${shard}/${SF4E_UI_RENDER_SHARDS}")
endforeach()
# The atlas rebuild on its own: every language and scale within the atlas
# budget, then random player text through many rebuilds.
add_test(NAME UiRenderAtlas COMMAND UiRenderTest)
set_tests_properties(UiRenderAtlas PROPERTIES TIMEOUT 300 ENVIRONMENT "SF4E_UI_RENDER_ATLAS_STRESS=150")
