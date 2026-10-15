# Every file the build takes from the unpacked Discord SDK: the headers the
# companion includes (discordpp.h includes cdiscord.h), the library it links,
# the DLL and the notice the install ships. A cached unpack missing any of
# them is unpacked again (sf4e_unpack_verified).
set(SF4E_DISCORD_SDK_FILES
    discord_social_sdk/License-Notices.txt
    discord_social_sdk/include/discordpp.h
    discord_social_sdk/include/cdiscord.h
    discord_social_sdk/lib/release/discord_partner_sdk.lib
    discord_social_sdk/bin/release/discord_partner_sdk.dll)
